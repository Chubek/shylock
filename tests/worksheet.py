"""Shared worksheet controller, actual MuLIB rendering, and terminal lifecycle."""
import json
import os
from pathlib import Path
import pty
import select
import shutil
import signal
import subprocess
import tempfile
import termios
import time

ROOT = Path(__file__).resolve().parents[1]
FIXTURE = ROOT / "tests/fixtures"


def run(*args, code=0, config=None, with_audit=False):
    env = dict(os.environ)
    env.pop("SHY_EXTENSIONS", None)
    env.pop("SHY_FORMULA_SANDBOX", None)
    env["SHY_SANDBOX"] = env.get("SHY_TEST_SANDBOX", "off")
    if config:
        env["SHY_EXTENSIONS"] = str(config)
    proc = subprocess.run([str(ROOT / "shylock"), *map(str, args)], cwd=ROOT,
                          env=env, text=True, capture_output=True, timeout=30)
    assert proc.returncode == code, (args, proc.returncode, proc.stdout, proc.stderr)
    output = json.loads(proc.stdout) if proc.stdout else None
    if with_audit:
        return output, [json.loads(line) for line in proc.stderr.splitlines() if line.startswith("{")]
    return output


def terminal_read(fd, needle, timeout=10):
    output = b""
    deadline = time.monotonic() + timeout
    while needle not in output and time.monotonic() < deadline:
        ready, _, _ = select.select([fd], [], [], max(0, deadline-time.monotonic()))
        if ready:
            chunk = os.read(fd, 65536)
            if not chunk:
                break
            output += chunk
    assert needle in output, (needle, output[-2000:])
    return output


