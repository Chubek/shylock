# NaiveFinder: deterministic, DOM-scoped price evidence

NaiveFinder extends Dragnet, without replacing its existing extractor, snapshots,
IR, transport, native SDK or model grounding checks. It uses the actual MuLIB
revision `eeb6de585303e1ea0dfd8bf2260328289b04587c`, without upstream patches or new
external dependencies. All matching commands below are offline.

```sh
./shylock snapshot https://example.org/cottages 1760000000 \
  tests/fixtures/naive-rentals.html > snapshot.json
./shylock find-rule snapshot.json plugins/dragnet/rules/rental.json \
  rental_price > found.json
./shylock find-rule-replay found.json
```

`SOURCE_JSON` accepts the same verified snapshot/browser-snapshot/IR inputs as
`flatten`. Existing IR is rebuilt and compared before use. Existing `find`,
`flatten`, `extract-snapshot` and `model-extract` retain their formats and behavior.
`find-rule` defaults to deterministic execution, even with API credentials present.

## Closed rule schema

All root members below are required; unknown members and duplicate keys (including
escaped aliases) are rejected. Rules are data, never scripts or host capabilities.
The hash covers **exact rule bytes**, not reserialized JSON.

```json
{
  "schema": "shylock.naive-rule/v1",
  "host": "example.org",
  "target": "rental_price",
  "container": ".listing-card",
  "listing": {"selector": ".listing-title", "attribute": ""},
  "fields": [
    {
      "selector": ".price",
      "attribute": "",
      "kind": "auto",
      "regex": "",
      "priority": 80,
      "currency_selector": "",
      "currency_attribute": "",
      "context_selector": ".basis"
    }
  ],
  "currency": "",
  "currency_source": "evidence",
  "decimal_separator": "",
  "group_separator": "",
  "maximum_candidates": 128,
  "maximum_containers": 64,
  "include_existing": true,
  "fallback": "none"
}
```

- `host`: exact lowercase source hostname, or `*`. No suffix wildcard or implicit
  subdomain matching. `target` must match the command's target exactly (1..128 bytes).
- Every selector is **one** MuLIB tag, `.class`, `#id` or `*` (<=128 bytes).
  Compound selectors, attributes, combinators, lists, pseudo-classes and XPath are
  rejected, not partially evaluated. Queries include the scope element itself.
- Containers are independent listing scopes. Nested matching containers are rejected;
  select leaf cards instead. The engine never assigns a quote by nearest byte distance.
- `listing` is a closed `{selector,attribute}` object. One matching node supplies
  trimmed text, or its named attribute. Zero/multiple matches preserve an unknown
  identity with a diagnostic. Even identical titles in different cards remain separate.
- `fields`: 1..32 closed objects with the seven required members shown above;
  `context_selector` is optional. All matching visible nodes are processed in DOM
  order. Empty `attribute` means subtree text; otherwise it is a simple attribute
  name (e.g. `content` or `data-price`). Nonexistent attributes produce no tokens.
- `context_selector`: optionally selects **one** label node inside the same listing,
  e.g. the adjacent `.basis`. Multiple label nodes fail closed. Its normalized text,
  selector and opening-element offset are recorded separately from monetary spans.
- `currency_selector` and `currency_attribute`: optionally obtain one explicit ISO
  code from container metadata. Multiple matching nodes leave currency unresolved
  with an explicit diagnostic. This prevents
  one card's EUR metadata from resolving another card's dollar symbol.
- `kind`: `auto`, `nightly_rate`, `weekly_rate`, `monthly_rate`, `stay_total`,
  `cleaning_fee`, `service_fee`, `tax`, `refundable_deposit`, `discounted_price`,
  `original_price`, `minimum_price`, `per_day` or `unknown`. Explicit kinds are
  rule assertions, not verification. Conflicting English labels produce diagnostics.
- `regex`: empty chooses the monetary recognizer (or bare numeric attribute
  recognizer). A custom pattern uses MuLIB Thompson-NFA syntax, <=256 bytes. Groups
  do not capture. Lookaround/backreferences are unavailable. Counted repetitions
  and empty-matching patterns are rejected. Each match must contain one complete
  number; partial numeric matches and overlong expressions fail instead of truncating.
- `priority`: integer 0..100, used only for inspection scores.
- `currency`: empty with `currency_source:"evidence"`, or a supported ISO code with
  `currency_source:"trusted_site_rule"` and a specific (not `*`) host. Selecting a
  trusted rule is an explicit currency assertion. Conflicting token/metadata/rule
  currencies remain unresolved. `$` and `£` alone do not establish an ISO currency;
  the euro symbol explicitly supplies EUR.
