# Dragnet collections and direct ledger pricing

The offline pipeline is now **replayed discovery → explicitly selected saved
snapshot → deterministic NaiveFinder evidence → explicit review → comparable
ledger cohort → evaluated forecast → bounded decision and checked stay quote**.

Run a complete synthetic demonstration into a **new** directory:

```sh
python3 examples/offline-dragnet.py /tmp/opencode/dragnet-example
```

It uses saved HTML only, seven asserted capture times, two named listings and a
common reviewed stay context. These are synthetic asking quotes, not live market
data, authenticated timestamps or bookings. It saves all artifacts, ledger,
forecast, decision and escaped MuLIB LaTeX report. PDF compilation is external.

## Collect explicit discovery hits

```sh
./shylock collect job.json > collection.json
./shylock collect-replay collection.json
```

The closed job schema requires exactly these members:

```json
{
  "schema":"shylock.collection-job/v1",
  "target":"rental_price",
  "sources":[{
    "discovery":"discovery.json", "hit":0,
    "snapshot":"snapshot.json", "rule":"rental-rule.json"
  }]
}
```

Paths are trusted local inputs, nonempty, <=4096 bytes and resolved relative to
the job. Target is 1..128 bytes; there are 1..8 ordered sources. Each source has
exactly the four members shown. Hit indexes are zero-based integers 0..63.
Discovery input is an individual `shylock.discovery/v1` record, not a discovery
batch; save the selected individual result when starting from `discover-many`.

Every discovery is fully replayed using its retained provider program and exact
parser revision/profile. Its selected hit must exist and match the listing
snapshot URL **exactly**. No redirect, normalization, proximity or cross-page
identity inference substitutes for this check. Listing snapshots need explicit
asserted timestamps. The snapshot input also accepts the existing verified
IR/browser-snapshot wrappers; consuming a browser snapshot does not replay the
outer automation trace.

On1x validates rules and runs the same DOM, classification, currency,
reconciliation and evidence pipeline as `find-rule`. There is no implicit
acquisition or model mode. Credentials, fallback settings, empty quotes and
unresolved currency cannot authorize a network request.

`shylock.collection/v1` embeds every complete discovery and NaiveFinder artifact,
ordered entry metadata, summary and `collection_id`. It never replaces original
bytes/programs/rules with file paths. Entries are `needs_review`, `empty` or
`partial_extraction`; extraction alone cannot admit a price to the ledger.

Duplicate `(snapshot_id, rule_blake3)` pairs retain evidence and `duplicate_of`
points to their first entry; other entries use null. Repeated pages/providers/
strategies are **not independent corroboration**. Summary candidate, conflict
and unresolved counts include duplicates. `unique_capture_rule_pairs` is not a
count of independent listings. Cross-capture alternatives remain unreconciled
until explicit review and the existing cohort conflict checks.

Invalid links/files/rules or native parsing/limit/allocation errors abort without
a misleading bundle. Valid empty extraction is exit 0. When legacy extraction
cannot complete but DOM evidence survives, the whole bundle is preserved with
`partial_extraction` and exit 2, including unaffected sources. Its null legacy
artifact remains explicit. Aggregate output is <=16 MiB, plus existing per-source
NaiveFinder bounds. Successful ordinary collections use exit 0.

Replay needs no original files: it reruns embedded discoveries and the complete
On1x extraction pipeline, rebuilds links, duplicates, statuses and summary, then
compares the entire collection. Replacing a hash cannot substitute for replay.
IDs are consistency hashes, not signatures or proof of capture chronology.

## Explicit review and retained lineage

```sh
./shylock review-collection prices.db collection.json 0 0 market alice context.json
./shylock ledger prices.db market
./shylock replay-collection prices.db COLLECTION_ID
```

Entry and candidate indexes are zero-based. Full collection replay precedes any
database write. The chosen extraction is replayed again before applying the
**same On1x admission gate** as `review`: checked amount, currency/scale, nightly
basis, unqualified price, listing, valid stay/guest context and known tax/fee
inclusion. Conflicts require explicit `conflict_resolution`. Rankings never
approve evidence. A fully replayed partial bundle may also be explicitly reviewed;
this does not claim that its missing legacy extraction succeeded.

The immutable collection is stored at `collection/ID`. The observation's new,
optional `collection` field holds `{collection_id,entry,discovery_id,hit,find_id}`.
It survives revisions and is copied into applicable cohort references, forecast
lineage and pricing audits. Existing reviews are not retroactively assigned a
collection. Stored collection replay copies the record, releases the ledger lock,
then reruns On1x; existing extraction replay remains independently available.

