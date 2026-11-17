"""Plugin source boundaries, composition, and optional clean build profiles."""
import argparse
import json
import os
import re
from pathlib import Path
import shlex
import shutil
import subprocess
import sys
import tempfile

from help import CORE, DRAGNET, POINDEXTER

ROOT = Path(__file__).resolve().parents[1]


def checked(args, **kwargs):
    result = subprocess.run(list(map(str, args)), capture_output=True, text=True,
                            timeout=600, **kwargs)
    assert result.returncode == 0, (args, result.stdout, result.stderr)
    return result.stdout


def layout():
    names = [path.name for path in (ROOT / "plugins").iterdir() if path.is_dir()]
    for source in (ROOT / "src").rglob("*"):
        if source.suffix not in (".c", ".h", ".on1x"):
            continue
        text = source.read_text().lower()
        for name in names:
            assert name not in source.name.lower(), source
            assert name not in text, (source, name)
        assert not re.search(r'(?:#\s*include|\bimport)\s+["<][^"<>]*plugins/', text), source
        for event in ("worksheet.opened", "worksheet.saved", "observation.reviewed"):
            assert event not in text, (source, event)
    with tempfile.TemporaryDirectory(prefix="shylock-plugin-composition-") as folder:
        build = Path(folder)
        generator = [sys.executable, ROOT / "tools/plugin_config.py", build]
        checked(generator, cwd=ROOT)
        core = (build / "main.on1x").read_text()
        native = (build / "builtin_plugins.c").read_text()
        assert "return -1;" in core and "return 1;" in native
        assert "plugins/" not in core + native
        args = [*generator, "--plugin", "dragnet", "--init",
                "plugins/dragnet/dragnet.h:shy_dragnet_init", "--event", "fixture.ready"]
        checked(args, cwd=ROOT)
        assert "dragnet_command" in (build / "main.on1x").read_text()
        assert "shy_dragnet_init()" in (build / "builtin_plugins.c").read_text()
        assert '"fixture.ready"' in (build / "builtin_plugins.c").read_text()
        times = [(build / name).stat().st_mtime_ns for name in ("main.on1x", "builtin_plugins.c")]
        checked(args, cwd=ROOT)
        assert times == [(build / name).stat().st_mtime_ns for name in ("main.on1x", "builtin_plugins.c")]
        checked(generator, cwd=ROOT)
        assert "dragnet" not in (build / "main.on1x").read_text()
        assert "dragnet" not in (build / "builtin_plugins.c").read_text()
        assert "fixture.ready" not in (build / "builtin_plugins.c").read_text()
    for selection, diagnostic in (("missing-plugin", "Unknown plugins"),
                                  ("poindexter", "Poindexter requires Dragnet")):
        result = subprocess.run(["make", "-n", f"PLUGINS={selection}"], cwd=ROOT,
                                capture_output=True, text=True, timeout=10)
        assert result.returncode != 0 and diagnostic in result.stderr, result
    print("Plugin source boundary and composition tests passed.", flush=True)


