# Mune formulas and case suites

Formulas are pure calculations, not effectful extensions. The compatible
`adjust_rate/v1` contract remains:

```lua
return function(reference_minor, weekend, premium_bp)
  if not weekend then return reference_minor end
  return reference_minor + (reference_minor * premium_bp + 5000) // 10000
end
```

Arguments are integer minor units, a Boolean weekend flag and integer basis
points. One returned integer must be in 0..1,000,000,000,000. On1x rounds to the
nearest tick (ties upward), then clamps to the inward-rounded floor/cap grid.
Currency/scale are unchanged; a formula cannot infer or convert them.

```sh
./shylock adjust formulas/weekend.lua 18000 1 1500 10000 30000 100
./shylock adjust formulas/weekday-discount.lua 18000 0 1500 10000 30000 100
./shylock adjust formulas/capped-weekend.lua 18000 1 5000 10000 30000 100
```

These produce 20700, 15300 and 19800. `weekday-discount.lua` interprets the basis
points as a weekday discount; `capped-weekend.lua` limits its premium to 10%.
They are example business policies, not claims of economically optimal prices.
`rounded.lua` demonstrates deterministic Mune math operations.

## Table-driven checks

```sh
./shylock formula-test formulas/weekend.lua formulas/cases/weekend.json
./shylock formula-test formulas/weekday-discount.lua formulas/cases/weekday-discount.json
./shylock formula-test formulas/capped-weekend.lua formulas/cases/capped-weekend.json
SHY_FORMULA_SANDBOX=process ./shylock formula-test formulas/weekend.lua formulas/cases/weekend.json
```

Suites are closed `shylock.formula-cases/v1` objects:

```json
{
  "schema": "shylock.formula-cases/v1",
  "cases": [{
    "name": "weekend premium", "reference_minor": 18000, "weekend": true,
    "premium_bp": 1500, "floor_minor": 10000, "cap_minor": 30000,
    "tick_minor": 100, "expected_minor": 20700
  }]
}
```

Provide 1..128 uniquely named cases, names of 1..128 bytes, Boolean weekend flags
and the same valid constraints as `adjust`, including at least one feasible tick.
Unknown fields/types and invalid constraints fail **before any evaluation**, with
exit 2 and no result JSON. `expected_minor` means the final constrained rate,
not the raw Lua proposal. Null expects a Mune parse/runtime/return-contract error;
a missing/unreadable formula does not count as a passing expected error. Expected
errors still produce ordinary diagnostics on stderr.

`shylock.formula-tests/v1` retains each complete input, proposal, adjusted value,
diagnostic, pass/fail status, source digest and isolation backend/profile, plus
contract, case-suite ID, runtime revision/API/limits and totals. `test_id` identifies
the result. Matching cases return 0; mismatches/unexpected errors return the full
result with exit 2. A passing suite requires the same nonempty source digest
throughout. Each case reads/evaluates a fresh state. This detects, but does not
lock out, concurrent source changes; use an immutable formula file when testing.

`src/formula.on1x` shares admission and grid functions between `adjust`,
`recommend` and `formula-test`. A hygienic macro generates repeated schema
carriers, expanded by the native On1x PPX in `src/ppx/fields.on1x`. Mune parsing,
typed value adaptation and VM ownership remain in the C adapter using
`third_party/mulib/include/mulib-mune.h`.

## Restricted runtime and failures

Each evaluation has a fresh state with deterministic `math`, but no random
generators, `host`, extension hooks, module/file/process/network/debug authority
or `_G`. Globals cannot cross evaluations. Limits remain 16-KiB source, bounded
parsing, 1-MiB runtime heap, 50000 steps, call depth 32 and 32 result values.
Native parsing memory is separate from the VM heap bound.

`adjust`/`recommend` keep their existing error semantics: exit 2, null adjusted
rate, reference and preceding forecast/cohort lineage preserved. Extensions may
observe the completed error decision but cannot inject privileges or replace it.
The default backend is for trusted authors. The optional [Linux sandbox](sandbox.md)
fails closed if workers are unavailable, never retrying formulas in-process.

Use `reports/formula-tests.json` with [Print Report](reports.md) for a typeset
summary. `make test` runs the examples, errors, fresh-state cases, invalid inputs
and process/in-process parity checks.
