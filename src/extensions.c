#define _POSIX_C_SOURCE 200809L
#include "extensions.h"
#include "mune_vm.h"
#include "sandbox.h"
#include "mulib-kvdb.h"
#include "mulib-mune.h"
#include <sys/file.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { CAP_LOG = 1, CAP_READ = 2, CAP_WRITE = 4, MAX_EXTENSIONS = 16,
    EVENT_LIMIT = 1024 * 1024, QUEUE_LIMIT = 8 * 1024 * 1024 };
typedef struct {
    char *name, *storage;
    char source_hash[65], failure[SHY_VM_ERROR];
    unsigned capabilities, hooks;
    mulib_json_t *events, *logs;
    shy_extension_vm *vm;
    shy_process *worker;
    mulib_kvdb_t *transaction;
    const mulib_json_t *payload;
    const char *event_id;
    int active, enabled, initialized, isolation, sandbox_active;
    size_t writes, calls;
} extension;
static extension extensions[MAX_EXTENSIONS];
static size_t extension_count, queued_bytes;
static mulib_json_t *reports, *queue;
static int explicit_command, failures;

static const char *const event_names[] = { "capture.extracted", "observation.reviewed", "cohort.created",
    "forecast.created", "decision.created", "optimization.created", "worksheet.opened", "worksheet.saved" };
static int event_valid(const char *event)
{
    for (size_t i = 0; i < sizeof event_names / sizeof *event_names; ++i)
        if (!strcmp(event, event_names[i])) return 1;
    return 0;
}

static char *relative_path(const char *file, const char *path)
{
    if (!*path || strlen(path) > 4096) return NULL;
    if (*path == '/') return strdup(path);
    const char *slash = strrchr(file, '/');
    size_t base = slash ? (size_t)(slash-file+1) : 0, n = strlen(path);
    if (base+n > 4096) return NULL;
    char *out = malloc(base+n+1);
    if (out) { memcpy(out, file, base); memcpy(out+base, path, n+1); }
    return out;
}

static mulib_json_t *read_json(const char *path, size_t limit)
{
    size_t n;
    char *data = shy_read(path, limit, &n);
    mulib_json_t *out = data ? mulib_json_parse(data, n, NULL) : NULL;
    free(data); return out;
}

static int capability(const char *name)
{
    if (!strcmp(name, "log")) return CAP_LOG;
    if (!strcmp(name, "kv.read")) return CAP_READ;
    if (!strcmp(name, "kv.write")) return CAP_WRITE;
    return 0;
}

static int capabilities(const mulib_json_t *array, unsigned *out)
{
    if (mulib_json_typeof(array) != MULIB_JSON_ARRAY || mulib_json_size(array) > 3) return 0;
    *out = 0;
    for (size_t i = 0; i < mulib_json_size(array); ++i) {
        size_t n;
        const char *name = mulib_json_text(mulib_json_at(array, i), &n);
        int cap = name && strlen(name) == n ? capability(name) : 0;
        if (!cap || (*out & (unsigned)cap)) return 0;
        *out |= (unsigned)cap;
    }
    return 1;
}

static int report(mulib_json_t *record)
{
    if (explicit_command) return shy_push(reports, record);
    char *text = record ? mulib_json_stringify(record, 0) : NULL;
    mulib_json_destroy(record);
    if (!text) return 0;
    int ok = fprintf(stderr, "%s\n", text) >= 0 && fflush(stderr) == 0;
    free(text); return ok;
}

static void diagnostic(const extension *e, const char *name, const char *phase, const char *message)
{
    ++failures;
    mulib_json_t *out = mulib_json_new(MULIB_JSON_OBJECT);
    if (!shy_add(out, "schema", mulib_json_string("shylock.extension-audit/v1")) ||
        !shy_add(out, "extension", mulib_json_string(name)) ||
        !shy_add(out, "phase", mulib_json_string(phase)) ||
        !shy_add(out, "isolation", mulib_json_string(e && e->isolation < 0 ? "invalid" : e && e->isolation ? "process" : "off")) ||
        !shy_add(out, "sandbox_profile", mulib_json_string(e && e->sandbox_active ? SHY_SANDBOX_PROFILE : "none")) ||
        !shy_add(out, "status", mulib_json_string("error")) ||
        !shy_add(out, "error", mulib_json_string(message))) { mulib_json_destroy(out); return; }
    (void)report(out);
}

