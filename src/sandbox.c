#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1
#endif
#include "sandbox.h"
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/resource.h>
#include <poll.h>
#include <fcntl.h>
#include <signal.h>
#include <time.h>
#include <unistd.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

struct shy_process { pid_t pid; int fd; };

static int64_t milliseconds(void)
{
    struct timespec time;
    return clock_gettime(CLOCK_MONOTONIC, &time) ? -1 : (int64_t)time.tv_sec*1000+time.tv_nsec/1000000;
}

static int transfer(int fd, void *bytes, size_t length, int sending, int64_t deadline)
{
    unsigned char *data = bytes;
    while (length) {
        if (deadline) {
            int64_t now = milliseconds();
            if (now < 0 || now >= deadline) return 0;
            struct pollfd ready = { fd, sending ? POLLOUT : POLLIN, 0 };
            int rc = poll(&ready, 1, (int)(deadline-now));
            if (rc < 0 && errno == EINTR) continue;
            if (rc <= 0) return 0;
            if (ready.revents & (POLLERR | POLLNVAL)) return 0;
            /* POLLIN|POLLHUP may still contain a final complete result. */
        }
        ssize_t n;
        if (sending && deadline) n = send(fd, data, length, MSG_NOSIGNAL);
        else n = sending ? write(fd, data, length) : read(fd, data, length);
        if (n < 0 && (errno == EINTR || (deadline && (errno == EAGAIN || errno == EWOULDBLOCK)))) continue;
        if (n <= 0) return 0;
        data += (size_t)n; length -= (size_t)n;
    }
    return 1;
}

static int send_json(int fd, const mulib_json_t *message, size_t limit, int64_t deadline)
{
    char *text = message ? mulib_json_stringify(message, 0) : NULL;
    size_t n = text ? strlen(text) : 0;
    if (!n || n > limit || n > UINT32_MAX) { free(text); return 0; }
    unsigned char header[] = { (unsigned char)(n>>24), (unsigned char)(n>>16), (unsigned char)(n>>8), (unsigned char)n };
    int ok = transfer(fd, header, sizeof header, 1, deadline) && transfer(fd, text, n, 1, deadline);
    free(text); return ok;
}

static mulib_json_t *receive_json(int fd, size_t limit, int64_t deadline)
{
    unsigned char header[4];
    if (!transfer(fd, header, sizeof header, 0, deadline)) return NULL;
    size_t n = (size_t)header[0]<<24 | (size_t)header[1]<<16 | (size_t)header[2]<<8 | header[3];
    if (!n || n > limit) return NULL;
    char *text = malloc(n);
    mulib_json_t *message = text && transfer(fd, text, n, 0, deadline) ? mulib_json_parse(text, n, NULL) : NULL;
    free(text);
    if (mulib_json_typeof(message) != MULIB_JSON_OBJECT) { mulib_json_destroy(message); return NULL; }
    return message;
}

mulib_json_t *shy_worker_receive(size_t limit) { return receive_json(SHY_IPC_FD, limit, 0); }
int shy_worker_send(const mulib_json_t *message, size_t limit) { return send_json(SHY_IPC_FD, message, limit, 0); }

void shy_process_destroy(shy_process *process)
{
    if (!process) return;
    if (process->fd >= 0) { (void)shutdown(process->fd, SHUT_RDWR); close(process->fd); }
    if (process->pid > 0) {
        int status;
        int64_t start = milliseconds(), end = start < 0 ? 0 : start+500;
        for (;;) {
            pid_t rc = waitpid(process->pid, &status, WNOHANG);
            if (rc == process->pid || (rc < 0 && errno == ECHILD)) break;
            if (rc < 0 && errno != EINTR) break;
            int64_t now = milliseconds();
            if (!end || now < 0 || now >= end) {
                (void)kill(process->pid, SIGKILL);
                while (waitpid(process->pid, &status, 0) < 0 && errno == EINTR) {}
                break;
            }
            (void)poll(NULL, 0, 5);
        }
    }
    free(process);
}

#ifdef __linux__
static int above_stdio(int fd)
{
    if (fd < 0 || fd >= 4) return fd;
    int copy = fcntl(fd, F_DUPFD_CLOEXEC, 4);
    close(fd); return copy;
}
#endif

