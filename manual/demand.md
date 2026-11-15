# Demand from explicit booking outcomes

```sh
./shylock demand tests/fixtures/booking-outcomes.json 1800000000 10 2 > demand.json
./shylock optimize demand.json 10000 20000 100 2000
```

`demand BOOKING_OUTCOMES_JSON AS_OF CAPACITY MIN_SAMPLES` creates a replayable
`shylock.demand/v1` artifact. `AS_OF` is a Unix-seconds information cutoff;
`CAPACITY` is the number of comparable listing-nights to predict, not a price.
The example forecasts expected booked nights out of ten available nights.

## Source contract

The input is a closed, versioned JSON object:

```json
{
  "schema": "shylock.booking-outcomes/v1",
  "cohort": "cottages/2-guests",
  "currency": "EUR",
  "scale": 2,
  "basis": "nightly",
  "context": {"guests": 2, "taxes": "included", "fees": "excluded"},
  "rows": [
    {
      "opportunity_id": "host:night:123",
      "listing_id": "host:cottage:1",
      "stay_date": "2026-10-25",
      "rate_minor": 18000,
      "outcome": "booked",
      "finalized_at": 1793491200,
      "source_ref": "host-export:receipt-123"
    }
  ]
}
```

Each row asserts **one offered listing-night at one exact nightly rate** and
its final `booked` or `not_booked` result. A night removed from sale, owner-blocked,
cancelled without a known final result, or offered at multiple rates cannot be
encoded as a single known-rate opportunity. Supply these outcomes from explicit
booking/offer records, not calendar scraping. The listing-night must have ended
before `finalized_at` (UTC). Valid dates start at 1970-01-01. Later finalizations
are excluded by `AS_OF`; malformed rows still fail validation, even after the
cutoff. Only eligible rows participate in duplicate/conflict checks.

The caller explicitly selects the cohort and asserts its comparability across
listings and dates. Currency scale must match Shylock's supported money scales.
All rows share the declared guest count (1..64), tax/fee scope and nightly basis.
Labels must contain 1..256 NUL-free bytes. Amounts are checked integer minor
units in 0..1,000,000,000,000. Wrong/missing/unknown fields or JSON types fail.
`source_ref` is an audit label, **not** an independently verified capture,
authenticated booking record or permission grant.

## Estimator and duplicate policy

`booked-share/v1` groups the eligible distinct listing-nights by exact rate:

```text
expected_units = CAPACITY * booked_units / offered_units
```

There is no interpolation, smoothing, monotonic fit or extrapolation to unobserved
rates. Every observed eligible rate needs at least `MIN_SAMPLES` distinct nights;
otherwise the whole export fails, without partial JSON. Zero bookings is a valid
zero-demand scenario. Bounds: 1..4096 source rows, 1..4096 minimum samples,
1..1,000,000 capacity, and nonnegative timestamps through year 9999. Inputs are
bounded to 16 MiB, and the serialized source must fit 2 MiB to stay inside the
upstream On1x text/buffer limits. Grouping is bounded in-memory work, not a
large-data engine.

Repeated IDs and matching listing/date/rate/outcome claims corroborate rather
than overweight counts. Alternate opportunity IDs for the same listing-night
also count once. All source rows remain attached, including corroboration and
future finalizations. Reusing an opportunity ID for another night, or disagreeing
on a listing-night's rate/outcome, returns exit 3. Publication order does not
silently resolve conflicts. Other invalid inputs return exit 2; success is 0.

## Replay and optimization

The artifact retains the complete source object, a canonical `source_id`, cutoff,
capacity, minimum samples, sorted scenarios with exact integer counts, and a
canonical `demand_id`. IDs use the same canonical JSON convention as cohort IDs:
sorted object keys, original array order and pinned MuLIB numeric serialization.
They identify parsed content, not original file bytes. Object key order and
whitespace do not change IDs; source array reordering may change them.

Before optimization, On1x validates the embedded source and reconstructs the
entire artifact. Editing counts, expected units, policy names or IDs alone fails,
even if a caller substitutes a hash. Optimization uses the same in-memory
document that was checked, with no file reread between replay and use. A
self-consistent replacement source is still a new caller assertion, not a signed
attestation. The result keeps the full artifact in `demand_lineage` and marks
`demand_origin` as `empirical_booking_outcomes`. MuLIB `priceopt` still owns
expected-profit scoring; floor/cap/tick/cost constraints remain unchanged.

Legacy arrays of `{ "rate_minor": ..., "expected_units": ... }` remain accepted
by `optimize`, with origin `externally_supplied` and no invented booking lineage.

## Limitations

Historical booking shares are descriptive, not causal demand estimates. Rates,
seasonality, lead times, listing quality, exposure and acquisition channels can
confound comparisons. The caller must supply comparable, fully offered nights
and truthful finalization times. This implementation does not infer bookings
from asking-price evidence or unavailability; verify a separate booking feed
before automating acquisition. It provides no confidence interval, calibration
guarantee, authenticated booking import or unattended rate publication.

## On1x implementation

`src/demand.on1x` owns admission, cutoff selection, corroboration/conflict policy,
per-rate counting and replay orchestration. It uses the upstream `List`, `Date`
and S-expression utilities. `src/demand_macros.on1x` contains hygienic,
single-evaluation checks and JSON-member emission.

`src/ppx/fields.on1x` is a native On1x PPX using `std.ppx`, not a Python code
generator. `@fields("closed", section, index, "name:type", ...)` generates
closed-object checks through the scalar document ABI. It accepts 1..32 distinct
literal fields of type `string`, `integer`, `array`, `object`, `boolean` or `null`; fresh lambda
parameters evaluate section/index exactly once. Ordinary compiler type and
ownership checks still run after expansion. This is not typed deriving or a
new security boundary.

GNU Make bootstraps the PPX before compiling the application, supplies the
upstream PPX schema explicitly, and sets a bounded 30-second transform deadline
(override `PPX_TIMEOUT_MS` for slower build hosts). No source-authorized PPX or
upstream changes are required. `src/document.c` only owns JSON/session lifetimes,
scalar lookups, canonical sealing, quoting, rational-to-JSON-number serialization
and the borrowed call into the existing optimizer. Its JSON/quote scratch is
copied into On1x buffers immediately; document strings never survive session close.

`make test` includes offline demand/replay/optimization tests, macro hygiene and
single-evaluation checks, malformed-PPX rejection, post-transform type checks and
allocator-failure cleanup tests. Fault injection uses GNU/LLD linker `--wrap`
support on the project's Linux toolchain.
