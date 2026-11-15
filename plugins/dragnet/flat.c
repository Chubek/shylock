#include "intelligence.h"
#include "workflow.h"
#include "mulib-html-treebuilder.h"
#include "mulib-readability.h"
#include "mulib-money.h"
#include <ctype.h>
#include <stdlib.h>
#include <string.h>

enum { FLAT_TEXT_LIMIT=262144, FLAT_BLOCK_LIMIT=2048 };
typedef struct {
    const char *html;
    const mulib_html_tree_node_t *group;
    mulib_shylock_buffer_t text;
    mulib_json_t *blocks;
    size_t begin,end,total;
} flatten_state;
int shy_intelligence_seal(mulib_json_t *out,const char *key)
{
    char digest[65]; return shy_json_digest(out,key,digest) && shy_add(out,key,mulib_json_string(digest));
}
static int hidden(const mulib_html_tree_node_t *n)
{
    static const char *const names[]={"script","style","noscript","template","nav","header","footer","aside","svg","head"};
    for (size_t i=0;i<sizeof names/sizeof *names;++i) if (!strcmp(n->tag,names[i])) return 1;
    const char *value; size_t length;
    return mulib_html_attribute(n->attributes.data,n->attributes.len,"hidden",&value,&length) ||
        (mulib_html_attribute(n->attributes.data,n->attributes.len,"aria-hidden",&value,&length) && length==4 && !memcmp(value,"true",4));
}
static int block_tag(const char *name)
{
    static const char *const names[]={"p","div","li","td","th","tr","h1","h2","h3","h4","article","section","main","body"};
    for (size_t i=0;i<sizeof names/sizeof *names;++i) if (!strcmp(name,names[i])) return 1;
    return 0;
}
static int flush(flatten_state *s)
{
    if (!s->text.len) { s->group=NULL; return 1; }
    if (s->total>=FLAT_TEXT_LIMIT) return 0;
    mulib_shylock_buffer_t readable={0}; readable.limit=FLAT_TEXT_LIMIT-s->total;
    int ok=mulib_readability_extract(s->text.data,s->text.len,&readable);
    if (ok && readable.len) {
        char digest[65]; shy_digest(s->html+s->begin,s->end-s->begin,digest);
        mulib_json_t *row=mulib_json_new(MULIB_JSON_OBJECT);
        ok=mulib_json_size(s->blocks)<FLAT_BLOCK_LIMIT &&
            shy_add(row,"index",mulib_json_number((double)mulib_json_size(s->blocks))) &&
            shy_add(row,"text",mulib_json_string_n(readable.data,readable.len)) &&
            shy_add(row,"byte_offset",mulib_json_number((double)s->begin)) &&
            shy_add(row,"byte_length",mulib_json_number((double)(s->end-s->begin))) &&
            shy_add(row,"span_blake3",mulib_json_string(digest));
        if (ok) ok=shy_push(s->blocks,row); else mulib_json_destroy(row);
        s->total+=readable.len;
    }
    mulib_shylock_buffer_free(&readable); mulib_shylock_buffer_free(&s->text);
    s->text.limit=FLAT_TEXT_LIMIT; s->group=NULL; return ok;
}
static int walk(flatten_state *s,const mulib_html_tree_node_t *node,const mulib_html_tree_node_t *group)
{
    for (;node;node=node->next_sibling) {
        if (!node->is_text && hidden(node)) { if (!flush(s)) return 0; continue; }
        if (node->is_text) {
            if (group!=s->group && !flush(s)) return 0;
            if (!s->text.len) { s->begin=node->offset; s->group=group; }
            s->end=node->offset+node->text.len;
            if (!mulib_shylock_append(&s->text,node->text.data,node->text.len)) return 0;
        } else {
            const mulib_html_tree_node_t *next=block_tag(node->tag) ? node : group;
            if (!walk(s,node->first_child,next)) return 0;
        }
    }
    return 1;
}
mulib_json_t *shy_flatten(const mulib_json_t *snapshot)
{
    if (!shy_snapshot_valid(snapshot)) return NULL;
    const char *html=shy_text(snapshot,"html");
    mulib_html_tree_node_t *tree=NULL;
    if (!mulib_html_tree_parse(html,strlen(html),50000,128,&tree)) return NULL;
    flatten_state s={0}; s.html=html; s.blocks=mulib_json_new(MULIB_JSON_ARRAY); s.text.limit=FLAT_TEXT_LIMIT;
    mulib_json_t *out=mulib_json_new(MULIB_JSON_OBJECT);
    int ok=s.blocks && walk(&s,tree,tree) && flush(&s) &&
        shy_add(out,"schema",mulib_json_string("shylock.page-ir/v1")) &&
        shy_add(out,"profile",mulib_json_string("static-semantic-blocks/v1")) &&
        shy_add(out,"mulib_revision",mulib_json_string(SHY_MULIB_REVISION)) &&
        shy_add(out,"snapshot",shy_clone(snapshot));
    if (ok) { ok=shy_add(out,"blocks",s.blocks); s.blocks=NULL; }
    if (ok) ok=shy_intelligence_seal(out,"ir_id");
    mulib_html_tree_free(tree); mulib_shylock_buffer_free(&s.text); mulib_json_destroy(s.blocks);
    if (!ok) { mulib_json_destroy(out); return NULL; }
    return out;
}
mulib_json_t *shy_flat_source(const char *path)
{
    size_t length; char *bytes=shy_read(path,SHY_DATA_LIMIT,&length);
    mulib_json_t *source=bytes ? mulib_json_parse(bytes,length,NULL) : NULL; free(bytes);
    int is_ir=!strcmp(shy_text(source,"schema"),"shylock.page-ir/v1");
    int is_browser=!strcmp(shy_text(source,"schema"),"shylock.browser-run/v1");
    mulib_json_t *ir=shy_flatten(is_ir || is_browser ? mulib_json_get(source,"snapshot") : source);
    if (is_ir && ir && !shy_json_equal(ir,source)) { mulib_json_destroy(ir); ir=NULL; }
    mulib_json_destroy(source); return ir;
}
static int contains(const char *text,const char *needle)
{
    size_t n=strlen(needle);
    for (size_t i=0;text[i];++i) {
        size_t j=0;
        while (j<n && text[i+j] && tolower((unsigned char)text[i+j])==tolower((unsigned char)needle[j])) ++j;
        if (j==n) return 1;
    }
    return 0;
}
static int match(mulib_json_t *matches,const mulib_json_t *block,const char *kind,
    const char *amount,const char *currency,const char *basis,const char *qualifier)
{
    if (mulib_json_size(matches)>=256) return 0;
    int scale=shy_currency_scale(currency); mulib_money_t money;
    int valid=*amount && scale>=0 && mulib_money_parse(amount,currency,(uint8_t)scale,&money) && money.minor>=0 && money.minor<=INT64_C(1000000000000);
    mulib_json_t *row=mulib_json_new(MULIB_JSON_OBJECT);
    int ok=shy_add(row,"kind",mulib_json_string(kind)) && shy_add(row,"status",mulib_json_string("needs_review")) &&
        shy_add(row,"block",shy_clone(block)) && shy_add(row,"amount_text",mulib_json_string(amount)) &&
        shy_add(row,"currency",mulib_json_string(currency)) && shy_add(row,"basis",mulib_json_string(basis)) &&
        shy_add(row,"qualifier",mulib_json_string(qualifier)) &&
        shy_add(row,"scale",scale>=0 ? mulib_json_number(scale) : mulib_json_new(MULIB_JSON_NULL)) &&
        shy_add(row,"minor",valid ? mulib_json_number((double)money.minor) : mulib_json_new(MULIB_JSON_NULL));
    if (ok) ok=shy_intelligence_seal(row,"match_id");
    if (ok) return shy_push(matches,row);
    mulib_json_destroy(row); return 0;
}
mulib_json_t *shy_find_information(const mulib_json_t *ir,const char *target)
{
    if (!*target || strlen(target)>256) return NULL;
    const mulib_json_t *blocks=mulib_json_get(ir,"blocks"),*snapshot=mulib_json_get(ir,"snapshot");
    mulib_json_t *out=mulib_json_new(MULIB_JSON_OBJECT),*matches=mulib_json_new(MULIB_JSON_ARRAY);
    int prices=!strcmp(target,"price") || !strcmp(target,"prices"),ok=out && matches;
    for (size_t i=0;ok && i<mulib_json_size(blocks);++i) {
        const mulib_json_t *block=mulib_json_at(blocks,i); const char *text=shy_text(block,"text"); size_t n=strlen(text);
        if (!prices) { if (contains(text,target)) ok=match(matches,block,"literal-text","","","unknown","unspecified"); continue; }
        for (size_t j=0;ok && j<n;++j) {
            if (j && (unsigned char)text[j-1]>32) continue;
            char currency[4]=""; size_t start=j;
            if (j+4<n && text[j]>='A' && text[j]<='Z' && text[j+1]>='A' && text[j+1]<='Z' && text[j+2]>='A' && text[j+2]<='Z' && text[j+3]==' ') {
                memcpy(currency,text+j,3); start=j+4;
            } else if (n-j>=3 && !memcmp(text+j,"\xe2\x82\xac",3)) { strcpy(currency,"EUR"); start=j+3; }
            else if (text[j]=='$') start=j+1;
            else continue;
            while (start<n && text[start]==' ') ++start;
            size_t end=start;
            while (end<n && ((text[end]>='0' && text[end]<='9') || text[end]=='.' || text[end]==',')) ++end;
            if (end==start || end-start>63) continue;
            char amount[64]; memcpy(amount,text+start,end-start); amount[end-start]=0;
            const char *basis="unknown";
            if (!strncmp(text+end," per night",10) && (!text[end+10] || !isalpha((unsigned char)text[end+10]))) basis="nightly";
            else if (!strncmp(text+end," total",6) && (!text[end+6] || !isalpha((unsigned char)text[end+6]))) basis="stay_total";
            else if (!strncmp(text+end," cleaning fee",13) && (!text[end+13] || !isalpha((unsigned char)text[end+13]))) basis="cleaning_fee";
            const char *qualifier=j>=5 && !memcmp(text+j-5,"from ",5) ? "from" : "unspecified";
            ok=match(matches,block,"currency-token",amount,currency,basis,qualifier); j=end-1;
        }
    }
    const char *html=shy_text(snapshot,"html");
    mulib_json_t *extraction=ok ? shy_capture_record(html,strlen(html),shy_text(snapshot,"source_url"),
        shy_integer(snapshot,"captured_at",INT64_C(253402300799)),"","","dragnet") : NULL;
    if (ok) ok=shy_add(out,"schema",mulib_json_string("shylock.page-find/v1")) &&
        shy_add(out,"target",mulib_json_string(target)) && shy_add(out,"method",mulib_json_string(prices ? "currency-token-scan/v1" : "case-insensitive-literal/v1")) &&
        shy_add(out,"ir",shy_clone(ir)) &&
        shy_add(out,"deterministic_status",mulib_json_string(extraction ? "extracted" : "extractor_unavailable_or_limit"));
    if (ok) { ok=shy_add(out,"extraction",extraction ? extraction : mulib_json_new(MULIB_JSON_NULL)); extraction=NULL; }
    if (ok) { ok=shy_add(out,"matches",matches); matches=NULL; }
    if (ok) ok=shy_intelligence_seal(out,"find_id");
    mulib_json_destroy(matches); mulib_json_destroy(extraction);
    if (!ok) { mulib_json_destroy(out); return NULL; }
    return out;
}
int64_t shy_flatten_command(const char *path)
{
    mulib_json_t *ir=shy_flat_source(path);
    return ir ? shy_emit(ir) : shy_error("invalid snapshot/IR or flattening limit");
}
int64_t shy_find_command(const char *path,const char *target)
{
    mulib_json_t *ir=shy_flat_source(path),*out=ir ? shy_find_information(ir,target) : NULL;
    mulib_json_destroy(ir);
    return out ? shy_emit(out) : shy_error("invalid source/target or deterministic matching limit");
}
