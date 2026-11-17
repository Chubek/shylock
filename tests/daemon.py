"""Offline daemon admission, scheduling, evidence replay and process lifecycle."""
import copy
import json
import os
from pathlib import Path
import signal
import subprocess
import tempfile
import time

ROOT = Path(__file__).resolve().parents[1]
ENV = dict(os.environ)
for key in ("SHY_NATIVE_PLUGINS", "SHY_EXTENSIONS", "SHY_PROVIDER_ROOT"):
    ENV.pop(key, None)


def run(*args, code=0):
    p = subprocess.run([ROOT / "shylock", *map(str, args)], env=ENV,
                       cwd=ROOT, capture_output=True, text=True, timeout=30)
    assert p.returncode == code, (args, p.returncode, p.stdout, p.stderr)
    assert "Sanitizer" not in p.stderr, p.stderr
    return p


def until(predicate, timeout=15):
    end = time.monotonic() + timeout
    while time.monotonic() < end:
        value = predicate()
        if value:
            return value
        time.sleep(.04)
    raise AssertionError("daemon condition timed out")


with tempfile.TemporaryDirectory(prefix="shylock-daemon-") as temp:
    folder = Path(temp)
    config = folder / "job.json"
    output = folder / "runs"
    page = folder / "page.html"
    page.write_bytes((ROOT / "tests/fixtures/naive-rentals.html").read_bytes())
    rule = folder / "rule.json"
    rule.write_bytes((ROOT / "plugins/dragnet/rules/rental.json").read_bytes())
    search = folder / "search.html"
    search.write_text('<a href="https://example.org/cottage"><h3>Cottage</h3></a>'
                      '<a href="https://other.example/ignored"><h3>Other</h3></a>'
                      '<a href="https://example.org/unmapped"><h3>Unmapped</h3></a>')
    job = dict(schema="shylock.daemon/v1", provider="google", query="lake cottage",
               limit=10, interval_seconds=1, allow_network=False, capture="search.html",
               output="runs", rule="rule.json", target="rental_price", hosts=["example.org"],
               pages=[dict(url="https://example.org/cottage", path="page.html", captured_at=1760000000)])

    def save(value):
        config.write_text(json.dumps(value))

    def manifests():
        return sorted(output.glob("run-*/run.json"))

    def once(value, code=0):
        save(value)
        before = set(manifests())
        result = run("daemon", config, "--once", code=code)
        assert not result.stdout
        created = set(manifests()) - before
        assert len(created) == 1, (created, result.stderr)
        path = created.pop()
        return path.parent, json.loads(path.read_text())

    # All configuration and rule validation precedes output creation/acquisition.
    for changes in ({"unexpected": 1}, {"interval_seconds": 0}, {"interval_seconds": True},
                    {"interval_seconds": 604801}, {"provider": "../google"}, {"query": ""},
                    {"capture": ""}, {"limit": 65}, {"hosts": ["*"]},
                    {"hosts": ["example.org", "example.org"]}, {"hosts": ["localhost"]},
                    {"pages": [dict(job["pages"][0], extra=True)]},
                    {"pages": [dict(job["pages"][0], captured_at=253402300800)]},
                    {"pages": job["pages"] * 2}, {"target": "wrong"}):
        save(dict(job, **changes))
        run("daemon", config, "--once", code=2)
        assert not output.exists(), changes
    save(job)
    config.write_text(config.read_text()[:-1] + ',"allow_network":true}')
    run("daemon", config, "--once", code=2)
    assert not output.exists()
    save(job)
    config.write_bytes(config.read_bytes() + b"\0")
    run("daemon", config, "--once", code=2)
    assert not output.exists()
    bad_rule = json.loads(rule.read_text())
    bad_rule["fields"][0]["selector"] = ".price span"
    rule.write_text(json.dumps(bad_rule))
    save(job)
    run("daemon", config, "--once", code=2)
    assert not output.exists()
    rule.write_bytes((ROOT / "plugins/dragnet/rules/rental.json").read_bytes())

    directory, audit = once(job)
    assert audit["schema"] == "shylock.daemon-run/v1" and audit["status"] == "complete"
    assert audit["selected"] == 1 and audit["failed"] == 0
    assert [e["status"] for e in audit["entries"]] == ["needs_review", "skipped_host", "skipped_offline"]
    assert (directory / "config.json").read_bytes() == config.read_bytes()
    assert (directory / "rule.json").read_bytes() == rule.read_bytes()
    snapshot = json.loads((directory / audit["entries"][0]["snapshot"]).read_text())
    found_path = directory / audit["entries"][0]["extraction"]
    found = json.loads(found_path.read_text())
    assert snapshot["captured_at"] == 1760000000 and snapshot["acquisition"] == "saved"
    assert found["summary"]["candidates"] > 0
    assert json.loads(run("find-rule-replay", found_path).stdout) == found
    discovery = json.loads((directory / "discovery.json").read_text())
    assert audit["discovery_id"] == discovery["discovery_id"]
    assert json.loads(run("discover-replay", directory / "discovery.json").stdout) == discovery
    next_directory, _ = once(job)
    assert next_directory != directory
    assert json.loads((next_directory / "hit-0-prices.json").read_text()) == found
    assert not list(output.glob("run-*/*.part"))
    assert (output / "daemon.pid").read_text() == ""

    # A bad selected page is recorded without hiding subsequent good evidence.
    broken = copy.deepcopy(job)
    broken["pages"][0]["path"] = "missing.html"
    broken["pages"].append(dict(url="https://example.org/unmapped", path="page.html", captured_at=1000))
    _, failed = once(broken, code=2)
    assert failed["status"] == "partial_failure" and failed["failed"] == 1
    assert [e["status"] for e in failed["entries"]] == ["snapshot_error", "skipped_host", "needs_review"]
    _, failed = once(dict(job, capture="missing.html"), code=2)
    assert failed["status"] == "discovery_error" and not failed["entries"]
    _, failed = once(dict(job, provider="missing-provider"), code=2)
    assert failed["status"] == "discovery_error" and failed["discovery"] == ""
    empty = folder / "empty.html"
    empty.write_text("<html>No hits</html>")
    _, no_hits = once(dict(job, capture="empty.html"))
    assert no_hits["status"] == "no_selected_hits"
    _, bounded = once(dict(job, limit=1))
    assert len(bounded["entries"]) == 1

    # PID lock, recurring runs, configuration/rule pinning and idle SIGINT.
    save(job)
    before = set(manifests())
    process = subprocess.Popen([ROOT / "shylock", "daemon", config], env=ENV,
                               cwd=ROOT, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    try:
        until(lambda: len(set(manifests()) - before) >= 1)
        pid_text = (output / "daemon.pid").read_text()
        assert int(pid_text) == process.pid
        run("daemon", config, "--once", code=2)
        assert (output / "daemon.pid").read_text() == pid_text
        # Edits are applied only on restart; already admitted sources are pinned.
        rule.write_text("invalid")
        config.write_text("invalid")
        until(lambda: len(set(manifests()) - before) >= 3)
        scheduled = sorted(set(manifests()) - before, key=lambda p: p.stat().st_mtime_ns)
        for manifest in scheduled:
            assert json.loads(manifest.read_text())["status"] == "complete"
        for a, b in zip(scheduled, scheduled[1:]):
            assert b.stat().st_mtime - a.stat().st_mtime >= .9
        process.send_signal(signal.SIGINT)
        stdout, stderr = process.communicate(timeout=10)
        assert process.returncode == 0 and not stdout and not stderr, (stdout, stderr)
        assert (output / "daemon.pid").read_text() == ""
    finally:
        if process.poll() is None:
            process.kill()
            process.communicate()
    rule.write_bytes((ROOT / "plugins/dragnet/rules/rental.json").read_bytes())

    # A transient source error does not stop subsequent scheduled runs.
    recovery = copy.deepcopy(job)
    recovery["pages"][0]["path"] = "recover.html"
    save(recovery)
    before = set(manifests())
    process = subprocess.Popen([ROOT / "shylock", "daemon", config, "--foreground"], env=ENV,
                               cwd=ROOT, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    try:
        until(lambda: set(manifests()) - before)
        (folder / "recover.html").write_bytes(page.read_bytes())
        until(lambda: any(json.loads(p.read_text())["status"] == "complete" for p in set(manifests()) - before))
        process.terminate()
        stdout, stderr = process.communicate(timeout=10)
        assert process.returncode == 0 and not stdout and "Sanitizer" not in stderr
    finally:
        if process.poll() is None:
            process.kill()
            process.communicate()

    # Detachment acknowledges initialization, returns with closed caller pipes,
    # keeps the lock in the supervisor, and releases it on SIGTERM.
    save(dict(job, interval_seconds=3600))
    before = set(manifests())
    run("daemon", config, "--detach")
    pid = int((output / "daemon.pid").read_text())
    try:
        until(lambda: set(manifests()) - before)
        assert (output / "daemon.log").exists()
        run("daemon", config, "--detach", code=2)
    finally:
        os.kill(pid, signal.SIGTERM)
        until(lambda: (output / "daemon.pid").read_text() == "")
    once(job)  # Restart after orderly shutdown reuses the lock inode safely.

print("Dragnet daemon admission, replay, scheduling, lock and shutdown tests passed.")
