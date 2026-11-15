#!/usr/bin/env python3
"""Synthetic saved-page demo. No network, model service or automatic publication."""
import copy
import json
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
if len(sys.argv) != 2:
    raise SystemExit("usage: python3 examples/offline-pricing.py NEW_OUTPUT_DIRECTORY")
destination = Path(sys.argv[1]).resolve()
destination.mkdir(parents=True, exist_ok=False)  # Never overwrite an existing audit.


def command(*args, text=False):
    p = subprocess.run([str(ROOT / "shylock"), *map(str, args)], cwd=ROOT,
                       capture_output=True, text=True, check=True, timeout=30)
    if text:
        return p.stdout
    return json.loads(p.stdout) if p.stdout else None


def save(name, value):
    path = destination / name
    path.write_text(json.dumps(value, sort_keys=True, indent=2) + "\n")
    return path


db = destination / "evidence.db"
start = 1793491200
for i in range(12):
    capture = command("extract", ROOT / "tests/fixtures/rental.html", "https://example.org/cottage", start + 60*i)
    path = save(f"capture-{i:02}.json", capture)
    command("review", db, path, 1, "cottages/2-guests", "offline-fixture-reviewer",
            ROOT / "tests/fixtures/rental.html", ROOT / "tests/fixtures/context.json")
cohort = save("cohort.json", command("cohort", db, "cottages/2-guests", "EUR", start, 60, 12))
forecast = save("forecast.json", command("forecast", cohort, ROOT / "policies/forecast.json"))
decision = save("decision.json", command("price", cohort, ROOT / "policies/pricing.json"))
demand = save("demand.json", command("demand", ROOT / "tests/fixtures/booking-outcomes.json", 1793500000, 10, 2))
policy = copy.deepcopy(json.loads((ROOT / "policies/pricing.json").read_text()))
policy["decision"] = "demand"
policy["formula"]["path"] = str(ROOT / "formulas/weekend.lua")
policy["constraints"].update(previous_minor=0, max_change_bp=0)
demand_policy = save("demand-policy.json", policy)
optimized = save("demand-decision.json", command("price", cohort, demand_policy, demand))
save("scenario-risk.json", command("optimize-model", ROOT / "policies/model.json"))
for name, template, data in (("forecast", "evaluated-forecast", forecast), ("decision", "pricing", decision),
                             ("demand-decision", "pricing", optimized)):
    (destination / f"{name}.tex").write_text(command("print-report", ROOT / f"reports/{template}.json", data, text=True))
print(f"Wrote synthetic evidence, evaluated forecasts, decisions, quotes and LaTeX reports to {destination}")