- `decimal_separator`: empty, `.` or `,`; `group_separator`: empty, `.`, `,` or
  ASCII space. A nonempty group requires a distinct explicit decimal separator.
  Group widths and minor-unit arithmetic are checked. Without a locale, only
  ungrouped integers and canonical dot fractions at the known currency scale are
  accepted; ambiguous comma/grouping and three-digit fractions remain unresolved.
- Candidate limit: 1..256; container limit: 1..128. Exceeding a limit is an error,
  never a successful truncated result.
- `include_existing`: retain and aggregate existing JSON-LD/microdata candidates
  when their source token belongs to a selected visible container. JSON-LD script
  text is allowed, but not inside hidden/noise ancestors. Global JSON-LD that cannot
  be reliably associated with a card stays in `legacy_extraction`, not silently
  assigned to the first card. JSON-LD `unitText` is classified by On1x; existing
  `unitCode:DAY` stays `per_day`, never implicitly nightly.
- `fallback`: `none` or `model_review`. It cannot authorize network access.

Shipped rules: `generic.json` scans paragraphs in a single body/listing;
`rental.json` uses independent `.listing-card` scopes; `examples/cottage.json`
demonstrates trusted-host currency, attribute identity/amounts and European separators.
They are examples to adapt to actual markup, not claims of universal site coverage.

## Recognition, interpretation and reconciliation

Native code supplies structural facts and recognized expressions. On1x chooses
strategies, currency resolution, amount interpretation, classification, aggregation,
scores, duplicate groups, conflicts and fallback decisions. English, word-bounded
labels distinguish nightly/weekly/monthly rates, totals, fees, tax and deposits.
Original/discounted/from qualifiers are separate from billing basis. Multiple
bases in one selected node remain ambiguous. Choose narrower fields or an explicit
scoped label instead of assuming every number in a card has the same meaning.

`shylock.naive-find/v1` retains the snapshot, exact rule source/hash, MuLIB revision,
complete existing extraction, ordered candidates, equivalence groups and summary.
Every candidate is `needs_review`, with unknown stay dates, guests, taxes and fees.
Listing identity is scoped to the container, not asserted to be a global booking ID.

Inspection score (`inspection-only/v1`): rule priority +10 known listing +10 explicit
currency +10 checked money +5 classified basis +5 exact span -20 unresolved issue.
Ties use source order. Scores do **not** choose a price or grant verified status.
Native structured representations use base priority 60, not an automatic truth premium.

Equivalent representations in the same container/basis/qualifier/currency/value
share a group while retaining **all** individual evidence records and IDs. Unknown
values group only with the same amount and original range. Agreement on one page
has `independent_sources:1`, not independent corroboration. Different known values
or currencies for the same scoped basis/qualifier retain alternatives and explicit
`material_conflict_requires_review` links. Different cards and original/from/current
qualifiers are not silently merged. No largest/smallest/first/majority winner exists.

## Provenance, replay and review

Candidate IDs hash their complete evidence (including rule/snapshot identities,
source sequence, classification, score and conflict state). Identical input yields
identical order and IDs. Changing the rule bytes or source requires new evidence.
`find_id` covers the complete deterministic artifact. Replay reruns **the same On1x
pipeline**, then compares the entire rebuilt artifact; rehashing tampered output
does not substitute for recomputation. Hashes are consistency IDs, not signatures
or authenticated capture times.

Text mapping records normalized offsets separately from original byte ranges:

- `exact` means unchanged contiguous original bytes.
- `covering-text-fragments` maps entity/whitespace normalization or split inline
  markup back to the original covering range. That range may contain tags or raw
  entity spellings; it is **not** claimed to be a verbatim normalized-text match.
- `existing-source-range` retains the older extractor's range and evidence ID,
  including its JSON Pointer in `legacy_extraction` for structured claims.

Every range has a digest and can be inspected against the full immutable snapshot.
Context label offsets identify opening elements, not normalized-text byte ranges.
Semicolon-terminated numeric references, euro/pound references and MuLIB's basic
named entities are decoded once; unsupported named entities remain literal.
Unicode NBSP, narrow/thin/figure spaces are normalized with source mappings.

Review accepts the deterministic artifact directly and replays it before native
storage admission. Optional HTML must exactly equal the embedded snapshot:

```sh
./shylock review prices.kvdb found.json 0 market reviewer "" stay-context.json
./shylock ledger prices.kvdb market
./shylock replay prices.kvdb EXTRACTION_ID
```

