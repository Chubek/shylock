#define _POSIX_C_SOURCE 200809L
#define _XOPEN_SOURCE 700
#include "daemon.h"
#include "dragnet.h"
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static int lock_fd=-1, output_fd=-1, saved_stdout=-1, sink_fd=-1, run_fd=-1;
static char output[4096], run_path[4096], sink_name[128], part_name[140];
static char *read_text;
static volatile sig_atomic_t stopping;
static struct sigaction old_term,old_int;
static int signals_set;
static pid_t owner;
static volatile sig_atomic_t worker=-1;

static void stop_signal(int number)
{
    int saved=errno;
    (void)number; stopping=1;
    if (worker>0) (void)kill((pid_t)worker,SIGTERM);
    errno=saved;
}
const char *shy_daemon_read(const char *path)
{
    size_t size;
    free(read_text); read_text=shy_read(path,65536,&size);
    return read_text ? read_text : "";
}
const char *shy_daemon_absolute(const char *path)
{
    static char absolute[4096];
    return realpath(path,absolute) ? absolute : "";
}
const char *shy_daemon_host(const char *url)
{
    static char host[256];
    return shy_dragnet_url_host(url,host) ? host : "";
}
int64_t shy_daemon_time(void) { return (int64_t)time(NULL); }
int64_t shy_daemon_stopping(void) { return stopping!=0; }

