#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1
#endif
#include "../src/sandbox.h"
#include <sys/socket.h>
#include <sys/mman.h>
#include <sys/ptrace.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <signal.h>
#include <time.h>
#include <unistd.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

#if defined(__linux__) && defined(__LP64__) && (defined(__x86_64__) || defined(__aarch64__))
static int probe(size_t which)
{
    int channels[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, channels)) return 0;
    int child_channel = fcntl(channels[1], F_DUPFD_CLOEXEC, 8);
    close(channels[1]);
    int empty = open("/dev/null", O_RDWR);
    int null = empty < 0 ? -1 : fcntl(empty, F_DUPFD_CLOEXEC, 8);
    if (empty >= 0) close(empty);
    if (child_channel < 0 || null < 0) { close(channels[0]); if (child_channel >= 0) close(child_channel); if (null >= 0) close(null); return 0; }
    long parent = (long)getpid();
    pid_t pid = fork();
    if (!pid) {
        if (dup2(null, 0) < 0 || dup2(null, 1) < 0 || dup2(null, 2) < 0 || dup2(child_channel, 3) < 0 || dup2(null, 4) < 0) _exit(90);
        void *memory = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        char error[SHY_VM_ERROR];
        if (memory == MAP_FAILED || !shy_sandbox_enter(0, parent, error)) _exit(91);
        switch (which) {
        case 0: if (write(3, "Y", 1) != 1) _exit(92); _exit(0);
        case 1: (void)syscall(SYS_openat, AT_FDCWD, "/dev/null", O_RDONLY, 0); break;
        case 2: (void)syscall(SYS_socket, AF_INET, SOCK_STREAM, 0); break;
        case 3: { char *const argv[] = { "/bin/true", NULL }; char *const env[] = { NULL }; (void)syscall(SYS_execve, argv[0], argv, env); break; }
        case 4: (void)syscall(SYS_clone, SIGCHLD, NULL, NULL, NULL, 0); break;
        case 5: (void)syscall(SYS_ptrace, PTRACE_TRACEME, 0, NULL, NULL); break;
        case 6: {
            char value = 0; struct iovec local = { &value, 1 }, remote = { memory, 1 };
            (void)syscall(SYS_process_vm_readv, parent, &local, 1, &remote, 1, 0); break;
        }
        case 7: (void)syscall(SYS_kill, getpid(), SIGCONT); break;
        case 8: (void)syscall(SYS_dup3, 3, 5, 0); break;
        case 9: { struct msghdr message; memset(&message, 0, sizeof message); (void)syscall(SYS_sendmsg, 3, &message, 0); break; }
        case 10: (void)write(1, "not allowed", 11); break;
        case 11: { char byte; (void)read(4, &byte, 1); break; }
        case 12: (void)mprotect(memory, 4096, PROT_READ | PROT_EXEC); break;
        case 13: (void)mmap(NULL, 4096, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0); break;
        case 14: (void)mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0); break;
        case 15: {
            void *huge = mmap(NULL, (size_t)1024 * 1024 * 1024, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
            _exit(huge == MAP_FAILED ? 0 : 93);
        }
        case 16: { volatile unsigned long work = 0; for (;;) { ++work; (void)work; } }
        default: break;
        }
        _exit(99); /* A supposedly denied syscall returned to user code. */
    }
    close(child_channel); close(null);
    if (pid < 0) { close(channels[0]); return 0; }
    int status = 0, finished = 0;
    for (size_t i = 0; i < 1000; ++i) {
        pid_t rc = waitpid(pid, &status, WNOHANG);
        if (rc == pid) { finished = 1; break; }
        if (rc < 0 && errno != EINTR) break;
        struct timespec pause = { 0, 10000000 };
        (void)nanosleep(&pause, NULL);
    }
    if (!finished) { (void)kill(pid, SIGKILL); while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {} }
    int ok = finished;
    if (which == 0) {
        char byte = 0; ok = ok && read(channels[0], &byte, 1) == 1 && byte == 'Y' && WIFEXITED(status) && WEXITSTATUS(status) == 0;
    } else if (which == 15) ok = ok && WIFEXITED(status) && WEXITSTATUS(status) == 0;
    else if (which == 16) ok = ok && WIFSIGNALED(status) && (WTERMSIG(status) == SIGKILL || WTERMSIG(status) == SIGXCPU);
    else ok = ok && WIFSIGNALED(status) && WTERMSIG(status) == SIGSYS;
    close(channels[0]); return ok;
}
#endif

int main(void)
{
    if (!shy_sandbox_supported()) { puts("{\"supported\":false}"); return 0; }
#if defined(__linux__) && defined(__LP64__) && (defined(__x86_64__) || defined(__aarch64__))
    static const char *const names[] = { "ipc", "filesystem", "network", "exec", "fork", "ptrace", "process_memory", "signal",
        "duplicate_fd", "descriptor_transfer", "stdout", "inherited_fd", "executable_protection", "executable_mapping", "shared_mapping", "address_limit", "cpu_limit" };
    int ok = 1;
    fputs("{\"supported\":true,\"probes\":{", stdout);
    for (size_t i = 0; i < sizeof names/sizeof *names; ++i) {
        int passed = probe(i); ok = ok && passed;
        printf("%s\"%s\":%s", i ? "," : "", names[i], passed ? "true" : "false");
    }
    puts("}}"); return ok ? 0 : 1;
#else
    return 1;
#endif
}
