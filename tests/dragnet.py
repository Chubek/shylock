import copy
import json
import os
from pathlib import Path
import subprocess
import tempfile

ROOT=Path(__file__).resolve().parents[1]
FIX=ROOT/"tests/fixtures"
def run(*args,code=0):
    env=dict(os.environ)
    env.pop("SHY_NATIVE_PLUGINS",None)
    p=subprocess.run([str(ROOT/"shylock"),*map(str,args)],cwd=ROOT,env=env,capture_output=True,text=True,timeout=20)
    assert p.returncode==code,(args,p.returncode,p.stdout,p.stderr)
    return json.loads(p.stdout) if p.stdout else None
with tempfile.TemporaryDirectory(prefix="shylock-dragnet-") as directory:
    folder=Path(directory)
    def save(name,obj):
        path=folder/name; path.write_text(json.dumps(obj)); return path
    for provider in ("google","bing"):
        result=run("discover",provider,"lake & cottage",FIX/f"{provider}.html",1)
        assert result["status"]=="results" and result["requested_limit"]==1
        assert result["snapshot"]["html"]==(FIX/f"{provider}.html").read_text()
        assert result["provider_source"]==(ROOT/f"third_party/mulib/library/elvi-scrape/{provider}.elvs").read_text()
        assert run("discover-replay",save("discovery.json",result))==result
        for change in ({"hits":[]},{"provider_blake3":"0"*64},{"status":"no_parsed_hits"},{"provider_source":"invalid"}):
            run("discover-replay",save("bad.json",dict(result,**change)),code=2)
    empty=folder/"empty.html"; empty.write_text("<html>No result</html>")
    assert run("discover","google","lake",empty)["status"]=="no_parsed_hits"
    run("discover","../google","lake",empty,code=2)
    run("discover","google","lake",empty,0,code=2)
    failed=run("discover","google","lake",folder/"missing.html",code=2)
    assert failed["status"]=="transport_error" and failed["snapshot"] is None
    batch=run("discover-many",ROOT/"plugins/dragnet/offline-job.json")
    assert batch["status"]=="complete" and [r["provider"] for r in batch["results"]]==["google","bing"]
    job=copy.deepcopy(batch["job"])
    for provider in job["providers"]:
        provider["capture"]=str(FIX/f'{provider["name"]}.html')
    job["providers"].append({"name":"missing-provider","capture":str(empty)})
    partial=run("discover-many",save("job.json",job),code=2)
    assert partial["status"]=="partial_failure" and len(partial["results"])==3
    job["providers"][-1]={"name":"google","capture":""}
    assert run("discover-many",save("job.json",job),code=2) is None  # Validate entire job before effects.
    snapshot=run("snapshot","https://example.org/cottage",1760000000,FIX/"rental.html")
    path=save("snapshot.json",snapshot)
    assert run("extract-snapshot",path,FIX/"rule.json")==run("extract",FIX/"rental.html","https://example.org/cottage",1760000000,FIX/"rule.json")
    for change in ({"html":"tampered"},{"byte_length":1},{"blake3":"0"*64},{"http_status":200},{"extra":True}):
        run("extract-snapshot",save("bad.json",dict(snapshot,**change)),code=2)
    page=folder/"richer.html"
    page.write_text('''<script type="application/ld+json">{"@type":"ItemList","itemListElement":[{"item":{"@type":"VacationRental","offers":{"@type":["Offer"],"priceCurrency":"EUR","priceSpecification":{"@type":"UnitPriceSpecification","price":"190.00","unitCode":"DAY"}}}},{"item":{"@type":"AggregateOffer","lowPrice":"150.00","priceCurrency":"EUR"}}]}</script>
<p>EUR <b>180</b>.00 per <span>night</span></p>
<p>EUR&nbsp;190.00 total</p>
<meta itemprop="price" content="200.00" data-currency="EUR">
<p>$210.00 per night</p><p>€220.00 per night</p>
<p>EUR 1.234,50 per night</p>
<script>EUR 999.00 per night</script>''')
    rule=json.loads((FIX/"rule.json").read_text())
    rule.update(decimal_separator=",",group_separator=".")
    richer=run("extract",page,"https://example.org/cottage",1000,save("rule.json",rule))
    rows=richer["candidates"]
    assert rows[0]["minor"]==19000 and rows[0]["basis"]=="per_day"
    assert rows[1]["qualifier"]=="from" and rows[1]["minor"]==15000
    split=next(r for r in rows if r["amount_text"]=="180.00")
    raw=page.read_text()[split["byte_offset"]:split["byte_offset"]+split["byte_length"]]
    assert raw=="EUR <b>180</b>.00 per <span>night"  # Full original byte range, not joined text offsets.
    assert next(r for r in rows if r["method"]=="microdata")["minor"]==20000
    assert next(r for r in rows if r["amount_text"]=="210.00")["currency"]==""
    localized=next(r for r in rows if r["amount_text"]=="1.234,50")
    assert localized["minor"]==123450 and localized["normalized_amount"]=="1234.50"
    assert not any(r["minor"]==99900 for r in rows)
    page.write_text("<p>"+" "*4078+"EUR 1.00 per nightly</p>")
    run("extract",page,"https://example.org/cottage",1000,code=2)  # Never turn a truncated 'nightly' into 'night'.
print("Google/Bing ElviScrape provenance, discovery replay, snapshots and richer extraction tests passed.")
