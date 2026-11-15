#include "mune_vm.h"
#include "mulib-mune.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

struct shy_extension_vm {
    mulib_mune_state_t *state;
    mulib_mune_result_t *roots;
    mulib_mune_value_t load, event, unload;
    shy_vm_broker broker;
    void *user;
    char failure[SHY_VM_ERROR];
};

void shy_vm_error(char out[SHY_VM_ERROR], const char *message)
{
    size_t i = 0;
    if (!message) message = "Mune allocation or execution failure";
    for (; message[i] && i < SHY_VM_ERROR-1; ++i) {
        unsigned char c = (unsigned char)message[i];
        out[i] = c >= 32 && c < 127 ? (char)c : '?';
    }
    out[i] = 0;
}

static int restrict_state(mulib_mune_state_t *state, const char *const *keep, size_t count)
{
    mulib_mune_value_t values[16], globals, key, value;
    if (count > sizeof values/sizeof *values) return 0;
    mulib_mune_result_t *roots = mulib_mune_result_create(state);
    if (!roots) return 0;
    int ok = 1;
    for (size_t i = 0; ok && i < count; ++i)
        ok = mulib_mune_get_global(state, keep[i], &values[i]) == MULIB_MUNE_OK &&
            mulib_mune_result_push(roots, values[i]);
    ok = ok && mulib_mune_get_global(state, "_G", &globals) == MULIB_MUNE_OK && globals.type == MULIB_MUNE_TABLE;
    while (ok) {
        size_t cursor = 0;
        int found = mulib_mune_table_next(state, globals, &cursor, &key, &value);
        if (found <= 0) { ok = found == 0; break; }
        ok = mulib_mune_raw_set(state, globals, key, mulib_mune_nil()) == MULIB_MUNE_OK;
    }
    for (size_t i = 0; ok && i < count; ++i) {
        if (!strcmp(keep[i], "math"))
            ok = mulib_mune_set_field(state, values[i], "random", mulib_mune_nil()) == MULIB_MUNE_OK &&
                mulib_mune_set_field(state, values[i], "randomseed", mulib_mune_nil()) == MULIB_MUNE_OK;
        if (ok) ok = mulib_mune_set_global(state, keep[i], values[i]) == MULIB_MUNE_OK;
    }
    mulib_mune_result_destroy(roots);
    return ok && mulib_mune_set_mode(state, MULIB_MUNE_TREE) == MULIB_MUNE_OK;
}

int shy_formula_eval(const char *source, size_t length, int64_t reference,
    int64_t weekend, int64_t premium, int64_t *rate, char error[SHY_VM_ERROR])
{
    *rate = -1; *error = 0;
    mulib_mune_error_t issue = { 0 };
    const mulib_mune_parse_options_t parse = { 16384, 4096, 4096, 8192, 100000 };
    const mulib_mune_runtime_options_t runtime = { 1024 * 1024, 50000, 32, 32 };
    mulib_mune_chunk_t *chunk = mulib_mune_parse(source, length, &parse, &issue);
    mulib_mune_state_t *state = chunk ? mulib_mune_state_create(&runtime) : NULL;
    mulib_mune_result_t *loaded = NULL, *result = NULL;
    static const char *const keep[] = { "math" };
    if (state && restrict_state(state, keep, 1) && mulib_mune_run(state, chunk, &loaded, &issue) == MULIB_MUNE_OK &&
        mulib_mune_result_count(loaded) == 1) {
        mulib_mune_value_t fn = mulib_mune_result_get(loaded, 0);
        mulib_mune_value_t args[] = { mulib_mune_integer(reference), mulib_mune_boolean(weekend != 0), mulib_mune_integer(premium) };
        if (fn.type == MULIB_MUNE_FUNCTION && mulib_mune_call(state, fn, args, 3, &result, &issue) == MULIB_MUNE_OK &&
            mulib_mune_result_count(result) == 1) {
            mulib_mune_value_t value = mulib_mune_result_get(result, 0);
            if (value.type == MULIB_MUNE_INTEGER && value.as.integer >= 0 && value.as.integer <= INT64_C(1000000000000))
                *rate = value.as.integer;
        }
    }
    if (*rate < 0) shy_vm_error(error, issue.message ? issue.message : "formula must return one bounded integer");
    mulib_mune_result_destroy(result); mulib_mune_result_destroy(loaded);
    mulib_mune_state_destroy(state); mulib_mune_chunk_destroy(chunk);
    return *rate >= 0;
}

static const char *argument(mulib_mune_value_t value, size_t limit)
{
    size_t n;
    const char *s = mulib_mune_value_string(value, &n);
    return s && n <= limit && strlen(s) == n ? s : NULL;
}

