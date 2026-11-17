#include "naive.h"
#include "naive-rules.h"
#include "intelligence.h"
#include "workflow.h"
#include "mulib-uri.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

nf_session shy_nf;
static uint64_t generation;
int nf_fail(void) { shy_nf.failed=1; return 0; }
int64_t shy_naive_close(void)
{
    mulib_json_destroy(shy_nf.snapshot); mulib_json_destroy(shy_nf.legacy);
    mulib_json_destroy(shy_nf.expected); mulib_json_destroy(shy_nf.result);
    mulib_html_tree_free(shy_nf.tree);
    free(shy_nf.nodes); free(shy_nf.rule); free(shy_nf.starts); free(shy_nf.ends);
    free(shy_nf.exact); free(shy_nf.scratch); mulib_shylock_buffer_free(&shy_nf.text);
    memset(&shy_nf,0,sizeof shy_nf); return 0;
}
static int begin(const mulib_json_t *snapshot, const char *rule)
{
    mulib_uri_t uri;
    if (!snapshot || !shy_snapshot_valid(snapshot) || strlen(rule)>NF_RULE ||
        !mulib_uri_parse(shy_text(snapshot,"source_url"),strlen(shy_text(snapshot,"source_url")),&uri) ||
        !uri.host.length || uri.host.length>253) return nf_fail();
    shy_nf.snapshot=shy_clone(snapshot);
    shy_nf.rule=malloc(strlen(rule)+1);
    if (!shy_nf.snapshot || !shy_nf.rule) return nf_fail();
    strcpy(shy_nf.rule,rule); shy_digest(rule,strlen(rule),shy_nf.rule_hash);
    memcpy(shy_nf.host,uri.host.data,uri.host.length);
    for (size_t i=0;i<uri.host.length;++i)
        if (shy_nf.host[i]>='A' && shy_nf.host[i]<='Z') shy_nf.host[i]+='a'-'A';
    /* Old extraction is retained verbatim. On1x decides which representations
       to aggregate, whether they are scoped, and what agreement means. */
    const char *html=shy_text(snapshot,"html");
    shy_nf.legacy=shy_capture_record(html,strlen(html),shy_text(snapshot,"source_url"),
        shy_integer(snapshot,"captured_at",INT64_C(253402300799)),"","","dragnet");
    if (!nf_dom_open()) return nf_fail();
    if (generation==INT64_MAX/65536) return nf_fail();
    ++generation;
    shy_nf.generation=generation; return 1;
}
int64_t shy_naive_begin(const char *source, const char *rule)
{
    shy_naive_close();
    size_t length; char *text=shy_read(rule,NF_RULE,&length);
    mulib_json_t *ir=text ? shy_flat_source(source) : NULL;
    int ok=ir && begin(mulib_json_get(ir,"snapshot"),text);
    free(text); mulib_json_destroy(ir);
    if (!ok) { shy_naive_close(); return 0; } return 1;
}
int shy_naive_begin_record(const mulib_json_t *record)
{
    mulib_json_t *copy=shy_clone(record);
    shy_naive_close();
    shy_nf.expected=copy;
    mulib_json_t *checked=copy ? shy_flatten(mulib_json_get(copy,"snapshot")) : NULL;
    int source_ok=checked!=NULL;
    mulib_json_destroy(checked);
    if (!copy || strcmp(shy_text(copy,"schema"),"shylock.naive-find/v1") ||
        !source_ok || !begin(mulib_json_get(copy,"snapshot"),shy_text(copy,"rule_source"))) {
        shy_naive_close(); return 0;
    }
    return 1;
}
int64_t shy_naive_replay_begin(const char *path)
{
    shy_naive_close();
    size_t n; char *s=shy_read(path,SHY_DATA_LIMIT,&n);
    mulib_json_t *record=s ? mulib_json_parse(s,n,NULL) : NULL;
    int ok=record && shy_naive_begin_record(record);
    mulib_json_destroy(record); free(s); return ok;
}
const char *shy_naive_rule(void) { return shy_nf.rule ? shy_nf.rule : ""; }
const char *shy_naive_meta(const char *key)
{
    if (!strcmp(key,"host")) return shy_nf.host;
    if (!strcmp(key,"rule_hash")) return shy_nf.rule_hash;
    if (!strcmp(key,"revision")) return SHY_MULIB_REVISION;
    if (!strcmp(key,"legacy_status")) return shy_nf.legacy ? "available" : "legacy_extraction_unavailable";
    if (!strcmp(key,"snapshot") || !strcmp(key,"legacy")) {
        if (!strcmp(key,"legacy") && !shy_nf.legacy) return "null";
        free(shy_nf.scratch);
        shy_nf.scratch=mulib_json_stringify(!strcmp(key,"snapshot") ? shy_nf.snapshot : shy_nf.legacy,0);
        if (!shy_nf.scratch) nf_fail();
        return shy_nf.scratch ? shy_nf.scratch : "null";
    }
    return shy_text(shy_nf.snapshot,key);
}
const char *shy_naive_identify(const char *text, const char *key)
{
    mulib_json_t *object=mulib_json_parse(text,strlen(text),NULL);
    char digest[65];
    int ok=object && shy_json_digest(object,key,digest) && shy_add(object,key,mulib_json_string(digest));
    free(shy_nf.scratch); shy_nf.scratch=ok ? mulib_json_stringify(object,0) : NULL;
    mulib_json_destroy(object);
    if (!shy_nf.scratch) nf_fail();
    return shy_nf.scratch ? shy_nf.scratch : "null";
}
const char *shy_naive_quote(const char *text)
{
    mulib_json_t *value=mulib_json_string(text);
    free(shy_nf.scratch); shy_nf.scratch=value ? mulib_json_stringify(value,0) : NULL;
    mulib_json_destroy(value);
    if (!shy_nf.scratch) nf_fail();
    return shy_nf.scratch ? shy_nf.scratch : "null";
}
const char *shy_naive_decode(const char *text)
{
    mulib_json_t *value=mulib_json_parse(text,strlen(text),NULL);
    size_t n=0; const char *decoded=mulib_json_text(value,&n);
    free(shy_nf.scratch); shy_nf.scratch=decoded && strlen(decoded)==n ? malloc(n+1) : NULL;
    if (shy_nf.scratch) memcpy(shy_nf.scratch,decoded,n+1); else nf_fail();
    mulib_json_destroy(value); return shy_nf.scratch ? shy_nf.scratch : "";
}
const char *shy_naive_claim_field(int64_t index,const char *key)
{
    const mulib_json_t *claim=index<0 ? NULL : mulib_json_at(mulib_json_get(shy_nf.legacy,"candidates"),(size_t)index);
    int64_t offset=shy_integer(claim,"byte_offset",SHY_PAGE_LIMIT),length=shy_integer(claim,"byte_length",SHY_PAGE_LIMIT);
    const char *html=shy_text(shy_nf.snapshot,"html"),*pointer=shy_text(claim,"json_pointer");
    size_t n=strlen(html);
    if (offset<0 || length<0 || (uint64_t)offset>n || (uint64_t)length>n-(size_t)offset) { nf_fail(); return ""; }
    mulib_json_t *root=mulib_json_parse(html+offset,(size_t)length,NULL);
    if (!root) { nf_fail(); return ""; }
    const mulib_json_t *node=root;
    /* Existing pointers are generated from a fixed set of Schema.org keys and
       array indexes; stop at the price property and expose its parent facts. */
    while (*pointer=='/' && node) {
        const char *end=strchr(pointer+1,'/');
        if (!end) break;
        char part[128]; size_t size=(size_t)(end-pointer-1);
        if (!size || size>=sizeof part) { node=NULL; break; }
        memcpy(part,pointer+1,size); part[size]=0;
        if (mulib_json_typeof(node)==MULIB_JSON_ARRAY) {
            char *tail; unsigned long at=strtoul(part,&tail,10);
            node=*tail ? NULL : mulib_json_at(node,(size_t)at);
        } else node=mulib_json_get(node,part);
        pointer=end;
    }
    if (!node) nf_fail();
    const char *value=shy_text(node,key);
    free(shy_nf.scratch); shy_nf.scratch=malloc(strlen(value)+1);
    if (shy_nf.scratch) strcpy(shy_nf.scratch,value); else nf_fail();
    mulib_json_destroy(root); return shy_nf.scratch ? shy_nf.scratch : "";
}
int64_t shy_naive_finish(const char *text)
{
    shy_nf.replayed=0;
    if (shy_nf.failed || strlen(text)>SHY_DATA_LIMIT) return nf_fail();
    const char *sealed=shy_naive_identify(text,"find_id");
    mulib_json_destroy(shy_nf.result); shy_nf.result=NULL;
    shy_nf.result=mulib_json_parse(sealed,strlen(sealed),NULL);
    if (mulib_json_typeof(shy_nf.result)!=MULIB_JSON_OBJECT || shy_nf.failed) return nf_fail();
    if (shy_nf.expected) {
        if (!shy_json_equal(shy_nf.expected,shy_nf.result)) return nf_fail();
        shy_nf.replayed=1;
    }
    return 1;
}
const char *shy_naive_span_digest(int64_t offset,int64_t length)
{
    const char *html=shy_text(shy_nf.snapshot,"html"); size_t n=strlen(html);
    static char digest[65];
    if (offset<0 || length<=0 || (uint64_t)offset>n || (uint64_t)length>n-(size_t)offset) { nf_fail(); return ""; }
    shy_digest(html+offset,(size_t)length,digest); return digest;
}
const mulib_json_t *shy_naive_verified(void)
{ return !shy_nf.failed && shy_nf.replayed ? shy_nf.result : NULL; }
int64_t shy_naive_emit(const char *mode,const char *profile,const char *target,int64_t call)
{
    mulib_json_t *out=shy_nf.failed ? NULL : shy_clone(shy_nf.result);
    if (!out) return 2;
    if (!strcmp(mode,"deterministic")) return shy_emit(out);
    /* The On1x caller alone chooses call/skip. This adapter only owns protocol
       resources. The complete deterministic artifact remains separate. */
    mulib_json_t *envelope=mulib_json_new(MULIB_JSON_OBJECT),*assisted=NULL;
    int success=0;
    if (call) {
        mulib_json_t *ir=shy_flatten(shy_nf.snapshot),*settings=mulib_json_parse(profile,strlen(profile),NULL);
        assisted=ir && settings ? shy_model_information(ir,settings,target,NULL,NULL,&success) : NULL;
        mulib_json_destroy(ir); mulib_json_destroy(settings);
    }
    int ok=shy_add(envelope,"schema",mulib_json_string("shylock.naive-assisted/v1")) &&
        shy_add(envelope,"mode",mulib_json_string(mode)) &&
        shy_add(envelope,"status",mulib_json_string(!call ? "model_not_needed" : success ? "model_assisted" : "deterministic_fallback"));
    if (ok) { ok=shy_add(envelope,"deterministic",out); out=NULL; }
    if (ok) { ok=shy_add(envelope,"model_result",assisted ? assisted : mulib_json_new(MULIB_JSON_NULL)); assisted=NULL; }
    mulib_json_destroy(out); mulib_json_destroy(assisted);
    if (!ok) { mulib_json_destroy(envelope); return 2; }
    int64_t emitted=shy_emit(envelope); return emitted ? emitted : call && !success ? 2 : 0;
}
