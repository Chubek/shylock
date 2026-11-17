import subprocess
import sys
import json
import os
from pathlib import Path
import tempfile

root = Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix="shylock-collection-alloc-", dir=os.environ.get("TMPDIR")) as tmp:
    folder = Path(tmp)
    env = dict(os.environ, SHY_BINARY=str(Path(sys.argv[2]).resolve()))
    for key in ("SHY_EXTENSIONS", "SHY_NATIVE_PLUGINS", "SHY_PROVIDER_ROOT"):
        env.pop(key, None)

    def artifact(name, *args):
        result = subprocess.run([str(root / "shylock"), *map(str, args)], env=env, cwd=root,
                                capture_output=True, text=True, check=True)
        path = folder / name
        path.write_text(result.stdout)
        return path

    # Isolate collection resource ownership from the separately fault-injected
    # extraction engine. A small valid empty artifact exercises every native
    # collection allocation without quadratic re-parsing of large quote trees.
    search, page, rules = folder / "search.html", folder / "page.html", folder / "rules.json"
    search.write_text('<a href="https://example.org/cottages"><h3>Cottage</h3></a>')
    page.write_text('<body><p>No quotes</p></body>')
    rule_data = json.loads((root / "plugins/dragnet/rules/rental.json").read_text())
    rule_data.update(container=".absent", include_existing=False)
    rules.write_text(json.dumps(rule_data))
    discovery = artifact("discovery.json", "discover", "google", "cottages", search)
    snapshot = artifact("snapshot.json", "snapshot", "https://example.org/cottages", 1000, page)
    job = folder / "job.json"
    job.write_text(json.dumps(dict(schema="shylock.collection-job/v1", target="rental_price", sources=[
        dict(discovery=str(discovery), hit=0, snapshot=str(snapshot), rule=str(rules))])))
    bundle = artifact("collection.json", "collect", job)
    env["SHY_COLLECTION_ALLOCATION_FIXTURE"] = str(bundle)
    proc = subprocess.run([sys.argv[1]], env=env, capture_output=True, text=True, timeout=180)
assert proc.returncode == 0 and "Sanitizer" not in proc.stderr, (proc.returncode, proc.stdout, proc.stderr[-8000:])
print(proc.stdout, end="")
