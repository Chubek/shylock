# Shylock

Shylock is an On1x command-line price intelligence engine. The On1x program
uses the upstream CommandLine, OS_Path and Parsec modules for command dispatch,
argument parsing, and path handling. On1x also owns rate adjustment constraints.
A narrow C11 bridge handles MuLIB buffers, JSON trees, typed Mune values, and
numeric kernels. The upstream On1x build helper assembles one implementation
translation unit for the selected kernels.

## Build

Place a checkout of [Chubek/mulib](https://github.com/Chubek/mulib) at
`third_party/mulib`. Tested against commit
`eeb6de585303e1ea0dfd8bf2260328289b04587c` (hardened Elvi engine). Requires GNU Make, Python 3,
a C11 compiler, and a C++20 compiler. The Makefile builds On1x using its
upstream bootstrap scanner, or accepts `ON1XC=/path/to/on1xc` for an existing
compiler. The CLI uses POSIX shell and process APIs. Live HTTPS uses curl for
transport and certificate validation. The fixture workflow needs no network.
Direct OpenAI calls additionally require OpenSSL development headers/libraries
(`libssl`, `libcrypto`, detected with `pkg-config openssl`) for the authenticated
TLS adapter beneath MuLIB's OpenAI client. Credentials are runtime-only.
The terminal worksheet uses MuLIB TUI/termkey. The graphical worksheet uses
MuLIB ImmGTK/GLX with an SDL2 window adapter, detected using `pkg-config`.
Install SDL2 development files and build with `GUI=1` to require native windows;
`GUI=0` removes the SDL2 dependency while retaining headless GUI rendering.
Interactive GUI text optionally uses MuLIB fontdraw with runtime FreeType and
a system monospace font; a small built-in ASCII fallback is always available.
Set `SHY_UI_FONT` to choose a font file.

```sh
./build.sh -j4
make test
./shylock help
```

`CFLAGS` are forwarded to the native C build. For a separate sanitizer build:

```sh
make -j4 test BUILD=build-sanitized ON1XC="$PWD/build/on1xc" \
  CC=clang CFLAGS='-fsanitize=address,undefined -fno-omit-frame-pointer -g' \
  PPX_TIMEOUT_MS=60000
```

The build also produces the companion `shylock-worker` executable. Keep it beside
`shylock` when installing/copying binaries if process isolation will be used.
The normal CLI does not require a worker at runtime when isolation is off.

### Command help

The driver has a grouped command index and a dedicated page for every enabled
command, with argument descriptions, operational notes and three examples:

```sh
./shylock help
./shylock help recommend
./shylock find-rule --help
./shylock review -h
./shylock help --color=always
```

No arguments, `-h` and `--help` show the index. `help COMMAND`, `COMMAND --help`,
`COMMAND -h`, and `--help COMMAND` show the same command page. Help is handled
before native plugins or extension hooks initialize; it does not read input
files, modify a ledger or perform network/model calls. Unknown topics and
malformed help options exit 2. Only compiled-in command pages are available.

Color defaults to `--color=auto`: terminals receive colored titles, headings and
examples, while redirected output stays plain. `TERM=dumb` disables automatic
terminal color. A nonempty `NO_COLOR` disables auto color; otherwise a nonempty
`FORCE_COLOR` enables it (`0` disables it). Explicit `--color=always` or
`--color=never` overrides the environment. These options apply to help only.
The shell launcher reports whether stdout is a terminal; On1x's CommandLine,
String and Char standard-library tools handle help routing and rendering.

Examples using fixture paths run from the repository root. Other named JSON
files and artifact IDs must be prepared through the documented workflows.
Live acquisition/model examples are identified on their command pages.

Built-in plugin authors: the help hook now has signature
`NAME_help(topic:string,color:bool)->bool`, replacing `NAME_help()`.
See [the help composition contract](manual/plugins.md) for the shared renderer
and return-value rules. Native extractor ABI and command data formats are unchanged.

### Plugin boundary and build profiles

`src/` contains only the engine and generic host interfaces. All Dragnet policy,
native adapters, CLI handlers and registration live in `plugins/dragnet`; all
Poindexter review/ledger/cohort and worksheet code lives in `plugins/poindexter`.
Each plugin's `plugin.mk` declares its sources, kernels and external dependencies.
It also declares plugin-specific extension events. The build generates only the selected command/help and initialization composition
in `BUILD`; the engine does not import or register any named plugin directly.
See [the built-in plugin contract](manual/plugins.md) to add a plugin without
editing the engine or root Makefile.

```sh
./build.sh -j4                                      # Dragnet + Poindexter (default)
./build.sh -j4 PLUGINS= BUILD=build-core             # No built-in plugins
./build.sh -j4 PLUGINS=dragnet BUILD=build-dragnet    # Extraction without ledger/UI
SHY_BINARY="$PWD/build-core/shylock" ./shylock help
make test-plugins                                  # Build/profile boundary checks
```

The plugin-free build needs neither OpenSSL nor SDL2. Generic native SDK loading,
explicit-plugin `extract`, formulas, extensions and pricing remain engine features.
Dragnet owns the legacy extraction default and provider-root fallback. Disabled
plugin commands are absent from help and return exit 2. No capture, evidence,
review, pricing or SDK schema changes are required.

Poindexter currently **requires Dragnet** for verified extraction and collection
replay; select both plugins when using the ledger or worksheet. Its Dragnet review
integration is loaded only with Poindexter, so Dragnet builds independently. This
is static build selection, separate from opt-in shared-library SDK loading.
Changing `PLUGINS` in the same `BUILD` directory invalidates the composition and
native configuration. `make test` runs the complete default-profile suite;
`make test-plugins` separately verifies a clean source tree with no plugins,
an independently added built-in, native SDK loading, standalone Dragnet extraction
and collection replay, and switching profiles in the same build directory.
It checks native symbols, linked libraries, commands and extension-event admission.

## Discover, capture, and extract

```sh
./shylock discover google 'lake cottage' tests/fixtures/google.html
./shylock discover bing 'lake cottage' tests/fixtures/bing.html
./shylock discover-many plugins/dragnet/offline-job.json
./shylock extract tests/fixtures/rental.html https://example.org/cottage \
  1760000000 tests/fixtures/rule.json
```

Omit the SERP snapshot argument for a live ElviScrape request. Use
`./shylock capture HTTPS_URL > listing.html` to snapshot an individual
listing. Supply its URL and capture timestamp when extracting. Search results
are returned as URLs for selection by the caller. A provider with changed
markup can produce `no_parsed_hits`. Live providers may block programmatic
requests. Set `SHY_PROVIDER_ROOT` to select another trusted directory of
`.elvs` files.

The upgraded Elvi parser filters inert/RCDATA markup, handles malformed heading
boundaries and ambiguous Google wrappers conservatively, and commits results only
after bounded parsing succeeds. Discovery records retain `parser_profile` and the
actual MuLIB revision. Exact `discover-replay` requires the recorded revision and
profile; retain the original executable for historical replay, or rediscover saved
HTML under this build to create new evidence. Existing CLI commands and hit fields
are unchanged. Search transport still uses Shylock's DNS-pinned curl adapter, not
MuLIB's optional TLS transport.

Dragnet now combines On1x provider/outcome policy and batch admission with C11
MuLIB adapters. Discovery retains the exact upstream provider program and
response; `discover-replay` verifies it without network access. `snapshot` stores
timestamped, content-identified page JSON, and `extract-snapshot` validates and
processes that source. Live transport bounds DNS/I/O, rejects private addresses,
pins public IPv4 answers and disables proxies/redirects while keeping TLS checks.
See [Dragnet commands, extraction versions and trust contracts](manual/dragnet.md).

Deterministic extraction walks JSON-LD offer paths, scans visible text for
ISO-currency amounts and explicit English rate labels, and optionally uses a
host-specific JSON rule that reads an agreed `data-*` attribute. Every
candidate has a stable evidence ID, snapshot byte range, span digest, extractor
version, currency scale (or null), and explicit unresolved fields. JSON-LD also
records a JSON Pointer relative to its script body. Site rules retain their
exact source and digest for replay. Version-2 rules scope attribute extraction
to a named offer container; see `tests/fixtures/scoped-rule.json`. Existing
unversioned rules remain readable as page-scoped version-1 rules. The tokenizer,
currencies and text patterns are bounded; this is not a full browser DOM.

An optional fifth extract argument takes an externally produced model reply.
Pass an empty fourth argument to use the reply without a site rule. Its schema
is `{"model":"model-id","blake3":"snapshot digest","suggestions":[{"offset":0,"quote":"exact capture bytes","amount":"180.00","currency":"EUR"}]}`.
Offsets are zero-based byte positions. The adapter checks the digest, exact
quoted bytes and amount presence. A matching quote establishes provenance.
Model interpretation remains review-only. Invalid or unreadable model replies
leave deterministic candidates available and add a `model_reply_invalid`
diagnostic instead of failing the entire extraction. Generate replies with the
model client of your choice, or use the explicit MuLIB OpenAI workflow below.
Ordinary extraction never calls a model implicitly.

## Browser automation, flat page IR and OpenAI

```sh
./shylock browse plugins/dragnet/browser-job.json > browser.json
./shylock flatten browser.json > page-ir.json
./shylock find page-ir.json prices
./shylock find page-ir.json parking
./shylock openai-account plugins/dragnet/openai-profile.json
```

The pinned MuLIB browser engine supports **static HTML** navigation, link
following, DOM reads and assertions—not JavaScript execution or Chromium.
On1x admits complete bounded jobs; C11 owns browser lifetimes and same-origin,
DNS-pinned acquisition. Saved-page jobs remain fully offline.

MuLIB HTML/readability kernels produce a flat, noise-filtered block IR retaining
the immutable page and original byte ranges. Local `find` scans canonical
currency/price tokens or literal target text without a model. Unknown currency,
rate scope and qualifiers remain explicit, and every result requires review.

For DOM-scoped deterministic scraping with declarative rules:

```sh
./shylock find-rule snapshot.json plugins/dragnet/rules/rental.json rental_price > found.json
./shylock find-rule-replay found.json
```

NaiveFinder keeps listing cards, nightly rates, totals and fees separate, supports
attributes/regexes and explicit locale policies, preserves source mappings, and
retains duplicate evidence and material conflicts for review. On1x owns rules,
classification, ranking and reconciliation; C11 adapts MuLIB primitives. No model
or network is required. See [the rule, replay and review manual](manual/dragnet-naive.md)
for supported selectors, strict limits and explicitly authorized model modes.

To connect an **OpenAI Platform API account**, set the environment variable named
by the profile through your secret manager; never put a key in the profile or
repository. Configure a model available to that account. `openai-account` reports
local configuration only, not successful server authentication. Then explicitly:

```sh
./shylock model-extract page-ir.json plugins/dragnet/openai-profile.json prices --live
```

Set the standard `OPENAI_BASE_URL` environment variable to use a trusted
Responses-compatible service instead of `https://api.openai.com/v1` (the default
when unset or empty). Include the complete base path; `/responses` is appended.
For example, `OPENAI_BASE_URL=https://gateway.example/v1`. Public HTTPS/address
and certificate checks still apply; local HTTP endpoints are not enabled.
The selected service receives the configured key and page content, and its URL
is retained in the audit. Never put credentials in the URL.

This sends content to the selected service and can incur API charges. MuLIB's `openai-client`
performs the Responses API request with structured output; Dragnet validates exact
source quotes before importing review-only suggestions. Credentials stay out of
audits. Failures retain deterministic results and diagnostics. API accounts are
not ChatGPT browser logins/subscriptions. See [browser/IR/model contracts, limits,
security and account setup](manual/intelligence.md).

## Verified review to recommendation

For offline **discovery → collection → explicit review → ledger forecast/pricing**,
see [Dragnet pricing integration](manual/dragnet-pricing.md). `collect`,
`collect-replay`, `review-collection` and `replay-collection` retain source links.
`forecast-ledger` and `price-ledger` use current reviews with explicit freshness/
coverage gates, without hand-authored arrays or temporary cohort files. Try:

```sh
python3 examples/offline-dragnet.py /tmp/opencode/dragnet-example
```

```sh
./shylock extract tests/fixtures/rental.html https://example.org/cottage 1000 > first.json
./shylock review prices.db first.json 1 cottages-eur alice \
  tests/fixtures/rental.html tests/fixtures/context.json
./shylock extract tests/fixtures/rental.html https://example.org/cottage 1060 > second.json
./shylock review prices.db second.json 1 cottages-eur alice \
  tests/fixtures/rental.html tests/fixtures/context.json
./shylock cohort prices.db cottages-eur EUR 1000 60 2 > cohort.json
./shylock recommend cohort.json 1 formulas/weekend.lua 1 1000 10000 30000 100
```

Verified review replays extraction against the original HTML, including saved
rule/model inputs, before storing immutable evidence in MuLIB KVDB. Reviewers
explicitly supply listing identity, stay dates, guests, taxes and fee scope.
Identical listing claims in one sample bucket corroborate rather than overweight
the mean. Conflicting amounts block export until explicitly reviewed. Reviews
are append-only with optimistic revision IDs; `reject`, `revise`, `history` and
`replay` expose those transitions. Cohort exports carry their review lineage
through forecasting and formula decisions. See [Poindexter](manual/poindexter.md)
for the schema, commands, compatibility and limitations.

The core engine now labels chronological cutoffs at the end of complete training
buckets, not before their retained captures. File-based `price`, like
`price-ledger`, rejects sampling intervals still open at `as_of`. Forecasts and
decisions fail closed on incomplete construction, stale cached results and
serialization/publication failures. New forecast/backtest artifacts add
`cutoff_policy`; corrected timestamps/metadata change derived IDs, without a
ledger migration or new dependency. See [pricing contracts](manual/pricing.md).

## Poindexter TUI and GUI

```sh
./shylock tui prices.db cottages-eur alice
./shylock gui prices.db cottages-eur alice
./shylock sheet prices.db cottages-eur
```

Both frontends share the versioned worksheet model and On1x controller. Rejected
or incomplete reviews can resolve unknown currency/basis/qualifier and stage
explicit conflict rationale, while quoted amounts and known source meaning stay
protected. Provenance columns retain Dragnet collection links. Event scripts are
fully preflighted before actions, and editor limits never silently truncate a
review. Valid documented v1 scripts and ledger files need no migration. Use
arrows to select a cell, Enter to edit context, `n` to enter a review reason,
and `s` to publish a new review revision. `/` filters, `o` sorts, `r` refreshes,
`d` discards staged edits and `q` quits. Evidence, monetary amounts, currency,
scale and provenance are read-only. Changes are staged until saved, validated
by the existing review policy, and protected against stale review heads.
The GUI also supports mouse selection and toolbar buttons. No database lock is
held while waiting for input. This is a reviewed-price worksheet, not a general
accounting workbook. See [worksheet controls and scripted rendering](manual/worksheet.md).

## Effectful Mune extensions

```sh
SHY_EXTENSIONS=extensions/example.json ./shylock estimate tests/fixtures/series.json 1
./shylock extension extensions/example.json decision.created decision.json
./shylock extension-replay extensions/monitor.json extensions/events/example.json
```

Extensions are opt-in and loaded independently of native extractor plugins.
Host configuration grants explicit `log`, `kv.read` and `kv.write` capabilities;
manifests cannot grant themselves authority. Each extension has a separate,
bounded Mune state with `on_load`, subscribed `on_event` and `on_unload` hooks.
KV access is restricted to a configured store and extension-specific namespace.
Per-hook writes and logs are staged, with failed hooks discarded and diagnosed.
Hooks cannot change a forecast, review, worksheet cell or formula environment.
Formula restrictions and `adjust_rate/v1` remain unchanged. See
[the extension contract](manual/extensions.md) for grants, lifetime, failure
semantics and the trusted-code limitation.

Saved-event replay validates the complete batch in On1x before loading any hook,
then delivers events in one Mune lifecycle with separate per-hook transactions.
The monitor example demonstrates durable duplicate checkpoints and formula-error
counters without granting price mutation. `report.created` and `demand.created`
are also available as opt-in completion events.

### Optional Linux process sandboxes

In-process execution remains the default. Enable isolated Lua execution with:

```sh
SHY_SANDBOX=process ./shylock recommend cohort.json 1 formulas/weekend.lua \
  1 1000 10000 30000 100
SHY_FORMULA_SANDBOX=process ./shylock adjust formulas/weekend.lua 18000 1 1500 10000 30000 100
SHY_EXTENSIONS=extensions/sandboxed.json ./shylock estimate tests/fixtures/series.json 1
```

`SHY_SANDBOX=process` selects isolation for formulas and configured extensions;
`SHY_FORMULA_SANDBOX=off|process` overrides just formulas. A trusted extension
configuration entry can independently select `"sandbox":"off"` or `"process"`.
Manifests cannot change this host policy.

Workers use a separate exec image, a cleared environment, closed inherited
handles, Linux seccomp syscall restrictions, resource limits and deadlines.
They cannot directly access files, networks, other processes or extension stores.
The parent validates and brokers permitted KV/log effects and owns commit/rollback.
Missing workers, unavailable isolation, policy violations and timeouts fail closed,
never by silently running the code in process. See [sandbox profiles and limits](manual/sandbox.md).

## Forecast and adjust

```sh
./shylock estimate tests/fixtures/series.json 2
./shylock backtest tests/fixtures/series.json 1 2
./shylock recommend tests/fixtures/series.json 2 formulas/weekend.lua \
  1 1000 10000 30000 100
./shylock adjust formulas/weekend.lua 18000 1 1500 10000 30000 100
./shylock optimize tests/fixtures/demand.json 10000 22000 100 10000
```

A series is a JSON array of equally spaced prices in one cohort, oldest
first, expressed as integer minor units in a single currency and scale.
Legacy arrays remain readable; versioned `cohort` exports additionally validate
their schema and integrity digest. Shylock reports a robust market median and a
damped Holt forecast. The horizon counts sample intervals. `recommend` uses
the forecast rounded to the nearest minor unit as the formula reference.
`optimize` separately scores supplied estimates of demand at candidate rates.
Its JSON array takes `rate_minor` and `expected_units` in each row. It uses
MuLIB's expected-profit objective and integer ticks.

`backtest` evaluates the same versioned damped-Holt estimator using expanding
chronological training prefixes, never future samples. It reports coverage,
MAE, RMSE, bias and per-cutoff outcomes. These are asking-price forecasts, not
booking-demand estimates or guaranteed achievable prices.

An `adjust_rate/v1` Lua chunk returns one function taking
`(reference_minor, weekend, premium_bp)` and producing one integer minor-unit
amount in the same currency and scale. On1x applies the allowed bounds and
tick grid. The formula uses a fresh Mune interpreter with globals cleared and
parser, memory, call and work limits. Use Shylock formulas from trusted
authors by default. The restricted in-process runtime is not an OS sandbox;
the optional process backend adds the boundary described above. Effectful extensions run through their own
lifecycle and permissions, never this formula environment. Formula failures return exit 2 and
preserve the reference and any preceding forecast in the result.

```sh
./shylock formula-test formulas/weekend.lua formulas/cases/weekend.json
./shylock formula-test formulas/weekday-discount.lua formulas/cases/weekday-discount.json
./shylock formula-test formulas/capped-weekend.lua formulas/cases/capped-weekend.json
```

On1x owns the case-suite policy and shares adjustment bounds/ticks with `adjust`
and `recommend`. Every case uses a fresh restricted Mune state, including in
the optional process backend. Results retain inputs, source/runtime provenance,
expected errors and pass/fail outcomes. See [formulas and examples](manual/formulas.md).

### Booking-outcome demand

```sh
./shylock demand tests/fixtures/booking-outcomes.json 1800000000 10 2 > demand.json
./shylock optimize demand.json 10000 20000 100 2000
```

The On1x demand policy derives per-rate booking shares from explicit finalized
listing-night outcomes, with a chronological cutoff, minimum sample requirement,
corroboration and conflict handling. Versioned artifacts retain source records
and replay before optimization; results preserve their demand lineage. Asking
prices and calendar unavailability are never treated as bookings. Legacy demand
arrays remain supported. See [the demand contract and limitations](manual/demand.md).

The build now bootstraps `src/ppx/fields.on1x`, a native On1x PPX using upstream
`std.ppx`, for closed JSON schema checks. Hygienic macros implement shared demand
guards and member emission; C remains the ownership/numeric-kernel adapter.
Runtime dependencies are unchanged, with no upstream patch. Linux allocation
fault tests require GNU/LLD linker `--wrap` support.

## Print Report (LaTeX)

```sh
./shylock adjust formulas/weekend.lua 18000 1 1500 10000 30000 100 > decision.json
./shylock print-report reports/decision.json decision.json > decision.tex
```

The On1x renderer takes a versioned JSON template, selects nested scalar fields
using JSON Pointers, and produces an escaped LaTeX article using MuLIB's `latex`
kernel. Supplied templates cover decisions, forecasts and formula-test summaries.
Validation/render failures produce no partial article. Literal values cannot
inject raw TeX, run formulas or load modules. A successful report can emit a
metadata-only extension event.

Output is `.tex` source, **not PDF or a physical printer job**. TeX compilation
and printing are explicit external steps; generating reports needs no TeX
installation or new external runtime dependency. The GNU Make helper still owns
one MuLIB implementation translation unit; no upstream patch is required. See
[template syntax, limits and printing](manual/reports.md).

## Evaluated pricing policies

```sh
./shylock forecast tests/fixtures/series.json policies/forecast.json
./shylock forecast tests/fixtures/seasonal-history.json policies/seasonal.json
./shylock optimize-model policies/model.json
./shylock quote policies/quote.json
./shylock price cohort.json policies/pricing.json
```

On1x now owns configurable model admission, rolling windows, disjoint chronological
selection/holdout evaluation, empirical error bands, movement/margin/tick guards,
tax/fee scope and a unified cohort-to-price workflow. C adapters call MuLIB
histprice, statsprice, priceopt and checked money kernels and retain audit owners.
Forecast policies support EMA, Holt, seasonal Holt-Winters, Kalman, AR(1), robust
trend and last/median baselines. Scenario optimization supports caller-specified
linear, isoelastic and logistic demand, weights and explicit risk aversion.

`price` requires a reviewed cohort with matching stay/currency/tax/fee context;
optional demand artifacts are replayed before use. Forecast, formula adjustment,
demand recommendation and stay quote remain distinct and auditable, including on
failure. Output is a proposal, not an automatic rate publication. Objective scores
use quoted-nightly-rate units, not after-tax accounting profit. The `as_of` checks
capture/finalization timestamps, not historical availability of review revisions.
Legacy pricing commands and formats remain compatible. See [pricing contracts,
limits and examples](manual/pricing.md); `reports/pricing.json` and
`reports/evaluated-forecast.json` typeset successful new results. No new external
dependency or upstream interface migration is required.

For a complete synthetic saved-page workflow, creating a new audit directory:

```sh
python3 examples/offline-pricing.py /tmp/shylock-pricing-example
```

It refuses to overwrite an existing directory and uses no network or model service.

## Plugin boundary

`include/shylock-sdk.h` is a standalone C11 SDK with version/size negotiation,
copied typed output, host allocation/digest callbacks and explicit load/extract/
unload lifetimes. `SHY_NATIVE_PLUGINS` opts into a versioned configuration of
trusted shared libraries. Failed loads roll back the batch; a failed sink callback
poisons the entire extraction even if ignored by the plugin. Configuration and
its digest remain in capture/evidence provenance. No MuLIB implementation macros
or executable-exported host symbols are needed by a plugin.

Build the example with `make build/fixed-price.so`. See [SDK configuration,
ownership, examples and ABI migration](manual/sdk.md). Existing compiled extractor
ABI v2 remains supported. `./shylock plugins` lists both kinds. **Native libraries
are trusted code, not sandboxed extensions**; loading can execute constructors.
Formula/process isolation does not sandbox them. Automatic offer acceptance is
not provided; Poindexter storage remains a separate compiled adapter.

## Poindexter progress

Poindexter now supports verified captures, typed review context, immutable review
revisions, explicit conflicts, cross-capture corroboration and provenance-bearing
cohorts. On1x owns admission and comparability policy; C11 owns JSON/KVDB lifetimes.
Legacy review, ledger and series inputs remain supported, with unverified
provenance explicitly distinguished from the verified workflow.

This implements the next evidence/review/pricing milestones, not the entire
`AGENTS.md` roadmap. Poindexter now has a shared worksheet TUI/GUI and effectful
Mune extensions with scoped log/KV capabilities. Dynamic native extractor loading
and DNS-pinned bounded Dragnet acquisition are implemented, as are static browser
jobs, flat IR matching and opt-in direct OpenAI calls. JavaScript browser execution,
website login and policy-compliant unattended scheduling remain separate work,
not implicit permissions granted by native SDK configuration.
No live-provider checks are claimed by the offline test suite.
