# AGENTS.md — Shylock

Shylock is an extensible price prediction engine, implemented using the [Mulib](https://github.com/Chubek/mulib) 
spatio-temporally composable infrastructure. Mulib offers several 'emulsifiers'
for its kernels and easy, safe programming (the 'space' part in spatio-temporal),
at the moment, it offers MuLL, a very basic, terse imperative language, and On1x,
a large, typesafe ML-like language with a vast standard library and several
metaprogramming methods. Shylock chooses to use MuLL in a limited amount,
but uses On1x (which is created using the author's own [libstone](https://github.com/Chubek/libstone))
compile infrastructure) for the heavy tasks.

Shylock has several means of extensibility. It ships with several plugins.
One of these plugins is Dragnet. This plugin allows you to search the web
using Google, Bing and other search engines (powered by Mulib's Elvi-Scraper
engine, it must be noted that I condone Julian Assange's moral bankrupty, however,
Elvi-Scraper is based on Surfraw) and finds the information and prices that
you want, and it uses several means to extract them, including LLMs through
OpenAI's API). 

Dragnet also offers several other features. Using Mulib's headless browser
engine, it can automate tasks, to be utilized beyond good or evil.

Another plugin shipped alongside Shylock is Poindexter. This plugin
leans more towards storage of price information, and may use Shylock's
custom formulae, which are written in Lua, to compute, for example, taxes.

**Note**: Poindexter is not a proper accounting software, and is not approved
by any state or entity.

Poindexter offers both a TUI and GUI spreadsheet, powered by Mulib kernels.

Head to [manual](./manual) to learn what Shylock is about!

## Project contract

Shylock is an extensible On1x engine for forecasting and adjusting prices.
The first market is short-stay vacation rentals. Dragnet is the first plugin.
It discovers listings using MuLIB ElviScrape and extracts evidence from
captured pages. Deterministic extraction must remain usable offline and without
a model. User-authored formulas run through MuLIB Mune under a calculation
contract. Effectful Lua extensions have a separate lifecycle and permissions.

Use On1x for domain types, orchestration, cohort policy, and pricing rules.
Use its upstream standard library first. Reuse canonical MuLIB kernels for
parsing, money, statistics, forecasting, optimization, and Lua. Put ABI
adaptation in C11. Keep ownership and lifetime explicit at each call. Use
reviewed Exotract or ExoML bindings as the upstream compiler supports them.
The current scalar bridge is a trusted C ABI adapter.

## Build and source map

The upstream checkout belongs at `third_party/mulib`. The delivered ZIP was
tested at MuLIB commit `1f78b6b595c54ae1f214f4cf013d97556881b8c8`. Record the
actual revision when retesting against a new checkout. The root GNU Makefile
and `build.sh` are the public build interface. The Makefile bootstraps the
On1x compiler from upstream or accepts `ON1XC=/path/to/on1xc`. Preserve the
upstream build helper's single MuLIB implementation translation unit. Bridge
and plugin sources include declarations only. Keep changes to Shylock local
unless the task explicitly calls for an upstream patch.

```sh
./build.sh -j4
make test
./shylock help
```

| Path | Responsibility |
| --- | --- |
| `src/main.on1x` | CLI workflow, stdlib argument parsing, and adjustment bounds and tick grid |
| `src/bridge.c` | Native file, digest, JSON, pricing-kernel, and Mune adapters |
| `src/shylock.h` | Versioned snapshot-extractor descriptor and borrowed capture contract |
| `plugins/dragnet/dragnet.c` | ElviScrape discovery, HTTPS adapter, deterministic extraction, and model reply import |
| `formulas/weekend.lua` | Example `adjust_rate/v1` formula |
| `tests/test.py` | Executable CLI integration tests using saved pages and a stubbed transport |

The current CLI supports `discover`, `capture`, `extract`, `estimate`,
`recommend`, and `optimize`. Dragnet is linked at build time. Live HTTPS
currently uses curl. The launcher hands arguments to the generated executable
through the environment because the pinned On1x entry point takes no argv.

## Development priorities

### 1. Connect evidence, review, and pricing

Implement a versioned pipeline from immutable capture to candidate, reviewed
observation, cohort, forecast, and decision. Make the status transitions
explicit. Keep existing CLI inputs readable during schema evolution. Add an
offline workflow that moves saved listing quotes through review and into
`recommend` without a hand-authored price array.

- Keep source URL, capture time, digest, byte range or JSON Pointer, extractor
  version, rule digest, and any review decision. Never overwrite a published
  observation when an offer changes. Make replay and retries idempotent.
- Represent amount as checked integer minor units with currency and scale.
  Carry listing identity, stay dates, guest count, rate basis, taxes, fees,
  and qualifiers independently. Preserve unknowns. A nightly rate, stay
  total, cleaning fee, and “from” price have distinct meanings.
- Reconcile claims for the same listing and stay by field. Keep disagreeing
  evidence and surface a review outcome. Treat repeated extraction of the same
  claim as corroboration. Select comparable reviewed observations explicitly.
- Acceptance: replay a fixture with competing amounts, duplicates, and a
  missing currency. Verify stable evidence IDs, explicit unresolved fields,
  and equivalent cohort output on a repeated run.

### 2. Expand Dragnet extraction and transport

Use `.elvs` programs for provider discovery. Keep listing rules independently
versioned and scoped to the host and offer container. Extend structured offer
walking, markup attributes, and evidence-preserving text extraction as fixtures
justify them. Handle localized separators, symbols, and split markup without
guessing a currency or time basis. Keep capture distinct from processing so
extractors can replay the exact input.

The model import currently checks a capture digest and exact quoted bytes and
marks suggestions for review. Preserve that contract while adding optional
live model integration through MuLIB's OpenAI-compatible client. Make model
calls opt-in. Validate output against the original capture and retain model
and request provenance. Model errors should leave deterministic results
available with an explicit diagnostic.

Before adding unattended acquisition, make transport behavior cover timeouts,
response limits, redirect policy, and address validation. Preserve certificate
and hostname verification. Track provider failures, parse drift, and empty
results separately where the evidence supports it. Test deterministically with
saved responses and injected transports. Report live checks separately.

### 3. Evolve formulas, extensions, and the SDK

Keep `adjust_rate/v1` compatible. It receives reference minor units, a weekend
Boolean, and premium basis points. It returns one bounded integer. On1x
validates and applies the floor, cap, and tick. Formula execution uses a fresh,
restricted Mune state with bounded parsing and evaluation. Preserve the
unadjusted forecast on error. Log the contract, source hash, runtime revision,
inputs, constraints, and outcome. Keep the formula environment and bindings
independent of extension privileges.

Add effectful Mune extensions through explicit events and scoped host
capabilities. Define ownership, error semantics, and state lifetime for each
event. Add extension loading separately from native SDK loading. Evolve the
compiled extractor descriptor into a versioned registry before adding
shared-library loading. Define ABI negotiation and ownership, then test a
second plugin with the same host.

Acceptance: a formula cannot invoke an extension hook, load a module, or
retain globals across evaluations. The test suite should continue rejecting
wrong return types and runaway execution and should verify clean failure on
plugin API mismatch. The existing restricted state is a runtime boundary for
trusted formulas. If supporting untrusted authors, add and test process
isolation separately.

### 4. Improve estimation and decision quality

Use `statsprice` for market references and `histprice` for reviewed,
equally spaced histories. Version cohort construction, sampling intervals,
missing-data treatment, and estimator parameters. Backtest using chronological
cutoffs and report evaluation coverage and error before adding more models.
Keep market observations, forecasts, formula-adjusted rates, and
demand-optimized recommendations distinct in types and output.

`optimize` currently takes externally supplied expected demand at each
candidate rate. Use `priceopt` when the demand input is supported. Derive
demand from suitable booking or availability evidence and document its
limitations. Asking-price observations alone describe supply-side quotes.
Test constraints at tick boundaries and preserve provenance for both demand
inputs and final recommendation.

## Verify before handing off

Run a clean build and `make test` against the pinned checkout or record the
replacement revision. Add fixture coverage for each changed behavior. Verify
CLI exit codes, stable JSON fields, capture replay, and decision audit records.
For bridge changes, test with the available C compiler diagnostics and check
allocation and cleanup on parse, limit, and callback failures. Document any
external dependency or interface migration in `README.md`. When delivering an
archive, build and test it from a clean extraction with MuLIB placed at the
documented path.

## Implemented progress in this archive

The repository gitlink resolves to MuLIB
151528b50b316aeea804ce2aebf66ef90b9e7100. The four proposed money-text,
webextract-offer, price-quote and price-backtest headers are absent in that
revision. Inspect the actual checkout before adopting those interfaces.

Poindexter now provides review, ledger and series commands. Domain admission
lives in src/poindexter.on1x. plugins/poindexter/store.c owns KVDB and JSON
lifetimes. Read manual/poindexter.md for the exact persistence, conflict,
currency, gap, and reviewer contracts. The writer lock must cover open,
read/modify and commit. Preserve idempotent imports and explicit conflict
results. Keep the existing discovery and pricing CLI tests passing.

The next priorities are richer typed observation context, review revisions,
cross-capture deduplication, then a spreadsheet data model and TUI/GUI views.
Carry currency scale explicitly when the ledger admits new parsers. Replace
trusted extraction-file provenance with verified capture references. Add
cohort lineage to forecast records. Use incremental snapshots or another
MuLIB storage kernel if full KVDB snapshots become a measured bottleneck.

Formulas now retain Mune math functions while removing random generators.
They still use fresh restricted states. Add tests whenever upstream Mune
changes its global environment. Implement effectful extensions with explicit
capabilities and lifecycle hooks. Native dynamic plugins, direct LLM calls,
and browser automation need independent implementations and verification.

Use ./build.sh -j4 and make test. Preserve the GNU Make build. The new
Poindexter suite covers the offline review-to-recommendation flow. Document
live-provider checks separately.

## The Use of On1x Metaprogramming Features

The agent is encouraged to use On1x's metaprogramming features. Hygenic macros
and PPX. Any PPX must be in `src/ppx`. Write the PPX in On1x itself using On1x's
`ppx` stadnard library.

## The use of On1x Standard Library & Standard PPX

The agent is encouraged to use On1x's standard library and standard PPX library.
