# Effectful Mune extensions

Extensions are synchronous, explicitly enabled Lua hooks hosted by MuLIB Mune.
They are separate from both `adjust_rate/v1` formulas and compiled native
extractors. Loading one never expands another runtime's privileges. The current
capability set provides structured logging and namespaced KVDB storage, **not**
networking, arbitrary filesystem access, processes, browser automation, dynamic
native code or observation approval.

## Enable explicitly

```sh
SHY_EXTENSIONS=extensions/example.json ./shylock recommend cohort.json 1 \
  formulas/weekend.lua 1 1000 10000 30000 100
./shylock extension extensions/example.json decision.created decision.json
./shylock extension-replay extensions/monitor.json extensions/events/example.json
```

With no `SHY_EXTENSIONS`, ordinary commands do not load or execute extensions.
The separate `extension` command sends one saved JSON event through a configured
lifecycle and returns `shylock.extension-result/v1` with the unchanged payload
and audit records. It exits 2 on extension failure. The compiled-extractor
registry and its ABI negotiation are unaffected.

## Host grants versus manifest requests

The host configuration is a trusted, user-selected file:

```json
{
  "schema": "shylock.extensions/v1",
  "extensions": [
    {
      "manifest": "audit/manifest.json",
      "grant": ["log", "kv.read", "kv.write"],
      "storage": "extension-state.db"
    }
  ]
}
```

Paths in the host configuration are relative to that configuration file, or
explicit absolute paths. A manifest requests a subset of the allowed grants:

```json
{
  "schema": "shylock.extension/v1",
  "name": "audit",
  "script": "audit.lua",
  "capabilities": ["log", "kv.read", "kv.write"],
  "events": ["decision.created", "worksheet.saved"]
}
```

The script path is relative to the manifest. Names contain 1..64 ASCII letters,
digits, `_` or `-`, and must be unique within a configuration. Unknown contracts,
capabilities, events, duplicate names, missing files or ungranted requests fail
before execution. Unrequested grants are not installed as authority. Storage is
required only when KV capabilities are requested. At most 16 instances, a
16-KiB host configuration/manifest and 64-KiB Lua source per instance are allowed.
No directory is scanned and no source is automatically discovered or loaded.

## Lua contract

```lua
return {
  api = "shylock.extension/v1",
  on_load = function(event, payload_json)
    host.log("loaded")
  end,
  on_event = function(event, payload_json)
    local id = host.get("$event_id")
    -- At-least-once delivery: authors can checkpoint stable event IDs.
    if not host.kv_get("seen/" .. id) then
      host.kv_set("last_event", event)
      host.kv_set("seen/" .. id, "yes")
      host.log("processed " .. id)
    end
  end,
  on_unload = function(event, payload_json)
    host.log("unloaded")
  end
}
```

The chunk returns exactly one descriptor. `on_event` is required; `on_load` and
`on_unload` are optional. Handlers return no values or Boolean `true`; other
return values are errors. `on_load`/`on_unload` receive their phase name and
`"null"` payload. Effects during top-level descriptor evaluation are forbidden,
even if a Lua `pcall` catches the denial.

By default each instance owns one in-process Mune state and rooted handler closures.
With `"sandbox":"process"` in its trusted host entry, or inherited
`SHY_SANDBOX=process`, the same state instead lives in a dedicated worker.
An explicit host entry `"sandbox":"off"` selects the in-process backend.
The manifest cannot override isolation. See [the process sandbox contract](sandbox.md).
The state lives
for one CLI command or one entire interactive worksheet session, so globals
may persist **between that instance's events**. States never cross invocations,
extensions, formula evaluations or the GUI's internal widget runtime. Host
callbacks borrow arguments/results only during the call. Copied JSON payloads,
KV handles and VM roots are explicitly released; no asynchronous callback or
borrowed input survives its phase.

### Host methods

| Method | Permission and contract |
| --- | --- |
| `host.get(key)` | Read a top-level event field. Scalars map to Lua values, null/missing to nil, arrays/objects to JSON strings. `$event_id` returns the reserved stable ID. No write authority. |
| `host.log(message)` | Requires `log`; stage one <=1024-byte message, at most 32 per hook. |
| `host.kv_get(key)` | Requires `kv.read`; read a <=4096-byte string or nil. Keys are nonempty, <=128 bytes. |
| `host.kv_set(key, value)` | Requires `kv.write`; stage a <=4096-byte non-NUL string, at most 64 writes per hook. No implicit delete or arbitrary path API. |

KV keys are prefixed by `ext/INSTANCE_NAME/`. A key containing slashes or `..`
is still a literal KV key, never a filesystem path, and cannot escape that
prefix. Only the host configuration selects the storage file. Separate stores
are recommended; if a store is shared, namespaced extensions cannot access each
other's keys or ledger keys. The lock file uses the same trusted-parent-directory
contract as Poindexter. All cooperating writers must honor the advisory lock.

The retained safe Lua environment includes deterministic math, string/table
helpers and basic assertions/iteration/conversion/pcall. `os`, `io`, `package`,
`require`, `load`, `loadfile`, `dofile`, `_G`, debug/FFI/native loading, coroutine
execution and math random generators are unavailable. Capability checks occur
in the host broker, not only by hiding globals. Workers receive no store path,
database descriptor or permission-setting API. Both backends use that same broker,
with at most 256 requests per hook. A caught capability denial
still fails the phase. Formulas do not get a `host` table at all.

## Events and ordering

Supported subscribed events:

- `capture.extracted`
- `observation.reviewed` (accepted or rejected published review revisions)
- `cohort.created`
- `forecast.created`
- `decision.created` (including formula-error audit decisions)
- `optimization.created`
- `worksheet.opened`
- `worksheet.saved`
- `report.created` (successful [LaTeX report](reports.md) metadata, not its body)
- `demand.created` (completed versioned booking-outcome demand artifacts)