Review retries/CAS revisions remain unchanged. Exact duplicate entries share the
first entry's collection link, making duplicate review idempotent. A changed
collection/reviewer/context for an already reviewed evidence ID can require an
explicit revision, never create another observation. Use genuinely scoped listing
identities: identical display names across sites do not establish global identity.
Harmless JSON key ordering/whitespace changes do not conflict at immutable
collection or extraction keys: their first stored representation is retained
unchanged after canonical comparison. Raw snapshot bytes still require exact equality.

## Forecast and price directly from reviewed observations

```sh
./shylock forecast-ledger prices.db selection.json forecast-policy.json
./shylock price-ledger prices.db selection.json pricing-policy.json > decision.json
./shylock print-report reports/pricing.json decision.json > report.tex
```

`price-ledger` also accepts a fourth demand-artifact argument for demand decisions,
under the existing `price` compatibility/replay contract. Asking quotes themselves
are supply-side evidence, never booking demand. Both commands require a closed
selection with exactly these fields:

```json
{
  "schema":"shylock.ledger-selection/v1",
  "cohort":"market", "currency":"EUR",
  "start":1793400000, "step":60, "count":7,
  "as_of":1793400419, "max_age_seconds":59, "minimum_listings":2
}
```

- Existing interval/currency bounds apply: 2..4096 buckets, positive step, checked
  timestamp range, supported currency, nonempty <=128-byte cohort.
- Complete half-open buckets only: `start + step * count - 1 <= as_of`. No future
  capture, partial current bucket, carry-forward or interpolation is admitted.
- Current accepted **review heads**, verified replay provenance and compatible
  currency/scale, nightly basis, stay, guests and tax/fee context are required.
  Out-of-window reviews do not affect selection.
- Every bucket needs `minimum_listings` distinct identities (1..16384). Duplicate
  representations cannot satisfy this coverage gate.
- Every retained capture in the **last** bucket must satisfy
  `as_of - captured_at <= max_age_seconds`. Equality passes; historical training
  buckets are exempt. Timestamps remain caller assertions, not authenticated times.
- Selection and pricing policy must use the **same `as_of` cutoff**.

Same-listing/context price disagreement remains conflict/exit 3; missing buckets
remain exit 4. Admission/schema/coverage/freshness failures exit 2 with diagnostics
and no recommendation. Structured incompatibility/conflict diagnostics may appear
on stdout, but are never disguised as successful pricing decisions.

The writer lock covers open, current-head selection, compatibility and cohort
construction. The engine takes an owned, validated copy before unlock. No formula,
demand processing, model call or deferred extension hook runs under that lock.
There is no temporary cohort file or database mutation.

Existing forecasts, holdout evaluation, Mune isolation, bounds, tick rounding,
checked charges and failed-decision artifacts remain unchanged. Forecast/formula
errors preserve evidence, never auto-publishing a rate. The forecast's additional
`ledger_selection` records the full selection, admission policy, minimum observed
bucket coverage, oldest last-bucket capture, latest capture and supply-side evidence
kind. `cohort_lineage` retains all observation/review/extraction/collection IDs.
In-memory series digests hash compact JSON bytes; differently formatted cohort
exports can have different raw-byte digests while retaining equivalent cohort IDs
and pricing results. Later review revisions can change a future ledger run; this
is not a frozen historical database replay.

## Implementation and verification

`plugins/dragnet/dragnet_collection.on1x` owns collection admission,
link/duplicate/status policy and replay; `plugins/dragnet/poindexter.on1x` owns
the optional ledger review/replay integration. `plugins/poindexter/ledger_pricing.on1x` owns selection, freshness/
coverage gates and engine orchestration. Native collection, Poindexter and engine
adapters own bounded resources and compose MuLIB primitives. There are no new
dependencies, upstream patches or extra implementation translation units.

New implementation mix: about **41% C11 / 59% On1x** (208/297 substantive
nonblank, noncomment lines). This measures the collection/ledger-pricing modules
and their added adapter, cohort-admission and dispatch code; it excludes raw FFI
declarations, imports/headers, the preceding Elvi upgrade, tests, examples, docs,
generated code and dependencies. Native code owns resources; On1x owns policy.

`tests/collection.py` covers discovery-to-decision integration, tampering,
duplicates, conflicts, unknown currency, partial extraction, stored replay,
current review heads, gaps and gate boundaries. Native allocation injection
covers bundle reconstruction and idempotent cleanup. All checks are offline;
none imply live provider availability or permission.
