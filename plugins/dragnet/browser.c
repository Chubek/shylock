#include "intelligence.h"
#include "workflow.h"
#include "dragnet.h"
#include "mulib-browser-engine.h"
#include "mulib-browserlocator.h"
#include "mulib-dom-query.h"
#include "mulib-readability.h"
#include "mulib-uri.h"
#include "mulib-utf8.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static struct {
    mulib_browser_engine_t engine;
    mulib_json_t *job,*trace;
    char *directory,*html,*url;
    size_t length;
    unsigned status;
    int initialized,network,saved;
    int64_t timestamp;
} browser;
int64_t shy_browser_close(void)
{
    if (browser.initialized) mulib_browser_engine_destroy(&browser.engine);
    mulib_json_destroy(browser.job); mulib_json_destroy(browser.trace);
    free(browser.directory); free(browser.html); free(browser.url);
    memset(&browser,0,sizeof browser); return 0;
}
static char *copy(const char *s)
{ size_t n=strlen(s)+1; char *p=malloc(n); if (p) memcpy(p,s,n); return p; }
static int navigate(const char *url)
{
    char *prior_html=browser.html,*prior_url=browser.url;
    size_t prior_length=browser.length; int prior_saved=browser.saved; unsigned prior_status=browser.status;
    browser.html=NULL; browser.url=NULL;
    unsigned status=0;
    int ok=mulib_browser_engine_navigate(&browser.engine,url,&status);
    if (ok) { free(prior_html); free(prior_url); }
    else {
        free(browser.html); free(browser.url); browser.html=prior_html; browser.url=prior_url;
        browser.length=prior_length; browser.saved=prior_saved; browser.status=prior_status;
    }
    return ok;
}
static int acquire(void *context,const char *url,mulib_shylock_buffer_t *body,unsigned *status,char *type,size_t cap)
{
    (void)context; char host[256];
    if (!shy_dragnet_url_host(url,host)) return 0;
    const mulib_json_t *pages=mulib_json_get(browser.job,"pages");
    char *bytes=NULL; size_t length=0; int saved=0,ok=0;
    for (size_t i=0;i<mulib_json_size(pages);++i) {
        const mulib_json_t *page=mulib_json_at(pages,i);
        if (strcmp(shy_text(page,"url"),url)) continue;
        const char *file=shy_text(page,"path"); char path[4096];
        int n=*file=='/' ? snprintf(path,sizeof path,"%s",file) : snprintf(path,sizeof path,"%s/%s",browser.directory,file);
        if (n<0 || (size_t)n>=sizeof path) return 0;
        bytes=shy_read(path,SHY_PAGE_LIMIT,&length); saved=1; break;
    }
    mulib_shylock_buffer_t live={0}; unsigned code=200; char media[128]="text/html";
    if (!saved && browser.network && shy_dragnet_get(url,shy_dragnet_https,NULL,&live,&code,media,sizeof media)) {
        bytes=copy(live.data ? live.data : ""); length=live.len;
    }
    if (bytes && length && code==200 && shy_dragnet_html_type(media) &&
        mulib_utf8_validate((const unsigned char *)bytes,length) && strlen(media)<cap) {
        char *owned_url=copy(url);
        ok=owned_url && mulib_shylock_append(body,bytes,length);
        if (ok) {
            free(browser.html); free(browser.url); browser.html=bytes; bytes=NULL; browser.url=owned_url;
            browser.length=length; browser.saved=saved; browser.status=code;
            *status=code; strcpy(type,"text/html");
        } else free(owned_url);
    }
    free(bytes); mulib_shylock_buffer_free(&live); return ok;
}
int64_t shy_browser_begin(const char *job,const char *directory)
{
    shy_browser_close(); browser.job=mulib_json_parse(job,strlen(job),NULL);
    browser.directory=copy(directory); browser.trace=mulib_json_new(MULIB_JSON_ARRAY);
    const char *url=shy_text(browser.job,"start_url");
    browser.network=mulib_json_boolean(mulib_json_get(browser.job,"allow_network"));
    browser.timestamp=shy_integer(browser.job,"captured_at",INT64_C(253402300799));
    mulib_browser_fetch_t fetch={0};
    fetch.client=(mulib_fetch_client_t){{4096,SHY_PAGE_LIMIT,0,1,0},acquire,NULL}; fetch.enforce_same_origin=1;
    mulib_browser_limits_t limits={0};
    limits.max_response_bytes=SHY_PAGE_LIMIT; limits.max_nodes=50000; limits.max_depth=128;
    limits.max_urls=32;
    limits.max_tasks=32; limits.max_script_steps=1;
    if (!browser.job || !browser.directory || !browser.trace || browser.timestamp<0 ||
        !mulib_browser_origin_parse(url,&fetch.initiator) || !mulib_browser_engine_init(&browser.engine,&fetch,&limits)) return 2;
    browser.initialized=1;
    return navigate(url) ? 0 : 2;
}
static int node_text(const mulib_html_tree_node_t *node,mulib_shylock_buffer_t *raw,int siblings)
{
    for (;node;node=siblings ? node->next_sibling : NULL) {
        if (!strcmp(node->tag,"script") || !strcmp(node->tag,"style") || !strcmp(node->tag,"template")) continue;
        if (node->is_text) { if (!mulib_shylock_append(raw,node->text.data,node->text.len)) return 0; }
        else if (!node_text(node->first_child,raw,1)) return 0;
    }
    return 1;
}
int64_t shy_browser_step(const char *action,const char *selector,const char *value)
{
    if (!browser.initialized) return 2;
    mulib_json_t *entry=mulib_json_new(MULIB_JSON_OBJECT);
    int ok=entry!=NULL;
    const mulib_html_tree_node_t *node=NULL;
    mulib_browserlocator_t locator={0};
    if (*selector) {
        ok=ok && mulib_browserlocator_init(&locator,MULIB_BROWSERLOCATOR_CSS,selector);
        if (ok) node=mulib_dom_query_first(browser.engine.document,locator.selector,50001);
        ok=ok && node;
    }
    if (ok && !strcmp(action,"navigate")) {
        ok=navigate(value);
    } else if (ok && !strcmp(action,"follow")) {
        const char *href; size_t length;
        ok=node && !strcmp(node->tag,"a") && mulib_html_attribute(node->attributes.data,node->attributes.len,"href",&href,&length) && length>0 && length<4096;
        char relative[4096]; char *resolved=NULL;
        if (ok) {
            memcpy(relative,href,length); relative[length]=0;
            mulib_uri_t base,ref;
            ok=mulib_uri_parse(browser.url,strlen(browser.url),&base) && mulib_uri_parse(relative,length,&ref) && (resolved=mulib_uri_resolve(&base,&ref))!=NULL;
        }
        if (ok) ok=navigate(resolved);
        free(resolved);
    } else if (ok && (!strcmp(action,"read") || !strcmp(action,"assert"))) {
        mulib_shylock_buffer_t raw={0},readable={0}; raw.limit=16384; readable.limit=16384;
        ok=node && node_text(node,&raw,0) && mulib_readability_extract(raw.data,raw.len,&readable);
        if (ok && !strcmp(action,"assert")) ok=readable.data && strstr(readable.data,value)!=NULL;
        if (ok) ok=shy_add(entry,"text",mulib_json_string_n(readable.data ? readable.data : "",readable.len));
        mulib_shylock_buffer_free(&raw); mulib_shylock_buffer_free(&readable);
    } else if (ok) ok=0;
    mulib_browserlocator_free(&locator);
    int built=shy_add(entry,"action",mulib_json_string(action)) && shy_add(entry,"selector",mulib_json_string(selector)) &&
        shy_add(entry,"value",mulib_json_string(value)) && shy_add(entry,"status",mulib_json_string(ok ? "completed" : "failed")) &&
        shy_add(entry,"url",mulib_json_string(browser.url ? browser.url : ""));
    if (built) built=shy_push(browser.trace,entry); else mulib_json_destroy(entry);
    return ok && built ? 0 : 2;
}
int64_t shy_browser_finish(int64_t success)
{
    mulib_json_t *out=mulib_json_new(MULIB_JSON_OBJECT);
    mulib_json_t *snapshot=browser.html ? shy_snapshot_record(browser.html,browser.length,browser.url,browser.timestamp,
        browser.saved ? "saved" : "https",browser.saved ? 0 : browser.status,"text/html") : mulib_json_new(MULIB_JSON_NULL);
    int ok=shy_add(out,"schema",mulib_json_string("shylock.browser-run/v1")) &&
        shy_add(out,"profile",mulib_json_string("mulib-static-html/v1")) &&
        shy_add(out,"status",mulib_json_string(success ? "completed" : "failed")) &&
        shy_add(out,"job",shy_clone(browser.job)) && shy_add(out,"trace",shy_clone(browser.trace));
    if (ok) { ok=shy_add(out,"snapshot",snapshot); snapshot=NULL; }
    if (ok) ok=shy_intelligence_seal(out,"run_id");
    mulib_json_destroy(snapshot);
    if (!ok) { mulib_json_destroy(out); return shy_error("cannot allocate browser audit"); }
    int64_t emitted=shy_emit(out); return emitted ? emitted : success ? 0 : 2;
}
