import copy
import json
import os
from pathlib import Path
import subprocess
import tempfile

ROOT=Path(__file__).resolve().parents[1]
FIX=ROOT/"tests/fixtures"
ELVI=ROOT/"third_party/mulib/tests/fixtures/elvi-scrape"
def run(*args,code=0,extra=None,diagnostic=None):
    env=dict(os.environ)
    env.pop("SHY_NATIVE_PLUGINS",None)
    env.pop("SHY_EXTENSIONS",None)
    env.pop("SHY_PROVIDER_ROOT",None)
    if extra:
        env.update(extra)
    p=subprocess.run([str(ROOT/"shylock"),*map(str,args)],cwd=ROOT,env=env,capture_output=True,text=True,timeout=20)
    assert p.returncode==code,(args,p.returncode,p.stdout[:2000],p.stderr)
    if diagnostic:
        assert diagnostic in p.stderr,p.stderr
    return json.loads(p.stdout) if p.stdout else None
with tempfile.TemporaryDirectory(prefix="shylock-dragnet-") as directory:
    folder=Path(directory)
    def save(name,obj):
        path=folder/name; path.write_text(json.dumps(obj)); return path
    for provider in ("google","bing"):
        result=run("discover",provider,"lake & cottage",FIX/f"{provider}.html",1)
        assert result["status"]=="results" and result["requested_limit"]==1
        assert result["parser_profile"]=="dragnet.elvi/hardened-v1"
        assert result["snapshot"]["html"]==(FIX/f"{provider}.html").read_text()
        assert result["provider_source"]==(ROOT/f"third_party/mulib/library/elvi-scrape/{provider}.elvs").read_text()
        assert run("discover-replay",save("discovery.json",result))==result
        for change in ({"hits":[]},{"provider_blake3":"0"*64},{"status":"no_parsed_hits"},{"provider_source":"invalid"},{"parser_profile":"unknown"},{"schema":"unknown"}):
            run("discover-replay",save("bad.json",dict(result,**change)),code=2)
        old=dict(result,mulib_revision="151528b50b316aeea804ce2aebf66ef90b9e7100")
        old.pop("parser_profile")
        run("discover-replay",save("old.json",old),code=2,diagnostic="use the original executable")
        limited=run("discover",provider,"lake & cottage",FIX/f"{provider}.html",64)
        assert limited["requested_limit"]==64 and limited["effective_limit"]==30
        assert ("num=30" if provider=="google" else "count=30") in limited["search_url"]
        assert run("discover-replay",save("limited.json",limited))==limited
    # Exercise the upgraded upstream fixtures through Shylock, not a parallel
    # scraper: inert/RCDATA contexts, heading closure and redirect ambiguity.
    for provider,name,hits in (
        ("google","inert.html",[("https://example.org/visible","Visible 🚀 guide"),("https://example.org/second","One two")]),
        ("google","redirects.html",[("https://example.org/safe?x=1&y=2","Safe")]),
        ("bing","headings.html",[("https://example.org/first","First"),("https://example.org/second","Second")]),
    ):
        result=run("discover",provider,"upgraded Elvi",ELVI/name)
        assert [(h["url"],h["title"]) for h in result["hits"]]==hits
        assert result["snapshot"]["html"]==(ELVI/name).read_text()
        assert run("discover-replay",save("hardened.json",result))==result
    titles=folder/"titles.html"
    titles.write_text('<a href="https://example.org/one"><h3>One\u00a0 two&#160; three</h3></a>'
                      '<a href="https://example.org/two"><h3>'+'a'*510+'&#x1F680;Z</h3></a>')
    result=run("discover","google","a & # café",titles)
    assert [h["title"] for h in result["hits"]]==["One two three","a"*510]
    assert "%23" in result["search_url"] and "#" not in result["search_url"]
    assert run("discover-replay",save("titles.json",result))==result
    # Malformed HTML remains best-effort, not automatically a parser failure.
    # A long adversarial tail must remain bounded and replay identically.
    hostile=folder/"hostile.html"
    hostile.write_text('<a href="https://example.org/safe"><h3>Safe</h3></a>'+"<a x='"*4096)
    bounded=run("discover","google","budget",hostile)
    assert bounded["hits"]==[{"url":"https://example.org/safe","title":"Safe"}]
    assert run("discover-replay",save("bounded.json",bounded))==bounded
    # Reject fragment-bearing provider requests at compilation, before transport.
    provider_source=(ROOT/"third_party/mulib/library/elvi-scrape/google.elvs").read_text()
    for directive in ("home", "search"):
        lines=provider_source.splitlines()
        for index,line in enumerate(lines):
            if line.startswith(directive+" "):
                lines[index]=(directive+' "https://www.google.com/#fragment"' if directive=="home" else
                              'search "https://www.google.com/#q={query}"')
        (folder/"google.elvs").write_text("\n".join(lines)+"\n")
        run("discover","google","test",FIX/"google.html",extra={"SHY_PROVIDER_ROOT":str(folder)},code=2)
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
    budget_job=copy.deepcopy(job)
    budget_job["providers"]=budget_job["providers"][:2]
    budget_job["providers"][0]["capture"]=str(hostile)
    budget=run("discover-many",save("budget-job.json",budget_job))
    assert budget["status"]=="complete"
    assert budget["results"][0]["hits"]==bounded["hits"]
    assert budget["results"][1]["status"]=="results"
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
