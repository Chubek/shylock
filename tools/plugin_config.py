#!/usr/bin/env python3
"""Compose enabled built-ins without teaching the engine about plugin names."""
import argparse
import json
import os
from pathlib import Path
import tempfile


def write_changed(path, text):
    path.parent.mkdir(parents=True, exist_ok=True)
    if path.exists() and path.read_text() == text:
        return
    with tempfile.NamedTemporaryFile(mode="w", dir=path.parent, delete=False) as stream:
        stream.write(text)
        staged = Path(stream.name)
    try:
        os.replace(staged, path)
    finally:
        staged.unlink(missing_ok=True)


def compose(build, core, plugins, initializers, events):
    entry = [f"import {json.dumps(str(core.resolve()))};"]
    for plugin in plugins:
        entry.append(f"import {json.dumps(str((Path('plugins') / plugin / 'cli.on1x').resolve()))};")
    entry += ["", "fn builtin_plugin_command(command:string,count:i64,a:string,b:string,c:string,d:string)->i64 {"]
    for index, plugin in enumerate(plugins):
        entry += [f"  let status{index}={plugin}_command(command,count,a,b,c,d);",
                  f"  if status{index}>=0 {{ return status{index}; }}"]
    entry += ["  return -1;", "}", "fn builtin_plugin_help() {"]
    entry += [f"  {plugin}_help();" for plugin in plugins]
    entry += ["}", ""]

    native = [f'#include {json.dumps(str((core.parent / "shylock.h").resolve()))}',
              f'#include {json.dumps(str((core.parent / "extensions.h").resolve()))}',
              '#include <string.h>']
    calls = []
    for initializer in initializers:
        header, function = initializer.split(":", 1)
        native.append(f"#include {json.dumps(str(Path(header).resolve()))}")
        calls.append(f"{function}()")
    native += ["", "int shy_plugins_init(void)", "{",
               f"    return {' && '.join(calls) if calls else '1'};", "}", ""]
    native += ["int64_t shy_builtin_event_known(const char *event)", "{", "    (void)event;"]
    native += [f"    if (event && !strcmp(event, {json.dumps(event)})) return 1;"
               for event in sorted(set(events))]
    native += ["    return 0;", "}", ""]
    write_changed(build / "main.on1x", "\n".join(entry))
    write_changed(build / "builtin_plugins.c", "\n".join(native))


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("build", type=Path)
    parser.add_argument("--core", type=Path, default=Path("src/main.on1x"))
    parser.add_argument("--plugin", action="append", default=[])
    parser.add_argument("--init", action="append", default=[])
    parser.add_argument("--event", action="append", default=[])
    args = parser.parse_args()
    compose(args.build, args.core, args.plugin, args.init, args.event)