static mulib_mune_status_t push_json(mulib_mune_state_t *state, mulib_mune_result_t *out, const mulib_json_t *value)
{
    mulib_mune_value_t result = mulib_mune_nil();
    char *encoded = NULL;
    const char *text = NULL; size_t n = 0;
    switch (mulib_json_typeof(value)) {
    case MULIB_JSON_NULL: break;
    case MULIB_JSON_BOOL: result = mulib_mune_boolean(mulib_json_boolean(value)); break;
    case MULIB_JSON_NUMBER: {
        double number = mulib_json_numeric(value);
        result = floor(number) == number && fabs(number) <= 9007199254740991.0 ? mulib_mune_integer((int64_t)number) : mulib_mune_number(number);
        break;
    }
    case MULIB_JSON_STRING: text = mulib_json_text(value, &n); break;
    default: encoded = mulib_json_stringify(value, 0); text = encoded; n = encoded ? strlen(encoded) : 0; break;
    }
    int ok = !text || mulib_mune_make_string(state, text, n, &result) == MULIB_MUNE_OK;
    if (mulib_json_typeof(value) > MULIB_JSON_STRING && !encoded) ok = 0;
    free(encoded);
    return ok && mulib_mune_result_push(out, result) ? MULIB_MUNE_OK : MULIB_MUNE_MEMORY;
}

static mulib_mune_status_t host_call(shy_extension_vm *vm, const char *op, const mulib_mune_value_t *args,
    size_t count, mulib_mune_result_t *out)
{
    int set = !strcmp(op, "kv_set"), log = !strcmp(op, "log");
    const char *key = count == (set ? 2u : 1u) ? argument(args[0], log ? 1024 : 128) : NULL;
    const char *value = set && count == 2 ? argument(args[1], 4096) : NULL;
    if (!key || (set && !value)) return MULIB_MUNE_INVALID;
    mulib_json_t *request = mulib_json_new(MULIB_JSON_OBJECT);
    int built = shy_add(request, "op", mulib_json_string(op)) && shy_add(request, "key", mulib_json_string(key));
    if (built && set) built = shy_add(request, "value", mulib_json_string(value));
    mulib_json_t *response = built ? vm->broker(vm->user, request) : NULL;
    mulib_json_destroy(request);
    if (!response || mulib_json_typeof(mulib_json_get(response, "ok")) != MULIB_JSON_BOOL ||
        !mulib_json_boolean(mulib_json_get(response, "ok"))) {
        shy_vm_error(vm->failure, response ? shy_text(response, "error") : "host broker failed");
        mulib_json_destroy(response); return MULIB_MUNE_RUNTIME;
    }
    mulib_mune_status_t status = set || log ? MULIB_MUNE_OK : push_json(vm->state, out, mulib_json_get(response, "value"));
    mulib_json_destroy(response); return status;
}

