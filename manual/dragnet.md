# Dragnet: ElviScrape discovery, snapshots and deterministic extraction

Dragnet combines **On1x policy/orchestration** (`src/dragnet.on1x`) with **C11
adapters** (`plugins/dragnet/{workflow,transport,dragnet}.c`). It directly compiles
and runs the upstream `include/mulib-elvi-scrape.h` interpreter against the real
`library/elvi-scrape/google.elvs` and `bing.elvs` programs. URL encoding, heading
selection, Google redirect rewriting, Bing direct links, excluded provider hosts,
result de-duplication and bounded parsing are MuLIB operations, not replacement
scrapers. The Makefile's upstream helper supplies their dependency closure in
one MuLIB implementation translation unit. No upstream files were changed.

## Discover and replay

```sh
./shylock discover google 'lake cottage' tests/fixtures/google.html 10 > google.json
./shylock discover bing 'lake cottage' tests/fixtures/bing.html 10 > bing.json
./shylock discover-replay google.json
./shylock discover-many plugins/dragnet/offline-job.json
```

Syntax is `discover PROVIDER QUERY [SERP_HTML [LIMIT]]`. Omit the saved response,
or use an empty string, for an explicit live request. Default limit is 10;
accepted limits are 1..64, clamped further by the provider program (Google/Bing
currently allow 30). Provider names are bounded ASCII identifiers and must match
the program's declared name. Queries are nonempty, <=2048 bytes, and encoded by
MuLIB rather than interpolated into a shell. `SHY_PROVIDER_ROOT` can select a
different **trusted** provider directory; paths cannot escape it through the
provider name. Editing an `.elvs` program changes recorded provenance.

`shylock.discovery/v1` retains provider/query, constructed search URL, requested
and effective limits, exact program source/digest, MuLIB revision, status,
diagnostic, response snapshot when successfully parsed, hits and discovery ID.
The old `provider`, `search_url`, `status` and `hits` fields remain available.
Each hit still has exactly `url` and `title`. URLs are proposals, not permission
to fetch them or evidence that they identify the same listing.

Outcomes are chosen by On1x:

| Status | Meaning / exit |
| --- | --- |
| `results` | Parsed at least one hit / 0 |
| `no_parsed_hits` | Successful parser with zero hits / 0 |
| `transport_error` | Saved-response read, address policy, DNS or HTTPS failure / 2 |
| `http_error` | Non-200 response, including redirects and rate limits / 2 |
| `response_error` | Wrong media, empty or invalid UTF-8 HTML / 2 |
| `parse_error` | ElviScrape failed to process the bounded response / 2 |

An invalid program/query or an allocation/publication failure may instead return
2 without an artifact. Zero hits cannot alone distinguish truly empty results,
a bot challenge or provider markup drift. Dragnet says so explicitly rather
than fabricating a cause. It never retries rate limits or bypasses challenges.

`discover-replay` needs no provider files, network or DNS: it recompiles the
**retained** program, parses the retained response, recomputes URL/hits/outcome
and all IDs, and checks the entire artifact for equivalence. It accepts successful
parser results, including zero hits; failure artifacts without a snapshot cannot
be replayed. Replaying does not refetch. IDs detect inconsistency, not forgery:
they are unkeyed hashes, not signatures, authenticated times or server proofs.
Search captures have `captured_at:null` because this command receives no asserted
capture timestamp; it does not falsely stamp saved bytes with today's time.

## Multiple providers

`discover-many JOB_JSON` admits the complete closed `shylock.discovery-job/v1`
object before acquiring anything. It has exactly `schema`, `query`, `limit`,
Boolean `allow_network`, and 1..8 `providers`, each with unique `name` and
`capture` string. Capture paths resolve relative to the job file. Empty capture
means live access and requires `allow_network:true`. The supplied offline job
sets it false and uses saved Google/Bing responses.

Providers execute sequentially. Failure of one does not hide another's results;
`shylock.discovery-batch/v1` preserves job, ordered results and `batch_id`.
Status is `complete`/exit 0 or `partial_failure`/exit 2. Provider configuration
errors have explicit per-provider `provider_error` rows. Allocator/publication
failures can abort the aggregate rather than publishing a misleading complete
batch. Retained result JSON is bounded to 16 MiB in aggregate. To replay an
individual successful result, save its object and use `discover-replay`.

No automatic listing fetch follows discovery. This keeps acquisition consent,
listing selection and processing separate.

## Immutable listing snapshots

```sh
./shylock snapshot https://example.org/cottage 1760000000 \
  tests/fixtures/rental.html > snapshot.json
./shylock extract-snapshot snapshot.json tests/fixtures/rule.json > extraction.json
```

`snapshot HTTPS_URL CAPTURE_UNIX_SECONDS [PAGE_HTML]` reads a supplied saved page
or explicitly fetches a live one. It emits `shylock.snapshot/v1` with exact UTF-8
HTML, byte count/digest, source URL, asserted timestamp, acquisition mode,
HTTP status (null for saved bytes), media type and `snapshot_id`. Time is required
in 0..253402300799; it is a caller assertion, not authenticated chronology.

`extract-snapshot SNAPSHOT_JSON [SITE_RULES [MODEL_REPLY_JSON [PLUGIN]]]` validates
the complete snapshot, metadata, body and IDs before extraction. It uses the
original URL/time/bytes and produces the ordinary `shylock.capture/v2` evidence
record. This output is equivalent to `extract` on the same saved bytes and inputs,
so existing review/cohort workflows continue working. Keep the snapshot alongside
the extraction; the latter is not a second copy of the page. Poindexter's current
verified review still takes the original HTML file as its explicit page input.