def profiles(compiler, mulib):
    # A real clean source tree with NO plugins proves the core build does not
    # merely leave disabled plugin references in an otherwise populated checkout.
    with tempfile.TemporaryDirectory(prefix="shylock-plugin-profiles-") as folder:
        root = Path(folder)
        for name in ("src", "tools", "include"):
            shutil.copytree(ROOT / name, root / name)
        for name in ("Makefile", "build.sh", "shylock"):
            shutil.copy2(ROOT / name, root / name)
        env = dict(os.environ)
        for key in ("SHY_BINARY", "SHY_NATIVE_PLUGINS", "SHY_EXTENSIONS", "SHY_PROVIDER_ROOT",
                    "SHY_SANDBOX", "SHY_FORMULA_SANDBOX", "MAKEFLAGS", "MFLAGS", "MAKEOVERRIDES", "MAKELEVEL"):
            env.pop(key, None)
        env["SHY_PROVIDER_ROOT"] = str(mulib / "library/elvi-scrape")

        def build(selection):
            print(f"Building isolated profile: {selection or '(no plugins)'}", flush=True)
            checked(["make", "-j4", f"PLUGINS={selection}", f"ON1XC={compiler}",
                     f"MULIB={mulib}", "PPX_TIMEOUT_MS=60000"], cwd=root, env=env)

        def run(*args, code=0, raw=False):
            result = subprocess.run([root / "shylock", *map(str, args)], cwd=root,
                                    env=env, capture_output=True, text=True, timeout=30)
            assert result.returncode == code, (args, result.returncode, result.stdout, result.stderr)
            return result.stdout if raw else json.loads(result.stdout) if result.stdout else None

        fixtures = ROOT / "tests/fixtures"

        def help_checks(expected):
            names = re.findall(r"^  ([a-z][a-z-]*) {2,}", run("help", raw=True), re.M)
            assert len(names) == len(set(names)) and set(names) == expected, names
            for name in names:
                assert run("help", name, raw=True).startswith(f"shylock {name}\n"), name
            for name in (CORE | DRAGNET | POINDEXTER) - expected:
                assert run("help", name, code=2) is None

        def save(name, value):
            path = root / name
            path.write_text(json.dumps(value))
            return path

        # Both extension entry points must validate the selected event vocabulary
        # before on_load can perform an effect, even for a replayed saved event.
        script = root / "extension.lua"
        script.write_text('return {api="shylock.extension/v1", '
                          'on_load=function() host.kv_set("loaded","yes") end, '
                          'on_event=function() end}')
        manifest = save("manifest.json", {"schema": "shylock.extension/v1", "name": "probe",
                        "script": script.name, "capabilities": ["kv.write"], "events": ["decision.created"]})
        extension = save("extension.json", {"schema": "shylock.extensions/v1", "extensions": [
            {"manifest": manifest.name, "grant": ["kv.write"], "storage": "events.db", "sandbox": "off"}]})
        payload = save("payload.json", {})

        def unavailable_events():
            storage = root / "events.db"
            assert not storage.exists()
            for event in ("worksheet.opened", "worksheet.saved", "observation.reviewed", "fixture.ready"):
                assert run("extension", extension, event, payload, code=2) is None
                batch = save("events.json", {"schema": "shylock.extension-events/v1",
                                            "events": [{"event": event, "payload": {}}]})
                assert run("extension-replay", extension, batch, code=2) is None
                assert not storage.exists()

        def core_checks():
            assert run("plugins") == []
            help_checks(CORE)
            help_text = run("help", raw=True)
            for command in ("discover", "snapshot", "flatten", "find-rule", "collect", "review", "ledger", "sheet", "tui", "gui"):
                assert not any(line.lstrip().startswith(command + " ") for line in help_text.splitlines()), command
                assert run(command, code=2) is None
                assert run("help", command, code=2) is None
                assert run(command, "--help", code=2) is None
            assert run("discover", "google", "lake", fixtures / "google.html", code=2) is None
            assert run("review", "unused.db", "unused.json", 0, "market", "reviewer", code=2) is None
            assert not (root / "unused.db").exists()
            assert run("estimate", fixtures / "series.json", 2)["reference_minor"] == 18000
            assert run("adjust", ROOT / "formulas/weekend.lua", 18000, 1, 1500, 10000, 30000, 100)["adjusted_minor"] == 20700
            unavailable_events()
            symbols = checked(["nm", "-g", root / "build/shylock"])
            for prefix in ("shy_dragnet", "shy_naive", "shy_collection", "shy_store", "shy_sheet", "shy_browser", "shy_openai"):
                assert prefix not in symbols, prefix
            libraries = checked(["ldd", root / "build/shylock"])
            for library in ("libssl", "libcrypto", "libSDL"):
                assert library not in libraries, libraries

        build("")
        core_checks()
        compiled = (root / "build/shylock").stat().st_mtime_ns
        build("")
        assert (root / "build/shylock").stat().st_mtime_ns == compiled
        # The generic registry and explicit-plugin extraction still work with
        # an independently compiled SDK plugin and no Dragnet implementation.
        library = root / "fixed-price.so"
        checked([*shlex.split(env.get("CC", "cc")), "-std=c11", "-Wall", "-Wextra",
                 "-Wpedantic", "-Werror", "-fPIC", "-shared",
                 *shlex.split(env.get("CFLAGS", "")), ROOT / "sdk/examples/fixed-price.c",
                 "-o", library])
        config = root / "native.json"
        config.write_text(json.dumps({"schema": "shylock.native-plugins/v1",
                                      "plugins": [{"path": library.name, "configuration": {}}]}))
        page = root / "page.html"
        page.write_text("EUR 1.00 per night")
        env["SHY_NATIVE_PLUGINS"] = str(config)
        assert [item["name"] for item in run("plugins")] == ["fixed-price"]
        assert run("extract", page, "https://example.org/", 1000, "", "", "fixed-price")["candidates"][0]["minor"] == 100
        env.pop("SHY_NATIVE_PLUGINS")

        # A new built-in contributes commands, native registration and events
        # through its own manifest. The copied engine and Makefile stay untouched.
        plugin = root / "plugins/probe"
        plugin.mkdir(parents=True)
        (plugin / "plugin.mk").write_text('''PLUGIN_ON1X += plugins/probe/cli.on1x
PLUGIN_NATIVE += plugins/probe/probe.c
PLUGIN_HEADERS += plugins/probe/probe.h
PLUGIN_INITIALIZERS += plugins/probe/probe.h:probe_init
PLUGIN_EVENTS += fixture.ready
''')
        (plugin / "cli.on1x").write_text('''fn probe_command(command:string,count:i64,a:string,b:string,c:string,d:string)->i64 {
  if command=="probe" && count==0 { println("probe handled"); return 7; }
  return -1;
}
fn probe_help(topic:string,color:bool)->bool {
  return help_entry(topic,color,"probe","probe","Probe plugin command.",
    "  No arguments.","  Test fixture.",
    "  ./shylock probe\\n  ./shylock probe\\n  ./shylock probe","Test fixture.");
}
''')
        (plugin / "probe.h").write_text("int probe_init(void);\n")
        (plugin / "probe.c").write_text('''#include "../../src/shylock.h"
#include "probe.h"
static int extract(const shy_capture *capture, mulib_json_t *items)
{ (void)capture; (void)items; return 1; }
static const shy_plugin probe = {SHY_EXTRACTOR_ABI, sizeof(shy_plugin), "probe", "probe/v1", extract};
int probe_init(void)
{ const shy_plugin *p=shy_plugin_find("probe"); return p==&probe || (!p && shy_plugin_register(&probe)); }
''')
        build("probe probe")
        help_checks(CORE | {"probe"})
        assert run("plugins") == [{"name": "probe", "version": "probe/v1", "abi": 2}]
        assert run("probe", code=7, raw=True) == "probe handled\n"
        assert len(re.findall(r"^  probe +Probe plugin command\.", run("help", raw=True), re.M)) == 1
        assert "Probe plugin command." in run("help", "probe", raw=True)
        assert "Probe plugin command." in run("probe", "--help", raw=True)
        manifest_data = json.loads(manifest.read_text())
        manifest_data["events"] = ["fixture.ready"]
        manifest.write_text(json.dumps(manifest_data))
        assert run("extension", extension, "fixture.ready", payload)["status"] == "ok"
        batch = save("events.json", {"schema": "shylock.extension-events/v1",
                                    "events": [{"event": "fixture.ready", "payload": {}}]})
        assert run("extension-replay", extension, batch)["status"] == "ok"
        (root / "events.db").unlink()
        manifest_data["events"] = ["decision.created"]
        manifest.write_text(json.dumps(manifest_data))

        shutil.copytree(ROOT / "plugins/dragnet", root / "plugins/dragnet")
        build("dragnet")
        help_checks(CORE | DRAGNET)
        assert [item["name"] for item in run("plugins")] == ["dragnet"]
        assert run("ledger", "unused.db", "market", code=2) is None
        discovery = run("discover", "google", "lake cottage", fixtures / "google.html")
        assert discovery["hits"][0]["url"] == "https://example.org/cottage"
        snapshot = root / "snapshot.json"
        snapshot.write_text(json.dumps(run("snapshot", "https://example.org/cottage", 1760000000,
                                          fixtures / "naive-rentals.html")))
        artifact = run("find-rule", snapshot, root / "plugins/dragnet/rules/rental.json", "rental_price")
        replay = root / "found.json"
        replay.write_text(json.dumps(artifact))
        assert run("find-rule-replay", replay) == artifact
        symbols = checked(["nm", "-g", root / "build/shylock"])
        assert "shy_dragnet" in symbols and "shy_store_open" not in symbols
        assert "libSDL" not in checked(["ldd", root / "build/shylock"])
        assert run("probe", code=2) is None
        unavailable_events()
        discovery_path = save("discovery.json", discovery)
        job = save("collection-job.json", {"schema": "shylock.collection-job/v1", "target": "rental_price",
                   "sources": [{"discovery": discovery_path.name, "hit": 0, "snapshot": snapshot.name,
                                "rule": str(root / "plugins/dragnet/rules/rental.json")}]})
        collection = run("collect", job)
        assert run("collect-replay", save("collection.json", collection)) == collection
        # Switching profiles in the same BUILD must remove commands, registry
        # entries, native symbols and transitive libraries rather than cache them.
        build("")
        core_checks()
    print("Clean plugin-free, new built-in, standalone Dragnet, SDK, event and profile-switch tests passed.")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--profiles", action="store_true")
    parser.add_argument("--on1xc", type=Path, default=ROOT / "build/on1xc")
    parser.add_argument("--mulib", type=Path, default=ROOT / "third_party/mulib")
    args = parser.parse_args()
    layout()
    if args.profiles:
        profiles(args.on1xc.resolve(), args.mulib.resolve())
