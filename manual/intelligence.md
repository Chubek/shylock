# Dragnet browser automation, page IR and OpenAI extraction

On1x owns job/profile admission and orchestration. C11 adapters own resources
and call MuLIB's `browser-engine`, `browserlocator`, `dom-query`,
`html-treebuilder`, `readability`, `money` and **`openai-client`** kernels.
No upstream files or implementation translation units are duplicated.

## Browser automation

```sh
./shylock browse plugins/dragnet/browser-job.json > browser.json
./shylock flatten browser.json > page-ir.json
./shylock find page-ir.json prices > prices.json
```

The example follows a saved search-page link, checks a heading and reads a
listing paragraph without network access. `shylock.browser-run/v1` retains the
job, ordered action trace, completion status and last successfully loaded snapshot.

**The pinned MuLIB browser engine is a static-HTML profile.** It performs document
acquisition, bounded tree construction, navigation/history and DOM queries. It
does not execute JavaScript, render CSS, drive Chromium, submit forms, log into
websites or wait for JavaScript-created elements. Unsupported actions/evaluation
are rejected rather than silently treated as successful automation.

A closed `shylock.browser-job/v1` object contains:

- `schema`, `start_url`, asserted `captured_at` (0..253402300799), Boolean
  `allow_network`, `pages` and `steps`.
- `pages`: <=32 unique `{ "url":"https://...", "path":"saved.html" }` mappings.
  Paths resolve relative to the job. Mapped URLs always use saved files; missing
  files cannot silently fall back to live acquisition.
- `steps`: 1..16 closed `{ "action":"...", "selector":"...", "value":"..." }`
  objects. `navigate` takes an absolute URL value and empty selector; `follow`
  takes a link selector and empty value; `read` takes a selector and empty value;
  `assert` takes a selector and a nonempty literal substring value.

Selectors are MuLIB's single-selector subset: a tag, `#id`, `.class` or `*`, not
combinators or XPath. The first match is used. On1x validates the complete job
before acquisition. Every navigation, including followed links, is restricted
to the initial origin and public HTTPS. Unmapped URLs require `allow_network:true`
and use Dragnet's verified, DNS-pinned GET transport. MuLIB resolves relative links.
There are no cookies, implicit subresource loads, redirects, scripts or model calls.

Limits: <=2 MiB document, 50000 tree nodes/depth 128, 16 KiB read text and 32 history
URLs. Failure stops later actions, preserves the last good source/DOM and returns
exit 2 with an audit where allocation permits. Success returns 0. State is always
destroyed; a failed navigation cannot pair an old DOM with a new source body.

## Flat, noise-filtered intermediate representation

```sh
./shylock snapshot https://example.org/cottage 1760000000 \
  tests/fixtures/rental.html > snapshot.json
./shylock flatten snapshot.json > page-ir.json
./shylock flatten page-ir.json
```

`flatten` accepts a validated snapshot, a browser-run's validated nested snapshot,
or an existing IR. Browser-run input consumes its snapshot; it does not assert
that the outer automation trace has been re-executed.

`shylock.page-ir/v1`, profile `static-semantic-blocks/v1`, contains the full immutable
snapshot, flat ordered `blocks`, MuLIB revision and canonical `ir_id`. Each block
has an index, readable `text`, original `byte_offset`/`byte_length` and span digest.

MuLIB supplies bounded HTML trees and readability/entity/whitespace normalization.
Inline text stays together: `EUR <b>180</b>.00 per night` becomes `EUR 180.00 per night`.
Dragnet removes `head`, script/style/noscript/template, nav/header/footer/aside/SVG
subtrees and explicit `hidden`/`aria-hidden="true"` subtrees. Original HTML remains
attached. A block range can include inline markup; normalized character positions
are **not** original HTML byte offsets.

This is deliberately lossy, not a perfect visual/accessibility tree. Valuable text
in a header/aside may be omitted; CSS-only hiding is not evaluated. JSON-LD is not
in the IR's visible blocks, but the separate deterministic extractor still walks
structured offers in the original page. Choose HTML model input when excluded
markup matters. Entity decoding uses MuLIB readability's documented basic subset.

