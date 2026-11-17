# Built-in plugin composition

The engine in `src/` owns pricing and generic host interfaces. Built-in plugin
implementation, command handlers, help, initialization and dependencies belong
under `plugins/NAME/`. Selecting a plugin is a build-time decision. Native
extractor shared libraries use the separate [versioned SDK](sdk.md).

## Build selection

```sh
./build.sh -j4 PLUGINS= BUILD=build-core
./build.sh -j4 PLUGINS=dragnet BUILD=build-dragnet
./build.sh -j4 PLUGINS='dragnet poindexter'
SHY_BINARY="$PWD/build-core/shylock" ./shylock help
```

The default is `dragnet poindexter`. Unknown selections fail before compilation;
duplicates and order are normalized. Poindexter's manifest explicitly requires
Dragnet for verified evidence replay. Dragnet does not require the ledger or UI.
A plugin-free engine builds even when the entire `plugins/` directory is absent.
It retains pricing, formulas, effectful extensions, native SDK loading and
`extract` with an explicit extractor name. Dragnet supplies the legacy default
extractor and provider-root fallback.

Generated `BUILD/main.on1x` composes the engine with selected plugin CLI modules.
Generated `BUILD/builtin_plugins.c` composes extractor initialization and plugin
event declarations. These files are build products, not source files to edit.
Changing selection regenerates composition and invalidates native configuration;
repeating an unchanged build preserves timestamps and avoids recompilation.

## Adding a built-in

Choose a name valid as an On1x identifier, and add `plugins/NAME/plugin.mk` and
`plugins/NAME/cli.on1x`. A manifest appends to the following variables using `+=`:

| Variable | Meaning |
| --- | --- |
| `PLUGIN_ON1X` | On1x source prerequisites; CLI imports define the compiled module graph |
| `PLUGIN_NATIVE` | C11 adapter sources |
| `PLUGIN_HEADERS` | Adapter and upstream headers that should trigger rebuilding |
| `PLUGIN_KERNELS` | Canonical MuLIB kernels passed to the upstream build helper |
| `PLUGIN_FLAGS` | Native compile/link flags and external dependencies |
| `PLUGIN_INITIALIZERS` | `path/to/header.h:function` pairs for extractor registration |
| `PLUGIN_EVENTS` | Additional allowed extension event names |
| `PLUGIN_ALLOCATION_NATIVE` | Native adapters used by the combined allocation test harness |

Paths are relative to the repository root. Keep external-library detection and
plugin-to-plugin dependency checks in the manifest. Adapters include kernel
declarations only; the upstream build helper owns the single implementation
translation unit. Declaring kernels here also supplies the allocation-test build.

The CLI module imports its implementation and defines:

```text
fn NAME_command(command:string,count:i64,a:string,b:string,c:string,d:string)->i64
fn NAME_help(topic:string,color:bool)->bool
```

The launcher supplies the command's first four arguments directly, with remaining
arguments available as `SHY_5` through `SHY_8` via the CommandLine standard library.
Return `-1` when unhandled, or a nonnegative CLI exit status when handled. A handled
failure must retain its exit status. Help receives an empty topic for the command
index, or an exact command name for a detailed page; return true when handled.
Use the shared On1x `help_entry(topic,color,name,usage,summary,arguments,notes,
examples,manual)` renderer for consistent styling. It prints an index row and
returns false for an empty topic, so a sequence of entries can render the whole
index. Each detailed page should include at least three invocation examples.
Help handlers must be pure output: they run before native initialization and
extension hooks, even if the runtime configuration is invalid.
The generated composition calls handlers in normalized plugin-name order before
the core dispatcher. Plugins should use distinct command names.

An initializer has signature `int function(void)`, returns 1 on success, and must
be idempotent: the registry can request initialization more than once. Extractor
descriptors and their strings remain valid for the process lifetime. Use
`shy_plugin_register` with the descriptor contract in `src/shylock.h`; the registry
rejects incompatible ABIs. Never duplicate registration logic in the core.

Event names are build-time declarations, available to manifest validation and
replay preflight without loading plugins or running extension hooks. Emission
uses the generic extension API in `src/extensions.h`; its queue copies borrowed
JSON and drains only after callers release domain locks. Event declarations do
not grant capabilities or expand the formula environment.

## Verification

Run `./build.sh -j4` and `make test` for the complete default-profile integration,
native, allocation-failure, formula and process-isolation suites. Run
`make test-plugins` for isolated core-only, new built-in and Dragnet-only builds,
SDK extraction, event rejection before effects, saved collection replay, native
symbol/library boundaries and profile switching. These checks are offline and
use MuLIB revision `eeb6de585303e1ea0dfd8bf2260328289b04587c`.