static mulib_json_t *broker_error(extension *e, const char *message)
{
    shy_vm_error(e->failure, message);
    mulib_json_t *out = mulib_json_new(MULIB_JSON_OBJECT);
    if (!shy_add(out, "ok", mulib_json_bool(0)) || !shy_add(out, "error", mulib_json_string(e->failure))) {
        mulib_json_destroy(out); return NULL;
    }
    return out;
}

static mulib_json_t *broker_value(extension *e, mulib_json_t *value)
{
    mulib_json_t *out = mulib_json_new(MULIB_JSON_OBJECT);
    if (!shy_add(out, "ok", mulib_json_bool(1))) {
        mulib_json_destroy(value); mulib_json_destroy(out); return broker_error(e, "cannot allocate broker reply");
    }
    if (!shy_add(out, "value", value)) { mulib_json_destroy(out); return broker_error(e, "cannot encode broker value"); }
    return out;
}

static const char *broker_string(const mulib_json_t *request, const char *field, size_t limit)
{
    size_t n;
    const char *value = mulib_json_text(mulib_json_get(request, field), &n);
    return value && strlen(value) == n && n <= limit ? value : NULL;
}

/* This is the authority boundary for BOTH backends. Never trust a worker's
   claimed permissions, namespace, paths, return types or transaction status. */
static mulib_json_t *host_broker(void *user, const mulib_json_t *request)
{
    extension *e = user;
    const char *op = broker_string(request, "op", 16);
    if (!e->active) return broker_error(e, "capability denied or inactive: outside lifecycle hook");
    if (++e->calls > 256) return broker_error(e, "extension broker request limit exceeded");
    if (!op) return broker_error(e, "invalid broker operation");
    const char *key = broker_string(request, "key", !strcmp(op, "log") ? 1024u : 128u);
    if (!key) return broker_error(e, "invalid broker arguments");
    if (!strcmp(op, "get")) {
        if (!strcmp(key, "$event_id")) return broker_value(e, mulib_json_string(e->event_id));
        const mulib_json_t *value = mulib_json_get(e->payload, key);
        return broker_value(e, value ? shy_clone(value) : mulib_json_new(MULIB_JSON_NULL));
    }
    unsigned needed = !strcmp(op, "log") ? CAP_LOG : !strcmp(op, "kv_get") ? CAP_READ : !strcmp(op, "kv_set") ? CAP_WRITE : 0;
    if (!needed) return broker_error(e, "unsupported extension broker operation");
    if (!(e->capabilities & needed)) return broker_error(e, needed == CAP_LOG ? "capability denied: log" :
        needed == CAP_READ ? "capability denied: kv.read" : "capability denied: kv.write");
    if (needed == CAP_LOG) {
        if (mulib_json_size(e->logs) >= 32) return broker_error(e, "extension log limit exceeded");
        if (!shy_push(e->logs, mulib_json_string(key))) return broker_error(e, "cannot stage extension log");
    } else {
        if (!*key || !e->transaction) return broker_error(e, "invalid scoped storage access");
        char scoped[224]; snprintf(scoped, sizeof scoped, "ext/%s/%s", e->name, key);
        if (needed == CAP_READ) {
            const void *value; size_t n;
            int rc = mulib_kvdb_get(e->transaction, scoped, strlen(scoped), &value, &n);
            if (rc == MULIB_KVDB_ENOTFOUND) return broker_value(e, mulib_json_new(MULIB_JSON_NULL));
            if (rc != MULIB_KVDB_OK || n > 4096) return broker_error(e, "scoped storage read failed or exceeded value limit");
            return broker_value(e, mulib_json_string_n(value, n));
        }
        const char *value = broker_string(request, "value", 4096);
        if (!value || e->writes >= 64)
            return broker_error(e, "invalid extension write or write limit exceeded");
        ++e->writes;
        if (mulib_kvdb_set(e->transaction, scoped, strlen(scoped), value, strlen(value)) != MULIB_KVDB_OK)
            return broker_error(e, "cannot stage extension write");
    }
    return broker_value(e, mulib_json_new(MULIB_JSON_NULL));
}