Limits are 2048 blocks and 256 KiB readable text, with bounded raw block buffers.
Excess fails rather than silently truncating. Existing IR is rebuilt from its
snapshot and compared in full, rejecting altered text/ranges/order/IDs. Hashes
prove replay consistency, not signatures or authenticated capture time.

## Simple matching without a model

```sh
./shylock find snapshot.json prices
./shylock find page-ir.json parking
```

`find SOURCE_JSON TARGET` always runs locally, even with an OpenAI key configured.
It accepts any `flatten` input. Wrap raw HTML with the saved-file `snapshot`
command first so URL/time/provenance are explicit.

Targets `price`/`prices` select a naive currency-token scan of normalized blocks.
It recognizes uppercase ISO codes, euro signs and `$`; `$` does **not** imply USD.
Canonical amounts use checked MuLIB money. Unknown currency/scale/amount stays
null, and ambiguous/localized separators are not guessed. Explicit nearby English
labels distinguish nightly, stay-total and cleaning-fee bases; `from` remains
qualified. Other targets use ASCII case-insensitive literal block matching,
not semantic search or arbitrary regular expressions.

`shylock.page-find/v1` retains IR, method/target, <=256 identified matches and
ordinary deterministic Dragnet extraction from the original page. Every match
has a source block and **needs_review** status. IR matches are distinct from the
native extractor's candidates, not automatically admitted observations. If native
extraction hits its own limits or lacks a required timestamp, IR matches can
survive with explicit `deterministic_status`; no successful extraction is invented.

## Connect an OpenAI API account

Choose a model available to your API account using this complete profile:

```json
{
  "schema":"shylock.openai-profile/v1",
  "model":"gpt-4.1-mini",
  "api_key_env":"OPENAI_API_KEY",
  "organization":"",
  "project":"",
  "input":"ir",
  "max_output_tokens":2048
}
```

An example ships at `plugins/dragnet/openai-profile.json`. Set the named environment
variable through a secret manager or non-echoing shell prompt. Do not put its
value in profiles, arguments, saved pages, git or shell history. Literal `api_key`
profile fields are rejected.

```sh
./shylock openai-account plugins/dragnet/openai-profile.json
```

This reports local key configuration only. `authenticated:null` explicitly means
no server validation was attempted. It never prints the key or performs a request.
Profiles are trusted local configuration, including the environment-variable name
and optional organization/project billing scope. The standard `OPENAI_BASE_URL`
environment variable selects the API base URL; unset or empty defaults to
`https://api.openai.com/v1`. For example:

```sh
OPENAI_BASE_URL=https://gateway.example:8443/openai/v1 \
  ./shylock openai-account plugins/dragnet/openai-profile.json
```

The same setting applies to `model-extract`. Supply the complete base path
(including `/v1` when required); Dragnet appends `/responses`, without adding
`/v1` itself. Trailing slashes are normalized away. HTTPS, public-address checks,
certificate verification and DNS pinning remain required. Explicit ports 1..65535
are supported; HTTP, localhost/private addresses, IPv6, credentials in the URL,
queries, fragments and malformed escapes are rejected. Invalid settings prevent
requests and preserve deterministic model-extraction results with
`outcome:"invalid_endpoint"`; the rejected URL is not copied into audits.

Only select a **trusted** Responses-API/structured-output-compatible endpoint:
it receives the configured API key, billing-scope headers and submitted page
content. The selected endpoint is recorded in account output, model audits and
generated model-reply provenance. Do not place secrets in the base URL. Profiles
and page content still cannot override the endpoint or enable redirects.

This uses an **OpenAI Platform API account/key**, not a ChatGPT subscription,
browser session cookie or password/OAuth impersonation flow. API model access,
billing and permissions belong to that account. No credentials are required for
saved browsing, flattening, simple matching or deterministic extraction.

## Explicit direct model-service calls

```sh
./shylock model-extract page-ir.json plugins/dragnet/openai-profile.json \
  prices --live > assisted.json
```

