"""Mune formula examples and typed, single-state extension batch replay."""
import copy
import json
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
BINARY = Path(os.environ.get("SHY_BINARY", ROOT / "build/shylock")).resolve()
policy = subprocess.run([str(BINARY.parent / "sandbox-probes")], capture_output=True, text=True, check=True)
MODES = ["off", "process"] if json.loads(policy.stdout)["supported"] else ["off"]


def run(*args, code=0, mode="off"):
    env = dict(os.environ, SHY_SANDBOX=mode)
    env.pop("SHY_EXTENSIONS", None)
    env.pop("SHY_FORMULA_SANDBOX", None)
    proc = subprocess.run([str(ROOT / "shylock"), *map(str, args)], cwd=ROOT,
                          env=env, text=True, capture_output=True, timeout=30)
    assert proc.returncode == code, (args, proc.returncode, proc.stdout, proc.stderr)
    return json.loads(proc.stdout) if proc.stdout else None


with tempfile.TemporaryDirectory(prefix="shylock-calculation-") as directory:
    folder = Path(directory)
    cases = folder / "cases.json"
    original = json.loads((ROOT / "formulas/cases/weekend.json").read_text())
    for mode in MODES:
        for name in ("weekend", "weekday-discount", "capped-weekend"):
            result = run("formula-test", ROOT / f"formulas/{name}.lua", ROOT / f"formulas/cases/{name}.json", mode=mode)
            assert result["failed"] == 0 and result["passed"] == len(result["cases"])
            assert all(row["status"] == "passed" and row["isolation"] == mode for row in result["cases"])
            assert len(result["test_id"]) == len(result["cases_id"]) == 64
            assert len({row["formula_blake3"] for row in result["cases"]}) == 1

        # Fresh Mune states in one process/batch; globals cannot cross cases.
        script = folder / "fresh.lua"
        script.write_text("seen=(seen or 0)+1;return function(reference) return reference+seen end")
        case = dict(original["cases"][0], tick_minor=1, expected_minor=18001)
        fresh = {"schema": original["schema"], "cases": [dict(case, name="one"), dict(case, name="two")]}
        cases.write_text(json.dumps(fresh))
        assert run("formula-test", script, cases, mode=mode)["passed"] == 2

        errors = dict(fresh, cases=[dict(case, name="restricted", expected_minor=None)])
        cases.write_text(json.dumps(errors))
        for text in ("return function() return host.log('denied') end", "return function() return math.random() end",
                     "return function() return require('x') end", "return function() return 1.5 end",
                     "return function() return 1,2 end", "return function() while true do end end"):
            script.write_text(text)
            checked = run("formula-test", script, cases, mode=mode)
            assert checked["passed"] == 1 and checked["cases"][0]["adjusted_minor"] is None
            assert checked["cases"][0]["formula_error"]
        failed = run("formula-test", folder / "missing.lua", cases, code=2, mode=mode)
        assert failed["failed"] == 1  # Missing source is not an expected runtime error.

        # Copy the host config, not its store: examples must not modify the repo.
        config = folder / f"monitor-{mode}.json"
        config.write_text(json.dumps({"schema": "shylock.extensions/v1", "extensions": [{
            "manifest": str(ROOT / "extensions/monitor/manifest.json"),
            "grant": ["log", "kv.read", "kv.write"], "storage": f"{mode}.db"}]}))
        events = ROOT / "extensions/events/example.json"
        result = run("extension-replay", config, events, mode=mode)
        assert result["batch"] == json.loads(events.read_text()) and result["status"] == "ok"
        audit = result["audit"]
        assert [r["phase"] for r in audit] == ["on_load", "decision.created", "decision.created", "decision.created", "on_unload"]
        assert audit[1]["logs"] == ["processed=1 formula_errors=0 session=1"]
        assert audit[2]["logs"] == ["duplicate decision.created session=2"]
        assert audit[3]["logs"] == ["processed=2 formula_errors=1 session=3"]
        assert audit[1]["event_id"] == audit[2]["event_id"] != audit[3]["event_id"]
        repeated = run("extension-replay", config, events, mode=mode)
        assert all(row["logs"][0].startswith("duplicate") for row in repeated["audit"][1:-1])

    mismatch = copy.deepcopy(original)
    mismatch["cases"][0]["expected_minor"] = 1
    cases.write_text(json.dumps(mismatch))
    result = run("formula-test", ROOT / "formulas/weekend.lua", cases, code=2)
    assert result["failed"] == 1 and result["passed"] == 5
    for key, value in (("weekend", 1), ("tick_minor", 0), ("cap_minor", 100), ("reference_minor", -1),
                       ("premium_bp", 10001), ("expected_minor", "20700"), ("extra", 1)):
        bad = copy.deepcopy(original)
        bad["cases"][0][key] = value
        cases.write_text(json.dumps(bad))
        assert run("formula-test", ROOT / "formulas/weekend.lua", cases, code=2) is None
    for entries in ([], original["cases"] * 22, [original["cases"][0]] * 2):
        cases.write_text(json.dumps(dict(original, cases=entries)))
        assert run("formula-test", ROOT / "formulas/weekend.lua", cases, code=2) is None

    # Validate the whole replay before even on_load may write a store.
    script = folder / "side-effect.lua"
    script.write_text('return {api="shylock.extension/v1",on_load=function() host.kv_set("loaded","yes") end,on_event=function() end}')
    manifest = folder / "manifest.json"
    manifest.write_text(json.dumps({"schema": "shylock.extension/v1", "name": "validation", "script": script.name,
                                    "capabilities": ["kv.write"], "events": ["decision.created"]}))
    config = folder / "validate.json"
    config.write_text(json.dumps({"schema": "shylock.extensions/v1", "extensions": [{
        "manifest": manifest.name, "grant": ["kv.write"], "storage": "must-not-exist.db"}]}))
    batch = folder / "batch.json"
    valid = json.loads((ROOT / "extensions/events/example.json").read_text())
    for change in ({"event": "unknown"}, {"payload": None}, {"extra": 1}):
        bad = copy.deepcopy(valid)
        bad["events"][-1].update(change)
        batch.write_text(json.dumps(bad))
        assert run("extension-replay", config, batch, code=2) is None
        assert not (folder / "must-not-exist.db").exists()
    for entries in ([], valid["events"] * 11):
        batch.write_text(json.dumps(dict(valid, events=entries)))
        assert run("extension-replay", config, batch, code=2) is None

    # Failure disables only that instance; per-hook effects are rolled back.
    script.write_text('''return {api="shylock.extension/v1",on_event=function()
host.kv_set("must-rollback","x");error("batch failure") end}''')
    result = run("extension-replay", config, ROOT / "extensions/events/example.json", code=2)
    assert result["status"] == "extension_error" and len(result["audit"]) == 1
    assert result["audit"][0]["effects"] == "discarded"

print("Formula example/check, fresh-state, and extension replay/lifecycle tests passed.")
