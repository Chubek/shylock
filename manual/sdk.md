# Native extractor SDK (C11)

`include/shylock-sdk.h` is the standalone public plugin interface. Plugins need
only a C11 compiler and this header: no MuLIB implementation macros, internal
Shylock headers, exported executable symbols, or second kernel implementation TU.
The current loader uses POSIX `dlopen`/`dlsym`/`dlclose` and links `libdl`.
Binaries must match the host architecture, C ABI and pointer/size representation.
This is not a portable cross-process or cross-architecture wire protocol.

## Build and use the example

```sh
make build/fixed-price.so
```

Create a configuration beside the repository root, for example `native.json`:

```json
{"schema":"shylock.native-plugins/v1","plugins":[
  {"path":"build/fixed-price.so","configuration":{}}
]}
```

```sh
SHY_NATIVE_PLUGINS=native.json ./shylock plugins
SHY_NATIVE_PLUGINS=native.json ./shylock extract example.html \
  https://example.org/listing 1760000000 '' '' fixed-price
```

`sdk/examples/fixed-price.c` extracts the exact example text `EUR 1.00 per night`.
It is intentionally small, not a general listing extractor. Dragnet remains the
default when no plugin is selected. `extract-snapshot` also accepts a plugin name.
`plugins` retains the existing extractor `abi:2` field; dynamic plugins additionally
report `sdk_abi:1` to distinguish the standalone SDK from the legacy host adapter.

Loading is opt-in, never directory scanning or automatic dependency installation.
The configuration is <=64 KiB, contains exactly `schema` and a nonempty `plugins`
array, and each entry contains exactly `path` and an object `configuration`.
Relative library paths resolve against the configuration file directory.
At most 16 total extractors, including compiled registrations, can be present.
Names are unique 1..64-byte lowercase ASCII letters/digits/underscore/hyphen.
Versions are nonempty UTF-8 strings <=64 bytes. Loading two batches concurrently
or modifying the legacy registry while a native batch is active is rejected.

## Negotiation and lifecycle

1. The library exports `shylock_plugin_query(uint32_t abi)`. Return a static
   `shy_sdk_plugin` for `SHY_SDK_ABI` (1), NULL for unsupported versions. Query
   must not perform effects or retain host resources.
2. Before any plugin callback, the loader checks ABI, structure extent, zero
   reserved flags, name/version, duplicate name and all three lifecycle callbacks.
   Unsupported/short descriptors are never invoked. `RTLD_NOW|RTLD_LOCAL` is used.
3. `on_load` receives the versioned host table, serialized configuration object
   and an initially NULL state pointer. Return exactly 1 on success. On failure,
   clean up all allocations yourself and leave state NULL. There is no matching
   `on_unload` for a failed load.
4. `extract` receives the successful instance state, versioned capture view and
   opaque sink. Return exactly 1 on success; otherwise all output is discarded.
   The same instance can service multiple extractions in one CLI operation.
5. Each successful load has exactly one `on_unload`, in reverse load order,
   followed by library closure. A later plugin failing to load rolls back earlier
   successful loads in that batch. Legacy compiled registrations survive rollback.

Calls occur synchronously on one host thread. Registry mutation/unload/extraction
re-entry during a callback is rejected. This registry is not a concurrent service
API. Plugins must not spawn work using borrowed capture/sink views. Normal CLI
teardown drains Mune extension events before unloading native extractors.

ABI v1 structures may have a larger extent, but plugins must not depend on
unnegotiated new fields or nonzero flags. Breaking changes require another ABI.
Both descriptor strings and callback code must remain valid until unload.

## Ownership and extraction transactions

- Capture bytes, URL, digest, rules and model-reply strings are immutable borrowed
  views for the synchronous `extract` call only. Byte extents exclude any NUL.
- The host function table has process lifetime. Use `allocate`/`release` as a
  matching pair for host allocations. Do not free host views or use libc `free`
  on another allocator's resources. Plugin-private resources remain plugin-owned.
- `candidate` and `diagnostic` **copy** all strings and values immediately. Stack
  values can be reused after a successful call. Never retain the sink pointer.
- A failed sink callback poisons the transaction. Ignoring that failure and
  returning 1 still causes the host to discard every candidate and diagnostic.
- Candidate ranges must be nonempty and lie entirely inside the exact capture.
  The host caps output at 256 candidates and 64 diagnostics. Diagnostic code and
  message limits are 64 and 1024 bytes; strings must be valid UTF-8.
- When `has_minor=1`, the amount must be a nonnegative integer <=1e12 and match
  canonical MuLIB money parsing at the currency's supported scale. When unknown,
  set `has_minor=0`; no zero/default currency is invented. Method/amount/basis/
  qualifier string limits are 64/128/32/32 bytes, currency <=3, pointer <=1023.
- JSON Pointers are optional; a pointer is relative to the structured document
  within the candidate's source range. The host validates extent and basic
  pointer spelling, not the plugin's semantic interpretation of the page.

The host adds capture ID, extractor version, span hash, checked scale, explicit
unresolved fields, review status and evidence ID. Dynamic captures additionally
retain serialized plugin configuration; its digest contributes to candidate IDs.
Do not place credentials in extractor configuration: it is audit provenance.
Verified review requires the same plugin/version/configuration to be available
and to reproduce the same output against the original HTML. Keep the original
library for historical replay; a version label is not executable authentication.

## Trust and separation

**Native plugins are trusted application code, not a security sandbox.** Loading
a shared library can execute constructors even before descriptor negotiation.
Rollback restores registry/lifecycle ownership, not arbitrary filesystem/network
effects. An incompatible or malicious library can still crash the process.
Only load reviewed libraries from trusted paths and protect those files against
replacement. No signature, binary attestation, hot reload, process isolation or
automatic offer acceptance is claimed.

The SDK's host table deliberately contains no formula execution, extension hook,
filesystem, network or database authority. Native code can still call OS APIs on
its own; the absence of host callbacks is not isolation. Mune formulas and
capability-scoped effectful extensions keep their independent lifecycles and
permissions. Process sandboxing for formulas does **not** sandbox a native plugin.

Legacy ABI v2 compiled extractors in `src/shylock.h` remain supported. Domain
admission, review/cohort/pricing and worksheet policy remain in On1x. The SDK and
registry implementation are C11; no upstream MuLIB change was necessary.

## Verification

`tests/sdk.py` compiles actual standalone shared libraries, checks ABI/size
rejection, duplicate/missing libraries, complete-batch rollback, successful and
failed lifecycle paths, callback poisoning, bad spans and inconsistent money.
`tests/native.c` retains second compiled-plugin and failed-extraction coverage.
The normal suite requires no internet access. See [Dragnet](dragnet.md) for the
default extraction pipeline and its independently versioned provider programs.
