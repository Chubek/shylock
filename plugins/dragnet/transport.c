#define _POSIX_C_SOURCE 200809L
#include "../../src/shylock.h"
#include "dragnet.h"
#include "mulib-url.h"
#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

int shy_dragnet_public_ipv4(const unsigned char a[4])
{
    return a[0]!=0 && a[0]!=10 && a[0]!=127 && a[0]<224 &&
        !(a[0]==100 && a[1]>=64 && a[1]<=127) && !(a[0]==169 && a[1]==254) &&
        !(a[0]==172 && a[1]>=16 && a[1]<=31) && !(a[0]==192 && (a[1]==0 || a[1]==168)) &&
        !(a[0]==192 && a[1]==88 && a[2]==99) && !(a[0]==198 && (a[1]==18 || a[1]==19)) &&
        !(a[0]==198 && a[1]==51 && a[2]==100) && !(a[0]==203 && a[1]==0 && a[2]==113);
}
int shy_dragnet_url_host(const char *url,char host[256])
{
    mulib_fetch_policy_t p={4096,SHY_PAGE_LIMIT,0,1,0};
    unsigned char decoded[4096]; size_t size;
    if (!url || strncmp(url,"https://",8) || !mulib_fetch_policy_check(&p,url) ||
        !mulib_url_decode(url,strlen(url),decoded,sizeof decoded,&size)) return 0;
    for (size_t i=0;i<size;++i) if (decoded[i]<32 || decoded[i]==127 || decoded[i]=='\\') return 0;
    for (const char *s=url;*s;++s) if ((unsigned char)*s<=32 || *s=='\\') return 0;
    size_t n=strcspn(url+8,"/?#");
    if (!n || n>253) return 0;
    size_t label=0;
    for (size_t i=0;i<n;++i) {
        unsigned char c=(unsigned char)url[8+i];
        if (c>='A' && c<='Z') c=(unsigned char)(c+32);
        if (c=='.') { if (!label || host[i-1]=='-') return 0; label=0; }
        else if (!((c>='a' && c<='z') || (c>='0' && c<='9') || c=='-') ||
            (!label && c=='-') || ++label>63) return 0;
        host[i]=(char)c;
    }
    host[n]=0;
    return label && host[n-1]!='-'; /* Only DNS hosts, implicit port 443. */
}
static int64_t now_ms(void)
{
    struct timespec t;
    return clock_gettime(CLOCK_MONOTONIC,&t) ? -1 : (int64_t)t.tv_sec*1000+t.tv_nsec/1000000;
}
/* Bounded drain and reap. Deadline applies even to a stalled injected executable
   or resolver; kill the entire child group on timeout, errors or excess bytes. */
