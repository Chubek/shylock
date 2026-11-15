#define _POSIX_C_SOURCE 200809L
#include "intelligence.h"
#include "dragnet.h"
#include "mulib-url.h"
#include "mulib-utf8.h"
#include <openssl/ssl.h>
#include <openssl/err.h>
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

typedef struct { SSL_CTX *context; SSL *ssl; int fd; int64_t deadline; } tls_session;
int shy_openai_endpoint_parse(const char *url,shy_openai_endpoint *out)
{
    mulib_uri_t uri;
    if (!url || !out || strlen(url)>4096 || !mulib_utf8_validate((const unsigned char *)url,strlen(url)) || strncmp(url,"https://",8) ||
        !mulib_uri_parse(url,strlen(url),&uri) || !uri.has_authority || uri.has_userinfo ||
        uri.has_query || uri.has_fragment || !uri.host.length || uri.host.length>253) return 0;
    unsigned char decoded[4096]; size_t length=0;
    if (!mulib_url_decode(url,strlen(url),decoded,sizeof decoded,&length)) return 0;
    for (size_t i=0;i<length;++i) if (decoded[i]<32 || decoded[i]==127 || decoded[i]=='\\') return 0;
    for (const char *p=url;*p;++p) if ((unsigned char)*p<=32 || *p=='\\') return 0;
    /* Reuse the listing transport's conservative hostname/public-URL policy,
       but model services may explicitly use another HTTPS port. */
    char authority[272]; shy_openai_endpoint endpoint={0};
    int n=snprintf(authority,sizeof authority,"https://%.*s/",(int)uri.host.length,uri.host.data);
    if (n<0 || (size_t)n>=sizeof authority || !shy_dragnet_url_host(authority,endpoint.host)) return 0;
    endpoint.port=443;
    if (uri.has_port) {
        if (!uri.port.length || uri.port.length>5) return 0;
        unsigned port=0;
        for (size_t i=0;i<uri.port.length;++i) {
            unsigned char c=(unsigned char)uri.port.data[i];
            if (c<'0' || c>'9') return 0;
            port=port*10+(unsigned)(c-'0');
        }
        if (!port || port>65535) return 0;
        endpoint.port=port;
    }
    length=strlen(url);
    while (length && url[length-1]=='/') --length;
    memcpy(endpoint.base_url,url,length); endpoint.base_url[length]=0;
    *out=endpoint; return 1;
}
int shy_openai_endpoint_env(shy_openai_endpoint *out)
{
    const char *url=getenv("OPENAI_BASE_URL");
    return shy_openai_endpoint_parse(url && *url ? url : "https://api.openai.com/v1",out);
}
static int64_t milliseconds(void)
{
    struct timespec t;
    return clock_gettime(CLOCK_MONOTONIC,&t) ? -1 : (int64_t)t.tv_sec*1000+t.tv_nsec/1000000;
}
static int wait_for(tls_session *s,int writing)
{
    for (;;) {
        int64_t now=milliseconds();
        if (now<0 || now>=s->deadline) return 0;
        struct pollfd p={s->fd,writing ? POLLOUT : POLLIN,0};
        int result=poll(&p,1,(int)(s->deadline-now));
        if (result<0 && errno==EINTR) continue;
        return result>0 && !(p.revents&POLLNVAL);
    }
}
static void close_tls(void *context)
{
    tls_session *s=context;
    if (s) { SSL_free(s->ssl); SSL_CTX_free(s->context); if (s->fd>=0) close(s->fd); free(s); }
}
static int handshake(void *context,int *verified)
{
    tls_session *s=context; *verified=0;
    for (;;) {
        if (milliseconds()<0 || milliseconds()>=s->deadline) return MULIB_TSL_EAUTH;
        int result=SSL_connect(s->ssl);
        if (result==1) {
            const unsigned char *alpn=NULL; unsigned length=0; SSL_get0_alpn_selected(s->ssl,&alpn,&length);
            if (SSL_get_verify_result(s->ssl)!=X509_V_OK || (length && (length!=8 || memcmp(alpn,"http/1.1",8)))) return MULIB_TSL_EAUTH;
            *verified=1; return MULIB_TSL_OK;
        }
        int error=SSL_get_error(s->ssl,result);
        if ((error!=SSL_ERROR_WANT_READ && error!=SSL_ERROR_WANT_WRITE) || !wait_for(s,error==SSL_ERROR_WANT_WRITE)) return MULIB_TSL_EAUTH;
    }
}
static int read_tls(void *context,void *buffer,size_t cap,size_t *count)
{
    tls_session *s=context; *count=0;
    for (;;) {
        if (milliseconds()<0 || milliseconds()>=s->deadline) return MULIB_TSL_EBACKEND;
        int result=SSL_read_ex(s->ssl,buffer,cap,count);
        if (result==1) return MULIB_TSL_OK;
        *count=0; int error=SSL_get_error(s->ssl,result);
        if (error==SSL_ERROR_ZERO_RETURN) return MULIB_TSL_EOF;
        if ((error!=SSL_ERROR_WANT_READ && error!=SSL_ERROR_WANT_WRITE) || !wait_for(s,error==SSL_ERROR_WANT_WRITE)) return MULIB_TSL_EBACKEND;
    }
}
static int write_tls(void *context,const void *buffer,size_t length,size_t *count)
{
    tls_session *s=context; *count=0;
    for (;;) {
        if (milliseconds()<0 || milliseconds()>=s->deadline) return MULIB_TSL_EBACKEND;
        int result=SSL_write_ex(s->ssl,buffer,length,count);
        if (result==1) return MULIB_TSL_OK;
        *count=0; int error=SSL_get_error(s->ssl,result);
        if ((error!=SSL_ERROR_WANT_READ && error!=SSL_ERROR_WANT_WRITE) || !wait_for(s,error==SSL_ERROR_WANT_WRITE)) return MULIB_TSL_EBACKEND;
    }
}
int shy_openai_connect(void *unused,const mulib_uri_t *endpoint,mulib_tsl_t *out)
{
    (void)unused;
    /* Only the trusted environment selects a service. MuLIB disables redirects;
       validate/pin the actual peer and verify that peer's certificate identity. */
    if (!endpoint || !out || endpoint->scheme.length!=5 || memcmp(endpoint->scheme.data,"https",5) ||
        !endpoint->has_authority || endpoint->has_userinfo || !endpoint->host.length || endpoint->host.length>253 ||
        endpoint->port.length>5) return 0;
    char authority[288];
    int n=snprintf(authority,sizeof authority,"https://%.*s:%.*s",(int)endpoint->host.length,endpoint->host.data,
        (int)endpoint->port.length,endpoint->port.data);
    shy_openai_endpoint peer_endpoint;
    if (n<0 || (size_t)n>=sizeof authority || !shy_openai_endpoint_parse(authority,&peer_endpoint)) return 0;
    char address[16]; if (!shy_dragnet_resolve(peer_endpoint.host,address)) return 0;
    tls_session *s=calloc(1,sizeof *s); if (!s) return 0;
    s->fd=-1; int64_t now=milliseconds(); if (now<0) { close_tls(s); return 0; }
    s->deadline=now+30000;
    s->fd=socket(AF_INET,SOCK_STREAM,0);
    if (s->fd<0 || fcntl(s->fd,F_SETFL,O_NONBLOCK)<0) { close_tls(s); return 0; }
    struct sockaddr_in peer={0}; peer.sin_family=AF_INET; peer.sin_port=htons((uint16_t)peer_endpoint.port);
    if (inet_pton(AF_INET,address,&peer.sin_addr)!=1) { close_tls(s); return 0; }
    int result=connect(s->fd,(const struct sockaddr *)&peer,sizeof peer);
    if (result<0 && errno!=EINPROGRESS) { close_tls(s); return 0; }
    if (result<0) {
        s->deadline=now+5000;
        int error=0; socklen_t length=sizeof error;
        if (!wait_for(s,1) || getsockopt(s->fd,SOL_SOCKET,SO_ERROR,&error,&length) || error) { close_tls(s); return 0; }
        s->deadline=now+30000;
    }
    s->context=SSL_CTX_new(TLS_client_method());
    if (!s->context || !SSL_CTX_set_default_verify_paths(s->context) || !SSL_CTX_set_min_proto_version(s->context,TLS1_2_VERSION)) { close_tls(s); return 0; }
    SSL_CTX_set_verify(s->context,SSL_VERIFY_PEER,NULL);
    s->ssl=SSL_new(s->context);
    static const unsigned char alpn[]={8,'h','t','t','p','/','1','.','1'};
    if (!s->ssl || !SSL_set_fd(s->ssl,s->fd)) { close_tls(s); return 0; }
    unsigned char ip[4];
    int identity=inet_pton(AF_INET,peer_endpoint.host,ip)==1 ?
        X509_VERIFY_PARAM_set1_ip_asc(SSL_get0_param(s->ssl),peer_endpoint.host) :
        SSL_set_tlsext_host_name(s->ssl,peer_endpoint.host) && SSL_set1_host(s->ssl,peer_endpoint.host);
    if (!identity || SSL_set_alpn_protos(s->ssl,alpn,sizeof alpn)) { close_tls(s); return 0; }
    static const mulib_tsl_ops_t ops={handshake,read_tls,write_tls,close_tls};
    if (!mulib_tsl_init(out,&ops,s)) { close_tls(s); return 0; }
    return 1;
}