The existing review contract still requires a checked nightly amount, explicit
currency/scale, non-from qualifier, listing, stay dates, guests and known tax/fee
inclusion. It never auto-admits extraction as a forecast input. A known listing or
original/discount/from qualifier cannot be overwritten to make it comparable.
Material conflicts or interpretation diagnostics additionally require a nonempty
`conflict_resolution` string in review context (<=256 bytes), retained with the
review decision. Revisions and spreadsheet admission use the same On1x gate.
Missing currency/basis can be resolved explicitly under the existing review
contract; ambiguous localized amounts should instead be re-extracted with a
reviewed locale rule. Initial acceptance remains idempotent. Stored replay verifies
both immutable bytes and the On1x reconstruction before returning the artifact.

## Model modes

```sh
./shylock find-rule snapshot.json rules.json rental_price deterministic
./shylock find-rule snapshot.json rules.json rental_price hybrid profile.json --live
./shylock find-rule snapshot.json rules.json rental_price model profile.json --live
```

Deterministic takes no profile or consent flag. Hybrid first computes deterministic
evidence; only `fallback:model_review` plus empty/unresolved/conflicting evidence
and **exact `--live` consent** permit assistance. Model mode explicitly requests
the existing grounded model workflow, also after preserving deterministic evidence.
Both model modes require a valid profile and exact consent, even if hybrid will skip
the call. No missing candidate, key, currency or conflict alone authorizes payment.

`shylock.naive-assisted/v1` wraps `deterministic` separately from `model_result`.
Model claims remain review-required and pass the existing capture/quote/currency
checks. They never rewrite the NaiveFinder candidates or resolve its conflicts.
Missing credentials/API errors preserve deterministic evidence with exit 2.
`model_not_needed` means no call was made. Use the nested deterministic artifact for
`find-rule-replay` and Poindexter review. Existing `OPENAI_BASE_URL`, TLS, public
address validation and audit contracts apply unchanged.

## Native interface and limits

`naive.h` documents a single lexical non-reentrant scalar-ABI session, matching
the existing document adapter. `begin` owns cloned snapshots/rules and a bounded
MuLIB tree. `query(scope,selector,after)` returns generation-checked preorder
handles; zero is the document scope/end sentinel, -1 is failure. Old nonzero handles
are rejected after close/reopen. No pointer crosses the On1x ABI. Scratch strings
must be copied before the next scratch operation; On1x uses managed buffers/strings.

`text`, `scan`, hit accessors, original offsets, source ownership, source digests,
JSON string encoding/decoding and checked `money` are native primitives. `scan`
owns normalized text/mapping/hits until the next text/scan call. Regexes and MuLIB
scrape-rule selector records are always released. Errors poison result publication;
`close` releases partial state and is idempotent. Only a finalized result that
compares equal to a replayed artifact can cross the native verified-review bridge.

Hard ceilings: 2 MiB HTML, 64 KiB rule, 8192 tree nodes including the document,
depth 96, 8192 normalized bytes per selected node, 64 matches per node, 128-byte
numeric token, 256-byte complete regex match, 256 candidates, 128 containers,
32 fields and 16 MiB final artifact. Shared structural traversal budget is two
million visits. Regex searches reserve a conservative remaining-text × pattern-size
budget per enumeration call, capped at 16 million units. The NFA compiler also
enforces its upstream bounds; counted repetitions are forbidden to bound state size.
No monetary expression is cut at a limit. Source admission additionally inherits
the existing IR limits (2048 blocks/256 KiB readable text).

Success is exit 0, including valid empty or unresolved review evidence. Validation,
limit, allocation or replay failure returns 2 without publishing a truncated batch.
If the legacy extractor cannot complete but DOM extraction can, its explicit
`legacy_status` and null `legacy_extraction` accompany preserved DOM evidence and
exit 2; allocation failures can likewise prevent artifact creation entirely.

MuLIB supplies treebuilder/tokenizer, DOM matching, readability, regex, scrape-rule,
money, JSON-LD, URI, UTF-8 and hashing. The GNU Make helper still generates exactly
one MuLIB implementation translation unit. Native adapters include declarations only.

Limitations: bounded HTML recovery is not full WHATWG parsing; no JS, CSS cascade,
computed visibility or network acquisition during matching. Noise tags and explicit
`hidden`/`aria-hidden` are filtered heuristically, not rendered. CSS-only hiding is
not detected. Structured data is not presumed truthful. Automatic cross-card/stay
identity inference, full CSS selectors, full named-entity coverage and model-based
automatic conflict resolution are intentionally absent.

Tests: `make test` runs saved-fixture integration tests, native handle/span/money/
regex tests, On1x classification/ranking/conflict/consent tests, and allocation-failure
cleanup. See `plugins/dragnet/AGENTS.md` for extension instructions.
