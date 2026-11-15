"""OS policy probes, isolated runtime parity, broker distrust and worker failures."""
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import time

ROOT = Path(__file__).resolve().parents[1]
BINARY = Path(os.environ.get("SHY_BINARY", ROOT / "build/shylock")).resolve()
probes = subprocess.run([str(BINARY.parent / "sandbox-probes")], text=True, capture_output=True, timeout=30)
assert probes.returncode == 0, (probes.stdout, probes.stderr)
policy = json.loads(probes.stdout)


def run(*args, code=0, sandbox="process", formula=None, config=None, binary=BINARY):
    env = dict(os.environ, SHY_BINARY=str(binary), SHY_SANDBOX=sandbox)
    env.pop("SHY_EXTENSIONS", None)
    env.pop("SHY_FORMULA_SANDBOX", None)
    if formula is not None:
        env["SHY_FORMULA_SANDBOX"] = formula
    if config:
        env["SHY_EXTENSIONS"] = str(config)
    proc = subprocess.run([str(ROOT / "shylock"), *map(str, args)], cwd=ROOT,
                          env=env, text=True, capture_output=True, timeout=15)
    assert proc.returncode == code, (args, proc.returncode, proc.stdout, proc.stderr)
    return json.loads(proc.stdout) if proc.stdout else None


adjust = ("adjust", "formulas/weekend.lua", 18000, 1, 1500, 10000, 30000, 100)
if not policy["supported"]:
    rejected = run(*adjust, code=2)
    assert rejected["status"] == "formula_error" and rejected["formula_sandbox_profile"] == "none"
    assert "unavailable" in rejected["formula_error"]
    print("Unsupported sandbox fails closed; default in-process execution remains available.")
    sys.exit(0)

assert all(policy["probes"].values()), policy
off = run(*adjust, sandbox="off")
isolated = run(*adjust)
assert isolated["adjusted_minor"] == off["adjusted_minor"] == 20700
assert isolated["formula_isolation"] == "process" and isolated["formula_sandbox_profile"] == "linux-seccomp/v1"
assert run(*adjust, formula="off")["formula_isolation"] == "off"
assert run(*adjust, sandbox="off", formula="process")["formula_isolation"] == "process"
invalid = run(*adjust, sandbox="invalid", code=2)
assert invalid["formula_isolation"] == "invalid" and invalid["adjusted_minor"] is None

