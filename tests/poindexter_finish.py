"""Poindexter review completion, event preflight and editor failure boundaries."""
import json
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
FIX = ROOT / "tests/fixtures"


def run(*args, code=0, raw=False):
    env = dict(os.environ, SHY_SANDBOX="off")
    for name in ("SHY_EXTENSIONS", "SHY_NATIVE_PLUGINS", "SHY_FORMULA_SANDBOX"):
        env.pop(name, None)
    p = subprocess.run([str(ROOT / "shylock"), *map(str, args)], cwd=ROOT, env=env,
                       capture_output=True, text=True, timeout=40)
    assert p.returncode == code, (args, p.returncode, p.stdout[:2000], p.stderr)
    return p if raw else json.loads(p.stdout) if p.stdout else None


with tempfile.TemporaryDirectory(prefix="shylock-poindexter-finish-", dir=os.environ.get("TMPDIR")) as directory:
    folder = Path(directory)

    def save(name, value):
        path = folder / name
        path.write_text(json.dumps(value, ensure_ascii=False))
        return path

    def events(items):
        return save("events.json", dict(schema="shylock.ui-events/v1", events=items))

    def edit(key, value):
        return [dict(action="select", row=0, column=key), dict(action="edit", text=value)]

    # A rejected unknown quote can be completed in either frontend. No amount
    # edit is available: explicit currency resolves the original decimal token.
    html = folder / "unknown.html"
    html.write_text('<body><meta itemprop="price" content="180.00"></body>')
    extraction = run("extract", html, "https://example.org/cottage", 1000)
    assert extraction["candidates"][0]["minor"] is None
    capture = save("unknown.json", extraction)
    context = json.loads((FIX / "context.json").read_text())
    reports = []
    for backend in ("tui", "gui"):
        db = folder / f"unknown-{backend}.db"
        run("reject", db, capture, 0, "market", "alice", "Review incomplete evidence", html)
        changes = []
        for key, value in {**context, "currency": "EUR", "basis": "nightly", "qualifier": "unspecified"}.items():
            changes += edit(key, str(value))
        changes += edit("decision", "accepted")
        changes += [dict(action="reason", text="Currency and stay checked against the offer"), dict(action="save"), dict(action="quit")]
        result = run(backend, db, "market", "bob", events(changes))
        row = run("ledger", db, "market")[0]
        assert (row["minor"], row["currency"], row["scale"], row["basis"], row["decision"]) == (18000, "EUR", 2, "nightly", "accepted")
        assert result["pending"] == {} and result["refresh_required"] is False
        assert result["rows"][0]["source_url"] == row["source_url"]
        assert result["rows"][0]["extraction_id"] == row["extraction_id"]
        history = run("history", db, row["observation_id"])
        assert len(history) == 2 and history[0]["evidence"] == history[1]["evidence"]
        # Known evidence must not be contradicted after resolving the unknowns.
        changes = edit("currency", "USD") + [dict(action="reason", text="not compatible"), dict(action="save")]
        # Source currency is unknown, so a later explicit reviewer resolution is
        # allowed. It remains a new revision rather than rewriting old evidence.
        changed = run(backend, db, "market", "bob", events(changes))
        assert changed["rows"][0]["currency"] == "USD"
        reports.append(history[-1])
    assert reports[0] == reports[1]

    # A NaiveFinder conflict can be resolved explicitly through the worksheet,
    # rather than requiring a separate handwritten context file or bypass.
    html.write_text('<body><div class="listing-card"><h2 class="listing-title">Cottage</h2>'
                    '<p class="price">EUR 180.00</p><p class="price">EUR 190.00</p><p class="basis">per night</p></div></body>')
    snapshot = save("snapshot.json", run("snapshot", "https://example.org/cottage", 1000, html))
    found = save("found.json", run("find-rule", snapshot, ROOT / "plugins/dragnet/rules/rental.json", "rental_price"))
    for backend in ("tui", "gui"):
        db = folder / f"conflict-{backend}.db"
        run("reject", db, found, 0, "market", "alice", "Resolve competing quotes")
        changes = []
        for key, value in context.items():
            if key != "listing_id":
                changes += edit(key, str(value))
        changes += edit("decision", "accepted")
        changes += [dict(action="reason", text="Manually reviewed the quote"), dict(action="save")]
        result = run(backend, db, "market", "bob", events(changes), code=2)
        assert result["pending_decision"] == "accepted" and run("ledger", db, "market")[0]["decision"] == "rejected"
        changes = changes[:-1] + edit("conflict_resolution", "Selected the published EUR 180 nightly offer; alternative retained") + [dict(action="save")]
        accepted = run(backend, db, "market", "bob", events(changes))
        assert accepted["rows"][0]["decision"] == "accepted"
        assert accepted["rows"][0]["conflict_resolution"].startswith("Selected the published")
        protected = edit("currency", "USD") + edit("basis", "unknown") + edit("qualifier", "from")
        result = run(backend, db, "market", "bob", events(protected), code=2)
        assert result["pending"] == {} and all(a["status"] == 2 for a in result["actions"] if a["action"] == "edit")

    # Preflight every structural event before the first save, including unknown
    # keys, internal action names, wrong JSON types and terminal control text.
    baseline_db = folder / "baseline.db"
    record = save("capture.json", run("extract", FIX / "rental.html", "https://example.org/cottage", 1000))
    run("review", baseline_db, record, 1, "market", "alice", FIX / "rental.html", FIX / "context.json")
    baseline = baseline_db.read_bytes()
    prefix = edit("guests", "3") + [dict(action="reason", text="Should not commit on malformed script"), dict(action="save")]
    bad_events = [dict(action="save", unexpected=True), dict(action="fatal"), dict(action="end"),
                  dict(action="select", row=True, column="guests"), dict(action="select", row=0, column="nonexistent"),
                  dict(action="resize", width="960", height=640), dict(action="key", key="nonexistent"),
                  dict(action="edit", text="bad\x1b[31m"), dict(action="edit", text="bad\u0000text")]
    for backend in ("tui", "gui"):
        for bad in bad_events:
            assert run(backend, baseline_db, "market", "bob", events(prefix + [bad]), code=2) is None
            assert baseline_db.read_bytes() == baseline
        malformed = save("bad-root.json", dict(schema="shylock.ui-events/v1", events=prefix, extra=True))
        assert run(backend, baseline_db, "market", "bob", malformed, code=2) is None
        assert baseline_db.read_bytes() == baseline

        overflow = [dict(action="key", key="n"), dict(action="key", key="text", text="a" * 600),
                    dict(action="key", key="text", text="b" * 600), dict(action="key", key="enter"), dict(action="save")]
        result = run(backend, baseline_db, "market", "bob", events(overflow), code=2)
        assert result["editor"] == dict(active=True, text="a" * 600, prompt="Review reason", input_rejected=True)
        assert result["pending_reason"] == "" and baseline_db.read_bytes() == baseline
        result = run(backend, baseline_db, "market", "bob", events(edit("guests", "3") +
                     [dict(action="key", key="n"), dict(action="key", key="text", text="unsaved"),
                      dict(action="quit"), dict(action="quit")]))
        assert result["pending"] == {} and result["pending_reason"] == "" and result["editor"]["active"] is False
        assert baseline_db.read_bytes() == baseline

    # Even compatibility evidence cannot launder a known qualified offer into
    # an unqualified nightly observation by changing review context.
    legacy = json.loads(record.read_text())
    legacy["candidates"][1]["qualifier"] = "original"
    supplied = save("launder.json", dict(context, qualifier="unspecified"))
    run("review", folder / "legacy.db", save("legacy.json", legacy), 1, "market", "bob", "", supplied, code=2)

print("Poindexter typed review completion, immutable meaning, script preflight and editor limit tests passed.")
