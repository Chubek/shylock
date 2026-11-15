"""Explicit finalized booking outcomes -> replayable demand -> priceopt."""
import copy
import json
import math
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
FIXTURE = ROOT / "tests/fixtures/booking-outcomes.json"
AS_OF = 1800000000


def run(*args, code=0):
    proc = subprocess.run([str(ROOT / "shylock"), *map(str, args)], cwd=ROOT,
                          capture_output=True, text=True, timeout=30)
    assert proc.returncode == code, (args, proc.returncode, proc.stdout, proc.stderr)
    if code:
        assert not proc.stdout, (args, proc.stdout)
        assert proc.stderr, args
    return json.loads(proc.stdout) if proc.stdout else None


with tempfile.TemporaryDirectory(prefix="shylock-demand-") as directory:
    folder = Path(directory)
    source, artifact = folder / "outcomes.json", folder / "demand.json"
    original = json.loads(FIXTURE.read_text())

    def derive(data, *, cutoff=AS_OF, capacity=10, minimum=2, code=0):
        source.write_text(json.dumps(data))
        return run("demand", source, cutoff, capacity, minimum, code=code)

    demand = derive(original)
    assert demand == derive(original)
    assert demand["schema"] == "shylock.demand/v1"
    assert demand["source"] == original and demand["estimator"] == "booked-share/v1"
    assert len(demand["source_id"]) == len(demand["demand_id"]) == 64
    scenarios = demand["scenarios"]
    assert [s["rate_minor"] for s in scenarios] == [10000, 20000]
    assert [s["offered_units"] for s in scenarios] == [3, 3]
    assert [s["booked_units"] for s in scenarios] == [2, 1]
    assert math.isclose(scenarios[0]["expected_units"], 20 / 3)
    assert math.isclose(scenarios[1]["expected_units"], 10 / 3)
    assert derive(dict(original, rows=list(reversed(original["rows"]))))["scenarios"] == scenarios
    # Canonical source IDs don't depend on whitespace or object key order.
    source.write_text(json.dumps(original, sort_keys=True, indent=2))
    assert run("demand", source, AS_OF, 10, 2) == demand

    artifact.write_text(json.dumps(demand))
    optimized = run("optimize", artifact, 10000, 20000, 100, 2000)
    assert optimized["recommended_minor"] == 20000
    assert optimized["demand_origin"] == "empirical_booking_outcomes"
    assert optimized["demand_lineage"] == demand
    assert math.isclose(optimized["expected_profit_minor"], 60000)
    assert run("optimize", artifact, 10000, 19999, 100, 2000)["recommended_minor"] == 10000
    run("optimize", artifact, 10001, 19999, 100, 2000, code=2)
    run("optimize", artifact, 0, 20000, 0, 0, code=2)

    # Every derived field is checked by replay, not just trusting the hash.
    changes = [("expected_units", 100), ("offered_units", 100), ("booked_units", 0), ("rate_minor", 1)]
    for key, value in changes:
        forged = copy.deepcopy(demand)
        forged["scenarios"][0][key] = value
        artifact.write_text(json.dumps(forged))
        run("optimize", artifact, 0, 20000, 1, 0, code=2)
    for change in ({"demand_id": "f" * 64}, {"source_id": "f" * 64}, {"estimator": "guess/v1"},
                   {"capacity": 20}, {"schema": "shylock.demand/v2"}, {"surprise": 1}):
        artifact.write_text(json.dumps(dict(demand, **change)))
        run("optimize", artifact, 0, 20000, 1, 0, code=2)

    # A self-consistent outer hash is insufficient: replay must check the counts.
    # Integer expected units let Python serialize the exact canonical numeric form.
    forged = derive(original, capacity=3)
    forged["scenarios"][0]["booked_units"] = 3
    forged["scenarios"][0]["expected_units"] = 3
    del forged["demand_id"]
    canonical = folder / "canonical.json"
    canonical.write_text(json.dumps(forged, sort_keys=True, separators=(",", ":")))
    forged["demand_id"] = run("extract", canonical, "https://example.org/audit", 1000)["blake3"]
    artifact.write_text(json.dumps(forged))
    run("optimize", artifact, 0, 20000, 1, 0, code=2)

    # Duplicate methods/IDs and cross-source corroboration never overweight a night.
    duplicated = copy.deepcopy(original)
    alias = dict(duplicated["rows"][0], opportunity_id="alias", source_ref="independent:receipt")
    duplicated["rows"] += [copy.deepcopy(duplicated["rows"][0]), alias]
    assert derive(duplicated)["scenarios"] == scenarios
    for key, value in (("rate_minor", 12345), ("outcome", "not_booked"), ("listing_id", "other")):
        conflict = copy.deepcopy(duplicated)
        conflict["rows"].append(dict(alias, **{key: value}))
        derive(conflict, code=3)

    future = copy.deepcopy(original)
    future["rows"].append(dict(future["rows"][0], opportunity_id="future", listing_id="future",
                               finalized_at=AS_OF + 1))
    assert derive(future)["scenarios"] == scenarios
    assert derive(future, cutoff=AS_OF + 1)["scenarios"][0]["offered_units"] == 4
    derive(original, cutoff=1793491199, code=2)
    derive(original, minimum=4, code=2)
    derive(original, minimum=0, code=2)
    derive(original, capacity=0, code=2)
    derive(original, capacity=1000001, code=2)
    derive(original, cutoff=-1, code=2)

    for key, value in (("schema", "shylock.capture/v2"), ("currency", ""), ("scale", 3),
                       ("scale", 2.5), ("basis", "stay_total"), ("cohort", ""), ("extra", 1)):
        derive(dict(original, **{key: value}), code=2)
    for change in ({"guests": 0}, {"guests": True}, {"taxes": "unknown"}, {"fees": None}, {"extra": 1}):
        derive(dict(original, context=dict(original["context"], **change)), code=2)
    for key, value in (("stay_date", "2026-02-30"), ("stay_date", "1969-12-31"),
                       ("stay_date", "2026-11-01"), ("finalized_at", 0), ("finalized_at", "1793491200"),
                       ("finalized_at", 253402300800), ("rate_minor", -1), ("rate_minor", 1.1),
                       ("rate_minor", True), ("rate_minor", 1000000000001), ("outcome", "unavailable"),
                       ("source_ref", ""), ("listing_id", "a\x00b"), ("extra", 1)):
        bad = copy.deepcopy(original)
        bad["rows"][0][key] = value
        derive(bad, code=2)
    missing = copy.deepcopy(original)
    del missing["rows"][0]["outcome"]
    derive(missing, code=2)
    for rows in ([], {}, [None], original["rows"] * 683):
        derive(dict(original, rows=rows), code=2)
    for raw in ("{", "[]", "null", '{"schema":"x"}', '{"rows":[]}' + " " * (16 * 1024 * 1024)):
        source.write_text(raw)
        run("demand", source, AS_OF, 10, 2, code=2)
    run("demand", folder / "missing", AS_OF, 10, 2, code=2)
    run("optimize", folder / "missing", 0, 10000, 100, 0, code=2)
    derive(dict(original, cohort="x" * (4 * 1024 * 1024 + 1)), code=2)
    # Escaped labels can inflate serialization beyond the On1x text limit.
    # Reject recoverably instead of tripping the runtime's fatal buffer bound.
    oversized = dict(original, rows=[dict(original["rows"][0], opportunity_id="\r" * 256,
                                         listing_id="\t" * 256, source_ref="\x01" * 256)] * 1024)
    derive(oversized, minimum=1, code=2)

    for currency, scale in (("JPY", 0), ("KWD", 3)):
        assert derive(dict(original, currency=currency, scale=scale))["scenarios"] == scenarios
    quoted = dict(original, cohort='cohort "quoted" \\ unicode é')
    assert derive(quoted)["source"] == quoted
    extremes = copy.deepcopy(original)
    for row in extremes["rows"]:
        row["rate_minor"] = 0 if row["rate_minor"] == 10000 else 1000000000000
        row["outcome"] = "not_booked"
    assert all(s["expected_units"] == 0 for s in derive(extremes, capacity=1000000)["scenarios"])

print("Booking-outcome demand, cutoff/deduplication, replay and optimization tests passed.")
