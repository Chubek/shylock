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
assert run("plugins") == [{"name": "dragnet", "version": "dragnet/v2", "abi": 2}]

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
scoped = run("extract", fixture / "scoped.html", target, 1760000000, fixture / "scoped-rule.json")
assert [row["minor"] for row in scoped["candidates"]] == [18000, 18000]
assert all(row["method"] == "site-rule" and row["rule_blake3"] for row in scoped["candidates"])
assert run("extract", page, "https://other.example/", 1760000000, fixture / "rule.json", status=2) is None
series = fixture / "series.json"
assert run("estimate", series, 2)["reference_minor"] == 18000
assert run("adjust", "formulas/weekend.lua", 18000, 1, 1500, 10000, 30000, 100)["adjusted_minor"] == 20700
assert run("adjust", "formulas/weekend.lua", 18050, 0, 1500, 10000, 18070, 100)["adjusted_minor"] == 18000
recommend = run("recommend", series, 2, "formulas/weekend.lua", 1, 1000, 10000, 30000, 100)
assert recommend["forecast_minor"] > 20000 and recommend["adjusted_minor"] == 23100

with tempfile.TemporaryDirectory() as temp:
    root = Path(temp)
    rule = json.loads((fixture / "scoped-rule.json").read_text())
    rule["container"]["value"] = "missing"
    absent_rule = root / "absent-rule.json"
    absent_rule.write_text(json.dumps(rule))
    absent = run("extract", fixture / "scoped.html", target, 1760000000, absent_rule)
    assert absent["candidates"] == [] and absent["diagnostics"][0]["code"] == "site_rule_no_container"
    rule["version"] = 3
    absent_rule.write_text(json.dumps(rule))
    run("extract", fixture / "scoped.html", target, 1760000000, absent_rule, status=2)
    bad = root / "bad.lua"
    for source in (
        "return function() return os.time() end",
        "return function() while true do end end",
        "return function() return 1.5 end",
        "return function() return 1, 2 end",
        "return function() return require('x') end",
        "return function() return _G end",
        "return function() return math.random() end",
        "return function() return math.randomseed(1) end",
        "return function() return extension.emit('x') end",
        "return function() return host.log('x') end",
        "return function() return load('return 1')() end",
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
    invalid_model = run(*args, reply)
    assert invalid_model["candidates"] == capture["candidates"]
    assert invalid_model["diagnostics"][0]["code"] == "model_reply_invalid"
    assert run(*args, root / "absent-model.json")["candidates"] == capture["candidates"]
    # One invalid suggestion rejects the entire model batch, not just its tail.
    model["suggestions"][0]["offset"] -= 1
    model["suggestions"].append(dict(model["suggestions"][0], offset=0))
    reply.write_text(json.dumps(model))
    assert run(*args, reply)["candidates"] == capture["candidates"]
    reply.write_bytes(b"\xff")
    assert run(*args, reply)["candidates"] == capture["candidates"]

    oversized = root / "oversized.html"
    oversized.write_text("x" * (2 * 1024 * 1024 + 1))
    run("extract", oversized, target, 1760000000, status=2)
    too_many = root / "too-many.html"
    too_many.write_text("<p>EUR 180.00 per night</p>\n" * 257)
    run("extract", too_many, target, 1760000000, status=2)

    curl = root / "curl"
    curl.write_text("#!/bin/sh\nprintf '<h2><a href=\"https://example.org/cottage\">Lake Cottage</a></h2>\\n200\\ntext/html\\n'\n")
    curl.chmod(0o755)
    assert run("discover", "bing", "lake cottage", path=root)["hits"][0]["url"] == target
    assert run("capture", target, path=root, raw=True).endswith("</h2>")
    for url in ("http://example.org/", "https://127.0.0.1/", "https://localhost/",
                "https://[::1]/", "https://user:pass@example.org/"):
        run("capture", url, path=root, status=2)
    # Redirects and wrong media types never become captures.
    for response in ("<html>redirect</html>\\n302\\ntext/html\\n",
                     "{}\\n200\\napplication/json\\n"):
        curl.write_text(f"#!/bin/sh\nprintf '{response}'\n")
        run("capture", target, path=root, status=2)
print("Shylock integration tests passed.")
