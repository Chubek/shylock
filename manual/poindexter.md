# Poindexter: evidence, review and cohorts

Poindexter is Shylock's local evidence ledger, not approved accounting software.
For collection-aware review and direct forecast/pricing from current review heads,
see [Dragnet pricing integration](dragnet-pricing.md).
It uses MuLIB KVDB, now retested with the rest of Shylock at revision
`eeb6de585303e1ea0dfd8bf2260328289b04587c`.
On1x owns admission, review orchestration and cohort comparability. C11 adapters
own JSON trees, immutable byte snapshots and database handles. An exclusive
advisory writer lock covers database open, read/modify, commit and close.

## Complete offline workflow

From the repository root, after `./build.sh -j4`:

```sh
./shylock extract tests/fixtures/rental.html https://example.org/cottage 1000 > first.json
./shylock review prices.db first.json 1 cottages-eur alice \
  tests/fixtures/rental.html tests/fixtures/context.json
./shylock extract tests/fixtures/rental.html https://example.org/cottage 1060 > second.json
./shylock review prices.db second.json 1 cottages-eur alice \
  tests/fixtures/rental.html tests/fixtures/context.json
./shylock ledger prices.db cottages-eur
./shylock cohort prices.db cottages-eur EUR 1000 60 2 > cohort.json
./shylock recommend cohort.json 1 formulas/weekend.lua 1 1000 10000 30000 100
```

The two fixture capture times represent two sampling buckets. No network or
model is required. Candidate indexes are zero-based; index 1 in `rental.html`
is its explicit EUR nightly quote. Actual acquisition times must be supplied
truthfully by the caller; Shylock cannot authenticate a supplied timestamp.

## Capture and evidence contract

`extract` emits `shylock.capture/v2` with status `extracted`:

- `blake3` identifies the original HTML bytes. The same bytes may be captured at
  different times.
- `capture_id` additionally identifies source URL and capture time.
- Each candidate has a stable `evidence_id`, extractor version, method, byte
  range and `span_blake3`. JSON-LD also has a script-relative JSON Pointer.
- Exact site-rule and imported-model source strings are retained for replay.
  Site-rule candidates retain the digest of the original rule bytes.
- Amounts retain their original text, checked integer `minor`, ISO currency,
  and explicit `scale`. Unknowns are null/empty, not inferred from a symbol.
  `unresolved_fields` lists fields still needing interpretation.
- Listing identity, stay dates, guest count, taxes and fees remain independent
  fields. A nightly rate, stay total, cleaning fee and qualified “from” price
  remain distinct evidence, not interchangeable prices.

Identifiers use recursively sorted JSON object keys, original array order and
the pinned MuLIB JSON numeric serialization. They do not depend on file paths
or candidate indexes. Schema/version changes may require an explicit migration.

Supplying `PAGE_HTML` to review verifies the entire extraction by deterministic
replay against those exact bytes. Forged amounts, offsets, hashes, source URLs,
capture times or replay inputs fail. Verified review stores the original HTML
under its digest and the full extraction under `extraction_id`, including all
competing/unreviewed candidates. These are immutable records within the same
atomic KVDB commit as the review. `replay DATABASE EXTRACTION_ID` reconstructs
and verifies an extraction using only those stored inputs.

Replay verifies provenance and extraction consistency, **not listing truth**,
capture-time authenticity or the correctness of a model's interpretation.
Reviewers still decide which evidence is meaningful and comparable. Keep the
original HTML files for independent audits and subsequent review commands.

## Review context and admission

```text
review DATABASE EXTRACTION_JSON INDEX COHORT REVIEWER [PAGE_HTML [CONTEXT_JSON]]
reject DATABASE EXTRACTION_JSON INDEX COHORT REVIEWER REASON [PAGE_HTML]
```

For verified acceptance, provide context like:

```json
{
  "listing_id": "example:cottage-1",
  "stay_start": "2026-12-04",
  "stay_end": "2026-12-07",
  "guests": 2,
  "basis": "nightly",
  "taxes": "included",
  "fees": "excluded"
}
```

