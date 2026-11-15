import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile

binary, mulib = map(Path, sys.argv[1:])
fixture = Path(__file__).parent / "fixtures"

def run(*args, status=0, raw=False, path=None):
    env = dict(os.environ, SHY_BINARY=str(binary.resolve()),
               SHY_PROVIDER_ROOT=str((mulib / "library/elvi-scrape").resolve()))
    if path is not None:
        env["PATH"] = f"{path}:{env['PATH']}"
    proc = subprocess.run(["./shylock", *map(str, args)], env=env, timeout=10,
                          capture_output=True, text=True)
    assert proc.returncode == status, (proc.args, proc.returncode, proc.stderr)
    return proc.stdout if raw else json.loads(proc.stdout) if proc.stdout else None

for provider in ("google", "bing"):
    result = run("discover", provider, "lake cottage", fixture / f"{provider}.html")
    assert result["hits"] == [{"title": "Lake Cottage", "url": "https://example.org/cottage"}]
    assert "lake%20cottage" in result["search_url"]

page = fixture / "rental.html"
target = "https://example.org/cottage"
args = ("extract", page, target, 1760000000, fixture / "rule.json")
capture = run(*args)
assert capture == run(*args)
assert capture["source_url"] == target
assert [(x["method"], x["basis"], x["minor"]) for x in capture["candidates"]] == [
    ("json-ld", "unknown", 18000),
    ("visible-text", "nightly", 18000),
    ("visible-text", "stay_total", 54000),
    ("visible-text", "cleaning_fee", 7500),
    ("visible-text", "nightly", 12000),
    ("site-rule", "nightly", 18000),
]
assert capture["candidates"][4]["qualifier"] == "from"
assert capture["candidates"][0]["json_pointer"] == "/@graph/0/offers/price"
assert run("extract", page, "https://other.example/", 1760000000, fixture / "rule.json", status=2) is None
series = fixture / "series.json"
assert run("estimate", series, 2)["reference_minor"] == 18000
assert run("adjust", "formulas/weekend.lua", 18000, 1, 1500, 10000, 30000, 100)["adjusted_minor"] == 20700
assert run("adjust", "formulas/weekend.lua", 18050, 0, 1500, 10000, 18070, 100)["adjusted_minor"] == 18000
recommend = run("recommend", series, 2, "formulas/weekend.lua", 1, 1000, 10000, 30000, 100)
assert recommend["forecast_minor"] > 20000 and recommend["adjusted_minor"] == 23100

with tempfile.TemporaryDirectory() as temp:
    root = Path(temp)
    bad = root / "bad.lua"
    for source in (
        "return function() return os.time() end",
        "return function() while true do end end",
        "return function() return 1.5 end",
        "return function() return 1, 2 end",
        "return function() return require('x') end",
        "return function() return _G end",
    ):
        bad.write_text(source)
        failed = run("adjust", bad, 18000, 1, 1000, 10000, 30000, 100, status=2)
        assert failed["reference_minor"] == 18000
        assert failed["status"] == "formula_error"
        assert failed["adjusted_minor"] is None

    demand = root / "demand.json"
    demand.write_text(json.dumps([{"rate_minor": p, "expected_units": d}
                                  for p, d in [(15000, 4), (18000, 3), (20000, 2)]]))
    assert run("optimize", demand, 10000, 22000, 100, 10000)["recommended_minor"] == 18000
    reply = root / "review.json"
    quote = "EUR 180.00 per night"
    model = {"model": "test-model", "blake3": capture["blake3"], "suggestions": [{
        "offset": page.read_bytes().index(quote.encode()),
        "quote": quote, "amount": "180.00", "currency": "EUR",
    }]}
    reply.write_text(json.dumps(model))
    candidate = run(*args, reply)["candidates"][-1]
    assert candidate["method"] == "model" and candidate["qualifier"] == "review_required"
    model["suggestions"][0]["offset"] += 1
    reply.write_text(json.dumps(model))
    assert run(*args, reply, status=2) is None

    curl = root / "curl"
    curl.write_text("#!/bin/sh\nprintf '<h2><a href=\"https://example.org/cottage\">Lake Cottage</a></h2>\\n200\\ntext/html\\n'\n")
    curl.chmod(0o755)
    assert run("discover", "bing", "lake cottage", path=root)["hits"][0]["url"] == target
    assert run("capture", target, path=root, raw=True).endswith("</h2>")
print("Shylock integration tests passed.")
