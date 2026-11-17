# Scheduled Dragnet price extraction

`daemon CONFIG_JSON [--foreground|--detach|--once]` runs an Elvi query, selects
results by exact allowed hostname, captures each selected listing, and applies
the configured [NaiveFinder rule](dragnet-naive.md). It uses the existing hardened
Elvi parser and DNS-pinned HTTPS transport. All workflow policy lives in
`plugins/dragnet/dragnet_daemon.on1x`; `daemon.c` owns POSIX resources.

## Try it offline

```sh
./shylock daemon plugins/dragnet/daemon-offline.json --once
ls dragnet-runs/run-*/run.json
```

The shipped example processes saved Google and listing HTML, with their asserted
capture time, and writes reviewable prices under `dragnet-runs/`. It needs no
network or model credentials. Repeated runs create separate directories; replaying
the same saved bytes, time and rule produces the same evidence IDs.

## Configure a live query

Save this as `query.json`, adapting the query, allowed hosts, and rule to the
actual listing markup:

```json
{
  "schema": "shylock.daemon/v1",
  "provider": "google",
  "query": "lake cottage nightly price site:example.org",
  "limit": 10,
  "interval_seconds": 3600,
  "allow_network": true,
  "capture": "",
  "output": "price-runs",
  "rule": "plugins/dragnet/rules/rental.json",
  "target": "rental_price",
  "hosts": ["example.org"],
  "pages": []
}
```

All fields are required; unknown and duplicate JSON members are rejected.
Configuration and rule files are bounded to 64 KiB. Admission validates the
complete config and rule before creating output or acquiring pages.

| Field | Contract |
| --- | --- |
| `provider` | Trusted `.elvs` name, 1..64 lowercase ASCII letters/digits/`_`/`-`; e.g. `google` or `bing`. `SHY_PROVIDER_ROOT` keeps its existing meaning. |
| `query` | Nonempty query, at most 2048 bytes. |
| `limit` | 1..64 discovered hits; the Elvi program clamps it to its provider limit before requesting/parsing. |
| `interval_seconds` | 1..604800 seconds **after the previous run finishes**. The first run starts immediately. |
| `allow_network` | Boolean authorization for live search and unmapped selected pages. |
| `capture` | Saved SERP HTML path, or empty for live search. Empty requires `allow_network:true`. |
| `output` | Output directory, created if absent; its parent must exist. |
| `rule`, `target` | NaiveFinder JSON file and its exact target name. Rules select containers, price fields, currency and billing meaning. |
| `hosts` | 1..32 unique exact lowercase HTTPS hostnames. No subdomain expansion or wildcard. A host-specific rule must agree with every selected host; `host:"*"` rules can cover several hosts. |
| `pages` | At most 64 unique `{ "url":"https://...", "path":"saved.html", "captured_at":1760000000 }` mappings. URLs must belong to allowed hosts; timestamps are asserted integers in 0..253402300799. |

Every path resolves relative to the config file, including `output`. The config
and exact rule bytes are pinned at startup; restart to apply edits. Saved HTML and
provider files are read afresh each run and retained in that run's evidence.
A mapped page always uses its saved file, even with network enabled; a missing
mapped file is an error and cannot fall back to a live request.

Only discovered hits are considered, in provider order. An off-list or invalid
HTTPS host yields `skipped_host`. An unmapped hit with network disabled yields
`skipped_offline`. Selection does not fetch subresources or follow redirects.
Multiple representations on one page retain the existing conflict/grouping rules.

## Run and stop

```sh
./shylock daemon query.json --foreground
./shylock daemon query.json --detach
kill -TERM "$(cat price-runs/daemon.pid)"
```

Foreground is the default and is suitable for a service manager. `--detach`
double-forks, creates a session, redirects stdin/stdout to `/dev/null` and stderr
to `output/daemon.log`, then acknowledges initialization to the caller. Startup
acknowledgement means config, lock, PID and logging setup succeeded; it does not
mean the first query succeeded. The process retains its working directory.

The supervisor holds an exclusive nonblocking advisory lock on `daemon.pid`.
Another instance using the same output directory exits 2. Do not delete or replace
this file while running. Its contents are cleared on orderly shutdown; stale PID
text after a crash does not prevent restart because the OS releases the lock.

