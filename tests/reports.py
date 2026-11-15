"""On1x template/layout/JSON Pointer policy using MuLIB's escaped LaTeX kernel."""
import copy
import json
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def run(*args, code=0, config=None):
    env = dict(os.environ)
    env.pop("SHY_EXTENSIONS", None)
    env.pop("SHY_FORMULA_SANDBOX", None)
    env["SHY_SANDBOX"] = "off"
    if config:
        env["SHY_EXTENSIONS"] = str(config)
    proc = subprocess.run([str(ROOT / "shylock"), *map(str, args)], cwd=ROOT,
                          env=env, text=True, capture_output=True, timeout=30)
    assert proc.returncode == code, (args, proc.returncode, proc.stdout, proc.stderr)
    audits = [json.loads(line) for line in proc.stderr.splitlines() if line.startswith("{")]
    if code:
        assert proc.stdout == "", proc.stdout
    return proc.stdout, audits


with tempfile.TemporaryDirectory(prefix="shylock-reports-") as directory:
    folder = Path(directory)
    template, data = folder / "template.json", folder / "data.json"
    decision, _ = run("adjust", "formulas/weekend.lua", 18000, 1, 1500, 10000, 30000, 100)
    data.write_text(decision)
    tex, _ = run("print-report", ROOT / "reports/decision.json", data)
    assert tex.startswith("\\documentclass{article}\n") and tex.endswith("\\end{document}\n")
    assert "\\title{Shylock price decision}" in tex and "Adjusted: 20700" in tex
    assert tex == run("print-report", ROOT / "reports/decision.json", data)[0]
    data.write_text(json.dumps(json.loads(decision), sort_keys=True, indent=2))
    assert tex == run("print-report", ROOT / "reports/decision.json", data)[0]

    for command, args, report in (
        ("estimate", ["tests/fixtures/series.json", 1], "forecast"),
        ("formula-test", ["formulas/weekend.lua", "formulas/cases/weekend.json"], "formula-tests"),
    ):
        output, _ = run(command, *args)
        data.write_text(output)
        assert "\\begin{document}" in run("print-report", ROOT / f"reports/{report}.json", data)[0]

    value = r"x_1 & 5% # $ { } ^ ~ \input{/etc/passwd}"
    source = {"title": "A&B", "value": value, "a/b": {"~key": [42, True, None, 1.25]}, "": "empty-key"}
    data.write_text(json.dumps(source))
    layout = {"schema": "shylock.report-template/v1", "title": "{{/title}}", "blocks": [
        {"kind": "section", "level": 1, "text": "Escaped {{/value}}"},
        {"kind": "section", "level": 2, "text": "Nested fields"},
        {"kind": "section", "level": 3, "text": "Scalar values"},
        {"kind": "paragraph", "text": "{{/a~1b/~0key/0}} {{/a~1b/~0key/1}} {{/a~1b/~0key/2}} {{/a~1b/~0key/3}} {{/}}"},
        {"kind": "paragraph", "text": "{{$data_id}} {{$template_id}}"},
    ]}
    template.write_text(json.dumps(layout))
    rendered, _ = run("print-report", template, data)
    assert "\\title{A\\&B}" in rendered
    assert r"x\_1 \& 5\% \# \$ \{ \} \textasciicircum{} \textasciitilde{} \textbackslash{}input\{/etc/passwd\}" in rendered
    assert "42 true null 1.25 empty-key" in rendered
    assert "\\subsection{Nested fields}" in rendered and "\\subsubsection{Scalar values}" in rendered
    assert r"\input{/etc/passwd}" not in rendered

    # Values containing marker syntax are not evaluated recursively.
    data.write_text(json.dumps(dict(source, title="{{/missing}}")))
    assert r"\title{\{\{/missing\}\}}" in run("print-report", template, data)[0]
    data.write_text(json.dumps(source))
    for placeholder in ("{{/missing}}", "{{/a~1b}}", "{{/a~1b/~0key}}", "{{/a~1b/~0key/00}}",
                        "{{/a~1b/~0key/-}}", "{{/a~1b/~0key/99999999}}", "{{/bad~2}}", "{{}}", "{{/title"):
        bad = copy.deepcopy(layout)
        bad["blocks"][-1]["text"] = placeholder  # Earlier blocks must not leak partial output.
        template.write_text(json.dumps(bad))
        run("print-report", template, data, code=2)
    for change in ({"kind": "raw-tex"}, {"level": 4}, {"level": True}, {"extra": 1}):
        bad = copy.deepcopy(layout)
        bad["blocks"][0].update(change)
        template.write_text(json.dumps(bad))
        run("print-report", template, data, code=2)
    for change in ({"schema": "wrong/v1"}, {"title": ""}, {"extra": 1}, {"blocks": []},
                   {"blocks": layout["blocks"] * 26}, {"title": "x" * 8193}):
        template.write_text(json.dumps(dict(layout, **change)))
        run("print-report", template, data, code=2)
    for text in ("{{/title}}" * 257, "x" * 8193, "invalid\x01text"):
        bad = copy.deepcopy(layout)
        bad["blocks"][-1]["text"] = text
        template.write_text(json.dumps(bad))
        run("print-report", template, data, code=2)
    template.write_text(json.dumps(layout))
    for value in ("x" * 8193, "a\x00b", "a\x01b"):
        data.write_text(json.dumps(dict(source, title=value)))
        run("print-report", template, data, code=2)
    data.write_text(json.dumps(source))
    for raw in (b"{", b"\xff"):
        template.write_bytes(raw)
        run("print-report", template, data, code=2)
    run("print-report", folder / "missing", data, code=2)
    template.write_text(json.dumps(layout))
    run("print-report", template, folder / "missing", code=2)

    # Completion events contain content IDs; they neither replace the TeX output
    # nor expose printing, TeX execution or formula capabilities to an extension.
    config = folder / "monitor.json"
    config.write_text(json.dumps({"schema": "shylock.extensions/v1", "extensions": [{
        "manifest": str(ROOT / "extensions/monitor/manifest.json"),
        "grant": ["log", "kv.read", "kv.write"], "storage": "report-state.db"}]}))
    plain, _ = run("print-report", template, data)
    with_hooks, audits = run("print-report", template, data, config=config)
    assert with_hooks == plain
    assert [row["phase"] for row in audits] == ["on_load", "report.created", "on_unload"]
    assert audits[1]["logs"] == ["processed=1 formula_errors=0 session=1"]
    repeated, again = run("print-report", template, data, config=config)
    assert repeated == plain and again[1]["event_id"] == audits[1]["event_id"]
    assert again[1]["logs"] == ["duplicate report.created session=1"]
    _, demand_audits = run("demand", "tests/fixtures/booking-outcomes.json", 1800000000, 10, 2, config=config)
    assert [row["phase"] for row in demand_audits] == ["on_load", "demand.created", "on_unload"]

    script = folder / "broken.lua"
    script.write_text('return {api="shylock.extension/v1",on_event=function() error("independent hook failure") end}')
    manifest = folder / "broken.json"
    manifest.write_text(json.dumps({"schema": "shylock.extension/v1", "name": "broken",
        "script": script.name, "capabilities": [], "events": ["report.created"]}))
    config.write_text(json.dumps({"schema": "shylock.extensions/v1", "extensions": [{
        "manifest": manifest.name, "grant": []}]}))
    preserved, audits = run("print-report", template, data, config=config)
    assert preserved == plain and audits[0]["status"] == "error"

    # The metadata digest identifies the actual printed bytes, not the input JSON.
    script.write_text('''return {api="shylock.extension/v1",on_event=function()
assert(host.get("format")=="latex" and host.get("body")==nil)
assert(#host.get("data_id")==64 and #host.get("template_id")==64)
host.log(host.get("latex_blake3")) end}''')
    manifest.write_text(json.dumps({"schema": "shylock.extension/v1", "name": "metadata",
        "script": script.name, "capabilities": ["log"], "events": ["report.created"]}))
    config.write_text(json.dumps({"schema": "shylock.extensions/v1", "extensions": [{
        "manifest": manifest.name, "grant": ["log"]}]}))
    checked, audits = run("print-report", template, data, config=config)
    printed = folder / "printed.tex"
    printed.write_text(checked)
    captured, _ = run("extract", printed, "https://example.org/report", 1000)
    assert audits[0]["logs"] == [json.loads(captured)["blake3"]]

print("On1x report templates, JSON Pointers, MuLIB LaTeX escaping, limits and completion hooks passed.")
