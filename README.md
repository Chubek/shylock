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
`1f78b6b595c54ae1f214f4cf013d97556881b8c8`. Requires GNU Make, Python 3,
a C11 compiler, and a C++20 compiler. The Makefile builds On1x using its
upstream bootstrap scanner, or accepts `ON1XC=/path/to/on1xc` for an existing
compiler. The CLI uses POSIX shell and process APIs. Live HTTPS uses curl for
transport and certificate validation. The fixture workflow needs no network.

```sh
./build.sh -j4
make test
./shylock help
```

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
candidate has a snapshot byte range and a method. JSON-LD additionally records
a JSON Pointer relative to its script body, and a site rule carries its hash.
The current tokenizer and set of currencies and text patterns are bounded.
Dates, guests, tax and fee scope still require review. Keep differing offers
and price bases distinct when assembling a cohort. Only reviewed comparable
observations should reach estimation.

An optional fifth extract argument takes an externally produced model reply.
Pass an empty fourth argument to use the reply without a site rule. Its schema
is `{"model":"model-id","blake3":"snapshot digest","suggestions":[{"offset":0,"quote":"exact capture bytes","amount":"180.00","currency":"EUR"}]}`.
Offsets are zero-based byte positions. The adapter checks the digest, exact
quoted bytes and amount presence. A matching quote establishes provenance.
Model interpretation remains review-only. Generate replies with the model
client of your choice. The CLI does not call a model service directly.

## Forecast and adjust

```sh
./shylock estimate tests/fixtures/series.json 2
./shylock recommend tests/fixtures/series.json 2 formulas/weekend.lua \
  1 1000 10000 30000 100
./shylock adjust formulas/weekend.lua 18000 1 1500 10000 30000 100
./shylock optimize tests/fixtures/demand.json 10000 22000 100 10000
```

A series is a JSON array of equally spaced prices in one cohort, oldest
first, expressed as integer minor units in a single currency and scale.
The caller prepares the cohort. Shylock reports a robust market median and a
damped Holt forecast. The horizon counts sample intervals. `recommend` uses
the forecast rounded to the nearest minor unit as the formula reference.
`optimize` separately scores supplied estimates of demand at candidate rates.
Its JSON array takes `rate_minor` and `expected_units` in each row. It uses
MuLIB's expected-profit objective and integer ticks.

An `adjust_rate/v1` Lua chunk returns one function taking
`(reference_minor, weekend, premium_bp)` and producing one integer minor-unit
amount in the same currency and scale. On1x applies the allowed bounds and
tick grid. The formula uses a fresh Mune interpreter with globals cleared and
parser, memory, call and work limits. Use Shylock formulas from trusted
authors. The restriction is tested against the pinned runtime and has no
separate operating-system sandbox. The effectful extension lifecycle from the
design PDF is a separate future API. Formula failures return exit 2 and
preserve the reference and any preceding forecast in the result.

## Plugin boundary

`src/shylock.h` defines the versioned snapshot-extractor descriptor. Dragnet
is compiled in through that interface. An extractor receives a borrowed
snapshot and appends JSON candidates to a host-owned array. Extend the host
registry and Makefile to add another compiled extractor. This project does
not include a dynamic SDK loader, market database, or automatic acceptance of
offers into a forecasting cohort.

## Poindexter progress

This version adds persistent reviewed observations, idempotent imports,
conflict detection, ledger output and evenly spaced cohort export. Domain
admission runs in On1x, and the native bridge uses MuLIB KVDB with writer
locking. See [the Poindexter manual](manual/poindexter.md) for commands and
review responsibilities. Formulas can now use deterministic Mune math
functions, as demonstrated by formulas/rounded.lua.

Build dependency revision for this archive is
151528b50b316aeea804ce2aebf66ef90b9e7100. The spreadsheet interfaces,
effectful extension lifecycle and dynamic plugin loader remain future work.
