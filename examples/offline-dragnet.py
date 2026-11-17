"""Create a new, fully offline discovery/review/pricing demonstration directory."""
import json
import os
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
output = Path(sys.argv[1]).resolve()
output.mkdir(parents=True, exist_ok=False)
env = dict(os.environ)
for key in ("SHY_EXTENSIONS", "SHY_NATIVE_PLUGINS", "SHY_PROVIDER_ROOT"):
    env.pop(key, None)


def run(*args):
    return subprocess.run([str(ROOT / "shylock"), *map(str, args)], cwd=ROOT, env=env,
                          capture_output=True, text=True, check=True).stdout


def save(name, value):
    path = output / name
    path.write_text(json.dumps(value, ensure_ascii=False))
    return path


(output / "discovery.json").write_text(run("discover", "google", "cottages", ROOT / "tests/fixtures/collection-search.html"))
sources = []
start, step, count = 1793400000, 60, 7
for i in range(count):
    name = f"snapshot-{i}.json"
    (output / name).write_text(run("snapshot", "https://example.org/cottages", start + i * step,
                                   ROOT / "tests/fixtures/collection-quotes.html"))
    sources.append(dict(discovery="discovery.json", hit=0, snapshot=name,
                        rule=str(ROOT / "plugins/dragnet/rules/rental.json")))
job = save("job.json", dict(schema="shylock.collection-job/v1", target="rental_price", sources=sources))
bundle = output / "collection.json"
bundle.write_text(run("collect", job))
assert json.loads(run("collect-replay", bundle)) == json.loads(bundle.read_text())
context = json.loads((ROOT / "tests/fixtures/context.json").read_text())
context.pop("listing_id")
context_path = save("context.json", context)
database = output / "prices.db"
for entry in range(count):
    for candidate in range(2):
        run("review-collection", database, bundle, entry, candidate, "cottages", "demo-reviewer", context_path)
selection = save("selection.json", dict(schema="shylock.ledger-selection/v1", cohort="cottages", currency="EUR",
                                        start=start, step=step, count=count, as_of=start + step * count - 1,
                                        max_age_seconds=59, minimum_listings=2))
pricing = json.loads((ROOT / "policies/pricing.json").read_text())
pricing["as_of"] = start + step * count - 1
pricing["formula"]["path"] = str(ROOT / "formulas/weekend.lua")
policy = save("pricing.json", pricing)
forecast_policy = save("forecast-policy.json", pricing["forecast"])
(output / "forecast.json").write_text(run("forecast-ledger", database, selection, forecast_policy))
decision = output / "decision.json"
decision.write_text(run("price-ledger", database, selection, policy))
(output / "report.tex").write_text(run("print-report", ROOT / "reports/pricing.json", decision))
print(f"Synthetic offline evidence, ledger, forecast, pricing audit and LaTeX: {output}")
