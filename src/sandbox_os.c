#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1
#endif
#include "sandbox.h"
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#if defined(__linux__) && defined(__LP64__) && (defined(__x86_64__) || defined(__aarch64__))
#include <linux/audit.h>
#include <linux/capability.h>
#include <linux/filter.h>
#include <linux/seccomp.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <signal.h>
#include <stddef.h>
#include <limits.h>
#define SHY_LINUX_SANDBOX 1
#else
#define SHY_LINUX_SANDBOX 0
#endif

int shy_sandbox_mode(const char *override)
{
    const char *mode = override && *override ? override : getenv("SHY_SANDBOX");
    if (!mode || !*mode || !strcmp(mode, "off")) return 0;
    return !strcmp(mode, "process") ? 1 : -1;
}

int shy_sandbox_supported(void) { return SHY_LINUX_SANDBOX; }

void shy_sandbox_close_fds(unsigned maximum)
{
#if SHY_LINUX_SANDBOX && defined(SYS_close_range)
    if (syscall(SYS_close_range, 4u, UINT_MAX, 0u) == 0) return;
#endif
    for (unsigned fd = 4; fd < maximum; ++fd) (void)close((int)fd);
}

#if SHY_LINUX_SANDBOX
static int limit(int resource, rlim_t value)
{
    struct rlimit bounds = { value, value };
    return setrlimit(resource, &bounds) == 0;
}
#define ALLOW(number) BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, (number), 0, 1), BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW)
#define ARG(index) ((unsigned)offsetof(struct seccomp_data, args[(index)]))
#endif

int shy_sandbox_enter(int extension_role, long parent, char error[SHY_VM_ERROR])
{
#if SHY_LINUX_SANDBOX
    static const char limits_error[] = "sandbox resource, credential or parent-death setup failed";
    /* Set both hard and soft limits; workers cannot raise them. CPU is a
       lifetime cap, independent of the parent's per-request wall deadline. */
    if (prctl(PR_SET_PDEATHSIG, SIGKILL) || (long)getppid() != parent ||
        !limit(RLIMIT_AS, (rlim_t)256 * 1024 * 1024) || !limit(RLIMIT_CORE, 0) ||
        !limit(RLIMIT_FSIZE, 0) || !limit(RLIMIT_NOFILE, 4) || !limit(RLIMIT_NPROC, 0) ||
        !limit(RLIMIT_MEMLOCK, 0) || !limit(RLIMIT_STACK, (rlim_t)8 * 1024 * 1024) ||
        !limit(RLIMIT_CPU, extension_role ? 60 : 2)) {
        memcpy(error, limits_error, sizeof limits_error); return 0;
    }
    struct __user_cap_header_struct header = { _LINUX_CAPABILITY_VERSION_3, 0 };
    struct __user_cap_data_struct capabilities[2] = { { 0 }, { 0 } };
    if (prctl(PR_CAP_AMBIENT, PR_CAP_AMBIENT_CLEAR_ALL, 0, 0, 0) ||
        syscall(SYS_capset, &header, capabilities) || prctl(PR_SET_DUMPABLE, 0) ||
        prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0)) {
        memcpy(error, limits_error, sizeof limits_error); return 0;
    }
    sigset_t signals; sigemptyset(&signals);
    if (sigprocmask(SIG_SETMASK, &signals, NULL)) {
        memcpy(error, limits_error, sizeof limits_error); return 0;
    }
    (void)signal(SIGPIPE, SIG_DFL);
#if defined(__x86_64__)
#define SHY_AUDIT_ARCH AUDIT_ARCH_X86_64
#else
#define SHY_AUDIT_ARCH AUDIT_ARCH_AARCH64
#endif
    struct sock_filter code[] = {
        BPF_STMT(BPF_LD | BPF_W | BPF_ABS, (unsigned)offsetof(struct seccomp_data, arch)),
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, SHY_AUDIT_ARCH, 1, 0),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_KILL_PROCESS),
        BPF_STMT(BPF_LD | BPF_W | BPF_ABS, (unsigned)offsetof(struct seccomp_data, nr)),
        /* Only the dedicated broker socket may carry data. */
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, SYS_read, 0, 4),
        BPF_STMT(BPF_LD | BPF_W | BPF_ABS, ARG(0)),
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, SHY_IPC_FD, 0, 1),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_KILL_PROCESS),
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, SYS_write, 0, 4),
        BPF_STMT(BPF_LD | BPF_W | BPF_ABS, ARG(0)),
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, SHY_IPC_FD, 0, 1),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_KILL_PROCESS),
        /* Heap allocations must be private anonymous, never executable. */
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, SYS_mmap, 0, 9),
        BPF_STMT(BPF_LD | BPF_W | BPF_ABS, ARG(2)),
        BPF_JUMP(BPF_JMP | BPF_JSET | BPF_K, PROT_EXEC, 0, 1),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_KILL_PROCESS),
        BPF_STMT(BPF_LD | BPF_W | BPF_ABS, ARG(3)),
        BPF_JUMP(BPF_JMP | BPF_JSET | BPF_K, MAP_SHARED | MAP_FIXED, 0, 1),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_KILL_PROCESS),
        BPF_JUMP(BPF_JMP | BPF_JSET | BPF_K, MAP_ANONYMOUS, 1, 0),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_KILL_PROCESS),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW),
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, SYS_mprotect, 0, 4),
        BPF_STMT(BPF_LD | BPF_W | BPF_ABS, ARG(2)),
        BPF_JUMP(BPF_JMP | BPF_JSET | BPF_K, PROT_EXEC, 0, 1),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_KILL_PROCESS),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW),
        ALLOW(SYS_brk), ALLOW(SYS_munmap), ALLOW(SYS_mremap), ALLOW(SYS_madvise),
        ALLOW(SYS_close), ALLOW(SYS_futex), ALLOW(SYS_clock_gettime), ALLOW(SYS_gettimeofday),
        ALLOW(SYS_getrandom), ALLOW(SYS_getpid), ALLOW(SYS_gettid),
        ALLOW(SYS_rt_sigaction), ALLOW(SYS_rt_sigprocmask), ALLOW(SYS_rt_sigreturn), ALLOW(SYS_sigaltstack),
        ALLOW(SYS_exit), ALLOW(SYS_exit_group),
        /* No open/stat, sockets, exec/fork/clone, ptrace/process_vm, ioctl,
           signals to other processes, descriptor transfer or privilege APIs. */
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_KILL_PROCESS)
    };
    struct sock_fprog filter = { (unsigned short)(sizeof code/sizeof *code), code };
    if (prctl(PR_SET_SECCOMP, SECCOMP_MODE_FILTER, &filter)) {
        static const char message[] = "sandbox seccomp filter installation failed";
        memcpy(error, message, sizeof message); return 0;
    }
    *error = 0;
    return 1;
#else
    (void)extension_role; (void)parent;
    static const char message[] = "process sandbox is unavailable on this platform/architecture";
    memcpy(error, message, sizeof message); return 0;
#endif
}