with tempfile.TemporaryDirectory(prefix="shylock-worksheet-") as directory:
    folder = Path(directory)
    db, capture, events = folder / "original.db", folder / "capture.json", folder / "events.json"
    for timestamp in (1000, 1060):
        record = run("extract", FIXTURE / "rental.html", "https://example.org/cottage", timestamp)
        capture.write_text(json.dumps(record))
        run("review", db, capture, 1, "cottages", "alice", FIXTURE / "rental.html", FIXTURE / "context.json")
    initial = run("sheet", db, "cottages")
    assert initial["schema"] == "shylock.worksheet/v1" and len(initial["rows"]) == 2
    assert {col["key"] for col in initial["columns"] if col["editable"]} == {
        "listing_id", "stay_start", "stay_end", "guests", "taxes", "fees", "decision",
        "currency", "basis", "qualifier", "conflict_resolution"}

    reports = []
    for backend in ("tui", "gui"):
        target = folder / f"{backend}.db"
        shutil.copyfile(db, target)
        changes = [
            {"action": "select", "row": 0, "column": "guests"},
            {"action": "edit", "text": "3"},
            {"action": "reason", "text": "Guest count confirmed"},
            {"action": "save"},
            {"action": "select", "row": 0, "column": "decision"},
            {"action": "edit", "text": "rejected"},
            {"action": "reason", "text": "Different guest cohort"},
            {"action": "save"}, {"action": "sort"}, {"action": "filter", "text": "rejected"},
            {"action": "resize", "width": 960 if backend == "gui" else 100, "height": 640 if backend == "gui" else 25},
            {"action": "quit"},
        ]
        events.write_text(json.dumps({"schema": "shylock.ui-events/v1", "events": changes}))
        args = [backend, target, "cottages", "bob", events]
        frame = folder / "worksheet.ppm"
        if backend == "gui":
            args.append(frame)
        result = run(*args)
        assert result["pending"] == {} and result["pending_reason"] == ""
        assert len(result["rows"]) == 1 and result["rows"][0]["guests"] == 3
        assert result["rows"][0]["decision"] == "rejected"
        assert result["render"]["backend"] == ("mulib-tui" if backend == "tui" else "mulib-immgtk")
        assert len(result["render"]["blake3"]) == 64 and result["render"]["frames"] >= len(changes)
        assert all(action["status"] == 0 for action in result["actions"])
        history = run("history", target, initial["rows"][0]["observation_id"])
        assert len(history) == 3 and history[0]["guests"] == 2 and history[1]["guests"] == 3
        assert history[0]["evidence"] == history[2]["evidence"]
        reports.append(result)
        if backend == "gui":
            pixels = frame.read_bytes()
            header = b"P6\n960 640\n255\n"
            assert pixels.startswith(header) and len(pixels) == len(header)+960*640*3
            assert len(set(pixels[len(header):])) > 10
    assert reports[0]["rows"] == reports[1]["rows"]

    # Actual ImmGTK press/capture/release dispatch, not a synthetic select.
    events.write_text(json.dumps({"schema": "shylock.ui-events/v1", "events": [
        {"action": "pointer", "x": 970, "y": 165, "down": True},
        {"action": "pointer", "x": 970, "y": 165, "down": False}, {"action": "quit"}]}))
    clicked = run("gui", db, "cottages", "bob", events)
    assert clicked["selected_column"] == "guests"
    assert any(action["action"] == "select" for action in clicked["actions"])

    # Effectful hooks run outside domain locks. Even a shared KVDB storage path
    # cannot deadlock or expose observation keys to an extension namespace.
    target = folder / "hooks.db"
    shutil.copyfile(db, target)
    extension = folder / "hooks.lua"
    extension.write_text('''return {api="shylock.extension/v1",on_event=function(event)
host.kv_set(event,"seen");host.log(event) end}''')
    manifest = folder / "hooks.manifest.json"
    manifest.write_text(json.dumps({"schema": "shylock.extension/v1", "name": "worksheet-hooks",
        "script": extension.name, "capabilities": ["log", "kv.write"],
        "events": ["worksheet.opened", "observation.reviewed", "worksheet.saved"]}))
    config = folder / "hooks.config.json"
    config.write_text(json.dumps({"schema": "shylock.extensions/v1", "extensions": [{
        "manifest": manifest.name, "grant": ["log", "kv.write"], "storage": target.name}]}))
    events.write_text(json.dumps({"schema": "shylock.ui-events/v1", "events": [
        {"action": "select", "row": 0, "column": "guests"}, {"action": "edit", "text": "3"},
        {"action": "reason", "text": "Hook integration"}, {"action": "save"}, {"action": "quit"}]}))
    hooked, audit = run("gui", target, "cottages", "bob", events, config=config, with_audit=True)
    assert [row["phase"] for row in audit] == ["worksheet.opened", "observation.reviewed", "worksheet.saved"]
    assert hooked["rows"][0]["guests"] == 3 and len(run("ledger", target, "cottages")) == 2

    # Evidence is read-only, validation failures retain pending edits, and no
    # initial or failed edit writes anything until a confirmed review revision.
    for backend in ("tui", "gui"):
        changes = [
            {"action": "select", "row": 0, "column": "minor"}, {"action": "edit", "text": "1"},
            {"action": "select", "row": 0, "column": "guests"}, {"action": "edit", "text": "1.5"},
            {"action": "edit", "text": "65"}, {"action": "edit", "text": "3"},
            {"action": "save"}, {"action": "refresh"}, {"action": "discard"},
            {"action": "select", "row": 0, "column": "taxes"}, {"action": "edit", "text": "unknown"},
            {"action": "reason", "text": "Incomplete context"}, {"action": "save"},
        ]
        events.write_text(json.dumps({"schema": "shylock.ui-events/v1", "events": changes}))
        report = run(backend, db, "cottages", "bob", events, code=2)
        assert report["pending"] == {"taxes": "unknown"} and report["pending_reason"] == "Incomplete context"
        assert run("sheet", db, "cottages")["rows"] == initial["rows"]
        assert any(action["status"] == 2 for action in report["actions"])

    # Identical shared text-entry behavior for both frontends, including UTF-8.
    for backend in ("tui", "gui"):
        target = folder / f"keys-{backend}.db"
        shutil.copyfile(db, target)
        changes = [
            {"action": "select", "row": 0, "column": "guests"},
            {"action": "key", "key": "enter"}, {"action": "key", "key": "backspace"},
            {"action": "key", "key": "text", "text": "4"}, {"action": "key", "key": "enter"},
            {"action": "key", "key": "n"}, {"action": "key", "key": "text", "text": "café review"},
            {"action": "key", "key": "enter"}, {"action": "key", "key": "s"}, {"action": "quit"},
        ]
        events.write_text(json.dumps({"schema": "shylock.ui-events/v1", "events": changes}))
        report = run(backend, target, "cottages", "bob", events)
        assert report["rows"][0]["guests"] == 4 and report["rows"][0]["reason"] == "café review"

    # Empty selections and clipped small surfaces still render correctly.
    events.write_text(json.dumps({"schema": "shylock.ui-events/v1", "events": [
        {"action": "filter", "text": "no matching listing"}, {"action": "down"}, {"action": "quit"}]}))
    for backend in ("tui", "gui"):
        assert run(backend, db, "cottages", "bob", events)["rows"] == []

    # Real terminal mode/input, not just model events. No DB lock is held while
    # a user thinks; an external writer can advance the selected review head.
    master, slave = pty.openpty()
    original_termios = termios.tcgetattr(slave)
    env = dict(os.environ)
    env.pop("SHY_EXTENSIONS", None)
    env.pop("SHY_FORMULA_SANDBOX", None)
    env["SHY_SANDBOX"] = env.get("SHY_TEST_SANDBOX", "off")
    process = subprocess.Popen([str(ROOT / "shylock"), "tui", str(db), "cottages", "bob"],
                               cwd=ROOT, env=env, stdin=slave, stdout=slave, stderr=slave)
    try:
        terminal_read(master, b"POINDEXTER")
        selected = initial["rows"][0]
        run("revise", db, selected["observation_id"], selected["review_id"], "rejected", "carol", "external review")
        os.write(master, b"llllll\r\x7f3\rnConfirmed\rs")
        terminal_read(master, b"Stale review")
        os.write(master, b"qq")
        process.wait(timeout=10)
        assert process.returncode == 0
        assert termios.tcgetattr(slave) == original_termios
        history = run("history", db, selected["observation_id"])
        assert len(history) == 2 and history[-1]["reviewer"] == "carol"
    finally:
        if process.poll() is None:
            process.kill()
            process.wait()
        os.close(master)
        os.close(slave)

    target = folder / "paste.db"
    shutil.copyfile(db, target)
    master, slave = pty.openpty()
    original_termios = termios.tcgetattr(slave)
    process = subprocess.Popen([str(ROOT / "shylock"), "tui", str(target), "cottages", "bob"],
                               cwd=ROOT, env=env, stdin=slave, stdout=slave, stderr=slave)
    try:
        terminal_read(master, b"POINDEXTER")
        reason = "café" * 65
        # Decoder chunks split the two-byte accent. Paste content is text,
        # never navigation/save shortcuts even though it contains letters.
        os.write(master, b"llllll\r\x7f3\rn\x1b[200~" + reason.encode() + b"\x1b[201~\rs")
        terminal_read(master, b"Saved a new review revision")
        os.write(master, b"q")
        process.wait(timeout=10)
        assert process.returncode == 0 and termios.tcgetattr(slave) == original_termios
        assert run("sheet", target, "cottages")["rows"][0]["reason"] == reason
    finally:
        if process.poll() is None:
            process.kill()
            process.wait()
        os.close(master)
        os.close(slave)

    master, slave = pty.openpty()
    original_termios = termios.tcgetattr(slave)
    process = subprocess.Popen([str(ROOT / "shylock"), "tui", str(db), "cottages", "bob"],
                               cwd=ROOT, env=env, stdin=slave, stdout=slave, stderr=slave)
    try:
        terminal_read(master, b"POINDEXTER")
        process.send_signal(signal.SIGTERM)
        process.wait(timeout=10)
        assert termios.tcgetattr(slave) == original_termios
    finally:
        if process.poll() is None:
            process.kill()
            process.wait()
        os.close(master)
        os.close(slave)

print("Worksheet revisions, TUI/GUI parity, rendering, text input, concurrency and terminal cleanup tests passed.")
