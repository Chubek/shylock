#define _POSIX_C_SOURCE 200809L
#include "sandbox.h"
#include <sys/resource.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>

static mulib_json_t *result(int ok, const char *error)
{
    mulib_json_t *out = mulib_json_new(MULIB_JSON_OBJECT);
    if (!shy_add(out, "op", mulib_json_string("result")) || !shy_add(out, "ok", mulib_json_bool(ok)) ||
        (!ok && !shy_add(out, "error", mulib_json_string(error)))) { mulib_json_destroy(out); return NULL; }
    return out;
}

static mulib_json_t *broker(void *user, const mulib_json_t *request)
{
    (void)user;
    if (!shy_worker_send(request, SHY_IPC_REPLY)) return NULL;
    return shy_worker_receive(SHY_IPC_REQUEST);
}

int64_t shy_worker_main(void)
{
    const char *role = getenv("SHY_WORKER_ROLE"), *parent = getenv("SHY_WORKER_PARENT");
    if (!role || !parent || !*parent || strspn(parent, "0123456789") != strlen(parent) ||
        (strcmp(role, "formula") && strcmp(role, "extension"))) _exit(125);
    int extension_role = !strcmp(role, "extension");
    long parent_pid = strtol(parent, NULL, 10);
    struct rlimit descriptors;
    unsigned maximum = getrlimit(RLIMIT_NOFILE, &descriptors) == 0 && descriptors.rlim_cur != RLIM_INFINITY && descriptors.rlim_cur < 1048576 ?
        (unsigned)descriptors.rlim_cur : 1048576;
    shy_sandbox_close_fds(maximum);
    char error[SHY_VM_ERROR] = { 0 };
    if (!shy_sandbox_enter(extension_role, parent_pid, error)) {
        mulib_json_t *failed = result(0, error);
        (void)shy_worker_send(failed, SHY_IPC_REPLY); mulib_json_destroy(failed); _exit(125);
    }
    mulib_json_t *hello = mulib_json_new(MULIB_JSON_OBJECT);
    int ready = shy_add(hello, "op", mulib_json_string("ready")) &&
        shy_add(hello, "profile", mulib_json_string(SHY_SANDBOX_PROFILE)) && shy_worker_send(hello, SHY_IPC_REPLY);
    mulib_json_destroy(hello);
    if (!ready) _exit(125);
    shy_extension_vm *vm = NULL;
    for (;;) {
        mulib_json_t *request = shy_worker_receive(SHY_IPC_REQUEST);
        if (!request) break;
        const char *op = shy_text(request, "op");
        mulib_json_t *reply = NULL;
        *error = 0;
        if (!extension_role && !strcmp(op, "formula")) {
            const char *source = shy_text(request, "source");
            int64_t reference = shy_integer(request, "reference", INT64_C(1000000000000));
            int64_t premium = shy_integer(request, "premium", 10000), rate = -1;
            const mulib_json_t *weekend = mulib_json_get(request, "weekend");
            int ok = strlen(source) <= 16384 && reference >= 0 && premium >= 0 && mulib_json_typeof(weekend) == MULIB_JSON_BOOL &&
                shy_formula_eval(source, strlen(source), reference, mulib_json_boolean(weekend), premium, &rate, error);
            reply = result(ok, *error ? error : "invalid formula worker input");
            if (ok && !shy_add(reply, "minor", mulib_json_number((double)rate))) { mulib_json_destroy(reply); reply = NULL; }
        } else if (extension_role && !strcmp(op, "init") && !vm) {
            const char *source = shy_text(request, "source");
            if (strlen(source) <= 65536) vm = shy_extension_vm_create(source, strlen(source), broker, NULL, error);
            reply = result(vm != NULL, *error ? error : "invalid extension worker source");
            if (vm && !shy_add(reply, "hooks", mulib_json_number(shy_extension_vm_hooks(vm)))) { mulib_json_destroy(reply); reply = NULL; }
        } else if (extension_role && !strcmp(op, "call") && vm) {
            int64_t hook = shy_integer(request, "hook", SHY_HOOK_UNLOAD);
            const char *event = shy_text(request, "event");
            const mulib_json_t *payload = mulib_json_get(request, "payload");
            char *encoded = payload ? mulib_json_stringify(payload, 0) : NULL;
            int ok = encoded && strlen(encoded) <= 1024 * 1024 && *event && strlen(event) <= 64 &&
                (hook == SHY_HOOK_LOAD || hook == SHY_HOOK_EVENT || hook == SHY_HOOK_UNLOAD) &&
                shy_extension_vm_call(vm, (unsigned)hook, event, encoded, error);
            free(encoded);
            reply = result(ok, *error ? error : "invalid extension worker call");
        } else reply = result(0, "unexpected sandbox worker request");
        mulib_json_destroy(request);
        int ok = reply && shy_worker_send(reply, SHY_IPC_REPLY);
        mulib_json_destroy(reply);
        if (!ok) break;
    }
    shy_extension_vm_destroy(vm);
    _exit(0);
}