static int worker_call(extension *e, unsigned hook, const char *event, const mulib_json_t *payload, char error[SHY_VM_ERROR])
{
    mulib_json_t *request = mulib_json_new(MULIB_JSON_OBJECT);
    int built = shy_add(request, "op", mulib_json_string("call")) && shy_add(request, "hook", mulib_json_number(hook)) &&
        shy_add(request, "event", mulib_json_string(event)) &&
        shy_add(request, "payload", payload ? shy_clone(payload) : mulib_json_new(MULIB_JSON_NULL));
    mulib_json_t *reply = built ? shy_process_request(e->worker, request, host_broker, e, error) : NULL;
    int ok = reply && mulib_json_boolean(mulib_json_get(reply, "ok"));
    if (!ok && !*error) shy_vm_error(error, reply && *shy_text(reply, "error") ? shy_text(reply, "error") : "cannot allocate sandbox extension call");
    mulib_json_destroy(reply); mulib_json_destroy(request); return ok;
}

static int phase(extension *e, const char *event, const mulib_json_t *payload, unsigned hook)
{
    if (!(e->hooks & hook)) return 1;
    mulib_json_t *identity = mulib_json_new(MULIB_JSON_ARRAY), *audit = NULL;
    char id[65] = { 0 };
    char *encoded = payload ? mulib_json_stringify(payload, 0) : strdup("null");
    int ok = encoded && strlen(encoded) <= EVENT_LIMIT &&
        shy_push(identity, mulib_json_string(event)) &&
        shy_push(identity, payload ? shy_clone(payload) : mulib_json_new(MULIB_JSON_NULL)) && shy_json_digest(identity, NULL, id);
    mulib_json_destroy(identity);
    *e->failure = 0; e->writes = e->calls = 0;
    e->logs = mulib_json_new(MULIB_JSON_ARRAY);
    int fd = -1, uncertain = 0;
    if (ok && (e->capabilities & (CAP_READ | CAP_WRITE))) {
        size_t n = strlen(e->storage);
        char *lock = malloc(n+6);
        if (lock) { memcpy(lock, e->storage, n); memcpy(lock+n, ".lock", 6); }
        fd = lock ? open(lock, O_CREAT | O_RDWR | O_CLOEXEC, 0600) : -1;
        free(lock);
        ok = fd >= 0 && flock(fd, LOCK_EX) == 0 &&
            mulib_kvdb_open(e->storage, (e->capabilities & CAP_WRITE) != 0, &e->transaction) == MULIB_KVDB_OK;
        if (!ok) shy_vm_error(e->failure, "cannot lock/open scoped extension storage");
    }
    char error[SHY_VM_ERROR] = { 0 };
    e->payload = payload; e->event_id = id; e->active = 1;
    if (ok && e->logs) ok = e->isolation ? worker_call(e, hook, event, payload, error) :
        shy_extension_vm_call(e->vm, hook, event, encoded, error);
    else ok = 0;
    e->active = 0; e->payload = NULL; e->event_id = NULL;
    if (*e->failure) ok = 0; /* denials remain fatal even if Lua/worker ignores them */
    if (ok && e->transaction && mulib_kvdb_dirty(e->transaction)) {
        int rc = mulib_kvdb_commit(e->transaction);
        uncertain = rc == MULIB_KVDB_UNCERTAIN; ok = rc == MULIB_KVDB_OK;
        if (!ok) shy_vm_error(e->failure, uncertain ? "extension commit durability uncertain; not retried" : "extension commit failed");
    }
    /* The parent owns commit, rollback and locks. No storage handle is ever
       transferred to an isolated worker. A failed/killed worker cannot commit. */
    mulib_kvdb_close(e->transaction); e->transaction = NULL;
    if (fd >= 0) close(fd);
    audit = mulib_json_new(MULIB_JSON_OBJECT);
    mulib_json_t *permissions = mulib_json_new(MULIB_JSON_ARRAY);
    int permissions_ok = permissions != NULL;
    if (e->capabilities & CAP_LOG) permissions_ok = permissions_ok && shy_push(permissions, mulib_json_string("log"));
    if (e->capabilities & CAP_READ) permissions_ok = permissions_ok && shy_push(permissions, mulib_json_string("kv.read"));
    if (e->capabilities & CAP_WRITE) permissions_ok = permissions_ok && shy_push(permissions, mulib_json_string("kv.write"));
    int built = shy_add(audit, "schema", mulib_json_string("shylock.extension-audit/v1")) &&
        shy_add(audit, "extension", mulib_json_string(e->name)) && shy_add(audit, "phase", mulib_json_string(event)) &&
        shy_add(audit, "source_blake3", mulib_json_string(e->source_hash)) &&
        shy_add(audit, "mulib_revision", mulib_json_string(SHY_MULIB_REVISION)) &&
        shy_add(audit, "mune_api", mulib_json_number(mulib_mune_version())) &&
        shy_add(audit, "runtime_steps", mulib_json_number(100000)) &&
        shy_add(audit, "runtime_heap_bytes", mulib_json_number(8 * 1024 * 1024)) &&
        shy_add(audit, "isolation", mulib_json_string(e->isolation ? "process" : "off")) &&
        shy_add(audit, "sandbox_profile", mulib_json_string(e->sandbox_active ? SHY_SANDBOX_PROFILE : "none")) &&
        shy_add(audit, "broker_requests", mulib_json_number((double)e->calls)) &&
        shy_add(audit, "event_id", mulib_json_string(id)) &&
        shy_add(audit, "status", mulib_json_string(ok ? "ok" : uncertain ? "uncertain" : "error")) &&
        shy_add(audit, "effects", mulib_json_string(ok ? "committed" : uncertain ? "uncertain" : "discarded"));
    if (permissions_ok && built) built = shy_add(audit, "capabilities", permissions);
    else { mulib_json_destroy(permissions); built = 0; }
    if (!ok) {
        ++failures;
        built = built && shy_add(audit, "error", mulib_json_string(*e->failure ? e->failure : *error ? error : "extension argument, allocation or payload limit failure"));
    }
    if (ok) {
        mulib_json_t *logs = e->logs; e->logs = NULL;
        if (built) built = shy_add(audit, "logs", logs); else mulib_json_destroy(logs);
    }
    if (built) { if (!report(audit)) { ++failures; ok = 0; } }
    else { mulib_json_destroy(audit); ++failures; ok = 0; }
    mulib_json_destroy(e->logs); e->logs = NULL;
    free(encoded);
    if (!ok) { e->enabled = 0; shy_process_destroy(e->worker); e->worker = NULL; }
    return ok;
}

