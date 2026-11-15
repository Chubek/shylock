#ifndef SHYLOCK_MUNE_VM_H
#define SHYLOCK_MUNE_VM_H
#include "shylock.h"
enum { SHY_VM_ERROR = 192, SHY_HOOK_LOAD = 1, SHY_HOOK_EVENT = 2, SHY_HOOK_UNLOAD = 4 };
typedef struct shy_extension_vm shy_extension_vm;
/* Broker borrows request, returns owned JSON. Authority stays in the caller,
   not in a Lua table or an isolated worker's reports. */
typedef mulib_json_t *(*shy_vm_broker)(void *, const mulib_json_t *);
void shy_vm_error(char out[SHY_VM_ERROR], const char *message);
int shy_formula_eval(const char *source, size_t length, int64_t reference,
    int64_t weekend, int64_t premium, int64_t *rate, char error[SHY_VM_ERROR]);
shy_extension_vm *shy_extension_vm_create(const char *source, size_t length,
    shy_vm_broker broker, void *user, char error[SHY_VM_ERROR]);
unsigned shy_extension_vm_hooks(const shy_extension_vm *vm);
int shy_extension_vm_call(shy_extension_vm *vm, unsigned hook, const char *event,
    const char *payload, char error[SHY_VM_ERROR]);
void shy_extension_vm_destroy(shy_extension_vm *vm);
#endif
