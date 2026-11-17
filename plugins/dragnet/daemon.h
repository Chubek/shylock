#ifndef SHYLOCK_DRAGNET_DAEMON_H
#define SHYLOCK_DRAGNET_DAEMON_H
#include <stdint.h>
/* Single synchronous OS session. Read/path strings are borrowed until the next
   call to the same accessor. No tree or FILE ownership crosses the scalar ABI.
   start: 1 worker, 0 detached parent, -1 failure. Commit returns 1 published,
   2 empty (no artifact), 0 I/O failure. Sink commit restores stdout
   even on failure; close releases the lock and all partial resources. */
const char *shy_daemon_read(const char *);
const char *shy_daemon_absolute(const char *);
const char *shy_daemon_host(const char *);
int64_t shy_daemon_start(const char *,int64_t);
int64_t shy_daemon_close(void);
int64_t shy_daemon_stopping(void);
int64_t shy_daemon_wait(int64_t);
int64_t shy_daemon_time(void);
const char *shy_daemon_run(void);
int64_t shy_daemon_sink(const char *,const char *);
int64_t shy_daemon_commit(void);
int64_t shy_daemon_write(const char *,const char *,const char *);
int64_t shy_daemon_fork(void);
int64_t shy_daemon_reap(void);
int64_t shy_daemon_exit(int64_t);
#endif