static int load_extension(const char *configuration, const mulib_json_t *entry)
{
    if (extension_count == MAX_EXTENSIONS) return 0;
    extension *e = &extensions[extension_count];
    memset(e, 0, sizeof *e);
    e->isolation = shy_sandbox_mode(NULL);
    if (mulib_json_get(entry, "sandbox")) {
        const char *mode = shy_text(entry, "sandbox");
        e->isolation = !strcmp(mode, "process") ? 1 : !strcmp(mode, "off") ? 0 : -1;
    }
    char *manifest_path = relative_path(configuration, shy_text(entry, "manifest"));
    mulib_json_t *manifest = manifest_path ? read_json(manifest_path, 16384) : NULL;
    unsigned grants = 0, requested = 0;
    const char *name = shy_text(manifest, "name");
    int ok = manifest && e->isolation >= 0 && !strcmp(shy_text(manifest, "schema"), "shylock.extension/v1") &&
        *name && strlen(name) <= 64 && strspn(name, "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-") == strlen(name) &&
        capabilities(mulib_json_get(entry, "grant"), &grants) &&
        capabilities(mulib_json_get(manifest, "capabilities"), &requested) && !(requested & ~grants);
    for (size_t i = 0; ok && i < extension_count; ++i) if (extensions[i].name && !strcmp(extensions[i].name, name)) ok = 0;
    const mulib_json_t *events = mulib_json_get(manifest, "events");
    ok = ok && mulib_json_typeof(events) == MULIB_JSON_ARRAY && mulib_json_size(events) <= 8;
    for (size_t i = 0; ok && i < mulib_json_size(events); ++i) {
        size_t n; const char *event = mulib_json_text(mulib_json_at(events, i), &n);
        ok = event && strlen(event) == n && event_valid(event);
    }
    e->name = strdup(*name ? name : "invalid-extension"); e->capabilities = requested;
    if (requested & (CAP_READ | CAP_WRITE)) { e->storage = relative_path(configuration, shy_text(entry, "storage")); ok = ok && e->storage; }
    e->events = ok ? shy_clone(events) : NULL;
    char *script = ok ? relative_path(manifest_path, shy_text(manifest, "script")) : NULL;
    size_t n = 0; char *source = script ? shy_read(script, 65536, &n) : NULL;
    char error[SHY_VM_ERROR] = { 0 };
    if (source) shy_digest(source, n, e->source_hash);
    ok = ok && e->name && e->events && source;
    if (ok && !e->isolation) {
        e->vm = shy_extension_vm_create(source, n, host_broker, e, error);
        ok = e->vm != NULL;
        if (ok) e->hooks = shy_extension_vm_hooks(e->vm);
    } else if (ok) {
        e->worker = shy_process_start(1, error); e->sandbox_active = e->worker != NULL;
        mulib_json_t *request = mulib_json_new(MULIB_JSON_OBJECT);
        int built = shy_add(request, "op", mulib_json_string("init")) && shy_add(request, "source", mulib_json_string(source));
        mulib_json_t *response = e->worker && built ? shy_process_request(e->worker, request, host_broker, e, error) : NULL;
        int64_t hooks = response ? shy_integer(response, "hooks", 7) : -1;
        ok = response && mulib_json_boolean(mulib_json_get(response, "ok")) && hooks >= 0 && (hooks & SHY_HOOK_EVENT);
        if (ok) e->hooks = (unsigned)hooks;
        else if (!*error) shy_vm_error(error, response && *shy_text(response, "error") ? shy_text(response, "error") : "invalid sandbox extension descriptor");
        mulib_json_destroy(response); mulib_json_destroy(request);
    }
    if (*e->failure) ok = 0;
    if (!ok) diagnostic(e, e->name ? e->name : "invalid-extension", "load", *e->failure ? e->failure : *error ? error :
        e->isolation < 0 ? "sandbox mode must be off or process" : "invalid manifest, duplicate name, missing source or capabilities not granted");
    e->enabled = ok; ++extension_count;
    free(source); free(script); free(manifest_path); mulib_json_destroy(manifest);
    if (ok) { e->initialized = 1; ok = phase(e, "on_load", NULL, SHY_HOOK_LOAD); }
    if (!ok) { shy_process_destroy(e->worker); e->worker = NULL; }
    return ok;
}