static int collect(pid_t pid,int fd,char *buffer,size_t cap,size_t *length,int ms)
{
    size_t used=0; int ok=buffer!=NULL, ended=0; int64_t start=now_ms();
    while (ok && !ended) {
        int64_t current=now_ms();
        if (start<0 || current<0 || current-start>=ms) { ok=0; break; }
        struct pollfd f={fd,POLLIN,0};
        int ready=poll(&f,1,(int)(ms-(current-start)));
        if (ready<0 && errno==EINTR) continue;
        if (ready<=0) { ok=0; break; }
        char extra; ssize_t got=read(fd,used<cap ? buffer+used : &extra,used<cap ? cap-used : 1);
        if (got<0 && errno==EINTR) continue;
        if (got<0) ok=0;
        else if (!got) ended=1;
        else if (used==cap) ok=0;
        else used+=(size_t)got;
    }
    close(fd);
    /* A child can close stdout and keep running: reap remains deadline-bounded. */
    int status=0; pid_t result=0;
    while (ok) {
        result=waitpid(pid,&status,WNOHANG);
        if (result<0 && errno==EINTR) continue;
        if (result<0) { ok=0; break; }
        if (result==pid) break;
        int64_t current=now_ms();
        if (current<0 || current-start>=ms) { ok=0; break; }
        (void)poll(NULL,0,5);
    }
    if (!ok) { (void)kill(-pid,SIGKILL); (void)kill(pid,SIGKILL); }
    if (result!=pid) do { result=waitpid(pid,&status,0); } while (result<0 && errno==EINTR);
    *length=used;
    return ok && result==pid && WIFEXITED(status) && !WEXITSTATUS(status);
}
int shy_dragnet_resolve(const char *host,char address[INET_ADDRSTRLEN])
{
    int fds[2]; if (pipe(fds)) return 0;
    pid_t pid=fork();
    if (!pid) {
        (void)setpgid(0,0); close(fds[0]);
        struct addrinfo hints={0},*list=NULL; hints.ai_family=AF_INET; hints.ai_socktype=SOCK_STREAM;
        int ok=!getaddrinfo(host,"443",&hints,&list); char selected[INET_ADDRSTRLEN]={0};
        for (struct addrinfo *p=list;ok && p;p=p->ai_next) {
            const struct sockaddr_in *in=(const struct sockaddr_in *)p->ai_addr;
            ok=p->ai_addrlen>=sizeof *in && shy_dragnet_public_ipv4((const unsigned char *)&in->sin_addr);
            if (ok && !*selected) ok=inet_ntop(AF_INET,&in->sin_addr,selected,sizeof selected)!=NULL;
        }
        freeaddrinfo(list);
        if (ok && *selected) ok=write(fds[1],selected,strlen(selected))==(ssize_t)strlen(selected);
        close(fds[1]); _exit(ok && *selected ? 0 : 1);
    }
    close(fds[1]); if (pid<0) { close(fds[0]); return 0; }
    (void)setpgid(pid,pid);
    size_t n=0;
    int ok=collect(pid,fds[0],address,INET_ADDRSTRLEN-1,&n,3000);
    address[n]=0; return ok && n>0;
}
int shy_dragnet_https(void *context,const char *url,mulib_shylock_buffer_t *body,
    unsigned *status,char *type,size_t cap)
{
    const shy_dragnet_transport *options=context;
    char host[256],address[INET_ADDRSTRLEN],pin[320];
    if (!body || !status || !type || !cap || !shy_dragnet_url_host(url,host) ||
        (options && options->deadline_ms>20000)) return 0;
    memset(address,0,sizeof address);
    int resolved=options && options->resolve ? options->resolve(options->context,host,address) : shy_dragnet_resolve(host,address);
    unsigned char raw[4];
    if (!resolved || !memchr(address,0,sizeof address) || inet_pton(AF_INET,address,raw)!=1 || !shy_dragnet_public_ipv4(raw)) return 0;
    snprintf(pin,sizeof pin,"%s:443:%s",host,address);
    int fds[2]; if (pipe(fds)) return 0;
    pid_t pid=fork();
    if (!pid) {
        (void)setpgid(0,0); close(fds[0]);
        if (dup2(fds[1],STDOUT_FILENO)<0) _exit(126);
        close(fds[1]);
        execlp("curl","curl","-q","--silent","--show-error","--globoff","--ipv4","--http1.1",
            "--proxy","","--noproxy","*","--proto","=https","--proto-redir","=https",
            "--tlsv1.2","--max-redirs","0","--retry","0","--connect-timeout","5","--max-time","15",
            "--max-filesize","2097152","--resolve",pin,"--write-out","\n%{http_code}\n%{content_type}\n",
            "--",url,(char *)NULL);
        _exit(127);
    }
    close(fds[1]); if (pid<0) { close(fds[0]); return 0; }
    (void)setpgid(pid,pid);
    size_t length=0,limit=SHY_PAGE_LIMIT+512;
    char *bytes=malloc(limit+1);
    int deadline=options && options->deadline_ms ? (int)options->deadline_ms : 20000;
    int ok=collect(pid,fds[0],bytes,limit,&length,deadline) && length>=6 && bytes[length-1]=='\n' && !memchr(bytes,0,length);
    if (ok) {
        bytes[length-1]=0;
        char *media=strrchr(bytes,'\n');
        ok=media && media>=bytes+4 && media[-4]=='\n' && strspn(media-3,"0123456789")==3 && strlen(media+1)<cap &&
            (size_t)(media-4-bytes)<=SHY_PAGE_LIMIT;
        if (ok) {
            unsigned code=(unsigned)((media[-3]-'0')*100+(media[-2]-'0')*10+media[-1]-'0');
            ok=mulib_shylock_append(body,bytes,(size_t)(media-4-bytes));
            if (ok) { *status=code; strcpy(type,media+1); }
        }
    }
    free(bytes); return ok;
}
int shy_dragnet_get(const char *url,mulib_fetch_transport_fn transport,void *context,
    mulib_shylock_buffer_t *body,unsigned *status,char *type,size_t cap)
{
    char host[256];
    mulib_fetch_client_t client={{4096,SHY_PAGE_LIMIT,0,1,0},transport,context};
    return shy_dragnet_url_host(url,host) && mulib_fetch_get(&client,url,body,status,type,cap);
}
