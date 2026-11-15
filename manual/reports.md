# Print Report: On1x templates and MuLIB LaTeX

```sh
./shylock recommend tests/fixtures/series.json 1 formulas/weekend.lua \
  1 1000 10000 30000 100 > decision.json
./shylock print-report reports/decision.json decision.json > decision.tex
./shylock estimate tests/fixtures/series.json 2 > forecast.json
./shylock print-report reports/forecast.json forecast.json > forecast.tex
./shylock formula-test formulas/weekend.lua formulas/cases/weekend.json > checks.json
./shylock print-report reports/formula-tests.json checks.json > checks.tex
```

`print-report TEMPLATE_JSON DATA_JSON` prints one complete LaTeX article to stdout.
Diagnostics and opt-in extension audits stay on stderr. Success is exit 0;
invalid inputs, unresolved substitutions, kernel/allocation/limit failures or
output I/O failures return 2. Validation/render failures print no partial article.
An output-stream failure can leave already-written bytes; stdout cannot be rolled
back. Redirect to a new staging file before replacing a published report.

MuLIB's `mulib-latex.h` constructs **source**, not a PDF or printer job. Shylock
does not invoke TeX, run shell escapes, load template packages or contact a
printer. Explicitly use your installed toolchain and printer separately, e.g.:

```sh
pdflatex -no-shell-escape -halt-on-error decision.tex
```

No TeX installation is required to generate reports. The kernel emits `article`;
font/Unicode coverage, pagination and engine compatibility belong to the external
toolchain, not a Shylock PDF guarantee.

## Template contract

Templates are closed JSON objects, not executable `.tex` or Lua programs:

```json
{
  "schema": "shylock.report-template/v1",
  "title": "Price decision for {{/listing/name}}",
  "blocks": [
    {"kind": "section", "level": 1, "text": "Recommendation"},
    {"kind": "paragraph", "text": "Rate: {{/adjusted_minor}} {{/currency}} minor units."},
    {"kind": "paragraph", "text": "Input: {{$data_id}}. Template: {{$template_id}}."}
  ]
}
```

Blocks are ordered `paragraph` objects with `text`, or `section` objects with
`text` and integer `level` 1..3 (section/subsection/subsubsection). Unknown keys,
kinds, types, levels and versions fail. There is no raw-LaTeX insertion,
conditionals, iteration, formula execution, module loading or environment access.

Literal and substituted text passes through MuLIB `mulib_tex_escape`: backslash,
braces, `#`, `$`, `%`, `&`, `_`, `^` and `~` are escaped. `\input{private}` is
printed as text, never installed as a command. Invalid UTF-8, embedded NULs and
unsupported control bytes fail.

## Substitutions and provenance

`{{/path/to/field}}` is a strict RFC 6901 JSON Pointer into the data document:

- `~0` decodes to `~`, `~1` to `/`; malformed escapes fail.
- `/cases/0/name` indexes an array. Indexes are decimal, nonnegative, at most
  seven digits, without leading zeros except `0`. `-` is not an index alias.
- `/` selects an object's empty-string key. Names remain case-sensitive.
- Only strings, numbers, Booleans and null can be substituted. Null prints
  `null`, not an invented price. Containers and missing fields fail.
- Marker contents are exact: no whitespace trimming, fallback or currency guessing.
- Inserted values are never recursively interpreted as another placeholder.

`{{$data_id}}` and `{{$template_id}}` are canonical BLAKE3 identifiers using the
existing sorted-object-key convention, original array order and pinned MuLIB
numeric serialization. They identify parsed content, not original file bytes,
signed approval or listing truth. Arbitrary report data is not authenticated or
admitted into pricing just because it can be rendered. Use audited CLI artifacts
and a template appropriate to their schema. Supplied templates label prices as
minor units rather than guessing currency from legacy series.

## Bounds, ownership and events

Templates allow 1..128 blocks and 8192-byte literal titles/text fields. Expanded
titles are at most 8192 bytes; paragraphs/headings at most 65536 bytes, with at
most 256 substitutions per field. Pointers are at most 1024 bytes and substituted
scalars at most 8192 bytes. Final LaTeX is bounded to 2 MiB. JSON reads retain the
existing 16-MiB file bound and parser/depth/allocation limits. Limits do not truncate.

`src/report.on1x` owns template admission, layout, JSON Pointer decoding/traversal,
substitution and completion-event orchestration. It uses the On1x `@fields` PPX
under `src/ppx` and upstream List/S-expression utilities. Template strings are
copied before closing their JSON owner; data views live only in their session.
`src/latex.c` only adapts MuLIB begin/append/finish/free, digesting and stdout
publication. No C template engine is implemented.

After successful printing, opt-in extensions receive `report.created` metadata:
`schema: shylock.report/v1`, `format: latex`, `data_id`, `template_id`,
`latex_blake3` and block count. It contains no report body. Hooks drain after
document/data owners close, cannot change emitted TeX, and do not replace a
successful report if they fail.

`make test` covers supplied templates, pointer/scalar selection, stable IDs,
escaping, nonrecursive substitution, malformed/oversized/control/NUL input,
completion hooks, native lifetimes and injected allocation failures.

The bundled report templates, including evaluated forecasts and pricing decisions,
were also smoke-tested with pdfLaTeX and `-no-shell-escape`. This is a separate
local check; `make test` does not require a TeX installation.
