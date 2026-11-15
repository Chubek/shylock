#define _POSIX_C_SOURCE 200809L
#include "../plugins/dragnet/intelligence.h"
#include "../plugins/dragnet/workflow.h"
#include "mulib-shylock-core.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    char *wire;
    size_t offset;
    mulib_shylock_buffer_t request;
    int connects,closes,verified;
    const char *host,*port;
} fixture;
static int handshake(void *context,int *verified)
{ fixture *f=context; *verified=f->verified; return 0; }
static int read_response(void *context,void *data,size_t cap,size_t *count)
{
    fixture *f=context; size_t left=strlen(f->wire)-f->offset;
    *count=left<cap ? left : cap; if (*count>37) *count=37;
    if (!*count) return MULIB_TSL_EOF;
    memcpy(data,f->wire+f->offset,*count); f->offset+=*count; return 0;
}
static int write_request(void *context,const void *data,size_t n,size_t *count)
{
    fixture *f=context; *count=n>73 ? 73 : n;
    return mulib_shylock_append(&f->request,data,*count) ? 0 : MULIB_TSL_EBACKEND;
}
static void close_fixture(void *context) { ((fixture *)context)->closes++; }
static int connect_fixture(void *context,const mulib_uri_t *endpoint,mulib_tsl_t *out)
{
    fixture *f=context; f->connects++;
    const char *host=f->host ? f->host : "api.openai.com",*port=f->port ? f->port : "443";
    if (endpoint->host.length!=strlen(host) || memcmp(endpoint->host.data,host,strlen(host)) ||
        endpoint->port.length!=strlen(port) || memcmp(endpoint->port.data,port,strlen(port))) return 0;
    static const mulib_tsl_ops_t ops={handshake,read_response,write_request,close_fixture};
    return mulib_tsl_init(out,&ops,f);
}
static char *response(const char *answer,int status)
{
    mulib_json_t *root=mulib_json_new(MULIB_JSON_OBJECT),*output=mulib_json_new(MULIB_JSON_ARRAY),*item=mulib_json_new(MULIB_JSON_OBJECT);
    mulib_json_t *content=mulib_json_new(MULIB_JSON_ARRAY),*part=mulib_json_new(MULIB_JSON_OBJECT);
    int ok=shy_add(root,"status",mulib_json_string("completed")) && shy_add(item,"type",mulib_json_string("message")) &&
        shy_add(part,"type",mulib_json_string("output_text")) && shy_add(part,"text",mulib_json_string(answer));
    if (ok) { ok=shy_push(content,part); part=NULL; }
    if (ok) { ok=shy_add(item,"content",content); content=NULL; }
    if (ok) { ok=shy_push(output,item); item=NULL; }
    if (ok) { ok=shy_add(root,"output",output); output=NULL; }
    char *body=ok ? mulib_json_stringify(root,0) : NULL,*wire=NULL;
    if (body) {
        size_t n=strlen(body)+256; wire=malloc(n);
        if (wire) snprintf(wire,n,"HTTP/1.1 %d Response\r\nContent-Type: application/json\r\nContent-Length: %zu\r\nConnection: close\r\n\r\n%s",status,strlen(body),body);
    }
    free(body); mulib_json_destroy(root); mulib_json_destroy(output); mulib_json_destroy(item); mulib_json_destroy(content); mulib_json_destroy(part);
    return wire;
}
int64_t shy_native_intelligence_test(void)
{
    const char *prior_base=getenv("OPENAI_BASE_URL"); char *saved_base=prior_base ? strdup(prior_base) : NULL;
    unsetenv("OPENAI_BASE_URL");
    const char html[]="<nav>EUR 999.00 total</nav><main><p>EUR <b>180</b>.00 per night</p><p>EUR 75.00 cleaning fee</p><p hidden>secret</p></main>";
    mulib_json_t *snapshot=shy_snapshot_record(html,strlen(html),"https://example.org/cottage",1000,"saved",0,"text/html");
    mulib_json_t *ir=snapshot ? shy_flatten(snapshot) : NULL;
    int ok=ir && mulib_json_size(mulib_json_get(ir,"blocks"))==2 && !strcmp(shy_text(mulib_json_at(mulib_json_get(ir,"blocks"),0),"text"),"EUR 180.00 per night");
    mulib_json_t *found=ir ? shy_find_information(ir,"prices") : NULL;
    ok=ok && found && mulib_json_size(mulib_json_get(found,"matches"))==2 &&
        shy_integer(mulib_json_at(mulib_json_get(found,"matches"),0),"minor",1000000)==18000;
    mulib_json_destroy(found); mulib_json_destroy(ir); mulib_json_destroy(snapshot);
    const char source[]="<main><p>EUR 180.00 per night</p></main>";
    snapshot=shy_snapshot_record(source,strlen(source),"https://example.org/cottage",1000,"saved",0,"text/html");
    ir=shy_flatten(snapshot);
    const char profile_source[]="{\"schema\":\"shylock.openai-profile/v1\",\"model\":\"fixture-model\",\"api_key_env\":\"SHY_TEST_OPENAI_KEY\",\"organization\":\"org-fixture\",\"project\":\"proj-fixture\",\"input\":\"ir\",\"max_output_tokens\":1024}";
    mulib_json_t *profile=mulib_json_parse(profile_source,strlen(profile_source),NULL);
    const char *prior=getenv("SHY_TEST_OPENAI_KEY"); char *saved=prior ? strdup(prior) : NULL;
    if (setenv("SHY_TEST_OPENAI_KEY","secret-test-key-not-an-account",1)) ok=0;
    static const char *const answers[]={
        "{\"suggestions\":[{\"block\":0,\"quote\":\"EUR 180.00 per night\",\"value\":\"180.00\",\"amount\":\"180.00\",\"currency\":\"EUR\"}]}",
        "{\"suggestions\":[{\"block\":0,\"quote\":\"EUR 190.00 per night\",\"value\":\"190.00\",\"amount\":\"190.00\",\"currency\":\"EUR\"}]}",
        "{\"suggestions\":[{\"block\":0,\"quote\":\"EUR 180.00 per night\",\"value\":\"180.00\",\"amount\":\"180.00\",\"currency\":\"USD\"}]}",
        "{\"suggestions\":[{\"block\":0,\"quote\":\"EUR 180.00 per night\",\"value\":\"180.00\",\"amount\":\"180.00\",\"currency\":\"EUR\"},{\"block\":1,\"quote\":\"bad\",\"value\":\"bad\",\"amount\":\"\",\"currency\":\"\"}]}",
        "not json",
        "{\"suggestions\":[{\"block\":0,\"quote\":\"EUR 180.00 per night\",\"value\":\"80.00\",\"amount\":\"80.00\",\"currency\":\"EUR\"}]}"
    };
    size_t count=sizeof answers/sizeof *answers;
    for (size_t i=0;ok && i<count+2;++i) {
        fixture f={0}; f.verified=i!=count+1; f.request.limit=262144;
        f.wire=response(i==count ? "secret-test-key-not-an-account" : answers[i<count ? i : 0],i==count ? 429 : 200);
        int success=0;
        mulib_json_t *out=f.wire ? shy_model_information(ir,profile,"prices",connect_fixture,&f,&success) : NULL;
        char *serialized=out ? mulib_json_stringify(out,0) : NULL;
        ok=out && serialized && !strstr(serialized,"secret-test-key-not-an-account") && !strstr(serialized,"Authorization") &&
            success==(i==0) && f.connects==1 && f.closes==1;
        const mulib_json_t *claims=mulib_json_get(out,"model_claims"),*audit=mulib_json_get(out,"model_audit");
        ok=ok && mulib_json_size(claims)==(i==0 ? 1u : 0u);
        if (i==0) ok=ok && !strcmp(shy_text(audit,"outcome"),"validated_suggestions") &&
            strstr(f.request.data,"Authorization: Bearer secret-test-key-not-an-account") &&
            strstr(f.request.data,"POST /v1/responses HTTP/1.1") && strstr(f.request.data,"json_schema");
        if (i==count) ok=ok && !strcmp(shy_text(audit,"outcome"),"api_error");
        if (i==count+1) ok=ok && !f.request.len;
        free(serialized); free(f.wire); mulib_shylock_buffer_free(&f.request); mulib_json_destroy(out);
    }
    shy_openai_endpoint endpoint;
    ok=ok && shy_openai_endpoint_env(&endpoint) && !strcmp(endpoint.base_url,"https://api.openai.com/v1") &&
        !strcmp(endpoint.host,"api.openai.com") && endpoint.port==443;
    const char custom[]="https://gateway.example:8443/openai/v1///";
    ok=ok && shy_openai_endpoint_parse(custom,&endpoint) && endpoint.port==8443 &&
        !strcmp(endpoint.host,"gateway.example") && !strcmp(endpoint.base_url,"https://gateway.example:8443/openai/v1");
    if (setenv("OPENAI_BASE_URL",custom,1)) ok=0;
    fixture custom_fixture={0}; custom_fixture.verified=1; custom_fixture.request.limit=262144;
    custom_fixture.host="gateway.example"; custom_fixture.port="8443"; custom_fixture.wire=response(answers[0],200);
    int custom_success=0;
    found=custom_fixture.wire ? shy_model_information(ir,profile,"prices",connect_fixture,&custom_fixture,&custom_success) : NULL;
    ok=ok && found && custom_success && custom_fixture.connects==1 && custom_fixture.closes==1 &&
        strstr(custom_fixture.request.data,"POST /openai/v1/responses HTTP/1.1") &&
        strstr(custom_fixture.request.data,"Host: gateway.example:8443") &&
        !strcmp(shy_text(mulib_json_get(found,"model_audit"),"endpoint"),"https://gateway.example:8443/openai/v1/responses");
    if (found) {
        const char *reply=shy_text(mulib_json_get(found,"extraction"),"model_reply_source");
        ok=ok && strstr(reply,"https://gateway.example:8443/openai/v1/responses");
    }
    mulib_json_destroy(found); free(custom_fixture.wire); mulib_shylock_buffer_free(&custom_fixture.request);
    static const char *const invalid[]={"http://gateway.example/v1","https://localhost/v1","https://127.0.0.1/v1", "https://[::1]/v1",
        "https://user:secret@gateway.example/v1","https://gateway.example/v1?key=secret","https://gateway.example/v1#fragment",
        "https://gateway.example:0/v1","https://gateway.example:65536/v1","https://gateway.example/%0a","https://gateway.example/%ZZ"};
    for (size_t i=0;ok && i<sizeof invalid/sizeof *invalid;++i) {
        if (shy_openai_endpoint_parse(invalid[i],&endpoint) || setenv("OPENAI_BASE_URL",invalid[i],1)) { ok=0; break; }
        fixture blocked={0}; int accepted=0;
        found=shy_model_information(ir,profile,"prices",connect_fixture,&blocked,&accepted);
        ok=found && !accepted && !blocked.connects && !blocked.request.len &&
            !strcmp(shy_text(mulib_json_get(found,"model_audit"),"outcome"),"invalid_endpoint");
        mulib_json_destroy(found);
    }
    unsetenv("OPENAI_BASE_URL");
    unsetenv("SHY_TEST_OPENAI_KEY");
    fixture f={0}; int success=1;
    found=shy_model_information(ir,profile,"prices",connect_fixture,&f,&success);
    ok=ok && found && !success && !f.connects && !strcmp(shy_text(mulib_json_get(found,"model_audit"),"outcome"),"credentials_unavailable");
    mulib_json_destroy(found);
    if (saved) { setenv("SHY_TEST_OPENAI_KEY",saved,1); free(saved); }
    if (saved_base) { setenv("OPENAI_BASE_URL",saved_base,1); free(saved_base); }
    mulib_json_destroy(snapshot); mulib_json_destroy(ir); mulib_json_destroy(profile);
    return ok;
}
