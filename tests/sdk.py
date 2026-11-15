"""Public C11-only SDK, dynamic ABI admission, lifecycle and transactional sinks."""
import json
import os
from pathlib import Path
import shlex
import subprocess
import tempfile

ROOT=Path(__file__).resolve().parents[1]
CC=shlex.split(os.environ.get("CC","cc"))
FLAGS=shlex.split(os.environ.get("SDK_TEST_CFLAGS",""))

with tempfile.TemporaryDirectory(prefix="shylock-sdk-") as directory:
    folder=Path(directory)
    config=folder/"native.json"
    log=folder/"lifecycle.log"
    page=folder/"page.html"
    page.write_text("EUR 1.00 per night")
    env=dict(os.environ,SHY_NATIVE_PLUGINS=str(config),SHY_SDK_PROBE_LOG=str(log))
    env.pop("SHY_EXTENSIONS",None)
    def run(*args,code=0):
        p=subprocess.run([str(ROOT/"shylock"),*map(str,args)],cwd=ROOT,env=env,capture_output=True,text=True,timeout=15)
        assert p.returncode==code,(args,p.returncode,p.stdout,p.stderr)
        return json.loads(p.stdout) if p.stdout else None
    def configuration(items):
        config.write_text(json.dumps({"schema":"shylock.native-plugins/v1","plugins":items}))
    for mode in range(7):
        library=folder/f"probe-{mode}.so"
        subprocess.run([*CC,"-std=c11","-Wall","-Wextra","-Wpedantic","-Werror","-fPIC","-shared",*FLAGS,
                        f"-DPROBE_MODE={mode}",str(ROOT/"tests/sdk_probe.c"),"-o",str(library)],check=True)
        configuration([{"path":library.name,"configuration":{}}])
        log.write_text("")
        result=run("extract",page,"https://example.org/",1000,"","","sdk-probe",code=0 if mode==0 else 2)
        if mode==0:
            assert result["plugin"]=="sdk-probe" and result["candidates"][0]["minor"]==100
            assert result["plugin_configuration"]=="{}" and len(result["candidates"][0]["plugin_configuration_blake3"])==64
            assert result["diagnostics"][0]["code"]=="probe_ran"
            assert log.read_text().splitlines()==["load","extract","unload"]
        else:
            assert result is None
            assert log.read_text().splitlines()==([] if mode in (1,2) else ["load"] if mode==6 else ["load","extract","unload"])
    configuration([{"path":"probe-0.so","configuration":{}},{"path":"probe-1.so","configuration":{}}])
    log.write_text("")
    assert run("plugins",code=2) is None
    assert log.read_text().splitlines()==["load","unload"]  # Atomic rollback.
    configuration([{"path":"probe-0.so","configuration":{}}]*2)
    log.write_text("")
    run("plugins",code=2)
    assert log.read_text().splitlines()==["load","unload"]
    example=folder/"fixed-price.so"
    subprocess.run([*CC,"-std=c11","-Wall","-Wextra","-Wpedantic","-Werror","-fPIC","-shared",*FLAGS,
                    str(ROOT/"sdk/examples/fixed-price.c"),"-o",str(example)],check=True)
    configuration([{"path":example.name,"configuration":{}}])
    assert len(run("plugins"))==2
    assert run("extract",page,"https://example.org/",1000,"","","fixed-price")["candidates"][0]["minor"]==100
    configuration([{"path":"missing.so","configuration":{}}])
    run("plugins",code=2)
print("Standalone C11 SDK, ABI rejection, batch rollback, lifecycle and poisoned-sink tests passed.")