Verified nightly admission requires a checked nonnegative amount at the known
currency scale, an unspecified qualifier, a nonempty listing identity, valid
ordered Gregorian stay dates, 1..64 guests, and explicitly included/excluded
taxes and fees. Unknowns remain evidence but cannot silently enter a verified
cohort. Empty or misspelled context fields and wrong JSON types fail.

Context may resolve an unknown basis or missing currency. A supplied currency
is parsed using MuLIB money, never by floating-point multiplication. Known
currencies, explicit stay-total/fee bases and “from” qualifiers cannot be
rewritten into contradictory nightly quotes. Model suggestions require explicit
review of both basis and qualifier. Every original candidate remains attached
to its review as `evidence`, independently of the review's resolved fields.
The shared On1x admission gate preserves known source currency, basis and
qualifier for all extractor families, including compatibility input. Unknowns
can be resolved explicitly; original/discounted/from claims cannot be laundered
into unqualified nightly observations through context.

Supported currency scales currently are:

- Scale 2: EUR, USD, GBP, CAD, AUD, CHF, SEK, NOK, DKK, NZD, PLN.
- Scale 0: JPY, KRW.
- Scale 3: KWD, BHD.

`reject` can reject incomplete evidence directly; it never requires accepting
an unresolved claim first. Rejected evidence and its provenance remain stored.
Reviewer names are audit labels, not authenticated identities or access control.

## Append-only review revisions

```text
ledger DATABASE COHORT
history DATABASE OBSERVATION_ID
revise DATABASE OBSERVATION_ID EXPECTED_REVIEW_ID accepted|rejected REVIEWER REASON [CONTEXT_JSON]
replay DATABASE EXTRACTION_ID
```

`ledger` shows the current review for each observation, including rejected rows.
`shylock.observation/v2` rows include `observation_id`, immutable `review_id`,
`parent_review_id`, revision number, decision, reviewer, reason, resolved
context, original evidence and capture/extraction provenance. `history` returns
all revisions oldest first. Accepting/rejecting an observation appends a new
review; it does not mutate or delete any published review or capture.

Use the current IDs from `ledger` when revising. The expected review ID is an
optimistic concurrency guard. A stale parent with different changes returns
exit 3. Repeating the exact same review/revision succeeds without adding a row,
including a retry after uncertain commit durability. Changing an initial review
requires `revise`, not silently re-importing over an existing review. Reaccepting
a rejected observation runs admission again; context corrections are auditable.

## Comparable cohort exports

```text
cohort DATABASE COHORT CURRENCY START STEP COUNT
series DATABASE COHORT CURRENCY START STEP COUNT
```

`cohort` emits `shylock.cohort/v1`. It admits only currently accepted,
verified, comparable reviews in the half-open interval
`[START, START + STEP * COUNT)`. Every selected review must agree on currency,
scale, nightly basis, stay dates, guests, tax/fee scope and qualifier. Listing
identity is explicitly supplied by the reviewer; URLs do not establish entity
identity and unknown entities are never automatically matched.

Within each bucket, matching listing/context claims with the same amount are
corroboration, including different methods and different captures. They count
once toward the bucket mean, but every observation/review reference remains in
`buckets[].evidence`. Disagreeing amounts for the same listing/context return
`review_conflict`, the disputed field and all conflicting claim IDs/amounts.
Other incompatible context returns `incompatible_cohort` with its field,
expected value and actual value. Conflicts require explicit rejection or
correction; the newest price does not silently win.

Each bucket's equally weighted distinct-listing arithmetic mean is rounded to
integer minor units using checked integer sums. This policy is versioned as
`distinct-listing-mean/v1`. Every bucket must contain observations; `reject/v1`
means no interpolation or forward filling. Bounds are 2..4096 buckets,
16384 selected review heads, positive sampling steps and year-9999 timestamps.