with tempfile.TemporaryDirectory(prefix="shylock-sandbox-") as directory:
    folder = Path(directory)
    source = folder / "formula.lua"
    for text in (
        "return function() return os.execute('anything') end",
        "return function() return host.kv_get('private') end",
        "return function() while true do end end",
        "return function() return 1.5 end",
        "return function() return require('x') end",
    ):
        source.write_text(text)
        failed = run("recommend", "tests/fixtures/series.json", 1, source, 1, 1000, 10000, 30000, 100, code=2)
        assert failed["status"] == "formula_error" and failed["forecast_minor"] > 0 and failed["adjusted_minor"] is None
        assert failed["formula_isolation"] == "process"
    source.write_text("seen=(seen or 0)+1;return function(reference) return reference+seen end")
    for _ in range(2):
        assert run("adjust", source, 18000, 0, 0, 10000, 30000, 1)["adjusted_minor"] == 18001

    payload = folder / "payload.json"
    payload.write_text('{"kind":"test","value":42}')
    script, manifest, config = folder / "extension.lua", folder / "manifest.json", folder / "config.json"
    script.write_text('''return {api="shylock.extension/v1",on_event=function()
host.kv_set("value","committed");host.log("ok") end}''')
    manifest.write_text(json.dumps({"schema": "shylock.extension/v1", "name": "isolated", "script": script.name,
        "events": ["decision.created"], "capabilities": ["log", "kv.read", "kv.write"]}))

    def configure(mode="process", caps=("log", "kv.read", "kv.write")):
        config.write_text(json.dumps({"schema": "shylock.extensions/v1", "extensions": [{
            "manifest": manifest.name, "grant": list(caps), "storage": "effects.db", "sandbox": mode}]}))

    configure()
    result = run("extension", config, "decision.created", payload, sandbox="off")
    assert result["audit"][0]["isolation"] == "process" and result["audit"][0]["logs"] == ["ok"]
    configure("off")
    assert run("extension", config, "decision.created", payload)["audit"][0]["isolation"] == "off"
    configure("typo")
    invalid = run("extension", config, "decision.created", payload, code=2)
    assert invalid["audit"][0]["phase"] == "load" and invalid["audit"][0]["isolation"] == "invalid"
    configure()

    # Run a copied main image with a missing/failed/malformed companion. These
    # are trusted fault-injection helpers, not alternate production sandboxes.
    staged = folder / "application"
    staged.mkdir()
    copied = staged / "shylock"
    worker = staged / "shylock-worker"
    shutil.copy2(BINARY, copied)
    missing = run(*adjust, binary=copied, code=2)
    assert missing["reference_minor"] == 18000 and missing["formula_sandbox_profile"] == "none"
    assert "startup" in missing["formula_error"]
    # A request for isolation MUST NOT retry the formula/extension in process.
    missing_extension = run("extension", config, "decision.created", payload, binary=copied, code=2)
    assert missing_extension["audit"][0]["phase"] == "load"
    assert missing_extension["audit"][0]["sandbox_profile"] == "none"

    helper_header = f"#!{sys.executable}\n" + '''
import json, os, struct
def exact(n):
    data=b""
    while len(data)<n:
        part=os.read(3,n-len(data))
        if not part: raise SystemExit(0)
        data+=part
    return data
def receive():
    n=struct.unpack(">I",exact(4))[0]
    return json.loads(exact(n))
def send(value):
    data=json.dumps(value).encode()
    os.write(3,struct.pack(">I",len(data))+data)
'''

    def helper(code):
        worker.write_text(helper_header + code)
        worker.chmod(0o755)

    helper("while True: pass\n")
    started = time.monotonic()
    timed = run(*adjust, binary=copied, code=2)
    assert "startup" in timed["formula_error"] and time.monotonic()-started < 8
    helper('os.write(3,struct.pack(">I",0xffffffff))\n')
    assert run(*adjust, binary=copied, code=2)["formula_sandbox_profile"] == "none"
    helper('send({"op":"ready","profile":"wrong/v2"})\n')
    assert run(*adjust, binary=copied, code=2)["formula_sandbox_profile"] == "none"

    # Malformed/out-of-range worker return values are not prices.
    for rate in (1.5, 10**13, True):
        helper('send({"op":"ready","profile":"linux-seccomp/v1"})\nreceive()\n' +
               f'send({{"op":"result","ok":True,"minor":{rate!r}}})\n')
        result = run(*adjust, binary=copied, code=2)
        assert result["adjusted_minor"] is None and result["reference_minor"] == 18000

    # Fatal worker outcomes after staged effects must roll back in the parent.
    healthy_worker = BINARY.parent / "shylock-worker"
    common = '''
send({"op":"ready","profile":"linux-seccomp/v1"})
receive()
send({"op":"result","ok":True,"hooks":2})
receive()
send({"op":"kv_set","key":"rollback","value":"must not commit"})
receive()
send({"op":"log","key":"must not publish"})
receive()
'''
    for ending in ('os._exit(42)\n', 'while True: pass\n', 'os.write(3,struct.pack(">I",0xffffffff))\n',
                   'send({"op":"result","ok":"not a bool"})\n'):
        pidfile = folder / "worker.pid"
        helper(f'open({str(pidfile)!r},"w").write(str(os.getpid()))\n' + common + ending)
        result = run("extension", config, "decision.created", payload, binary=copied, code=2)
        phase = result["audit"][0]
        assert phase["effects"] == "discarded" and "logs" not in phase
        assert result["payload"] == {"kind": "test", "value": 42}
        assert not Path(f"/proc/{pidfile.read_text()}").exists(), "worker must be killed and reaped"
        shutil.copy2(healthy_worker, worker)
        original = script.read_text()
        script.write_text('''return {api="shylock.extension/v1",on_event=function()
assert(host.kv_get("rollback")==nil);host.log("rollback verified") end}''')
        checked = run("extension", config, "decision.created", payload, binary=copied)
        assert checked["audit"][0]["logs"] == ["rollback verified"]
        script.write_text(original)

    # A compromised worker cannot grant itself privileges or choose a KV
    # namespace. Even if it ignores a denial and claims success, commit fails.
    manifest_data = json.loads(manifest.read_text())
    manifest_data["capabilities"] = ["log"]
    manifest.write_text(json.dumps(manifest_data))
    configure(caps=("log",))
    helper('''send({"op":"ready","profile":"linux-seccomp/v1"})
receive();send({"op":"result","ok":True,"hooks":2});receive()
send({"op":"kv_set","key":"../head/x","value":"forged","capabilities":["kv.write"],"namespace":"host"})
receive();send({"op":"result","ok":True})
''')
    result = run("extension", config, "decision.created", payload, binary=copied, code=2)
    assert "capability denied" in result["audit"][0]["error"] and result["audit"][0]["effects"] == "discarded"

    # Exec removes the caller's environment, and inherited descriptors cannot
    # substitute for the private IPC channel when standard input is closed.
    env = dict(os.environ, SHY_BINARY=str(BINARY), SHY_FORMULA_SANDBOX="process", SHY_SANDBOX="off")
    p = subprocess.run([str(ROOT / "shylock"), *map(str, adjust)], cwd=ROOT, env=env,
                       preexec_fn=lambda: os.close(0), capture_output=True, text=True, timeout=15)
    assert p.returncode == 0 and json.loads(p.stdout)["adjusted_minor"] == 20700

    # The launcher does not carry a GUI/ledger fd, environment secret, or
    # instrumentation preload into the exec image (independent of seccomp).
    inspection = folder / "launch.json"
    helper(f'''data={{"secret":os.environ.get("SHY_TEST_SECRET"),"preload":os.environ.get("LD_PRELOAD"),"fds":[]}}
for fd in os.listdir("/proc/self/fd"):
    try: data["fds"].append(os.readlink("/proc/self/fd/"+fd))
    except OSError: pass
open({str(inspection)!r},"w").write(json.dumps(data))
send({{"op":"ready","profile":"linux-seccomp/v1"}});receive();send({{"op":"result","ok":True,"minor":18000}})
''')
    sensitive = folder / "parent-private.txt"
    sensitive.write_text("not worker input")
    with sensitive.open() as inherited:
        env = dict(os.environ, SHY_BINARY=str(copied), SHY_SANDBOX="process", SHY_TEST_SECRET="not worker input")
        env.pop("SHY_FORMULA_SANDBOX", None)
        p = subprocess.run([str(ROOT / "shylock"), *map(str, adjust)], cwd=ROOT, env=env,
                           pass_fds=(inherited.fileno(),), capture_output=True, text=True, timeout=15)
    assert p.returncode == 0, (p.stdout, p.stderr)
    inspected = json.loads(inspection.read_text())
    assert inspected["secret"] is None and inspected["preload"] is None
    assert str(sensitive) not in inspected["fds"]
    assert len(inspected["fds"]) == 4 and inspected["fds"].count("/dev/null") == 3

# Repeat full lifecycle, storage, worksheet and PTY behavior through workers.
env = dict(os.environ, SHY_TEST_SANDBOX="process", SHY_BINARY=str(BINARY))
for suite in ("extensions.py", "worksheet.py"):
    proc = subprocess.run([sys.executable, str(ROOT / "tests" / suite)], cwd=ROOT, env=env,
                          capture_output=True, text=True, timeout=90)
    assert proc.returncode == 0, (suite, proc.stdout, proc.stderr)
print("OS isolation, resource limits, worker failures, broker authority and sandboxed runtime/UI tests passed.")
