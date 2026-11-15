#define _POSIX_C_SOURCE 200809L
#include "shylock.h"
#include "../include/shylock-sdk.h"
#include "mulib-money.h"
#include "mulib-utf8.h"
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { MAX_PLUGINS = 16 };
typedef struct {
    const shy_plugin *legacy;
    shy_plugin view;
    const shy_sdk_plugin *sdk;
    void *state, *library;
    char *configuration;
} registration;
static registration registry[MAX_PLUGINS];
static size_t count;
static int busy;
struct shy_sdk_sink { mulib_json_t *items, *diagnostics; size_t length; int failed; };

static int text(const char *s, size_t max)
{
    return s && strnlen(s, max+1) <= max && mulib_utf8_validate((const unsigned char *)s, strlen(s));
}
static int name_valid(const char *s)
{
    return text(s, 64) && *s && strspn(s, "abcdefghijklmnopqrstuvwxyz0123456789_-") == strlen(s);
}
const shy_plugin *shy_plugin_at(size_t i)
{ return i < count ? registry[i].legacy : NULL; }
const shy_plugin *shy_plugin_find(const char *name)
{
    if (name) for (size_t i=0; i<count; ++i)
        if (!strcmp(registry[i].legacy->name, name)) return registry[i].legacy;
    return NULL;
}
const char *shy_plugin_configuration(const shy_plugin *p)
{
    for (size_t i=0;i<count;++i) if (registry[i].legacy==p) return registry[i].configuration;
    return NULL;
}
int shy_plugin_register(const shy_plugin *p)
{
    for (size_t i=0;i<count;++i) if (registry[i].sdk) return 0;
    if (busy || !p || p->abi != SHY_EXTRACTOR_ABI || p->size < sizeof *p ||
        !name_valid(p->name) || !text(p->version, 64) || !*p->version || !p->extract ||
        count == MAX_PLUGINS || shy_plugin_find(p->name)) return 0;
    registry[count++].legacy = p; return 1;
}
static int emit_candidate(shy_sdk_sink *sink, const shy_sdk_candidate *c)
{
    if (!sink || sink->failed) return 0;
    int ok = c && c->size >= sizeof *c && text(c->method,64) && *c->method &&
        text(c->amount_text,128) && *c->amount_text && text(c->currency,3) &&
        text(c->basis,32) && *c->basis && text(c->qualifier,32) && *c->qualifier &&
        (!c->json_pointer || (text(c->json_pointer,1023) && (!*c->json_pointer || *c->json_pointer=='/'))) &&
        c->byte_length && c->byte_offset <= sink->length && c->byte_length <= sink->length-c->byte_offset &&
        (c->has_minor == 0 || c->has_minor == 1) && mulib_json_size(sink->items)<256;
    if (ok && c->has_minor) {
        mulib_money_t money;
        int scale = shy_currency_scale(c->currency);
        ok = scale>=0 && c->minor>=0 && c->minor<=INT64_C(1000000000000) &&
            mulib_money_parse(c->amount_text,c->currency,(uint8_t)scale,&money) && money.minor==c->minor;
    }
    mulib_json_t *row = ok ? mulib_json_new(MULIB_JSON_OBJECT) : NULL;
    if (ok) ok = shy_add(row,"method",mulib_json_string(c->method)) &&
        shy_add(row,"amount_text",mulib_json_string(c->amount_text)) &&
        shy_add(row,"currency",mulib_json_string(c->currency)) &&
        shy_add(row,"basis",mulib_json_string(c->basis)) &&
        shy_add(row,"qualifier",mulib_json_string(c->qualifier)) &&
        shy_add(row,"minor",c->has_minor ? mulib_json_number((double)c->minor) : mulib_json_new(MULIB_JSON_NULL)) &&
        shy_add(row,"byte_offset",mulib_json_number((double)c->byte_offset)) &&
        shy_add(row,"byte_length",mulib_json_number((double)c->byte_length));
    if (ok && c->json_pointer) ok=shy_add(row,"json_pointer",mulib_json_string(c->json_pointer));
    if (ok) { ok=shy_push(sink->items,row); row=NULL; }
    mulib_json_destroy(row); if (!ok) sink->failed=1; return ok;
}
static int emit_diagnostic(shy_sdk_sink *sink,const char *code,const char *message)
{
    if (!sink || sink->failed) return 0;
    int ok=text(code,64) && *code && text(message,1024) && mulib_json_size(sink->diagnostics)<64;
    mulib_json_t *row=ok ? mulib_json_new(MULIB_JSON_OBJECT) : NULL;
    if (ok) ok=shy_add(row,"code",mulib_json_string(code)) && shy_add(row,"message",mulib_json_string(message));
    if (ok) { ok=shy_push(sink->diagnostics,row); row=NULL; }
    mulib_json_destroy(row); if (!ok) sink->failed=1; return ok;
}
static const shy_sdk_host host={SHY_SDK_ABI,sizeof(shy_sdk_host),malloc,free,shy_digest,emit_candidate,emit_diagnostic};

