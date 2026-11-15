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
`151528b50b316aeea804ce2aebf66ef90b9e7100`. Requires GNU Make, Python 3,
a C11 compiler, and a C++20 compiler. The Makefile builds On1x using its
upstream bootstrap scanner, or accepts `ON1XC=/path/to/on1xc` for an existing
compiler. The CLI uses POSIX shell and process APIs. Live HTTPS uses curl for
transport and certificate validation. The fixture workflow needs no network.
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
  CC=clang CFLAGS='-fsanitize=address,undefined -fno-omit-frame-pointer -g'
```

The build also produces the companion `shylock-worker` executable. Keep it beside
`shylock` when installing/copying binaries if process isolation will be used.
The normal CLI does not require a worker at runtime when isolation is off.

## Discover, capture, and extract

```sh
./shylock discover google 'lake cottage' tests/fixtures/google.html
./shylock discover bing 'lake cottage' tests/fixtures/bing.html
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
model client of your choice. The CLI does not call a model service directly.

## Verified review to recommendation

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

## Poindexter TUI and GUI

```sh
./shylock tui prices.db cottages-eur alice
./shylock gui prices.db cottages-eur alice
./shylock sheet prices.db cottages-eur
```

Both frontends share the versioned worksheet model and On1x controller. Use
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

## Plugin boundary

`src/shylock.h` defines extractor ABI v2 and `src/registry.c` negotiates compiled
descriptors. Registration checks ABI, descriptor size, unique names and callbacks.
An extractor receives borrowed immutable input and appends owned candidates to a
host-owned array; failure discards all output. `./shylock plugins` lists registered
extractors. Native tests exercise a second extractor, incompatible descriptors
and callback failure. Poindexter storage is a separate compiled adapter, not an
extractor. Shared-library loading and automatic offer acceptance are not provided.

## Poindexter progress

Poindexter now supports verified captures, typed review context, immutable review
revisions, explicit conflicts, cross-capture corroboration and provenance-bearing
cohorts. On1x owns admission and comparability policy; C11 owns JSON/KVDB lifetimes.
Legacy review, ledger and series inputs remain supported, with unverified
provenance explicitly distinguished from the verified workflow.

This implements the next evidence/review/pricing milestones, not the entire
`AGENTS.md` roadmap. Poindexter now has a shared worksheet TUI/GUI and effectful
Mune extensions with scoped log/KV capabilities. Dynamic native loading, live
model calls and browser automation remain future work. Transport
still needs DNS/address-validation hardening before unattended acquisition.
No live-provider checks are claimed by the offline test suite.
