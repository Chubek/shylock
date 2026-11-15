"""Offline capture -> reviewed observations -> versioned cohort -> decision."""
import concurrent.futures
import copy
import json
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
FIXTURE = ROOT / "tests/fixtures"


def run(*args, code=0):
    proc = subprocess.run([str(ROOT / "shylock"), *map(str, args)], cwd=ROOT,
                          text=True, capture_output=True, timeout=15)
    assert proc.returncode == code, (args, proc.returncode, proc.stdout, proc.stderr)
    return json.loads(proc.stdout) if proc.stdout else None


with tempfile.TemporaryDirectory(prefix="shylock-pipeline-") as directory:
    folder = Path(directory)
    db, artifact = folder / "prices.db", folder / "extraction.json"
    page, ctx = FIXTURE / "competing.html", FIXTURE / "context.json"
    context = json.loads(ctx.read_text())
    cohort = "cottage/weekend/2-guests"
    url = "https://example.org/cottage"
    first = run("extract", page, url, 1000)
    assert first == run("extract", page, url, 1000)
    assert first["schema"] == "shylock.capture/v2"
    ids = [row["evidence_id"] for row in first["candidates"]]
    assert len(ids) == len(set(ids))
    assert first["candidates"][2]["currency"] == ""
    assert first["candidates"][2]["minor"] is None
    assert first["candidates"][2]["scale"] is None
    assert {"currency", "scale", "listing_id", "guests"} <= set(first["candidates"][2]["unresolved_fields"])
    assert first["candidates"][3]["amount_text"] == "180"
    artifact.write_text(json.dumps(first, sort_keys=True, indent=2))

    # Byte verification AND deterministic replay, not just trusting a supplied digest.
    for field, value in (("minor", 1), ("byte_offset", 0), ("currency", "USD"), ("evidence_id", "a" * 64)):
        forged = copy.deepcopy(first)
        forged["candidates"][4][field] = value
        artifact.write_text(json.dumps(forged))
        run("review", db, artifact, 4, cohort, "alice", page, ctx, code=2)
    forged = dict(first, captured_at=1060)
    artifact.write_text(json.dumps(forged))
    run("review", db, artifact, 4, cohort, "alice", page, ctx, code=2)
    changed_page = folder / "changed.html"
    changed_page.write_bytes(page.read_bytes() + b" ")
    artifact.write_text(json.dumps(first))
    run("review", db, artifact, 4, cohort, "alice", changed_page, ctx, code=2)
    assert run("ledger", db, cohort) == []

    # Unknown context is never silently admitted into a verified cohort.
    run("review", db, artifact, 4, cohort, "alice", page, code=2)
    bad_context = folder / "bad-context.json"
    for change in ({"stay_start": "2026-02-30"}, {"guests": 0}, {"guests": 2.5},
                   {"stay_end": "2026-12-03"}, {"taxes": "unknown"}, {"scale": 3},
                   {"currency": "USD"}, {"guest": 2}):
        bad_context.write_text(json.dumps(dict(context, **change)))
        run("review", db, artifact, 4, cohort, "alice", page, bad_context, code=2)
    run("review", db, artifact, 8, cohort, "alice", page, ctx, code=2)  # "from" is not an exact quote
    run("review", db, artifact, 7, cohort, "alice", page, ctx, code=2)  # a stay total cannot become a nightly rate

    # Incomplete evidence can be rejected without first pretending it is accepted.
    rejected_db = folder / "rejected.db"
    reject_args = ("reject", rejected_db, artifact, 3, cohort, "alice", "currency missing", page)
    run(*reject_args)
    run(*reject_args)
    rejected = run("ledger", rejected_db, cohort)[0]
    assert rejected["decision"] == "rejected" and rejected["minor"] is None
    assert rejected["evidence"]["currency"] == ""
    assert len(run("history", rejected_db, rejected["observation_id"])) == 1
    resolved_context = folder / "resolved-context.json"
    resolved_context.write_text(json.dumps(dict(context, currency="EUR")))
    run("revise", rejected_db, rejected["observation_id"], rejected["review_id"], "accepted",
        "bob", "currency confirmed against original source", resolved_context)
    reaccepted = run("ledger", rejected_db, cohort)[0]
    assert reaccepted["decision"] == "accepted" and reaccepted["minor"] == 18000
    assert reaccepted["admission"] == "comparable/v1"
    assert reaccepted["evidence"]["currency"] == ""
    assert len(run("history", rejected_db, rejected["observation_id"])) == 2

    # Replays and concurrent identical imports are idempotent under the writer lock.
    args = ("review", db, artifact, 4, cohort, "alice", page, ctx)
    with concurrent.futures.ThreadPoolExecutor(max_workers=4) as pool:
        list(pool.map(lambda _: run(*args), range(4)))
    run(*args)
    run("review", db, artifact, 4, "other", "alice", page, ctx, code=3)
    assert len(run("ledger", db, cohort)) == 1
    saved = run("ledger", db, cohort)[0]
    assert run("replay", db, saved["extraction_id"]) == first
    run("replay", db, "f" * 64, code=2)

    # Duplicate methods/ranges and independent captures corroborate, not overweight.
    for index in (0, 5):
        run("review", db, artifact, index, cohort, "alice", page, ctx)
    run("review", db, artifact, 2, cohort, "alice", page, ctx, code=2)
    currency_context = folder / "currency-context.json"
    currency_context.write_text(json.dumps(dict(context, currency="EUR")))
    run("review", db, artifact, 2, cohort, "alice", page, currency_context)
    changed = run("extract", changed_page, url, 1005)
    artifact.write_text(json.dumps(changed))
    run("review", db, artifact, 4, cohort, "alice", changed_page, ctx)

    # Same content at a genuinely different capture time gets a different capture ID.
    second = run("extract", page, url, 1060)
    assert second["blake3"] == first["blake3"]
    assert second["capture_id"] != first["capture_id"]
    assert second["candidates"][4]["evidence_id"] != ids[4]
    artifact.write_text(json.dumps(second))
    run("review", db, artifact, 4, cohort, "alice", page, ctx)
    export_args = ("cohort", db, cohort, "EUR", 1000, 60, 2)
    exported = run(*export_args)
    assert exported == run(*export_args)
    assert exported["values"] == [18000, 18000]
    assert [bucket["distinct_listings"] for bucket in exported["buckets"]] == [1, 1]
    assert len(exported["buckets"][0]["evidence"]) == 5
    run("cohort", db, cohort, "USD", 1000, 60, 2, code=3)
    run("cohort", db, cohort, "EUR", 1000, 60, 3, code=4)

    # Keep competing claims, expose the disputed field, and reject by revision.
    artifact.write_text(json.dumps(first))
    run("review", db, artifact, 1, cohort, "bob", page, ctx)
    dispute = run(*export_args, code=3)
    assert dispute["status"] == "review_conflict" and dispute["field"] == "minor"
    assert {claim["minor"] for claim in dispute["claims"]} == {18000, 19900}
    disputed = next(row for row in run("ledger", db, cohort) if row["minor"] == 19900)
    revision_args = ("revise", db, disputed["observation_id"], disputed["review_id"],
                     "rejected", "carol", "stale quote confirmed by review")
    run(*revision_args)
    run(*revision_args)  # retry is the same event, not a third revision
    run("revise", db, disputed["observation_id"], disputed["review_id"], "rejected",
        "dave", "different stale update", code=3)
    history = run("history", db, disputed["observation_id"])
    assert len(history) == 2 and history[0] == disputed
    assert history[1]["parent_review_id"] == disputed["review_id"]
    assert [row["decision"] for row in history] == ["accepted", "rejected"]
    assert run(*export_args) == exported

    # Cohort context cannot conflate different stays, guests, taxes or fee scope.
    other_context = folder / "other-context.json"
    other_context.write_text(json.dumps(dict(context, listing_id="example:other", guests=4)))
    run("review", db, artifact, 6, cohort, "alice", page, other_context)
    assert run(*export_args, code=3)["field"] == "guests"
    incompatible = next(row for row in run("ledger", db, cohort) if row.get("guests") == 4)
    run("revise", db, incompatible["observation_id"], incompatible["review_id"], "rejected",
        "alice", "different guest cohort")
    assert run(*export_args) == exported

    # Forecast and formula-error decisions both retain reviewed cohort lineage.
    series = folder / "cohort.json"
    series.write_text(json.dumps(exported))
    estimate = run("estimate", series, 1)
    assert estimate["kind"] == "forecast" and estimate["cohort_lineage"] == exported
    decision = run("recommend", series, 1, "formulas/weekend.lua", 1, 1000, 10000, 30000, 100)
    assert decision["adjusted_minor"] == 19800 and decision["cohort_lineage"] == exported
    assert decision["estimator"]["name"] == "damped-holt/v1"
    bad_formula = folder / "bad.lua"
    bad_formula.write_text("return function() return host.log('not permitted') end")
    failed = run("recommend", series, 1, bad_formula, 1, 1000, 10000, 30000, 100, code=2)
    assert failed["forecast_minor"] == 18000 and failed["cohort_lineage"] == exported
    tampered = dict(exported, values=[1, 2])
    series.write_text(json.dumps(tampered))
    run("estimate", series, 1, code=2)

    # Legacy review/series remain usable, but unverified rows cannot enter a rich cohort.
    legacy_db = folder / "legacy.db"
    run("review", legacy_db, artifact, 4, "legacy", "alice")
    run("cohort", legacy_db, "legacy", "EUR", 1000, 60, 2, code=3)
    assert run("ledger", legacy_db, "legacy")[0]["provenance"] == "legacy-unverified"

    # Chronological backtesting never sees values after each training cutoff.
    data = folder / "history.json"
    data.write_text("[10000,11000,12000,13000,14000,15000]")
    report = run("backtest", data, 2, 2)
    assert report["attempted"] == report["evaluated"] == 3 and report["coverage"] == 1
    assert [fold["target_index"] for fold in report["folds"]] == [3, 4, 5]
    data.write_text("[10000,11000,99000,88000,77000,66000]")
    changed_report = run("backtest", data, 2, 2)
    assert report["folds"][0]["forecast_minor"] == changed_report["folds"][0]["forecast_minor"]
    data.write_text("[10000,10000,10000,10000]")
    assert run("backtest", data, 1, 2)["mae_minor"] == 0
    run("backtest", data, 3, 2, code=2)
    run("backtest", data, 0, 2, code=2)
    data.write_text("[100,1,1,1]")
    no_coverage = run("backtest", data, 1, 2, code=2)
    assert no_coverage["coverage"] == 0 and no_coverage["mae_minor"] is None
    assert all(fold["status"] == "forecast_error" for fold in no_coverage["folds"])

    # Currency scale is explicit end-to-end, never assumed to be two decimals.
    scale_page = FIXTURE / "scales.html"
    scale_db = folder / "scales.db"
    for timestamp in (1000, 1060):
        scale_capture = run("extract", scale_page, url, timestamp)
        artifact.write_text(json.dumps(scale_capture))
        assert [(row["scale"], row["minor"]) for row in scale_capture["candidates"]] == [
            (0, 18000), (3, 18125), (2, None), (2, None)]
        for index, currency in enumerate(("JPY", "KWD")):
            run("review", scale_db, artifact, index, currency, "alice", scale_page, ctx)
        run("review", scale_db, artifact, 2, "EUR", "alice", scale_page, ctx, code=2)
        run("review", scale_db, artifact, 3, "EUR", "alice", scale_page, ctx, code=2)
    for currency, scale, minor in (("JPY", 0, 18000), ("KWD", 3, 18125)):
        scale_cohort = run("cohort", scale_db, currency, currency, 1000, 60, 2)
        assert scale_cohort["scale"] == scale and scale_cohort["values"] == [minor, minor]
        series.write_text(json.dumps(scale_cohort))
        assert run("estimate", series, 1)["cohort_lineage"]["scale"] == scale

print("Verified evidence, review revisions, cohort lineage and backtest tests passed.")