SIGTERM/SIGINT request graceful shutdown and are forwarded to the active run.
The current capture/extraction completes within its existing resource limits,
then later hits are skipped. Idle shutdown is immediate (within 100 ms).
Live DNS/HTTPS calls retain their existing deadlines. Saved files are trusted
local inputs and should be regular files, not blocking pipes/devices.

Runs are sequential, never overlapping. Scheduling uses a monotonic clock, with
no catch-up bursts after a slow run or wall-clock change. Each run uses a fresh
child process because the pinned On1x runtime retains text snapshots until exit.
The supervisor retains only admitted configuration. An unexpected worker exit
or storage failure stops the supervisor with exit 2; ordinary provider/page errors
are recorded and the next scheduled run proceeds after the configured interval.

`--once` uses the same pipeline and lock, without waiting: exit 0 for complete or
valid empty/no-selected-hit runs, exit 2 for failures or interruption. Persistent
foreground mode exits 0 on requested shutdown, independently of prior run errors.

## Results and provenance

Each unique `output/run-UNIXTIME-RANDOM/` directory contains:

- `config.json` and `rule.json`: exact admitted input bytes.
- `discovery.json`: the ordinary replayable discovery artifact, including retained
  Elvi source, response, parser profile, runtime revision, hits and discovery ID.
  If provider preparation/publication fails entirely, it is absent.
- `hit-N-snapshot.json`: immutable saved/live page snapshot for the zero-based
  discovery hit. Live timestamps are the local request-start time; saved timestamps
  remain the mapping's original assertion, never the current daemon clock.
- `hit-N-prices.json`: ordinary deterministic `shylock.naive-find/v1` evidence,
  retaining its snapshot, exact rule, candidates, conflicts and unknowns.
- `run.json`: written last, a `shylock.daemon-run/v1` manifest with start/end times,
  discovery ID, hit-index/URL links, artifact names and per-stage outcomes.

Files publish atomically without overwriting earlier files; each file and its run
directory are synced. `.part` files are unpublished. A run without `run.json` is
incomplete (for example after a crash, allocation limit or disk failure), and
must not be interpreted as successful. No automatic retention/deletion is applied.
The manifest is an operational index, not a cryptographic signature or a substitute
for replaying its referenced evidence.

Run statuses are `complete`, `no_selected_hits`, `discovery_error`,
`partial_failure`, `interrupted`, or `storage_error`. Entry statuses distinguish
`needs_review`, `skipped_host`, `skipped_offline`, `snapshot_error`,
`extraction_error`, `partial_extraction`, `interrupted`, and `storage_error`.
An empty artifact filename means that no corresponding complete file is available.
A successful price extraction can contain zero candidates; inspect its summary.
Legacy partial-extraction status remains independent of DOM candidates.

```sh
./shylock discover-replay price-runs/run-TIME-ID/discovery.json
./shylock find-rule-replay price-runs/run-TIME-ID/hit-0-prices.json
```

Price artifacts remain **needs review** and can enter the existing Poindexter
review workflow. The daemon performs deterministic extraction; it neither invokes
model services nor automatically accepts observations into pricing cohorts.

## Verification and build notes

This uses the existing POSIX/Linux process and filesystem dependencies; no service,
database, scheduler package or upstream patch is required. MuLIB remains pinned at
`eeb6de585303e1ea0dfd8bf2260328289b04587c`. `make test` includes `tests/daemon.py`:
closed-config admission, saved-page processing and replay, partial failures,
repeated scheduling, pinned configuration/rules, writer exclusion, detachment,
shutdown and restart. Tests use no public DNS, live provider or model calls.

The larger command catalog reached the compiler's bounded PPX wire-tree size.
The local `fields` PPX now emits compact calls to an On1x structural-check loop
in `src/ppx/fields_runtime.on1x`. Compile-time descriptor validation, fresh binding
names, single evaluation and the compiler's mandatory schema/type validation are
preserved; no upstream limits were raised. Standalone users of this local PPX must
import its runtime helper alongside the document adapter declarations.

Implementation measurement: 220 added C11 lines and 289 added On1x lines
(43% / 57%), covering daemon implementation, CLI/rule integration and supporting
PPX/runtime changes. Counts exclude headers, tests, docs, generated code and
dependencies; existing extraction algorithms are reused.