Legacy `capture HTTPS_URL` still writes raw HTML. Legacy
`extract PAGE_HTML URL TIME [RULES [MODEL_REPLY [PLUGIN]]]` remains supported,
including HTTP URLs for offline provenance; no live HTTP transport is enabled.

## Transport contract

Live search, raw capture and snapshots share one bounded transport:

- Public HTTPS, verified certificate/hostname, TLS >=1.2, implicit port 443.
  Reject credentials, controls, decoded control/backslash escapes, malformed
  escapes, local hosts, ambiguous numeric hosts and IPv6 (conservative v1 policy).
- Resolve IPv4 in a separate bounded process (3 seconds). Reject private,
  loopback, link-local, shared, documentation, benchmark, multicast and other
  conservatively reserved ranges. If **any** IPv4 answer is blocked, fail closed.
- Pin the chosen public address with curl `--resolve` while retaining the original
  hostname for SNI/certificate checks. Force IPv4. Disable proxies, redirects,
  retries and curl startup configuration; never use `--insecure`.
- Curl has 5-second connect and 15-second transfer deadlines. The host also bounds
  pipe collection and child reaping to 20 seconds; DNS has its separate 3 seconds.
  Timeout, limit or callback failures kill/reap the child group. Closing stdout
  while leaving a subprocess running cannot bypass the host deadline.
- Retain at most 2 MiB body plus bounded status/media metadata. Reject excess,
  embedded NULs, empty captures, non-200 responses and non-HTML media at admission.

`curl`, PATH, CA configuration and native plugins are trusted local application
inputs. TLS remains curl's implementation; this does not claim to use MuLIB's
TLS handshake adapter. MuLIB ElviScrape/fetch policy and parsing remain canonical.
The C embedding header exposes a trusted injected transport/resolver interface
for deterministic tests; **no environment switch disables DNS checks** in the CLI.
The built-in resolver checks IPv4 only because the live transport forces IPv4.
This transport does not add robot-policy compliance, consent, cookies, sessions,
authentication, browser JavaScript, anti-bot evasion or automatic retry scheduling.

## Extraction version 3

Dragnet now advertises `dragnet/v3`; extractor host ABI remains v2. New evidence
must have new extractor-version-based IDs. Existing capture JSON stays readable,
but older v2 evidence is not silently rewritten or falsely verified by a v3
extractor. Preserve the old executable for exact historical replay, or re-extract
the saved page and explicitly review the newly versioned observations.

Deterministic methods include:

- Bounded JSON-LD walking through `@graph`, `mainEntity`, `offers`,
  `priceSpecification`, `itemListElement`, `item` and `hasPart`. Recognize string
  or array `@type`, inherit explicit offer currencies into price specifications,
  and preserve AggregateOffer `lowPrice` as a `from` claim. Unknown price basis
  stays unknown; `unitCode:DAY` is `per_day`, not an invented overnight stay.
- Visible ISO-code prices and explicit English `per night`, `total` and
  `cleaning fee` labels. Join bounded inline markup while mapping every rendered
  byte back to the original HTML range. Recognize NBSP spellings. Script, style
  and template content cannot become visible evidence. Euro symbols map to EUR;
  `$` is deliberately **not** assumed USD. Unsupported codes keep null minor/scale.
  A visible run is limited to 4096 bytes; exceeding it fails rather than splitting
  a monetary expression or its qualifier into a misleading partial match.
- Microdata `itemprop="price" content="..."`; only explicit same-element
  `data-currency` supplies currency. Without it, the currency remains unresolved.
- Existing host-specific, independently hashed `data-*` rules. Version 2 scopes
  the rule to an identified offer container; version 1 remains readable.

Rules can additionally declare `decimal_separator` (`.`/`,`) and optional
`group_separator` (`.`/`,`/space, different from decimal). Grouping is validated
before normalization. The original amount text/range remains attached;
`normalized_amount` and `rule_blake3` explain localized parsing when changed.
Canonical JSON-LD/model amounts are not reinterpreted with page locale rules.
Without explicit locale rules ambiguous separators are not guessed into money.

Extraction retains <=256 candidates, source/script pointers, exact ranges and
span hashes, extractor/rule versions, checked money, unresolved context and
review status. Repeated exact replay produces the same evidence IDs. Exceeding
limits or required-rule failures discard output rather than silently truncating
offers. Malformed JSON-LD blocks are skipped so other deterministic evidence can
survive. This is a bounded lexical HTML subset, not a WHATWG browser DOM, and does
not infer fees, taxes, stays, listing identity or acceptance from a price alone.

Externally generated model replies remain optional, capture-digest-bound,
exact-quote-validated and **review-only**. Invalid replies are transactional and
leave deterministic results with an explicit diagnostic. This implementation
does not add direct model-service calls or browser automation. Those require
separate authenticated transports/permissions and verification, not a hidden
side effect of searching or extracting a page. Native SDK loading is independent
of Mune formulas/effectful extensions; see [the C11 SDK](sdk.md).

## Verification

`make test` covers both actual upstream providers with saved pages, query/limit
admission, multi-provider partial results, provenance/replay tampering, snapshot
equivalence, structured and split-markup extraction, locale and unresolved symbols,
dynamic SDK transactions, injected DNS/transport failure and bounded child cleanup.
Allocation-failure tests cover response/batch ownership and failed publication.
The suite requires no public DNS, internet, model key or live provider consent.
Live-provider behavior is separate; successful offline fixtures do not prove
current Google/Bing availability, provider permission or markup compatibility.