#define HOST_FN(name) \
static mulib_mune_status_t host_##name(mulib_mune_state_t *state, const mulib_mune_value_t *args, \
    size_t count, mulib_mune_result_t *out, void *user) \
{ (void)state; return host_call(user, #name, args, count, out); }
HOST_FN(log)
HOST_FN(get)
HOST_FN(kv_get)
HOST_FN(kv_set)
#undef HOST_FN

void shy_extension_vm_destroy(shy_extension_vm *vm)
{
    if (!vm) return;
    mulib_mune_result_destroy(vm->roots); mulib_mune_state_destroy(vm->state); free(vm);
}

shy_extension_vm *shy_extension_vm_create(const char *source, size_t length,
    shy_vm_broker broker, void *user, char error[SHY_VM_ERROR])
{
    *error = 0;
    shy_extension_vm *vm = calloc(1, sizeof *vm);
    mulib_mune_error_t issue = { 0 };
    const mulib_mune_parse_options_t parse = { 65536, 16384, 16384, 32768, 250000 };
    const mulib_mune_runtime_options_t runtime = { 8 * 1024 * 1024, 100000, 32, 32 };
    mulib_mune_chunk_t *chunk = vm ? mulib_mune_parse(source, length, &parse, &issue) : NULL;
    mulib_mune_result_t *loaded = NULL;
    if (!vm || !chunk) goto fail;
    vm->broker = broker; vm->user = user;
    vm->state = mulib_mune_state_create(&runtime);
    vm->roots = vm->state ? mulib_mune_result_create(vm->state) : NULL;
    static const char *const keep[] = { "assert", "error", "ipairs", "pairs", "next", "pcall",
        "select", "tonumber", "tostring", "type", "math", "string", "table" };
    if (!vm->roots || !restrict_state(vm->state, keep, sizeof keep/sizeof *keep)) goto fail;
    mulib_mune_value_t host;
    if (mulib_mune_make_table(vm->state, &host) != MULIB_MUNE_OK || !mulib_mune_result_push(vm->roots, host)) goto fail;
    static const char *const names[] = { "log", "get", "kv_get", "kv_set" };
    static const mulib_mune_native_fn functions[] = { host_log, host_get, host_kv_get, host_kv_set };
    for (size_t i = 0; i < sizeof names/sizeof *names; ++i) {
        mulib_mune_value_t fn;
        if (mulib_mune_make_function(vm->state, functions[i], vm, &fn) != MULIB_MUNE_OK ||
            !mulib_mune_result_push(vm->roots, fn) || mulib_mune_set_field(vm->state, host, names[i], fn) != MULIB_MUNE_OK) goto fail;
    }
    if (mulib_mune_set_global(vm->state, "host", host) != MULIB_MUNE_OK ||
        mulib_mune_run(vm->state, chunk, &loaded, &issue) != MULIB_MUNE_OK || mulib_mune_result_count(loaded) != 1 || *vm->failure) goto fail;
    mulib_mune_value_t descriptor = mulib_mune_result_get(loaded, 0), api;
    if (descriptor.type != MULIB_MUNE_TABLE || !mulib_mune_result_push(vm->roots, descriptor) ||
        mulib_mune_get_field(vm->state, descriptor, "api", &api) != MULIB_MUNE_OK) goto fail;
    const char *api_name = argument(api, 64);
    if (!api_name || strcmp(api_name, "shylock.extension/v1") ||
        mulib_mune_get_field(vm->state, descriptor, "on_load", &vm->load) != MULIB_MUNE_OK ||
        mulib_mune_get_field(vm->state, descriptor, "on_event", &vm->event) != MULIB_MUNE_OK ||
        mulib_mune_get_field(vm->state, descriptor, "on_unload", &vm->unload) != MULIB_MUNE_OK ||
        (vm->load.type != MULIB_MUNE_NIL && vm->load.type != MULIB_MUNE_FUNCTION) ||
        vm->event.type != MULIB_MUNE_FUNCTION ||
        (vm->unload.type != MULIB_MUNE_NIL && vm->unload.type != MULIB_MUNE_FUNCTION) ||
        !mulib_mune_result_push(vm->roots, vm->load) || !mulib_mune_result_push(vm->roots, vm->event) ||
        !mulib_mune_result_push(vm->roots, vm->unload)) goto fail;
    mulib_mune_result_destroy(loaded); mulib_mune_chunk_destroy(chunk); return vm;
fail:
    shy_vm_error(error, vm && *vm->failure ? vm->failure : issue.message ? issue.message : "invalid extension API, descriptor or allocation failure");
    mulib_mune_result_destroy(loaded); mulib_mune_chunk_destroy(chunk); shy_extension_vm_destroy(vm); return NULL;
}

unsigned shy_extension_vm_hooks(const shy_extension_vm *vm)
{
    return (vm->load.type == MULIB_MUNE_FUNCTION ? SHY_HOOK_LOAD : 0) | SHY_HOOK_EVENT |
        (vm->unload.type == MULIB_MUNE_FUNCTION ? SHY_HOOK_UNLOAD : 0);
}

int shy_extension_vm_call(shy_extension_vm *vm, unsigned hook, const char *event,
    const char *payload, char error[SHY_VM_ERROR])
{
    *error = 0; *vm->failure = 0;
    mulib_mune_value_t function = hook == SHY_HOOK_LOAD ? vm->load : hook == SHY_HOOK_EVENT ? vm->event : vm->unload;
    if (function.type == MULIB_MUNE_NIL) return 1;
    mulib_mune_result_t *roots = mulib_mune_result_create(vm->state), *result = NULL;
    mulib_mune_error_t issue = { 0 };
    mulib_mune_value_t args[2];
    int ok = roots && mulib_mune_make_string(vm->state, event, strlen(event), &args[0]) == MULIB_MUNE_OK &&
        mulib_mune_result_push(roots, args[0]) && mulib_mune_make_string(vm->state, payload, strlen(payload), &args[1]) == MULIB_MUNE_OK &&
        mulib_mune_result_push(roots, args[1]) && mulib_mune_call(vm->state, function, args, 2, &result, &issue) == MULIB_MUNE_OK;
    if (ok) {
        size_t count = mulib_mune_result_count(result);
        mulib_mune_value_t value = mulib_mune_result_get(result, 0);
        ok = count == 0 || (count == 1 && value.type == MULIB_MUNE_BOOLEAN && value.as.boolean);
        if (!ok) shy_vm_error(error, "handler must return no values or true");
    }
    if (*vm->failure) { ok = 0; shy_vm_error(error, vm->failure); }
    if (!ok && !*error) shy_vm_error(error, issue.message);
    mulib_mune_result_destroy(result); mulib_mune_result_destroy(roots); return ok;
}
