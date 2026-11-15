"""Scoped, synchronous Mune extension lifecycle and failure isolation."""
import concurrent.futures
import json
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def run(*args, code=0, config=None):
    env = dict(os.environ)
    env.pop("SHY_EXTENSIONS", None)
    env.pop("SHY_FORMULA_SANDBOX", None)
    env["SHY_SANDBOX"] = env.get("SHY_TEST_SANDBOX", "off")
    if config is not None:
        env["SHY_EXTENSIONS"] = str(config)
    proc = subprocess.run([str(ROOT / "shylock"), *map(str, args)], cwd=ROOT,
                          env=env, text=True, capture_output=True, timeout=20)
    assert proc.returncode == code, (args, proc.returncode, proc.stdout, proc.stderr)
    audit = []
    for line in proc.stderr.splitlines():
        if line.startswith("{"):
            audit.append(json.loads(line))
    return (json.loads(proc.stdout) if proc.stdout else None), audit


with tempfile.TemporaryDirectory(prefix="shylock-extensions-") as directory:
    folder = Path(directory)
    payload = folder / "event.json"
    original = {"kind": "decision", "adjusted_minor": 18000, "currency": "EUR"}
    payload.write_text(json.dumps(original))

    def configure(source, *, caps=("log", "kv.read", "kv.write"), grants=None, name="counter", events=None):
        script = folder / f"{name}.lua"
        script.write_text(source)
        manifest = folder / f"{name}.manifest.json"
        manifest.write_text(json.dumps({"schema": "shylock.extension/v1", "name": name,
            "script": script.name, "capabilities": list(caps),
            "events": events or ["forecast.created", "decision.created", "worksheet.opened", "worksheet.saved"]}))
        config = folder / f"{name}.config.json"
        config.write_text(json.dumps({"schema": "shylock.extensions/v1", "extensions": [{
            "manifest": manifest.name, "grant": list(caps if grants is None else grants), "storage": "effects.db"}]}))
        return config

    source = '''
return {
  api="shylock.extension/v1",
  on_load=function() host.log("loaded") end,
  on_event=function(event,payload)
    local id=host.get("$event_id")
    local value=tonumber(host.kv_get("count") or "0")
    if not host.kv_get("seen/"..id) then
      value=value+1
      host.kv_set("count",tostring(value))
      host.kv_set("seen/"..id,"yes")
    end
    session=(session or 0)+1
    host.log("count="..value)
    host.log("session="..session)
  end,
  on_unload=function() host.log("unloaded") end
}
'''
    config = configure(source)
    result, _ = run("extension", config, "decision.created", payload)
    assert result["payload"] == original
    assert [row["phase"] for row in result["audit"]] == ["on_load", "decision.created", "on_unload"]
    event = result["audit"][1]
    assert event["logs"] == ["count=1", "session=1"]
    assert len(event["event_id"]) == len(event["source_blake3"]) == 64
    assert event["effects"] == "committed"
    payload.write_text(json.dumps(original, sort_keys=True, indent=2))
    repeated, _ = run("extension", config, "decision.created", payload)
    assert repeated["audit"][1]["event_id"] == event["event_id"]
    assert repeated["audit"][1]["logs"] == ["count=1", "session=1"]

    command = ("recommend", "tests/fixtures/series.json", 1, "formulas/weekend.lua", 1, 1000, 10000, 30000, 100)
    plain, _ = run(*command)
    with_extensions, audit = run(*command, config=config)
    assert with_extensions == plain
    assert [row["phase"] for row in audit] == ["on_load", "forecast.created", "decision.created", "on_unload"]
    assert audit[1]["logs"][1] == "session=1" and audit[2]["logs"][1] == "session=2"

    # Extension globals/capabilities do not enter a formula state.
    bad_formula = folder / "bad-formula.lua"
    bad_formula.write_text("return function() return host.kv_set('x','y') end")
    failed, audit = run("adjust", bad_formula, 18000, 0, 0, 10000, 30000, 100, code=2, config=config)
    assert failed["status"] == "formula_error" and failed["reference_minor"] == 18000
    assert any(row["phase"] == "decision.created" for row in audit)
    isolated = configure('''secret=99
return {api="shylock.extension/v1", on_event=function() host.log("ok") end}''', caps=("log",), name="isolated")
    bad_formula.write_text("return function() return secret or 18000 end")
    output, _ = run("adjust", bad_formula, 18000, 0, 0, 10000, 30000, 100, config=isolated)
    assert output["adjusted_minor"] == 18000

    # A failed event rolls back its scoped KV writes AND staged logs.
    rollback = configure('''return {api="shylock.extension/v1",on_event=function()
host.kv_set("rollback","not committed");host.log("must not publish");error("boom") end}''', name="rollback")
    result, _ = run("extension", rollback, "decision.created", payload, code=2)
    assert result["payload"] == original
    assert result["audit"][-1]["effects"] == "discarded" and "logs" not in result["audit"][-1]
    read_back = configure('''return {api="shylock.extension/v1",on_event=function()
assert(host.kv_get("rollback")==nil);host.log("rolled back") end}''', name="rollback")
    result, _ = run("extension", read_back, "decision.created", payload)
    assert result["audit"][0]["logs"] == ["rolled back"]

    denied = configure('''return {api="shylock.extension/v1",on_event=function()
pcall(function() host.kv_set("x","forbidden") end) end}''', caps=("log",), name="denied")
    result, _ = run("extension", denied, "decision.created", payload, code=2)
    assert "capability denied" in result["audit"][0]["error"]
    missing_grant = configure(source, caps=("log", "kv.read", "kv.write"), grants=("log",), name="ungranted")
    result, _ = run("extension", missing_grant, "decision.created", payload, code=2)
    assert result["audit"][0]["phase"] == "load"

    for source in (
        'return {api="wrong/v9",on_event=function() end}',
        'return {api="shylock.extension/v1",on_event=function() return "wrong" end}',
        'return {api="shylock.extension/v1",on_event=function() while true do end end}',
        'return {api="shylock.extension/v1",on_event=function() require("evil") end}',
        'pcall(function() host.log("outside lifecycle") end);return {api="shylock.extension/v1",on_event=function() end}',
    ):
        broken = configure(source, caps=("log",), name="broken")
        result, _ = run("extension", broken, "decision.created", payload, code=2)
        assert result["status"] == "extension_error" and result["payload"] == original
        # Opt-in hook errors do not replace or fail a valid core recommendation.
        output, diagnostic = run(*command, config=broken)
        assert output == plain and any(row["status"] == "error" for row in diagnostic)

    safe = configure('''return {api="shylock.extension/v1",on_event=function()
assert(os==nil and io==nil and package==nil and debug==nil and require==nil and load==nil and _G==nil)
assert(math.random==nil and host.review==nil and host.formula==nil)
host.log("restricted") end}''', caps=("log",), name="safe")
    result, _ = run("extension", safe, "decision.created", payload)
    assert result["audit"][0]["logs"] == ["restricted"]

    # Shared storage still exposes only each extension's own key namespace.
    entries = []
    for name in ("alpha", "beta"):
        cfg = configure(f'''return {{api="shylock.extension/v1",on_event=function()
assert(host.kv_get("owner")==nil or host.kv_get("owner")=="{name}")
host.kv_set("owner","{name}");host.log(host.kv_get("owner")) end}}''', name=name)
        entries += json.loads(cfg.read_text())["extensions"]
    combined = folder / "combined.json"
    combined.write_text(json.dumps({"schema": "shylock.extensions/v1", "extensions": entries}))
    result, _ = run("extension", combined, "decision.created", payload)
    assert [row["logs"] for row in result["audit"]] == [["alpha"], ["beta"]]

    # Read/modify/commit is covered by the extension writer lock.
    concurrent_config = configure('''return {api="shylock.extension/v1",on_event=function()
local count=tonumber(host.kv_get("count") or "0")+1
host.kv_set("count",tostring(count));host.log(tostring(count)) end}''', name="concurrent")
    with concurrent.futures.ThreadPoolExecutor(max_workers=4) as pool:
        outputs = list(pool.map(lambda _: run("extension", concurrent_config, "decision.created", payload)[0], range(4)))
    assert sorted(int(row["audit"][0]["logs"][0]) for row in outputs) == [1, 2, 3, 4]

print("Extension lifecycle, capabilities, transactions, concurrency and formula isolation tests passed.")
