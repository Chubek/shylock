# Optional Lua process sandboxes

Shylock supports two execution backends for user-authored formulas and Mune
extensions. `off` preserves the existing restricted **in-process** runtime;
`process` selects a separate Linux worker with OS-enforced restrictions. This
does not sandbox the whole CLI, terminal/SDL interface, acquisition transport or
the GUI's compiled-in widget script. Those remain trusted application code.

## Select explicitly

```sh
# Select the process backend for formulas and all configured extensions.
SHY_SANDBOX=process ./shylock recommend cohort.json 1 formulas/weekend.lua \
  1 1000 10000 30000 100

# Select/override only the formula backend.
SHY_FORMULA_SANDBOX=process ./shylock adjust formulas/weekend.lua \
  18000 1 1500 10000 30000 100

# The example host config opts this extension into a worker.
SHY_EXTENSIONS=extensions/sandboxed.json ./shylock estimate tests/fixtures/series.json 1
```

Unset/empty settings inherit `SHY_SANDBOX`, whose default is `off`. Both variables
accept only `off` and `process`. `SHY_FORMULA_SANDBOX` overrides just formulas. A
trusted extension **host configuration entry** independently sets its backend:

```json
{
  "schema": "shylock.extensions/v1",
  "extensions": [
    {
      "manifest": "audit/manifest.json",
      "grant": ["log", "kv.read", "kv.write"],
      "storage": "extension-state.db",
      "sandbox": "process"
    }
  ]
}
```

An omitted entry setting inherits the global selection. `"sandbox":"off"` is
an explicit host opt-out. An invalid setting fails that Lua execution instead
of being interpreted as an opt-out. Manifests and Lua code cannot change the
backend, grant authority or select an executable.

## Installation and availability

The GNU Make build produces `shylock` and a minimal companion `shylock-worker`.
The parent locates the companion beside its actual executable using Linux
`/proc/self/exe`, not the working directory or `PATH`. Keep the pair together in
a trusted installation directory. Existing `SHY_BINARY`/`BUILD` overrides work
when both files are placed in that build directory. The default in-process
backend does not need the companion at runtime.

The profile is implemented for 64-bit Linux x86-64 and AArch64, requiring kernel
seccomp filtering, `no_new_privs` and the credential/resource setup below.
Verification in this checkout ran on Linux x86-64 at MuLIB revision
`151528b50b316aeea804ce2aebf66ef90b9e7100`; AArch64 was not runtime-tested here.
No external sandbox service or libseccomp dependency is required.

Unsupported systems/ABIs, restrictive outer policies, missing or incompatible
workers and failed setup report an explicit error. There is **no automatic
fallback** after a process backend was requested. In-process operation remains
available by explicitly selecting `off`.

## `linux-seccomp/v1` boundary

Each worker is forked and **execed** into the small companion image before it
receives source or event data. It does not retain the parent's address space,
extension states, credentials in environment variables, ledger memory or GUI
connections. The launcher replaces stdin/stdout/stderr with `/dev/null`, retains
only a dedicated connected IPC descriptor, closes inherited handles and replaces
the environment with fixed locale, role and parent-identity values. Dynamic
loading of the trusted executable/libraries finishes before restrictions are
installed; no untrusted source is parsed or executed before the ready handshake.

Before accepting source, the worker:

- Drops effective/permitted/inheritable and ambient Linux capabilities.
- Sets `no_new_privs`, disables dumpability and core files, and arranges SIGKILL
  on parent death, checking the parent-identity setup race.
- Installs the seccomp architecture-checked syscall allowlist. Unknown/forbidden
  calls kill the worker; Lua cannot catch or weaken that policy.
- Retains bounded private anonymous, non-executable memory operations, ordinary
  allocator/signal/time helpers and read/write **only on the IPC descriptor**.
- Denies path/file acquisition, networking, new processes/threads, program/module
  execution, tracing/other-process memory access, descriptor transfer/duplication,
  privilege changes and signaling other processes. New executable or shared
  memory mappings are denied. The interpreter always uses Mune's tree mode,
  not JIT/native module execution.

The process limits are fixed for this version:

| Limit | Formula worker | Extension worker |
| --- | --- | --- |
| Address space, including native parser memory and loaded image | 256 MiB | 256 MiB |
| VM heap | 1 MiB | 8 MiB |
| Source | 16 KiB | 64 KiB |
| Evaluation steps | 50000 | 100000 per hook |
| Call depth / result values | 32 / 32 | 32 / 32 |
| CPU time | 2 seconds per worker | 60 seconds over the instance's lifetime |
| Parent-enforced wall deadline | 2 seconds per evaluation | 2 seconds per initialization/hook request, including broker round trips |
| Startup handshake deadline | 3 seconds | 3 seconds |
| Stack / open descriptor ceiling | 8 MiB / 4 | 8 MiB / 4 |
| File size / core / locked memory / new process limits | 0 | 0 |

Address space is not a physical-RSS/cgroup limit. The extension CPU cap is
cumulative, not reset for each event; a long-lived worksheet instance can exhaust
it. The parent owns and reaps workers, kills unresponsive workers and bounds IPC
frames and request counts. No unbounded wait for an isolated runtime is used.
Existing trusted storage-lock acquisition/commit is outside the worker deadline.

## Capability brokerage and transactions

Formula workers receive source and scalar `adjust_rate/v1` inputs, never host
capabilities. The parent checks the response type/range; On1x still applies
floor/cap/tick policy. Each formula evaluation gets a fresh worker/state.

An extension instance gets one worker for its command/worksheet lifetime,
preserving the documented state lifetime between `on_load`, events and healthy
`on_unload`. Each instance remains independent of formulas and other extensions.

The extension worker receives event data and the same restricted `host` API, but
**the parent** owns grants, instance identity, store paths, locks, namespaces and
transactions. Only four typed broker operations are accepted: event-field read,
log, KV read and KV write. No arbitrary syscall/path request is forwarded.
Requests and replies are bounded and validated independently of worker claims.
At most 256 broker requests, 64 writes and 32 log entries are accepted per hook.
The parent rejects a worker trying to forge permissions, choose another
namespace, send invalid types or ignore a capability denial.

Granted effects are staged in the parent. A completed successful hook may commit
them; failure, policy death, malformed IPC, resource exhaustion or timeout before
completion discards the pending writes/logs and disables that instance. The
worker never receives a database descriptor or a way to commit itself. A KVDB
post-replacement durability failure remains `uncertain`, not claimed rollback.
Already committed earlier hooks are not undone. Delivery/retries retain the
existing at-least-once contract and stable event IDs.

No hook runs while the core ledger lock is held. This remains true when the host
brokers calls from a worker, including worksheet saves and shared store paths.

## Failures and audit records

Formula failures preserve the reference and preceding forecast/lineage, return
exit 2 and leave `adjusted_minor` null. Decision records add `formula_isolation`,
`formula_sandbox_profile` and a diagnostic `formula_error`. A failed startup has
profile `none`, not a false claim that a filter was active.

Extension audits include `isolation`, effective `sandbox_profile`, broker request
count, source/runtime revisions, event identity and the existing effect outcome.
An optional hook failure still does not replace core CLI JSON or its exit code;
the explicit `extension` command returns exit 2 on failure. Crashes and invalid
worker results are diagnostics, never a request to rerun in process.

## Verification and limitations

`tests/sandbox_probe.c` exercises the actual kernel policy in separate native
children: IPC success; denied filesystem/network/exec/fork/tracing/process-memory,
signals, descriptor duplication/transfer, stdout/inherited-handle access,
executable/shared mappings; and address-space/CPU limits. `tests/sandbox.py`
checks result parity, configuration overrides, missing workers, bad handshakes,
malformed/oversized replies, crash/timeout rollback, forged permissions,
environment/descriptor cleanup and fresh formula state. It also repeats the
extension and real-PTY/worksheet suites through isolated workers.

The parent, in-process VM and protocol handling can be tested under ASan/UBSan.
Strict worker/probe executables intentionally omit sanitizer instrumentation:
ASan's large virtual mappings and runtime syscalls conflict with this production
profile. The same VM implementation is sanitizer-tested through the in-process
backend. No sanitizer-specific weakened sandbox profile is substituted.

This is defense in depth, **not a security certification or VM/container**.
The Linux kernel, trusted worker image/shared libraries, parent-side JSON/IPC
parsing, broker, storage kernels and their native code remain in the trusted
computing base. Workers retain the caller's UID/GID (with capabilities cleared),
and no user/mount/network namespace is created. Granted event data/log/KV access
is intentional authority, not confidential from the selected extension. Host
configuration, install directory and data-store permissions must remain trusted.
Do not treat this implementation alone as approval to run arbitrary hostile code
in a high-assurance or multi-tenant service without further security review.