int64_t shy_extensions_begin(const char *configuration)
{
    if (!*configuration) return 0;
    mulib_json_t *config = read_json(configuration, 16384);
    const mulib_json_t *items = mulib_json_get(config, "extensions");
    if (!config || strcmp(shy_text(config, "schema"), "shylock.extensions/v1") ||
        mulib_json_typeof(items) != MULIB_JSON_ARRAY || mulib_json_size(items) > MAX_EXTENSIONS) {
        diagnostic(NULL, "host", "configuration", "invalid extension host configuration"); mulib_json_destroy(config); return 2;
    }
    for (size_t i = 0; i < mulib_json_size(items); ++i) (void)load_extension(configuration, mulib_json_at(items, i));
    mulib_json_destroy(config); return failures ? 2 : 0;
}

int shy_extensions_event(const char *event, const mulib_json_t *payload)
{
    if (!event_valid(event)) return 0;
    int ok = 1;
    for (size_t i = 0; i < extension_count; ++i) {
        extension *e = &extensions[i];
        if (!e->enabled) continue;
        for (size_t j = 0; j < mulib_json_size(e->events); ++j) {
            const char *name = mulib_json_text(mulib_json_at(e->events, j), NULL);
            if (!strcmp(name, event)) { if (!phase(e, event, payload, SHY_HOOK_EVENT)) ok = 0; break; }
        }
    }
    return ok;
}

