"""Generate the same embedded PPX schema as the upstream bootstrap build."""
import pathlib
import sys

root, output = map(pathlib.Path, sys.argv[1:])
template = (root / "cmake/ppx_schema.hpp.in").read_text()
schema = (root / "grammar/PPX.schema.json").read_text()
output.parent.mkdir(parents=True, exist_ok=True)
output.write_text(template.replace("@ON1X_SCHEMA_TEXT@", schema))