The exact `--live` flag is mandatory consent. This sends page content to the selected
service and can incur API charges. There are no automatic retries, model calls from
`find`, fallback providers or model-executed tools.

MuLIB's **OpenAI client kernel** builds and performs Responses operation
`createResponse`, sets authentication/scope headers, handles bounded HTTP and
parses the response. Requests use `store:false`, explicit model, a 256..4096
output-token budget and strict JSON schema. Instructions treat page/target data
as untrusted evidence, not policy. Prompting alone is not a security guarantee:
host-side grounding is checked separately, and interpretation stays review-only.

`input:"ir"` sends readable blocks with original source quotes. Blocks with raw
spans >4096 bytes have explicitly empty source quotes and cannot support model
claims. `input:"html"` sends original HTML with block index -1. Serialized input
must fit 128 KiB; it is never silently truncated. Deterministic results survive
request-budget failure.

Suggestions must name a valid block and provide a unique exact contiguous quote
from its **original HTML range**, containing the verbatim value. A monetary amount
must occur in that quote, parse as checked money and have an explicitly evidenced
ISO code (or EUR euro sign). Currency is not inferred from geography. Host checks
types, bounds, duplicates, unsupported claims, malformed JSON, refusals and
incomplete responses. One invalid suggestion rejects the whole model batch.
A quote proves grounding, not correct tax/stay interpretation or comparability.

Generic claims remain `model_claims` with `review_required`. Valid monetary
suggestions also pass through the existing capture-digest/exact-quote importer
as native review-only candidates. Split-markup prices with no contiguous source
amount can still be found by local IR matching; the model may not fabricate a
verbatim HTML quote for them.

The result retains local matches/extraction plus `model_audit`: model/input mode,
prompt contract, request/response digests, runtime, token budget, scope, HTTP status
and outcome. Request/response/IR provenance is also retained in generated model
reply source for downstream review replay. No API key, Authorization header,
unvalidated provider output or raw provider error body is published. Preserve the
complete assisted artifact alongside any extraction used for review.
The request digest covers canonical JSON, not authentication headers; the response
digest covers exact received body bytes. Output-token limits do not fix input-token
costs: review the selected model's pricing before enabling live calls.

Missing keys, transport/API errors, refusals, invalid suggestions and request
limits return exit 2 with `status:"deterministic_fallback"` and local results when
allocation permits. Successful validated suggestions return 0 with
`status:"model_assisted"`; an empty suggestion list is not proof the target exists.
No result is automatically reviewed or priced.

## TLS, dependencies and verification

The C11 API transport composes MuLIB TSL/HTTPS/MuW3 with an OpenSSL authenticated
TLS backend. It validates public IPv4 DNS answers and pins the connection while
retaining the selected endpoint's SNI and certificate/hostname (or IP SAN) checks. TLS >=1.2, HTTP/1.1,
system trust roots, no proxy/redirect/retry, 5-second connect and 30-second total
TLS/HTTP deadlines apply (plus <=3-second DNS). Responses are bounded to 1 MiB
plus bounded headers; abrupt EOF is not authenticated TLS shutdown. This API path
does not use curl or place credentials in subprocess arguments.

**New build dependency:** OpenSSL development headers/libraries (`libssl`,
`libcrypto`), detected by `pkg-config openssl`, with conventional linker flags as
fallback. MuLIB still supplies the OpenAI protocol client; OpenSSL is only the
host TLS backend. Browser page acquisition continues using the existing curl
adapter. Local PATH, trust-root configuration and native plugins remain trusted.

`tests/intelligence.py` covers offline browser traces/failures, verified IR,
noise/split markup, matching, redaction, endpoint configuration and missing-key fallback. Native tests
inject a verified TSL fixture through the real MuLIB OpenAI client, checking
authentication/request construction, fragmented I/O, API errors, bad quotes/
currency, custom endpoint paths/ports, transactional batches and failed handshakes. Allocation injection checks
IR, matching, model and browser cleanup. No CLI setting bypasses TLS/address
validation or enables the injected test connector. Tests never use a real account or make
paid calls; live service/browser-provider checks must be reported separately.
