#include "collection.h"
#include "naive.h"
#include "workflow.h"
#include <stdlib.h>
#include <string.h>

static mulib_json_t *expected, *entries, *pending, *result;
static char *scratch;
static size_t bytes;
static int failed, checked, verified;
static int fail(void) { failed=1; return 0; }
static mulib_json_t *read_record(const char *path)
{
    size_t n; char *s=shy_read(path,SHY_DATA_LIMIT,&n);
    mulib_json_t *out=s ? mulib_json_parse(s,n,NULL) : NULL;
    free(s); return out;
}
int64_t shy_collection_close(void)
{
    mulib_json_destroy(expected); mulib_json_destroy(entries);
    mulib_json_destroy(pending); mulib_json_destroy(result); free(scratch);
    expected=NULL; entries=NULL; pending=NULL; result=NULL; scratch=NULL;
    bytes=0; failed=0; checked=0; verified=0;
    shy_dragnet_close(); shy_naive_close(); return 0;
}
int64_t shy_collection_begin(const char *path)
{
    shy_collection_close();
    if (*path && !(expected=read_record(path))) return fail();
    entries=mulib_json_new(MULIB_JSON_ARRAY);
    return entries ? 1 : fail();
}
int shy_collection_begin_record(const mulib_json_t *record)
{
    mulib_json_t *copy=shy_clone(record); shy_collection_close(); expected=copy;
    entries=mulib_json_new(MULIB_JSON_ARRAY);
    return expected && entries ? 1 : fail();
}
int64_t shy_collection_ready(void) { return verified && !failed; }
int64_t shy_collection_size(void)
{ return (int64_t)mulib_json_size(mulib_json_get(expected,"entries")); }
int64_t shy_collection_entry_hit(int64_t index)
{ return index<0 ? -1 : shy_integer(mulib_json_at(mulib_json_get(expected,"entries"),(size_t)index),"hit",63); }
const char *shy_collection_target(void) { return shy_text(expected,"target"); }
int64_t shy_collection_source(int64_t index,const char *discovery,const char *source,const char *rule)
{
    mulib_json_destroy(pending); pending=NULL; checked=0;
    shy_naive_close(); shy_dragnet_close();
    if (failed || index<0 || index>=8) return fail();
    const mulib_json_t *entry=mulib_json_at(mulib_json_get(expected,"entries"),(size_t)index);
    pending=expected ? shy_clone(mulib_json_get(entry,"discovery")) : read_record(discovery);
    if (!pending || shy_dragnet_replay_record(pending)!=0) return fail();
    int ok=expected ? shy_naive_begin_record(mulib_json_get(entry,"extraction")) : (int)shy_naive_begin(source,rule);
    return ok ? 1 : fail();
}
const char *shy_collection_discovery_field(const char *key) { return shy_text(pending,key); }
const char *shy_collection_hit(int64_t index)
{
    return index<0 ? "" : shy_text(mulib_json_at(mulib_json_get(pending,"hits"),(size_t)index),"url");
}
int64_t shy_collection_check(const char *status,const char *diagnostic)
{
    checked=!failed && shy_dragnet_check(status,diagnostic);
    return checked ? 1 : fail();
}
const char *shy_collection_view(const char *key)
{
    const mulib_json_t *record=shy_naive_result();
    record=mulib_json_get(record,key);
    free(scratch); scratch=record ? mulib_json_stringify(record,0) : NULL;
    if (!scratch || strlen(scratch)>SHY_PAGE_LIMIT) { free(scratch); scratch=NULL; fail(); }
    return scratch ? scratch : "";
}
int64_t shy_collection_append(const char *metadata)
{
    mulib_json_t *row=mulib_json_parse(metadata,strlen(metadata),NULL);
    if (failed || !checked || !entries || mulib_json_size(entries)>=8 || !shy_naive_result()) goto bad;
    if (!shy_add(row,"discovery",shy_clone(pending)) ||
        !shy_add(row,"extraction",shy_clone(shy_naive_result()))) goto bad;
    char *s=mulib_json_stringify(row,0);
    if (!s) goto bad;
    size_t n=strlen(s); free(s);
    if (n>SHY_DATA_LIMIT-bytes) goto bad;
    if (!shy_push(entries,row)) { row=NULL; goto bad; }
    bytes+=n; shy_naive_close(); shy_dragnet_close(); return 1;
bad:
    mulib_json_destroy(row); return fail();
}
int64_t shy_collection_finish(const char *metadata,int64_t emit)
{
    verified=0;
    if (failed || shy_nf.failed || !entries) return fail();
    mulib_json_destroy(result); result=mulib_json_parse(metadata,strlen(metadata),NULL);
    char digest[65];
    if (!shy_add(result,"entries",shy_clone(entries)) ||
        !shy_json_digest(result,"collection_id",digest) ||
        !shy_add(result,"collection_id",mulib_json_string(digest))) return fail();
    char *s=mulib_json_stringify(result,0);
    int bounded=s && strlen(s)<=SHY_DATA_LIMIT; free(s);
    if (!bounded || (expected && !shy_json_equal(expected,result))) return fail();
    verified=expected!=NULL;
    return emit ? (shy_emit(shy_clone(result))==0 ? 1 : fail()) : 1;
}
int64_t shy_collection_review_begin(int64_t index)
{
    if (!verified || failed || index<0) return 0;
    const mulib_json_t *row=mulib_json_at(mulib_json_get(result,"entries"),(size_t)index);
    return row && shy_naive_begin_record(mulib_json_get(row,"extraction"));
}
const mulib_json_t *shy_collection_verified(void) { return verified && !failed ? result : NULL; }