int shy_plugin_extract(const shy_plugin *p,const shy_capture *capture,mulib_json_t *items)
{
    if (busy || !capture || !items) return 0;
    registration *r=NULL;
    for (size_t i=0;i<count;++i) if (registry[i].legacy==p) r=&registry[i];
    if (!r) return 0;
    busy=1;
    int ok;
    if (r->sdk) {
        shy_sdk_capture input={SHY_SDK_ABI,sizeof input,capture->html,capture->length,capture->url,
            capture->digest,capture->rules_source,capture->model_source};
        shy_sdk_sink sink={items,capture->diagnostics,capture->length,0};
        ok=r->sdk->extract(r->state,&input,&sink)==1 && !sink.failed;
    } else ok=p->extract(capture,items)==1;
    busy=0; return ok;
}
static int unavailable(const shy_capture *c,mulib_json_t *items) { (void)c; (void)items; return 0; }
static void rollback(size_t start)
{
    busy=1;
    while (count>start) {
        registration *r=&registry[--count];
        if (r->sdk) r->sdk->on_unload(r->state);
        if (r->library) dlclose(r->library);
        free(r->configuration);
        memset(r,0,sizeof *r);
    }
    busy=0;
}
int64_t shy_native_plugins_end(void)
{
    if (busy) return 2;
    /* Legacy registrations have no lifecycle and remain available. Dynamic
       registrations are appended as one batch and removed in reverse order. */
    size_t start=count;
    while (start && registry[start-1].sdk) --start;
    rollback(start); return 0;
}
int64_t shy_native_plugins_begin(const char *path)
{
    if (!path || !*path) return 0;
    if (strlen(path)>4095) return shy_error("native plugin configuration path too long");
    if (busy || !shy_plugins_init()) return shy_error("native plugin registry unavailable");
    for (size_t i=0;i<count;++i) if (registry[i].sdk) return shy_error("native plugin batch already active");
    size_t length,start=count;
    char *bytes=shy_read(path,65536,&length);
    mulib_json_t *config=bytes ? mulib_json_parse(bytes,length,NULL) : NULL;
    free(bytes);
    const mulib_json_t *plugins=mulib_json_get(config,"plugins");
    int ok=config && mulib_json_size(config)==2 && !strcmp(shy_text(config,"schema"),"shylock.native-plugins/v1") &&
        mulib_json_typeof(plugins)==MULIB_JSON_ARRAY && mulib_json_size(plugins)>0 && mulib_json_size(plugins)<=MAX_PLUGINS-count;
    for (size_t i=0;ok && i<mulib_json_size(plugins);++i) {
        const mulib_json_t *item=mulib_json_at(plugins,i), *options=mulib_json_get(item,"configuration");
        const char *file=shy_text(item,"path");
        char resolved[4096];
        const char *slash=strrchr(path,'/');
        int n=*file=='/' ? snprintf(resolved,sizeof resolved,"%s",file) :
            snprintf(resolved,sizeof resolved,"%.*s%s%s",slash ? (int)(slash-path) : 1,slash ? path : ".","/",file);
        if (mulib_json_size(item)!=2 || !*file || n<0 || (size_t)n>=sizeof resolved ||
            mulib_json_typeof(options)!=MULIB_JSON_OBJECT) { ok=0; break; }
        void *library=dlopen(resolved,RTLD_NOW|RTLD_LOCAL);
        void *symbol=library ? dlsym(library,SHY_SDK_ENTRY) : NULL;
        shy_sdk_query_fn query=NULL;
        /* POSIX dlsym conversion without a non-ISO C function-pointer cast. */
        _Static_assert(sizeof query==sizeof symbol,"POSIX function pointer representation");
        memcpy(&query,&symbol,sizeof query);
        busy=1;
        const shy_sdk_plugin *p=query ? query(SHY_SDK_ABI) : NULL;
        busy=0;
        ok=p && p->abi==SHY_SDK_ABI && p->size>=sizeof *p && !p->flags && name_valid(p->name) &&
            text(p->version,64) && *p->version && p->on_load && p->extract && p->on_unload && !shy_plugin_find(p->name);
        char *settings=ok ? mulib_json_stringify(options,0) : NULL;
        void *state=NULL;
        busy=1;
        int loaded=settings && p->on_load(&host,settings,&state)==1;
        busy=0;
        if (!loaded) { free(settings); if (library) dlclose(library); ok=0; break; }
        registration *r=&registry[count++];
        r->sdk=p; r->state=state; r->library=library;
        r->configuration=settings;
        r->view=(shy_plugin){SHY_EXTRACTOR_ABI,sizeof(shy_plugin),p->name,p->version,unavailable};
        r->legacy=&r->view;
    }
    mulib_json_destroy(config);
    if (!ok) { rollback(start); return shy_error("native plugin configuration, ABI or load failure (batch rolled back)"); }
    return 0;
}
