"""Negative compiler tests for Shylock's native On1x PPX."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile

compiler, processor, upstream, executable = map(lambda p: Path(p).resolve(), sys.argv[1:])
env = dict(os.environ, ON1X_PPX_SCHEMA=str(upstream / "grammar/PPX.schema.json"))
positive = subprocess.run([str(executable)], env=env, capture_output=True, text=True, timeout=30)
assert positive.returncode == 0 and positive.stdout == "section\nindex\ncondition\nvalue\nexpected failure\n", positive
with tempfile.TemporaryDirectory(prefix="shylock-ppx-") as directory:
    folder = Path(directory)
    source = folder / "bad.on1x"
    for expression, diagnostic in (
        ('@fields("unknown", "", -1, "x:string")', "expected closed"),
        ('@fields("closed", "", -1)', "expected closed"),
        ('@fields("closed", "", -1, "x:float")', "unsupported field type"),
        ('@fields("closed", "", -1, "x:string", "x:integer")', "duplicate field"),
        ('@fields("closed", "", -1, "broken")', "expected key:type"),
        ('@fields("closed", "", -1, 42)', "descriptors must be string literals"),
        ('@fields("closed", "", -1, ":string")', "expected key:type"),
        ('@fields("closed", "", -1, ' + ", ".join(f'"k{i}:string"' for i in range(33)) + ')', "1..32 fields"),
        # Reconstruction must still undergo ordinary On1x type checking.
        ('@fields("closed", 42, -1, "x:string")', "expected string"),
    ):
        source.write_text('import "' + str(Path(__file__).resolve().parents[1] / "src/ppx/fields_runtime.on1x") + '";\n' + '''
extern fn shy_document_has(s:string,i:i64,k:string,t:string)->i64;
extern fn shy_document_size(s:string,i:i64,k:string)->i64;
fn main() { println(''' + expression + '''); }
''')
        proc = subprocess.run([str(compiler), "--emit-c", str(source), "--stdlib", str(upstream / "stdlib"),
                               "--ppx", "fields", str(processor), "-o", str(folder / "bad.c")],
                              env=env, capture_output=True, text=True, timeout=30)
        assert proc.returncode != 0 and diagnostic in proc.stderr, (expression, proc.stdout, proc.stderr)

print("On1x macro/PPX hygiene, single evaluation, schema rejection and post-transform type checks passed.")