`on_load` runs first. Subscribed events run in order; `recommend` sends its
forecast before its decision. CLI output events and review events are deferred
until domain operations release their locks. Worksheet open/save hooks run
outside those locks, and review events are drained before `worksheet.saved`.
A healthy instance receives `on_unload` at command/session end. A failed instance
is disabled for the rest of that session and destroyed without executing further
hooks, including its unload hook. Other healthy instances continue.

Events are copies of completed results, not interceptors: a hook cannot replace
a price, alter evidence, bypass review, approve an observation or mutate the
payload. Core CLI JSON and exit codes remain unchanged if opt-in hooks fail.
Automatic hook audits go to stderr as JSON lines, alongside any ordinary CLI
diagnostics. The standalone `extension` command instead returns its audits in
the JSON result and reports hook failure with exit 2.

## Transactions and failures

Each individual lifecycle hook is one host-effect transaction. With KV access,
the host locks before open/read/modify and holds the lock through commit/close.
Successful hooks commit their KV snapshot, then publish staged logs. A parser,
runtime, return-type, capability, limit, allocation or pre-replacement commit
failure closes the uncommitted KV snapshot and discards all staged logs/writes
for that hook. Previously successful hooks remain committed. Lua globals changed
before an error are not rolled back; disabling that instance prevents their reuse.

If KVDB replaced its file but directory fsync failed, the audit says `uncertain`:
effects may be visible and durability is uncertain. This is not claimed as
rollback, and the host never retries automatically. Delivery across process
retries is at-least-once, not exactly-once. Stable IDs hash the event name and
canonical JSON payload so an author can deduplicate in the same scoped
transaction. Use versioned keys when changing an extension's effect contract.

Audit records include extension name, source hash, event ID, phase, effective
capabilities, Mune API/MuLIB revision, runtime limits, outcome, effect status,
committed logs or a diagnostic. Payloads are bounded to 1 MiB; the deferred queue
is bounded to 32 events/8 MiB. Limits leave the core result available and emit an
explicit extension diagnostic rather than dropping a forecast or observation.

The runtime uses an 8-MiB VM heap, 100000 evaluation steps, call depth 32 and
32 result values, with separate bounded parsing. Parsing/native memory is not
included in the VM heap budget. The default in-process backend remains a boundary
for **trusted extension authors**, not an OS sandbox. The optional Linux worker
backend adds address-space separation, seccomp, process limits and deadlines;
it is not a security certification, VM/container, authentication mechanism or
irreversible external-service transaction guarantee. Sandbox selection and
effective profile are included in audit records, including startup failures.

`make test` exercises API mismatch, wrong return types, runaway execution,
ungranted/denied capabilities, top-level effects, transaction rollback, namespace
isolation, concurrent read/modify/commit, lifecycle/state retention, idempotent
author checkpoints, worksheet hooks outside domain locks, and unchanged formula
restrictions while extensions are enabled.
The sandbox suite repeats lifecycle and worksheet tests through workers and
tests kernel-enforced restrictions, failed startup, malformed IPC, crashes,
deadlines, rollback and a worker attempting to forge its own permissions.

## Saved-event replay and monitor example

`extension-replay CONFIG_JSON EVENTS_JSON` delivers a saved batch in one lifecycle.
It emits `shylock.extension-replay/v1` with the unchanged batch, phase audits and
status; configuration/hook failures return 2. Events are caller assertions, not
verified imports or a way to approve observations. `SHY_EXTENSIONS` is not also
loaded for this explicit command.

```json
{
  "schema": "shylock.extension-events/v1",
  "events": [
    {"event": "decision.created", "payload": {"status": "adjusted", "adjusted_minor": 20700}},
    {"event": "decision.created", "payload": {"status": "formula_error", "adjusted_minor": null}}
  ]
}
```

On1x (`src/extensions.on1x`) checks the complete closed batch/envelopes and known
events before loading any extension, including `on_load`. Batches contain 1..32
object payloads, at most 1 MiB each, with serialized batches at most 2 MiB.
Retained audit records are additionally bounded to 8 MiB. Invalid fields, events,
types or sizes fail without lifecycle effects.

After validation: `on_load`, ordered subscribed events, then `on_unload`. Each
instance retains its own Mune state between batch deliveries. Each hook still
has a separate locked KV transaction: a later failure does not roll back earlier
commits. Failed instances receive no further hooks; other healthy instances
continue. A replay is not one atomic transaction.

`extensions/monitor/monitor.lua` demonstrates explicit capabilities, durable
versioned event checkpoints, formula-error counters and session-local delivery
counts. `extensions/events/example.json` intentionally repeats the first decision:
it counts once in storage but twice as a delivery. `extensions/monitor.json`
uses `extensions/monitor-state.db` relative to that config (ignored by git).
Choose your own trusted store rather than sharing it with production records.

```sh
./shylock extension-replay extensions/monitor.json extensions/events/example.json
SHY_SANDBOX=process ./shylock extension-replay extensions/monitor.json extensions/events/example.json
SHY_EXTENSIONS=extensions/monitor.json ./shylock adjust \
  formulas/weekend.lua 18000 1 1500 10000 30000 100
```

Stable IDs prevent the example recounting checkpointed events on replay. These
counters are audit examples, not billing or authenticated accounting. Replay and
report completion grant no `host.print`, TeX, file, network, formula-evaluation
or domain-mutation capability. `tests/calculation.py` checks replay in-process
and in workers, including state retention, duplicate IDs and pre-load validation.
