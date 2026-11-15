#ifndef SHYLOCK_SANDBOX_H
#define SHYLOCK_SANDBOX_H
#include "mune_vm.h"
enum { SHY_IPC_FD = 3, SHY_IPC_REQUEST = 2 * 1024 * 1024, SHY_IPC_REPLY = 32768,
    SHY_SANDBOX_TIMEOUT_MS = 2000, SHY_SANDBOX_STARTUP_MS = 3000 };
#define SHY_SANDBOX_PROFILE "linux-seccomp/v1"
typedef struct shy_process shy_process;
/* off=0, process=1, invalid=-1; NULL/empty inherits SHY_SANDBOX. */
int shy_sandbox_mode(const char *override);
int shy_sandbox_supported(void);
int shy_sandbox_enter(int extension_role, long parent, char error[SHY_VM_ERROR]);
void shy_sandbox_close_fds(unsigned maximum);
shy_process *shy_process_start(int extension_role, char error[SHY_VM_ERROR]);
/* request and broker requests are borrowed; response and broker replies owned.
   A single absolute deadline includes all RPC round trips in this request. */
mulib_json_t *shy_process_request(shy_process *process, const mulib_json_t *request,
    shy_vm_broker broker, void *user, char error[SHY_VM_ERROR]);
void shy_process_destroy(shy_process *process);
/* Blocking framed I/O for the isolated worker only. Parent uses deadlines. */
mulib_json_t *shy_worker_receive(size_t limit);
int shy_worker_send(const mulib_json_t *message, size_t limit);
#endif