static int write_all(int fd,const char *text,size_t length)
{
    while (length) {
        ssize_t n=write(fd,text,length);
        if (n<0 && errno==EINTR) continue;
        if (n<=0) return 0;
        text+=n; length-=(size_t)n;
    }
    return 1;
}
static int pid_write(void)
{
    char pid[64]; int n=snprintf(pid,sizeof pid,"%ld\n",(long)getpid());
    return ftruncate(lock_fd,0)==0 && lseek(lock_fd,0,SEEK_SET)==0 &&
        write_all(lock_fd,pid,(size_t)n) && fsync(lock_fd)==0;
}
int64_t shy_daemon_close(void)
{
    if (saved_stdout>=0) {
        fflush(stdout); dup2(saved_stdout,STDOUT_FILENO); close(saved_stdout); saved_stdout=-1;
        clearerr(stdout);
    }
    if (sink_fd>=0) { close(sink_fd); sink_fd=-1; }
    if (run_fd>=0) { if (*part_name) unlinkat(run_fd,part_name,0); close(run_fd); run_fd=-1; }
    if (lock_fd>=0) { if (owner==getpid()) (void)ftruncate(lock_fd,0); close(lock_fd); lock_fd=-1; }
    if (output_fd>=0) { close(output_fd); output_fd=-1; }
    if (signals_set) {
        sigaction(SIGTERM,&old_term,NULL); sigaction(SIGINT,&old_int,NULL); signals_set=0;
    }
    free(read_text); read_text=NULL; *part_name=0; return 0;
}
int64_t shy_daemon_start(const char *directory,int64_t detach)
{
    if (lock_fd>=0 || strlen(directory)>=sizeof output) return -1;
    if (mkdir(directory,0700)<0 && errno!=EEXIST) return -1;
    output_fd=open(directory,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
    if (output_fd<0) return -1;
    strcpy(output,directory);
    lock_fd=openat(output_fd,"daemon.pid",O_RDWR|O_CREAT|O_NOFOLLOW|O_CLOEXEC,0600);
    struct stat st;
    if (lock_fd<0 || fstat(lock_fd,&st)<0 || !S_ISREG(st.st_mode) || flock(lock_fd,LOCK_EX|LOCK_NB)<0) {
        /* A competing instance owns the pid contents; never truncate them. */
        if (lock_fd>=0) close(lock_fd);
        lock_fd=-1; shy_daemon_close(); return -1;
    }
    stopping=0;
    struct sigaction action={0}; action.sa_handler=stop_signal; sigemptyset(&action.sa_mask);
    if (sigaction(SIGTERM,&action,&old_term)<0) { shy_daemon_close(); return -1; }
    if (sigaction(SIGINT,&action,&old_int)<0) {
        sigaction(SIGTERM,&old_term,NULL); shy_daemon_close(); return -1;
    }
    signals_set=1;
    if (detach) {
        int ready[2];
        if (pipe(ready)<0) { shy_daemon_close(); return -1; }
        fflush(NULL);
        pid_t child=fork();
        if (child<0) { close(ready[0]); close(ready[1]); shy_daemon_close(); return -1; }
        if (child>0) {
            close(ready[1]); char result=0; ssize_t n;
            do { n=read(ready[0],&result,1); } while (n<0 && errno==EINTR);
            close(ready[0]);
            while (waitpid(child,NULL,0)<0 && errno==EINTR) {}
            /* flock belongs to the inherited open description. Parent must not
               truncate/unlock the worker's PID file. */
            close(lock_fd); lock_fd=-1; shy_daemon_close();
            return n==1 && result==1 ? 0 : -1;
        }
        close(ready[0]);
        if (setsid()<0) _exit(2);
        pid_t grandchild=fork();
        if (grandchild<0) _exit(2);
        if (grandchild>0) _exit(0);
        umask(077);
        int log=openat(output_fd,"daemon.log",O_WRONLY|O_APPEND|O_CREAT|O_NOFOLLOW|O_CLOEXEC,0600);
        int null=open("/dev/null",O_RDWR|O_CLOEXEC);
        if (log<0 || null<0 || dup2(null,STDIN_FILENO)<0 || dup2(null,STDOUT_FILENO)<0 ||
            dup2(log,STDERR_FILENO)<0 || !pid_write()) _exit(2);
        close(log); close(null);
        if (!write_all(ready[1],"\1",1)) _exit(2);
        close(ready[1]); owner=getpid(); return 1;
    }
    if (!pid_write()) { shy_daemon_close(); return -1; }
    owner=getpid(); return 1;
}
/* The pinned On1x runtime retains string snapshots until process exit. Fork a
   fresh bounded run from the admitted configuration, not an ever-growing pool. */
int64_t shy_daemon_fork(void)
{
    fflush(NULL); worker=fork();
    if (worker<0) return -1;
    return worker==0 ? 1 : 0;
}
int64_t shy_daemon_reap(void)
{
    if (worker<=0) return -1;
    int status;
    for (;;) {
        if (stopping) (void)kill(worker,SIGTERM);
        if (waitpid(worker,&status,0)==worker) break;
        if (errno!=EINTR) return -1;
    }
    worker=-1;
    if (!WIFEXITED(status)) return -1;
    return WEXITSTATUS(status);
}
int64_t shy_daemon_exit(int64_t code)
{
    shy_daemon_close(); fflush(NULL); _exit((int)code);
}
int64_t shy_daemon_wait(int64_t seconds)
{
    struct timespec now;
    if (seconds<0 || seconds>604800 || clock_gettime(CLOCK_MONOTONIC,&now)<0) return -1;
    time_t end=now.tv_sec+(time_t)seconds; long nano=now.tv_nsec;
    while (!stopping) {
        if (clock_gettime(CLOCK_MONOTONIC,&now)<0) return -1;
        if (now.tv_sec>end || (now.tv_sec==end && now.tv_nsec>=nano)) return 1;
        struct timespec delay={0,100000000};
        if (nanosleep(&delay,NULL)<0 && errno!=EINTR) return -1;
    }
    return 0;
}
const char *shy_daemon_run(void)
{
    if (run_fd>=0) { close(run_fd); run_fd=-1; }
    int n=snprintf(run_path,sizeof run_path,"%s/run-%lld-XXXXXX",output,(long long)shy_daemon_time());
    if (output_fd<0 || n<0 || (size_t)n>=sizeof run_path || !mkdtemp(run_path)) return "";
    run_fd=open(run_path,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
    return run_fd>=0 && fsync(output_fd)==0 ? run_path : "";
}
int64_t shy_daemon_sink(const char *directory,const char *name)
{
    if (run_fd<0 || strcmp(directory,run_path) || sink_fd>=0 || saved_stdout>=0 || !*name ||
        strlen(name)>=sizeof sink_name || strspn(name,"abcdefghijklmnopqrstuvwxyz0123456789-.")!=strlen(name)) return 0;
    strcpy(sink_name,name); snprintf(part_name,sizeof part_name,"%s.part",name);
    sink_fd=openat(run_fd,part_name,O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW|O_CLOEXEC,0600);
    if (sink_fd<0) { *part_name=0; return 0; }
    if (fflush(stdout)!=0 || (saved_stdout=dup(STDOUT_FILENO))<0 || dup2(sink_fd,STDOUT_FILENO)<0) {
        if (saved_stdout>=0) close(saved_stdout);
        saved_stdout=-1; close(sink_fd); sink_fd=-1; unlinkat(run_fd,part_name,0); *part_name=0; return 0;
    }
    (void)fcntl(saved_stdout,F_SETFD,FD_CLOEXEC); clearerr(stdout); return 1;
}
int64_t shy_daemon_commit(void)
{
    if (sink_fd<0 || saved_stdout<0) return 0;
    int ok=fflush(stdout)==0 && !ferror(stdout);
    if (dup2(saved_stdout,STDOUT_FILENO)<0) ok=0;
    close(saved_stdout); saved_stdout=-1; clearerr(stdout);
    struct stat st;
    int empty=0;
    if (fstat(sink_fd,&st)<0 || st.st_size>SHY_DATA_LIMIT || fsync(sink_fd)<0) ok=0;
    else empty=st.st_size==0;
    if (close(sink_fd)<0) ok=0;
    sink_fd=-1;
    /* Hard-link publication is atomic and cannot overwrite an earlier artifact. */
    if (ok && !empty && linkat(run_fd,part_name,run_fd,sink_name,0)<0) ok=0;
    if (unlinkat(run_fd,part_name,0)<0) ok=0;
    *part_name=0;
    if (fsync(run_fd)<0) ok=0;
    return ok ? (empty ? 2 : 1) : 0;
}
int64_t shy_daemon_write(const char *directory,const char *name,const char *text)
{
    if (!shy_daemon_sink(directory,name)) return 0;
    int ok=fputs(text,stdout)>=0;
    return shy_daemon_commit()==1 && ok;
}