Exports include currency/scale, reviewed context, sampling interval, policy
versions, values, per-bucket review/capture references and a canonical
`cohort_id` integrity digest. `estimate`, `recommend` and `backtest` accept this
object directly. Forecasts and formula decisions retain the complete
`cohort_lineage`, estimator parameters and source/runtime revisions, including
formula-error decisions. Cohort digests are integrity identifiers, not signed
attestations or a substitute for trusted storage/review.

## Compatibility and exits

Existing five-argument `review`, `series` arrays and legacy extraction files
remain readable. Without the original HTML, review is explicitly marked
`legacy-unverified`; it does not become verified merely because it has a hash.
`series` remains the compatibility array export; it checks currency/scale/basis
but cannot establish missing stay context. Do not treat it as equivalent to
verified `cohort`. It can use legacy rows, and deduplicates only explicitly
identified listings. Existing flat v1 KVDB rows remain readable in ledger/series
without rewriting the database; re-import original captures for verified review.
Those pre-revision rows do not have a mutable revision head.

| Exit | Meaning |
| --- | --- |
| 0 | Success, including an identical retry |
| 2 | Invalid/unresolved input, failed replay, I/O/allocation failure, formula failure or uncertain commit durability |
| 3 | Conflicting/stale review or incompatible cohort; cohort conflicts also emit diagnostic JSON |
| 4 | Missing sample bucket; no partial cohort is emitted |

KVDB commits atomic snapshots and retains all provenance. Full snapshots and
bounded in-memory selection are not an incremental large-scale database. Use a
trusted stable parent directory and cooperating processes that honor the lock.
Ledger revision history is an application audit trail, not protection against
someone directly replacing local database files.

## Estimation and remaining scope

For configurable MuLIB estimators, disjoint selection/holdout evaluation,
checked stay quotes and unified guarded decisions, see [pricing policies](pricing.md).
The existing `estimate`, `recommend` and `backtest` v1 contracts below are retained.

`backtest SERIES_JSON HORIZON MIN_TRAINING_SAMPLES` uses expanding training
prefixes and chronological cutoffs. Each forecast uses only samples preceding
its target; failed forecasts reduce reported coverage. Outputs include MAE,
RMSE, bias and each fold's training size/target/outcome. The estimator remains
MuLIB damped Holt with alpha 0.4, beta 0.2 and phi 0.9, versioned as
`damped-holt/v1`. Asking prices describe supply, not booking demand. `optimize`
accepts externally supplied demand estimates or replayed artifacts from the
[booking-outcome demand workflow](demand.md), and audits its input digest and
floor/cap/tick/cost constraints. Asking-price cohorts never become booking demand.

Mune formulas retain deterministic math but not random, file, process, module,
debug or extension capabilities. Native tests check two formula evaluations in
one process for state isolation. The default backend is in-process; an optional
[Linux process sandbox](sandbox.md) isolates formula and extension execution.
The shared [worksheet TUI/GUI](worksheet.md) completes rejected/incomplete reviews,
resolves unknown currency/basis/qualifier and records explicit conflict rationale
through the same admission and append-only revision contract. It exposes source
and collection lineage without making quoted amounts editable. Closed scripts
are admitted before actions; rejected editor input cannot be silently truncated
or saved. [Effectful extensions](extensions.md)
have a separate state/lifecycle and scoped host capabilities; formulas never
inherit them. [Dynamic native extractors](sdk.md), opt-in live model calls and
[static browser jobs](intelligence.md) have separate explicit implementations;
they are not worksheet permissions. No unattended acquisition or live-provider
verification is claimed here.

`make test` runs the existing CLI and Poindexter suites, verified pipeline
fixtures, concurrent idempotent imports, review conflicts/revisions, replay
tampering, scoped rules, backtests, ABI negotiation and callback/lifetime tests.
The worksheet suite covers both renderers, revisions, read-only evidence,
stale writers, real PTY input and terminal cleanup. Extension tests cover scoped
permissions, failed-hook rollback, lifecycle/state lifetime and formula isolation.