int shy_extensions_enqueue(const char *event, const mulib_json_t *payload)
{
    if (!extension_count) return 1;
    char *text = mulib_json_stringify(payload, 0);
    size_t n = text ? strlen(text) : QUEUE_LIMIT+1; free(text);
    if (!event_valid(event) || n > EVENT_LIMIT || queued_bytes > QUEUE_LIMIT-n || (queue && mulib_json_size(queue) >= 32)) {
        diagnostic(NULL, "host", event, "extension event queue/payload limit exceeded; core result preserved"); return 0;
    }
    if (!queue) queue = mulib_json_new(MULIB_JSON_ARRAY);
    mulib_json_t *item = mulib_json_new(MULIB_JSON_OBJECT);
    int ok = shy_add(item, "event", mulib_json_string(event)) && shy_add(item, "payload", shy_clone(payload));
    if (!ok) mulib_json_destroy(item);
    if (!ok || !shy_push(queue, item)) { diagnostic(NULL, "host", event, "cannot allocate extension event"); return 0; }
    queued_bytes += n; return 1;
}

int64_t shy_extensions_drain(void)
{
    /* Hooks run only after Poindexter releases its lock, including worker RPC. */
    mulib_json_t *events = queue; queue = NULL; queued_bytes = 0;
    for (size_t i = 0; i < mulib_json_size(events); ++i) {
        const mulib_json_t *item = mulib_json_at(events, i);
        (void)shy_extensions_event(shy_text(item, "event"), mulib_json_get(item, "payload"));
    }
    mulib_json_destroy(events); return failures ? 2 : 0;
}

int64_t shy_extensions_end(void)
{
    shy_extensions_drain();
    for (size_t i = 0; i < extension_count; ++i) {
        extension *e = &extensions[i];
        if (e->initialized && e->enabled) (void)phase(e, "on_unload", NULL, SHY_HOOK_UNLOAD);
        shy_process_destroy(e->worker); shy_extension_vm_destroy(e->vm);
        free(e->name); free(e->storage); mulib_json_destroy(e->events); memset(e, 0, sizeof *e);
    }
    extension_count = 0;
    int status = failures ? 2 : 0; failures = 0; return status;
}

int64_t shy_extension_command(const char *configuration, const char *event, const char *payload_path)
{
    if (!event_valid(event)) return shy_error("unknown extension event");
    mulib_json_t *payload = read_json(payload_path, EVENT_LIMIT);
    if (!payload) return shy_error("cannot read bounded extension event JSON");
    explicit_command = 1; reports = mulib_json_new(MULIB_JSON_ARRAY);
    (void)shy_extensions_begin(configuration); (void)shy_extensions_event(event, payload);
    int64_t status = shy_extensions_end(); explicit_command = 0;
    mulib_json_t *out = mulib_json_new(MULIB_JSON_OBJECT);
    int ok = shy_add(out, "schema", mulib_json_string("shylock.extension-result/v1")) &&
        shy_add(out, "status", mulib_json_string(status ? "extension_error" : "ok")) && shy_add(out, "event", mulib_json_string(event));
    if (!ok) { mulib_json_destroy(out); mulib_json_destroy(payload); mulib_json_destroy(reports); reports = NULL; return 2; }
    if (!mulib_json_set(out, "payload", payload)) { mulib_json_destroy(payload); mulib_json_destroy(out); mulib_json_destroy(reports); reports = NULL; return 2; }
    mulib_json_t *audit = reports; reports = NULL;
    if (!shy_add(out, "audit", audit)) { mulib_json_destroy(out); return 2; }
    return shy_emit(out) ? 2 : status;
}
