"""Offline browser workflows, noise-filtered IR and model-free fallback contracts."""
import copy
import json
import os
from pathlib import Path
import subprocess
import tempfile

ROOT=Path(__file__).resolve().parents[1]
FIX=ROOT/"tests/fixtures"
def run(*args,code=0,extra=None):
    env=dict(os.environ)
    env.pop("SHY_NATIVE_PLUGINS",None)
    env.pop("SHY_EXTENSIONS",None)
    env.pop("OPENAI_API_KEY",None)  # The suite never calls a real paid service.
    env.pop("OPENAI_BASE_URL",None)
    if extra:
        env.update(extra)
    p=subprocess.run([str(ROOT/"shylock"),*map(str,args)],cwd=ROOT,env=env,capture_output=True,text=True,timeout=30)
    assert p.returncode==code,(args,p.returncode,p.stdout,p.stderr)
    return json.loads(p.stdout) if p.stdout else None
with tempfile.TemporaryDirectory(prefix="shylock-intelligence-") as directory:
    folder=Path(directory)
    def save(name,value):
        path=folder/name; path.write_text(json.dumps(value)); return path
    original=json.loads((ROOT/"plugins/dragnet/browser-job.json").read_text())
    job=copy.deepcopy(original)
    job["pages"][0]["path"]=str(FIX/"browser-start.html")
    job["pages"][1]["path"]=str(FIX/"rental.html")
    browser=run("browse",save("job.json",job))
    assert browser["status"]=="completed" and browser["profile"]=="mulib-static-html/v1"
    assert [row["action"] for row in browser["trace"]]==["assert","follow","read"]
    assert browser["trace"][-1]["text"]=="EUR 180.00 per night"
    assert browser["snapshot"]["source_url"]=="https://example.org/cottage"
    assert browser==run("browse",folder/"job.json")
    snapshot=save("snapshot.json",browser["snapshot"])
    ir=run("flatten",snapshot)
    assert ir==run("flatten",save("browser.json",browser))
    assert ir==run("flatten",save("ir.json",ir))
    assert ir["snapshot"]==browser["snapshot"]
    found=run("find",snapshot,"prices")
    assert found==run("find",folder/"ir.json","prices")
    assert [r["minor"] for r in found["matches"]]==[18000,54000,7500,12000]
    assert found["extraction"]["schema"]=="shylock.capture/v2"
    assert len(found["find_id"])==64
    text=run("find",snapshot,"Quoted nightly")
    assert len(text["matches"])==1 and text["matches"][0]["kind"]=="literal-text"
    assert run("find",snapshot,"absent")["matches"]==[]
    tampered=copy.deepcopy(ir); tampered["blocks"][0]["text"]="EUR 1.00"
    assert run("find",save("bad-ir.json",tampered),"prices",code=2) is None
    page=folder/"noise.html"
    page.write_text('''<html><head><title>Noisy title</title></head><body>
<header>EUR 999.00 total</header><nav>EUR 998.00 total</nav>
<main><h1>Cottage &amp; lake</h1><p>EUR <strong>180</strong>.00 per night</p>
<p>WiFi &amp; parking included.</p><p hidden>EUR 997.00 total</p>
<p aria-hidden="true">EUR 996.00 total</p><script>EUR 995.00 total</script>
<template>EUR 994.00 total</template><p>$ 25.00 cleaning fee</p></main>
<footer>EUR 993.00 total</footer></body></html>''')
    snap=run("snapshot","https://example.org/noise",1000,page)
    flat=run("flatten",save("noise.json",snap))
    assert [b["text"] for b in flat["blocks"]]==["Cottage & lake","EUR 180.00 per night","WiFi & parking included.","$ 25.00 cleaning fee"]
    prices=run("find",save("noise-ir.json",flat),"prices")["matches"]
    assert [m["minor"] for m in prices]==[18000,None]
    assert prices[0]["basis"]=="nightly" and prices[1]["currency"]==""
    for block in flat["blocks"]:
        start=block["byte_offset"]; end=start+block["byte_length"]
        assert snap["html"].encode()[start:end]
    changed=copy.deepcopy(job); changed["steps"][1]["selector"]="#missing"
    failed=run("browse",save("failed.json",changed),code=2)
    assert failed["status"]=="failed" and failed["trace"][-1]["status"]=="failed"
    assert failed["snapshot"]["source_url"]==job["start_url"]
    changed=copy.deepcopy(job); changed["steps"]=[{"action":"navigate","selector":"","value":"https://other.example/listing"}]
    failed=run("browse",save("failed.json",changed),code=2)
    assert failed["snapshot"]["source_url"]==job["start_url"]  # Failed navigation preserves the last good document.
    for step in ({"action":"evaluate","selector":"","value":"fetch('secret')"},
                 {"action":"read","selector":"main p","value":""},
                 {"action":"read","selector":"p","value":"","extra":1}):
        changed=copy.deepcopy(job); changed["steps"].append(step)
        assert run("browse",save("invalid.json",changed),code=2) is None
    changed=copy.deepcopy(job); changed["pages"]=[]
    assert run("browse",save("missing.json",changed),code=2)["snapshot"] is None
    profile=ROOT/"plugins/dragnet/openai-profile.json"
    account=run("openai-account",profile)
    assert account["configured"] is False and account["authenticated"] is None
    assert account["endpoint"]=="https://api.openai.com/v1"
    assert run("openai-account",profile,extra={"OPENAI_BASE_URL":""})==account
    endpoint="https://gateway.example:8443/openai/v1"
    custom=run("openai-account",profile,extra={"OPENAI_BASE_URL":endpoint+"///"})
    assert custom["endpoint"]==endpoint and custom["authenticated"] is None
    configured=run("openai-account",profile,extra={"OPENAI_API_KEY":"secret-not-an-account"})
    assert configured["configured"] is True and "secret-not-an-account" not in json.dumps(configured)
    fallback=run("model-extract",snapshot,profile,"prices","--live",code=2)
    assert fallback["status"]=="deterministic_fallback"
    assert fallback["model_audit"]["outcome"]=="credentials_unavailable"
    assert fallback["extraction"]==found["extraction"] and fallback["matches"]==found["matches"]
    assert fallback["model_claims"]==[]
    custom_fallback=run("model-extract",snapshot,profile,"prices","--live",code=2,extra={"OPENAI_BASE_URL":endpoint+"/"})
    assert custom_fallback["model_audit"]["endpoint"]==endpoint+"/responses"
    assert custom_fallback["model_audit"]["outcome"]=="credentials_unavailable"
    for bad in ("http://gateway.example/v1","https://localhost/v1","https://127.0.0.1/v1",
                "https://[::1]/v1","https://key:secret@gateway.example/v1","https://gateway.example/v1?api_key=secret",
                "https://gateway.example/v1#fragment","https://gateway.example:0/v1","https://gateway.example:65536/v1",
                "https://gateway.example/%0a","https://gateway.example/%ZZ","/v1"):
        assert run("openai-account",profile,code=2,extra={"OPENAI_BASE_URL":bad}) is None
        invalid=run("model-extract",snapshot,profile,"prices","--live",code=2,extra={"OPENAI_BASE_URL":bad})
        assert invalid["model_audit"]["outcome"]=="invalid_endpoint" and invalid["model_audit"]["endpoint"] is None
        assert invalid["extraction"]==found["extraction"] and invalid["model_claims"]==[]
    assert run("model-extract",snapshot,profile,"prices",code=2) is None  # Consent flag is mandatory.
    config=json.loads(profile.read_text()); config["api_key"]="do-not-store-keys"
    assert run("openai-account",save("invalid-profile.json",config),code=2) is None
print("Static browser, verified semantic IR, naive matching, account profile and deterministic model fallback tests passed.")
