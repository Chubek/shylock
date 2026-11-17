"""Saved discovery -> replayed collection -> explicit review -> ledger pricing."""
import copy
import json
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
FIX = ROOT / "tests/fixtures"
RULE = ROOT / "plugins/dragnet/rules/rental.json"


def run(*args, code=0, raw=False):
    env = dict(os.environ, SHY_SANDBOX="off")
    for key in ("SHY_EXTENSIONS", "SHY_NATIVE_PLUGINS", "SHY_PROVIDER_ROOT", "OPENAI_API_KEY", "OPENAI_BASE_URL"):
        env.pop(key, None)
    p = subprocess.run([str(ROOT / "shylock"), *map(str, args)], cwd=ROOT, env=env,
                       capture_output=True, text=True, timeout=60)
    assert p.returncode == code, (args, p.returncode, p.stdout[:2000], p.stderr)
    return p if raw else json.loads(p.stdout) if p.stdout else None


with tempfile.TemporaryDirectory(prefix="shylock-collection-", dir=os.environ.get("TMPDIR")) as directory:
    folder = Path(directory)

    def save(name, value):
        path = folder / name
        path.write_text(json.dumps(value, ensure_ascii=False))
        return path

    discovery = run("discover", "google", "cottages", FIX / "collection-search.html")
    save("discovery.json", discovery)
    (folder / "rule.json").write_bytes(RULE.read_bytes())
    sources = []
    start, step, count = 1793400000, 60, 7
    for i in range(count):
        snapshot = run("snapshot", "https://example.org/cottages", start + i * step, FIX / "collection-quotes.html")
        save(f"snapshot-{i}.json", snapshot)
        sources.append(dict(discovery="discovery.json", hit=0, snapshot=f"snapshot-{i}.json", rule="rule.json"))
    job = dict(schema="shylock.collection-job/v1", target="rental_price", sources=sources + [sources[0]])
    job_path = save("job.json", job)
    collected = run("collect", job_path)
    bundle = save("bundle.json", collected)
    assert collected == run("collect", job_path) == run("collect-replay", bundle)
    assert collected["schema"] == "shylock.collection/v1" and collected["status"] == "needs_review"
    assert collected["summary"] == dict(entries=8, duplicate_entries=1, unique_capture_rule_pairs=7,
                                      candidate_representations=16, conflicting_candidates=0,
                                      unresolved_candidates=0, partial_entries=0)
    assert collected["entries"][-1]["duplicate_of"] == 0
    for entry in collected["entries"]:
        assert entry["discovery"] == discovery
        assert entry["status"] == "needs_review" and len(entry["extraction"]["candidates"]) == 2
        assert entry["extraction"] == run("find-rule-replay", save("single.json", entry["extraction"]))

    # All paths are job-relative. Replay is self-contained even after originals
    # move away, and never opens a file or network endpoint named by page data.
    (folder / "discovery.json").rename(folder / "moved.json")
    assert run("collect-replay", bundle) == collected
    (folder / "moved.json").rename(folder / "discovery.json")
    for change in ({"schema": "unknown"}, {"target": "prices"}, {"allow_network": True},
                   {"sources": []}, {"sources": sources * 2}):
        assert run("collect", save("bad-job.json", dict(job, **change)), code=2) is None
    for change in ({"hit": 1}, {"hit": 64}, {"hit": True}, {"snapshot": "missing.json"},
                   {"rule": ""}, {"model": "live"}):
        bad = copy.deepcopy(job)
        bad["sources"][0].update(change)
        assert run("collect", save("bad-job.json", bad), code=2) is None
    for mutate in (
        lambda b: b.update(collection_id="0" * 64),
        lambda b: b["summary"].update(candidate_representations=1),
        lambda b: b["entries"][0].update(hit=1),
        lambda b: b["entries"][-1].update(duplicate_of=None),
        lambda b: b["entries"][0]["discovery"]["hits"][0].update(title="forged"),
        lambda b: b["entries"][0]["extraction"]["candidates"][0].update(minor=1),
        lambda b: b["entries"][0]["extraction"]["snapshot"].update(html="forged"),
        lambda b: b.update(extra=True),
    ):
        bad = copy.deepcopy(collected); mutate(bad)
        assert run("collect-replay", save("tampered.json", bad), code=2) is None
        db = folder / "untouched.db"
        run("review-collection", db, folder / "tampered.json", 0, 0, "market", "tester", code=2)
        assert not db.exists()

    # Extraction is never implicit admission. Review each listing with explicit
    # context; repeated sources/retries retain one evidence/review identity.
    database = folder / "market.db"
    context = json.loads((FIX / "context.json").read_text())
    context.pop("listing_id", None)
    context_path = save("context.json", context)
    run("review-collection", database, bundle, 0, 0, "market", "tester", code=2)
    for i in range(count):
        for candidate in range(2):
            run("review-collection", database, bundle, i, candidate, "market", "tester", context_path)
    rows = run("ledger", database, "market")
    assert len(rows) == 14
    run("review-collection", database, bundle, 7, 0, "market", "tester", context_path)
    assert run("ledger", database, "market") == rows
    reordered = folder / "reordered.json"
    reordered.write_text(json.dumps(collected, sort_keys=True))
    assert run("collect-replay", reordered) == collected
    run("review-collection", database, reordered, 0, 0, "market", "tester", context_path)
    assert run("ledger", database, "market") == rows
    for row in rows:
        assert row["collection"]["collection_id"] == collected["collection_id"]
        assert row["collection"]["find_id"] == collected["entries"][row["collection"]["entry"]]["extraction"]["find_id"]
    assert run("replay-collection", database, collected["collection_id"]) == collected
    assert run("replay-collection", database, "0" * 64, code=2) is None

    cohort = run("cohort", database, "market", "EUR", start, step, count)
    assert cohort["values"] == [20000] * count
    assert all(b["distinct_listings"] == 2 for b in cohort["buckets"])
    assert all(r["collection"]["collection_id"] == collected["collection_id"] for b in cohort["buckets"] for r in b["evidence"])
    as_of = start + step * count - 1
    selection = dict(schema="shylock.ledger-selection/v1", cohort="market", currency="EUR", start=start,
                     step=step, count=count, as_of=as_of, max_age_seconds=59, minimum_listings=2)
    selection_path = save("selection.json", selection)
    policy = json.loads((ROOT / "policies/pricing.json").read_text())
    policy["as_of"] = as_of
    policy["formula"]["path"] = str(ROOT / "formulas/weekend.lua")
    policy_path = save("pricing.json", policy)
    priced = run("price-ledger", database, selection_path, policy_path)
    assert priced["status"] == "priced" and priced["recommended_minor"] == 19800
    assert priced["forecast"]["cohort_lineage"] == cohort
    admission = priced["forecast"]["ledger_selection"]
    assert admission["selection"] == selection and admission["minimum_observed_listings"] == 2
    assert admission["latest_capture"] == start + 6 * step
    assert priced == run("price-ledger", database, selection_path, policy_path)
    forecast = run("forecast-ledger", database, selection_path, save("forecast-policy.json", policy["forecast"]))
    assert forecast == priced["forecast"]
    exported = run("price", save("cohort.json", cohort), policy_path)
    assert exported["recommended_minor"] == priced["recommended_minor"] and exported["quote"] == priced["quote"]
    assert exported["forecast"]["cohort_lineage"] == priced["forecast"]["cohort_lineage"]
    for change in ({"minimum_listings": 3}, {"max_age_seconds": 58}, {"as_of": as_of - 1},
                   {"currency": "USD"}, {"count": 1}, {"count": 0}, {"step": 0}, {"extra": 1}):
        p = run("price-ledger", database, save("bad-selection.json", dict(selection, **change)), policy_path, code=2 if change.get("currency") != "USD" else 3, raw=True)
        assert not p.stdout or json.loads(p.stdout)["status"] == "incompatible_cohort"
    future_policy = save("future-policy.json", dict(policy, as_of=as_of + 1))
    assert run("price-ledger", database, selection_path, future_policy, code=2) is None
    assert run("ledger", database, "market") == rows

    # Gaps and current review heads are authoritative, not cached extraction
    # scores or an earlier successful decision. Rejection removes coverage.
    latest = next(r for r in rows if r["captured_at"] == start + 6 * step and r["listing_id"] == "Lake Cottage")
    run("revise", database, latest["observation_id"], latest["review_id"], "rejected", "tester", "exclude")
    assert run("history", database, latest["observation_id"])[-1]["collection"] == latest["collection"]
    assert run("price-ledger", database, selection_path, policy_path, code=2) is None
    gaps = dict(selection, count=count + 1, as_of=as_of + step)
    assert run("price-ledger", database, save("gap.json", gaps), save("gap-policy.json", dict(policy, as_of=as_of + step)), code=4) is None
    conflict_html = folder / "conflict.html"
    conflict_html.write_text('<p>EUR 190.00 per night</p>')
    other_capture = run("extract", conflict_html, "https://example.org/cottages", start + 1)
    other_context = save("other-context.json", dict(context, listing_id="Lake Cottage"))
    run("review", database, save("other-capture.json", other_capture), 0, "market", "tester", conflict_html, other_context)
    conflict_result = run("price-ledger", database, selection_path, policy_path, code=3)
    assert conflict_result["status"] == "review_conflict" and conflict_result["field"] == "minor"

    # Conflict and missing-currency evidence survive collection without becoming
    # a reviewed observation. Use the same admission gate as standalone NaiveFinder.
    html = folder / "disagree.html"
    html.write_text('<body><div class="listing-card"><h2 class="listing-title">Lake Cottage</h2>'
                    '<p class="price">EUR 180.00</p><p class="price">EUR 190.00</p><p class="basis">per night</p>'
                    '</div><div class="listing-card"><h2 class="listing-title">Unknown Cottage</h2>'
                    '<p class="price">$200</p><p class="basis">per night</p></div></body>')
    evidence = run("snapshot", "https://example.org/cottages", start, html)
    save("disagree.json", evidence)
    dispute_job = dict(job, sources=[dict(sources[0], snapshot="disagree.json")])
    dispute = run("collect", save("dispute-job.json", dispute_job))
    assert dispute["summary"]["conflicting_candidates"] == 2 and dispute["summary"]["unresolved_candidates"] == 1
    dispute_path = save("dispute.json", dispute)
    assert run("collect-replay", dispute_path) == dispute
    db = folder / "dispute.db"
    run("review-collection", db, dispute_path, 0, 0, "market", "tester", context_path, code=2)
    assert run("ledger", db, "market") == []
    resolution = save("resolution.json", dict(context, conflict_resolution="Selected published nightly quote; alternative retained"))
    run("review-collection", db, dispute_path, 0, 0, "market", "tester", resolution)
    run("review-collection", db, dispute_path, 0, 2, "market", "tester", context_path, code=2)
    assert len(run("ledger", db, "market")) == 1

    # A completely valid empty extraction is review evidence, not an acquisition
    # failure or an authorization to invoke a model or invent a nightly price.
    empty_rule = json.loads(RULE.read_text()); empty_rule["container"] = ".absent"
    save("empty-rule.json", empty_rule)
    empty = run("collect", save("empty-job.json", dict(job, sources=[dict(sources[0], rule="empty-rule.json")])))
    assert empty["summary"]["candidate_representations"] == 0 and empty["entries"][0]["status"] == "empty"

    html.write_text('<body><div class="listing-card"><h2 class="listing-title">Lake Cottage</h2>'
                    '<p class="price">EUR 180.00</p><p class="basis">per night</p><p>' + 'x' * 5000 + '</p></div></body>')
    save("partial-snapshot.json", run("snapshot", "https://example.org/cottages", start, html))
    partial_job = dict(job, sources=[dict(sources[0], snapshot="partial-snapshot.json"), sources[1]])
    partial = run("collect", save("partial-job.json", partial_job), code=2)
    assert partial["status"] == "partial_extraction" and partial["summary"]["partial_entries"] == 1
    assert partial["entries"][0]["extraction"]["legacy_extraction"] is None
    assert partial["entries"][1]["status"] == "needs_review"
    partial_path = save("partial-bundle.json", partial)
    assert run("collect-replay", partial_path, code=2) == partial
    db = folder / "partial.db"
    run("review-collection", db, partial_path, 0, 0, "market", "tester", context_path)
    assert len(run("ledger", db, "market")) == 1
    assert run("replay-collection", db, partial["collection_id"], code=2) == partial

print("Replayable discovery collections, explicit review lineage and gated ledger pricing tests passed.")
