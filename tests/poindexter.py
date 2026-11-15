import json, os, pathlib, subprocess, tempfile
root=pathlib.Path(__file__).resolve().parents[1]
def run(*args, code=0):
    p=subprocess.run([str(root/"shylock"),*map(str,args)],cwd=root,text=True,capture_output=True)
    assert p.returncode==code,(args,p.returncode,p.stderr)
    return json.loads(p.stdout) if p.stdout else None
with tempfile.TemporaryDirectory() as directory:
    folder=pathlib.Path(directory); db=folder/"prices.db"; capture=folder/"capture.json"
    original=run("extract","tests/fixtures/rental.html","https://example.org/cottage",1000)
    for timestamp in (1000,1060):
        record=dict(original,captured_at=timestamp)
        # Distinct capture content hashes represent separate saved observations.
        record["blake3"]=("a" if timestamp==1000 else "b")*64
        capture.write_text(json.dumps(record))
        run("review",db,capture,1,"cottage/eu/tax-included","alice")
        run("review",db,capture,1,"cottage/eu/tax-included","alice")
        run("review",db,capture,1,"other","alice",code=3)
    assert len(run("ledger",db,"cottage/eu/tax-included"))==2
    series=run("series",db,"cottage/eu/tax-included","EUR",1000,60,2)
    assert series==[18000,18000]
    run("series",db,"cottage/eu/tax-included","EUR",1000,60,3,code=4)
    run("series",db,"cottage/eu/tax-included","USD",1000,60,2,code=3)
    run("review",db,capture,2,"cottage/eu/tax-included","alice",code=2)
    path=folder/"series.json"; path.write_text(json.dumps(series))
    result=run("recommend",path,1,"formulas/weekend.lua",1,1000,10000,30000,100)
    assert result["adjusted_minor"]==19800
print("Poindexter integration tests passed.")
assert run("adjust","formulas/rounded.lua",18000,1,1000,10000,30000,100)["adjusted_minor"]==19800