shy_process *shy_process_start(int extension_role, char error[SHY_VM_ERROR])
{
    if (!shy_sandbox_supported()) { shy_vm_error(error, "process sandbox is unavailable on this platform/architecture"); return NULL; }
#ifdef __linux__
    char executable[4096];
    ssize_t length = readlink("/proc/self/exe", executable, sizeof executable-1);
    if (length <= 0 || (size_t)length >= sizeof executable-1) { shy_vm_error(error, "cannot locate sandbox worker beside executable"); return NULL; }
    executable[length] = 0;
    char *slash = strrchr(executable, '/');
    static const char name[] = "shylock-worker";
    if (!slash || (size_t)(slash-executable)+sizeof name+1 > sizeof executable) {
        shy_vm_error(error, "sandbox worker path is too long"); return NULL;
    }
    memcpy(slash+1, name, sizeof name);
    int channels[2];
    if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, channels)) { shy_vm_error(error, "cannot create sandbox IPC socket"); return NULL; }
    channels[0] = above_stdio(channels[0]); channels[1] = above_stdio(channels[1]);
    int null = above_stdio(open("/dev/null", O_RDWR | O_CLOEXEC));
    shy_process *process = calloc(1, sizeof *process);
    if (null < 0 || channels[0] < 0 || channels[1] < 0 || !process) {
        if (null >= 0) close(null);
        close(channels[0]); close(channels[1]); free(process); shy_vm_error(error, "cannot allocate sandbox process"); return NULL;
    }
    char parent[64]; snprintf(parent, sizeof parent, "SHY_WORKER_PARENT=%ld", (long)getpid());
    char *const env[] = { "LC_ALL=C", parent, extension_role ? "SHY_WORKER_ROLE=extension" : "SHY_WORKER_ROLE=formula", NULL };
    char *const argv[] = { executable, NULL };
    struct rlimit descriptors;
    unsigned maximum = getrlimit(RLIMIT_NOFILE, &descriptors) == 0 && descriptors.rlim_cur != RLIM_INFINITY && descriptors.rlim_cur < 1048576 ?
        (unsigned)descriptors.rlim_cur : 1048576;
    pid_t pid = fork();
    if (!pid) {
        /* No allocation/stdio/VM work between fork and exec, even in SDL's
           multithreaded process. No inherited GUI, ledger or terminal handles. */
        if (dup2(null, STDIN_FILENO) < 0 || dup2(null, STDOUT_FILENO) < 0 || dup2(null, STDERR_FILENO) < 0 ||
            dup2(channels[1], SHY_IPC_FD) < 0 || fcntl(SHY_IPC_FD, F_SETFD, 0) < 0) _exit(125);
        shy_sandbox_close_fds(maximum);
        execve(executable, argv, env);
        _exit(126);
    }
    close(null); close(channels[1]);
    if (pid < 0) { close(channels[0]); free(process); shy_vm_error(error, "cannot fork sandbox worker"); return NULL; }
    process->pid = pid; process->fd = channels[0];
    int flags = fcntl(process->fd, F_GETFL);
    if (flags < 0 || fcntl(process->fd, F_SETFL, flags | O_NONBLOCK) < 0) {
        shy_process_destroy(process); shy_vm_error(error, "cannot configure sandbox IPC"); return NULL;
    }
    mulib_json_t *hello = receive_json(process->fd, SHY_IPC_REPLY, milliseconds()+SHY_SANDBOX_STARTUP_MS);
    int ok = hello && !strcmp(shy_text(hello, "op"), "ready") && !strcmp(shy_text(hello, "profile"), SHY_SANDBOX_PROFILE);
    if (!ok) shy_vm_error(error, hello && *shy_text(hello, "error") ? shy_text(hello, "error") : "sandbox startup failed, timed out or worker is missing");
    mulib_json_destroy(hello);
    if (!ok) { shy_process_destroy(process); return NULL; }
    *error = 0; return process;
#else
    (void)extension_role;
    shy_vm_error(error, "process sandbox is unavailable on this platform/architecture"); return NULL;
#endif
}

mulib_json_t *shy_process_request(shy_process *process, const mulib_json_t *request,
    shy_vm_broker broker, void *user, char error[SHY_VM_ERROR])
{
    *error = 0;
    if (!process) { shy_vm_error(error, "sandbox worker is not running"); return NULL; }
    int64_t deadline = milliseconds()+SHY_SANDBOX_TIMEOUT_MS;
    if (!send_json(process->fd, request, SHY_IPC_REQUEST, deadline)) {
        shy_vm_error(error, "sandbox request failed or exceeded its deadline"); return NULL;
    }
    for (size_t count = 0; count <= 256; ++count) {
        mulib_json_t *response = receive_json(process->fd, SHY_IPC_REPLY, deadline);
        if (!response) { shy_vm_error(error, "sandbox worker exited, violated policy, timed out or sent invalid IPC"); return NULL; }
        if (!strcmp(shy_text(response, "op"), "result")) {
            if (mulib_json_typeof(mulib_json_get(response, "ok")) != MULIB_JSON_BOOL ||
                strlen(shy_text(response, "error")) >= SHY_VM_ERROR) {
                mulib_json_destroy(response); shy_vm_error(error, "invalid sandbox result"); return NULL;
            }
            return response;
        }
        if (!broker || count == 256) {
            mulib_json_destroy(response); shy_vm_error(error, "sandbox sent an unauthorized or excessive broker request"); return NULL;
        }
        mulib_json_t *reply = broker(user, response);
        mulib_json_destroy(response);
        int ok = reply && send_json(process->fd, reply, SHY_IPC_REQUEST, deadline);
        mulib_json_destroy(reply);
        if (!ok) { shy_vm_error(error, "sandbox broker response failed or exceeded its deadline"); return NULL; }
    }
    shy_vm_error(error, "sandbox broker request limit exceeded"); return NULL;
}
