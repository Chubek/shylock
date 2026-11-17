"""Chronological model selection, checked quotes and end-to-end pricing policy."""
import copy
from decimal import Decimal, ROUND_HALF_UP
import json
import math
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
FIXTURE = ROOT / "tests/fixtures"
BINARY = Path(os.environ.get("SHY_BINARY", ROOT / "build/shylock")).resolve()


def run(*args, code=0, config=None, sandbox="off", raw=False):
    env = dict(os.environ, SHY_SANDBOX=sandbox)
    env.pop("SHY_FORMULA_SANDBOX", None)
    env.pop("SHY_EXTENSIONS", None)
    if config:
        env["SHY_EXTENSIONS"] = str(config)
    p = subprocess.run([str(ROOT / "shylock"), *map(str, args)], cwd=ROOT, env=env,
                       capture_output=True, text=True, timeout=30)
    assert p.returncode == code, (args, p.returncode, p.stdout, p.stderr)
    if raw:
        return p
    return json.loads(p.stdout) if p.stdout else None


with tempfile.TemporaryDirectory(prefix="shylock-engine-") as directory:
    folder = Path(directory)
    policy_file, values_file = folder / "policy.json", folder / "values.json"

    def save(path, data):
        path.write_text(json.dumps(data))
        return path

    default = json.loads((ROOT / "policies/forecast.json").read_text())
    baseline = run("forecast", FIXTURE / "series.json", ROOT / "policies/forecast.json")
    assert baseline["selected_estimator"]["model"] == "robust-trend"
    assert baseline["forecast_minor"] == baseline["reference_minor"] == 21000
    assert baseline["interval"]["radius_minor"] == 0
    assert baseline == run("forecast", FIXTURE / "series.json", ROOT / "policies/forecast.json")
    assert len(baseline["forecast_id"]) == 64
    ar = next(c for c in baseline["candidates"] if c["candidate"]["model"] == "ar1")
    assert ar["selection"]["coverage"] == 0 and ar["selection"]["mae_minor"] is None

    chronological = dict(default, horizon=2, window=4, selection_samples=3, validation_samples=3)
    chronological["candidates"] = [{"name": "last", "model": "last"}, {"name": "trend", "model": "robust-trend"}]
    history = [10000 + 100 * i for i in range(12)]
    first = run("forecast", save(values_file, history), save(policy_file, chronological))
    assert first["selected_estimator"]["name"] == "trend" and first["validation"]["mae_minor"] == 0
    assert [f["target_index"] for f in first["validation"]["folds"]] == [9, 10, 11]
    assert first["validation"]["folds"][0]["window_start_index"] == 4
    second = run("forecast", save(values_file, history[:9] + [90000, 80000, 70000]), policy_file)
    assert first["candidates"] == second["candidates"]  # No validation leakage into selection.
    assert first["selected_estimator"] == second["selected_estimator"]
    assert first["validation"]["folds"][0]["forecast_minor"] == second["validation"]["folds"][0]["forecast_minor"]

    # Exercise all available histprice models and baseline families separately.
    seasonal = json.loads((ROOT / "policies/seasonal.json").read_text())
    candidates = default["candidates"] + [seasonal["candidates"][-1]]
    for candidate in candidates:
        p = dict(seasonal, candidates=[candidate], window=16)
        result = run("forecast", FIXTURE / "seasonal-history.json", save(policy_file, p))
        assert result["validation"]["coverage"] == 1 and result["forecast_minor"] >= 0
        if candidate["model"] == "kalman":
            assert result["model_variance_minor_squared"] > 0
        else:
            assert result["model_variance_minor_squared"] is None
    result = run("forecast", FIXTURE / "seasonal-history.json", ROOT / "policies/seasonal.json")
    assert result["selected_estimator"]["model"] == "seasonal" and result["forecast_minor"] == 12000

    for method, parameter in (("median", 0), ("huber", 0), ("trimmed", 100000), ("winsorized", 100000),
                              ("quantile", 1000000), ("kde", 0)):
        p = dict(default, market_method=method, market_parameter_ppm=parameter)
        result = run("forecast", FIXTURE / "series.json", save(policy_file, p))
        assert math.isfinite(result["market_reference_minor"])
        if method == "quantile":
            assert result["market_reference_minor"] == 20000
    only_ar = dict(default, candidates=[{"name": "unstable", "model": "ar1"}])
    failed = run("forecast", save(values_file, [10000] * 12), save(policy_file, only_ar), code=2)
    assert failed["status"] == "no_eligible_model" and failed["forecast_minor"] is None
    assert failed["selected_estimator"] is None
    for change in ({"horizon": 0}, {"window": 1}, {"window": 1025}, {"selection_samples": 0},
                   {"validation_samples": 65}, {"interval_bp": 10000}, {"minimum_training": True},
                   {"market_method": "guess"}, {"market_parameter_ppm": 1}, {"extra": 1},
                   {"candidates": default["candidates"] * 3}):
        assert run("forecast", FIXTURE / "series.json", save(policy_file, dict(default, **change)), code=2) is None
    for candidate in ({"name": "x", "model": "unknown"}, {"name": "x", "model": "ema", "alpha_bp": 10001},
                      {"name": "x", "model": "holt", "alpha_bp": 100, "beta_bp": 100, "phi_bp": 0},
                      {"name": "x", "model": "kalman", "process_variance": 0, "measurement_variance": 0}):
        assert run("forecast", FIXTURE / "series.json", save(policy_file, dict(default, candidates=[candidate])), code=2) is None

    quote = json.loads((ROOT / "policies/quote.json").read_text())
    assert run("quote", ROOT / "policies/quote.json")["total_minor"] == 76560
    for currency, scale in (("EUR", 2), ("JPY", 0), ("KWD", 3)):
        for mode in ("inclusive", "exclusive"):
            for scope in ("accommodation", "accommodation_and_fees"):
                q = copy.deepcopy(quote)
                q.update(currency=currency, scale=scale, rate_minor=12345)
                q["charges"].update(tax_mode=mode, tax_scope=scope, nights=7, tax_bp=1750)
                result = run("quote", save(policy_file, q))
                base = 12345 * 7
                taxable = base + (7500 if scope == "accommodation_and_fees" else 0)
                denominator = 11750 if mode == "inclusive" else 10000
                tax = int((Decimal(taxable) * 1750 / denominator).quantize(Decimal(1), rounding=ROUND_HALF_UP))
                assert result["tax_minor"] == tax
                assert result["total_minor"] == base + 7500 + (tax if mode == "exclusive" else 0)
                assert result["subtotal_ex_tax_minor"] + tax == result["total_minor"]
                assert result == run("quote", policy_file)
    q = copy.deepcopy(quote)
    q.update(rate_minor=1)
    q["charges"].update(nights=3, cleaning_minor=0, tax_bp=5000, tax_mode="exclusive")
    assert run("quote", save(policy_file, q))["tax_minor"] == 2  # Aggregate, not per-night rounding.
    for change in ({"nights": 0}, {"nights": 366}, {"nights": True}, {"tax_bp": 10001}, {"tax_mode": "unknown"}, {"extra": 1}):
        q = copy.deepcopy(quote)
        q["charges"].update(change)
        assert run("quote", save(policy_file, q), code=2) is None
    q = dict(quote, rate_minor=1000000000000)
    assert run("quote", save(policy_file, q), code=2) is None

    models = json.loads((ROOT / "policies/model.json").read_text())
    result = run("optimize-model", ROOT / "policies/model.json")
    def model_score(rate, p):
        scores, weights = [], []
        for m in p["models"]:
            shape = m["shape_ppm"] / 1000000
            if m["kind"] == "linear":
                units = max(0, m["scale_units"] - shape * rate)
            elif m["kind"] == "isoelastic":
                units = m["scale_units"] * (rate / m["reference_minor"]) ** -shape
            else:
                units = m["scale_units"] / (1 + math.exp((rate - m["reference_minor"]) / shape))
            scores.append((rate - (p["constraints"]["unit_cost_minor"] if p["constraints"]["objective"] == "profit" else 0)) * units)
            weights.append(m["weight_bp"])
        mean = sum(s*w for s, w in zip(scores, weights)) / sum(weights)
        std = math.sqrt(sum(w*(s-mean)**2 for s, w in zip(scores, weights)) / sum(weights))
        return mean - p["risk_bp"] / 10000 * std
    expected = max(range(10000, 30001, 100), key=lambda rate: (model_score(rate, models), -rate))
    assert result["recommended_minor"] == expected == 15200
    assert result["risk_adjusted_score_minor"] < result["expected_profit_minor"]
    for kind, shape in (("isoelastic", 2000000), ("logistic", 4000000000)):
        p = copy.deepcopy(models)
        p["models"] = [dict(p["models"][0], kind=kind, reference_minor=18000, shape_ppm=shape)]
        p["risk_bp"] = 0
        answer = run("optimize-model", save(policy_file, p))
        expected = max(range(10000, 30001, 100), key=lambda rate: (model_score(rate, p), -rate))
        assert answer["recommended_minor"] == expected
    p = copy.deepcopy(models)
    p["constraints"].update(previous_minor=18000, max_change_bp=0)
    assert run("optimize-model", save(policy_file, p))["recommended_minor"] == 18000
    p["constraints"]["previous_minor"] = 18050
    assert run("optimize-model", save(policy_file, p), code=2) is None
    p = copy.deepcopy(models)
    p["constraints"].update(tick_minor=1, floor_minor=0, cap_minor=1000000)
    assert run("optimize-model", save(policy_file, p), code=2) is None
    p = copy.deepcopy(models)
    for row in p["models"]:
        row["weight_bp"] = 0
    assert run("optimize-model", save(policy_file, p), code=2) is None

    # Produce an actual verified cohort, rather than constructing an unsigned fake.
    db, capture_file = folder / "prices.db", folder / "capture.json"
    for i in range(12):
        captured = run("extract", FIXTURE / "rental.html", "https://example.org/cottage", 1000 + 60*i)
        run("review", db, save(capture_file, captured), 1, "cottages/2-guests", "tester",
            FIXTURE / "rental.html", FIXTURE / "context.json")
    reviewed = run("cohort", db, "cottages/2-guests", "EUR", 1000, 60, 12)
    cohort_file = save(folder / "cohort.json", reviewed)
    pricing = json.loads((ROOT / "policies/pricing.json").read_text())
    pricing["formula"]["path"] = str(ROOT / "formulas/weekend.lua")
    result = run("price", cohort_file, save(policy_file, pricing))
    assert result["status"] == "priced" and result["recommended_minor"] == 19800
    assert result["forecast"]["cohort_lineage"] == reviewed and result["quote"]["total_minor"] == 67900
    assert result["formula"]["reference_minor"] == 18000 and result["formula"]["effective_cap_minor"] == 19800
    assert result == run("price", cohort_file, policy_file)
    assert len(result["decision_id"]) == len(result["policy_id"]) == len(result["quote"]["quote_id"]) == 64
    # The final interval, not just its latest capture, must have ended at as_of.
    interval_end = reviewed["start"] + reviewed["step"] * reviewed["count"] - 1
    assert run("price", cohort_file, save(policy_file, dict(pricing, as_of=interval_end)))["status"] == "priced"
    assert run("price", cohort_file, save(policy_file, dict(pricing, as_of=interval_end-1)), code=2) is None
    save(policy_file, pricing)
    observed = run("price", cohort_file, policy_file, config=ROOT / "extensions/example.json", raw=True)
    assert json.loads(observed.stdout) == result
    audits = [json.loads(line) for line in observed.stderr.splitlines() if line.startswith("{")]
    assert [a["phase"] for a in audits] == ["on_load", "forecast.created", "decision.created", "on_unload"]
    for change in ({"currency": "USD"}, {"as_of": 1000}, {"as_of": 1900000000}, {"extra": 1}):
        assert run("price", cohort_file, save(policy_file, dict(pricing, **change)), code=2) is None
    p = copy.deepcopy(pricing)
    p["charges"]["nights"] = 4
    assert run("price", cohort_file, save(policy_file, p), code=2) is None
    p["charges"].update(nights=3, tax_mode="exclusive")
    assert run("price", cohort_file, save(policy_file, p), code=2) is None
    assert run("price", FIXTURE / "series.json", save(policy_file, pricing), code=2) is None

    bad_formula = folder / "bad.lua"
    bad_formula.write_text("return function() return host.log('not allowed') end")
    p = copy.deepcopy(pricing)
    p["formula"]["path"] = str(bad_formula)
    failed = run("price", cohort_file, save(policy_file, p), code=2)
    assert failed["status"] == "formula_error" and failed["forecast"]["forecast_minor"] == 18000
    assert failed["recommended_minor"] is None and failed["quote"] is None and failed["formula"]["error"]
    p = copy.deepcopy(pricing)
    p["forecast"]["candidates"] = [{"name": "constant AR", "model": "ar1"}]
    failed = run("price", cohort_file, save(policy_file, p), code=2)
    assert failed["status"] == "forecast_error" and failed["forecast"]["status"] == "no_eligible_model"
    assert failed["formula"] is None

    demand_file = save(folder / "demand.json", run("demand", FIXTURE / "booking-outcomes.json", pricing["as_of"], 10, 2))
    p = copy.deepcopy(pricing)
    p["decision"] = "demand"
    p["constraints"].update(previous_minor=0, max_change_bp=0)
    demanded = run("price", cohort_file, save(policy_file, p), demand_file)
    assert demanded["formula_adjusted_minor"] == 20700 and demanded["recommended_minor"] == 20000
    assert demanded["quote"]["total_minor"] == 68500
    assert demanded["optimization"]["objective_basis"] == "quoted-nightly-rate/v1"
    assert demanded["demand_lineage"] == json.loads(demand_file.read_text())
    p["constraints"]["floor_minor"] = 21000
    failed = run("price", cohort_file, save(policy_file, p), demand_file, code=2)
    assert failed["status"] == "demand_error" and failed["formula_adjusted_minor"] == 21000
    assert failed["recommended_minor"] is None  # No silent formula fallback.
    p["constraints"]["floor_minor"] = 10000
    save(policy_file, p)
    future = save(folder / "future.json", run("demand", FIXTURE / "booking-outcomes.json", pricing["as_of"] + 1, 10, 2))
    assert run("price", cohort_file, policy_file, future, code=2)["status"] == "demand_error"
    forged = json.loads(demand_file.read_text())
    forged["scenarios"][0]["expected_units"] = 1000
    assert run("price", cohort_file, policy_file, save(future, forged), code=2)["status"] == "demand_error"
    run("price", cohort_file, policy_file, FIXTURE / "demand.json", code=2)
    sandbox = subprocess.run([str(BINARY.parent / "sandbox-probes")], capture_output=True, text=True, check=True)
    if json.loads(sandbox.stdout)["supported"]:
        isolated = run("price", cohort_file, save(policy_file, pricing), sandbox="process")
        assert isolated["recommended_minor"] == result["recommended_minor"]
        assert isolated["formula"]["isolation"] == "process"
    data_file = save(folder / "decision.json", result)
    tex = run("print-report", ROOT / "reports/pricing.json", data_file, raw=True).stdout
    assert "67900" in tex and "\\begin{document}" in tex

    # Captures within a bucket may occur after its start. Both backtesting APIs
    # must label the true complete training-bucket cutoff, strictly before target.
    for i in range(5):
        captured = run("extract", FIXTURE / "rental.html", "https://example.org/offset", 1000 + 60*i + 59)
        run("review", db, save(capture_file, captured), 1, "offset", "tester",
            FIXTURE / "rental.html", FIXTURE / "context.json")
    offset = run("cohort", db, "offset", "EUR", 1000, 60, 5)
    selected = save(folder / "offset.json", offset)
    p = dict(default, minimum_training=3, horizon=1, selection_samples=1, validation_samples=1)
    evaluated = run("forecast", selected, save(policy_file, p))
    two_step = run("forecast", selected, save(policy_file, dict(p, horizon=2, minimum_training=2,
                   candidates=[{"name": "last", "model": "last"}])))
    backtested = run("backtest", selected, 1, 2)
    for report in (evaluated, two_step, backtested, run("backtest", selected, 2, 2)):
        assert report["cutoff_policy"] == "complete-training-buckets/v1"
        folds = report["folds"] if "folds" in report else report["validation"]["folds"] + [
            fold for candidate in report["candidates"] for fold in candidate["selection"]["folds"]]
        for fold in folds:
            n = fold["training_samples"]
            assert fold["cutoff"] == 1000 + n*60 - 1
            assert fold["cutoff"] < fold["target_at"]
            assert all(ref["captured_at"] <= fold["cutoff"] for bucket in offset["buckets"][:n] for ref in bucket["evidence"])
    p = copy.deepcopy(pricing)
    p["forecast"].update(minimum_training=3, selection_samples=1, validation_samples=1)
    p["as_of"] = 1299
    assert run("price", selected, save(policy_file, p))["status"] == "priced"
    assert run("price", selected, save(policy_file, dict(p, as_of=1298)), code=2) is None

    # Stay length is Gregorian, not a subtraction of YYYYMMDD encodings.
    for name, start, end, nights in (("leap", "2028-02-28", "2028-03-01", 2),
                                      ("year", "2026-12-31", "2027-01-02", 2),
                                      ("century", "2100-02-28", "2100-03-01", 1)):
        context = json.loads((FIXTURE / "context.json").read_text())
        context.update(stay_start=start, stay_end=end)
        context_file = save(folder / "context.json", context)
        for i in range(5):
            capture = run("extract", FIXTURE / "rental.html", f"https://example.org/{name}", 1000 + 60*i)
            run("review", db, save(capture_file, capture), 1, name, "tester", FIXTURE / "rental.html", context_file)
        selected = save(folder / f"{name}.json", run("cohort", db, name, "EUR", 1000, 60, 5))
        p = copy.deepcopy(pricing)
        p["forecast"].update(selection_samples=1, validation_samples=1)
        p["charges"]["nights"] = nights
        assert run("price", selected, save(policy_file, p))["quote"]["charges"]["nights"] == nights

    # Buffered stdout failure is a command failure, even after a valid result
    # was constructed. This exercises real publication rather than a mock sink.
    if Path("/dev/full").exists():
        env = dict(os.environ, SHY_SANDBOX="off")
        for key in ("SHY_EXTENSIONS", "SHY_FORMULA_SANDBOX", "SHY_NATIVE_PLUGINS"):
            env.pop(key, None)
        commands = [("forecast", FIXTURE / "series.json", ROOT / "policies/forecast.json"),
                    ("backtest", FIXTURE / "series.json", 1, 2),
                    ("quote", ROOT / "policies/quote.json"), ("optimize-model", ROOT / "policies/model.json"),
                    ("price", cohort_file, save(policy_file, pricing))]
        for command in commands:
            with open("/dev/full", "wb") as sink:
                failed = subprocess.run([str(ROOT / "shylock"), *map(str, command)], cwd=ROOT, env=env,
                                        stdout=sink, stderr=subprocess.PIPE, timeout=30)
            assert failed.returncode == 2 and b"Sanitizer" not in failed.stderr, (command, failed.returncode, failed.stderr)

print("Forecast selection/holdout, kernels, quotes, scenario risk and verified pricing tests passed.")
