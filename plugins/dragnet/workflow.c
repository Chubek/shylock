#include "../../src/shylock.h"
#include "dragnet.h"
#include "workflow.h"
#include "mulib-elvi-scrape.h"
#include "mulib-utf8.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

/* One lexical session, no borrowed source survives close. On1x chooses outcome
   policy. This adapter loads programs, owns responses, invokes MuLIB and seals. */
static mulib_json_t *discovery;
static mulib_json_t *expected;
static mulib_json_t *batch;
static mulib_json_t *batch_results;
static size_t batch_bytes;
static int batch_failed;
static int64_t hit_count;
int64_t shy_dragnet_close(void) { mulib_json_destroy(discovery); mulib_json_destroy(expected); discovery=NULL; expected=NULL; hit_count=0; return 0; }
int shy_dragnet_html_type(const char *s)
{
    size_t n=strcspn(s,"; \t");
    return (n==9 && !strncasecmp(s,"text/html",n)) || (n==21 && !strncasecmp(s,"application/xhtml+xml",n));
}
static int seal(mulib_json_t *out,const char *key)
{
    char digest[65]; return shy_json_digest(out,key,digest) && shy_add(out,key,mulib_json_string(digest));
}
mulib_json_t *shy_snapshot_record(const char *bytes,size_t length,const char *url,int64_t at,
    const char *mode,unsigned status,const char *type)
{
    char host[256],digest[65];
    if (!length || length>SHY_PAGE_LIMIT || memchr(bytes,0,length) ||
        !mulib_utf8_validate((const unsigned char *)bytes,length) || !shy_dragnet_url_host(url,host) ||
        at< -1 || at>INT64_C(253402300799)) return NULL;
    shy_digest(bytes,length,digest);
    mulib_json_t *out=mulib_json_new(MULIB_JSON_OBJECT);
    if (!shy_add(out,"schema",mulib_json_string("shylock.snapshot/v1")) ||
        !shy_add(out,"source_url",mulib_json_string(url)) ||
        !shy_add(out,"captured_at",at<0 ? mulib_json_new(MULIB_JSON_NULL) : mulib_json_number((double)at)) ||
        !shy_add(out,"acquisition",mulib_json_string(mode)) ||
        !shy_add(out,"http_status",status ? mulib_json_number(status) : mulib_json_new(MULIB_JSON_NULL)) ||
        !shy_add(out,"content_type",mulib_json_string(type)) ||
        !shy_add(out,"blake3",mulib_json_string(digest)) ||
        !shy_add(out,"byte_length",mulib_json_number((double)length)) ||
        !shy_add(out,"html",mulib_json_string_n(bytes,length)) || !seal(out,"snapshot_id")) {
        mulib_json_destroy(out); return NULL;
    }
    return out;
}
/* Returns 0 parsed, 1 provider error, 2 transport/read failure, 3 HTTP error,
   4 media/encoding failure, 5 parser failure. Allocation failure is -1. */
static int prepare(const char *provider,const char *query,const char *program_source,
    const char *bytes,size_t length,const char *mode,unsigned status,const char *type,int64_t limit)
{
    mulib_elvi_scrape_program_t program;
    mulib_elvi_scrape_error_t error={0};
    char url[4096],digest[65];
    if (!mulib_elvi_scrape_compile(program_source,strlen(program_source),&program,&error) ||
        strcmp(program.name,provider) || !mulib_elvi_scrape_url(&program,query,(unsigned)limit,url,sizeof url)) return 1;
    shy_digest(program_source,strlen(program_source),digest);
    mulib_json_t *out=mulib_json_new(MULIB_JSON_OBJECT),*hits=mulib_json_new(MULIB_JSON_ARRAY);
    int code=0;
    if (!bytes) code=2;
    else if (status && status!=200) code=3;
    else if (!shy_dragnet_html_type(type) || !length || memchr(bytes,0,length) ||
        !mulib_utf8_validate((const unsigned char *)bytes,length)) code=4;
    mulib_elvi_scrape_result_t results[64]; size_t count=0;
    if (!code && !mulib_elvi_scrape_parse(&program,bytes,length,results,(size_t)limit,&count)) code=5;
    int ok=out && hits && shy_add(out,"schema",mulib_json_string("shylock.discovery/v1")) &&
        shy_add(out,"provider",mulib_json_string(provider)) && shy_add(out,"query",mulib_json_string(query)) &&
        shy_add(out,"search_url",mulib_json_string(url)) &&
        shy_add(out,"provider_source",mulib_json_string(program_source)) &&
        shy_add(out,"provider_blake3",mulib_json_string(digest)) &&
        shy_add(out,"mulib_revision",mulib_json_string(SHY_MULIB_REVISION)) &&
        shy_add(out,"requested_limit",mulib_json_number((double)limit)) &&
        shy_add(out,"effective_limit",mulib_json_number((double)((unsigned)limit<program.max_results ? (unsigned)limit : program.max_results))) &&
        shy_add(out,"http_status",status ? mulib_json_number(status) : mulib_json_new(MULIB_JSON_NULL)) &&
        shy_add(out,"error_code",mulib_json_number(code));
    if (ok) ok=shy_add(out,"snapshot",!code ? shy_snapshot_record(bytes,length,url,-1,mode,status,type) : mulib_json_new(MULIB_JSON_NULL));
    for (size_t i=0;ok && i<count;++i) {
        mulib_json_t *hit=mulib_json_new(MULIB_JSON_OBJECT);
        ok=shy_add(hit,"url",mulib_json_string(results[i].url)) && shy_add(hit,"title",mulib_json_string(results[i].title));
        if (ok) ok=shy_push(hits,hit); else mulib_json_destroy(hit);
    }
    if (ok) { ok=shy_add(out,"hits",hits); hits=NULL; }
    mulib_json_destroy(hits);
    if (!ok) { mulib_json_destroy(out); return -1; }
    discovery=out; hit_count=(int64_t)count; return code;
}
int64_t shy_dragnet_prepare(const char *provider,const char *query,const char *capture,const char *root,int64_t limit)
{
    shy_dragnet_close();
    if (limit<1 || limit>64 || !*query || strlen(query)>2048 || !*provider || strlen(provider)>64 ||
        strspn(provider,"abcdefghijklmnopqrstuvwxyz0123456789_-")!=strlen(provider)) return 1;
    char path[4096],url[4096];
    int n=snprintf(path,sizeof path,"%s/%s.elvs",root,provider);
    size_t length;
    char *source=n>0 && (size_t)n<sizeof path ? shy_read(path,65536,&length) : NULL;
    mulib_elvi_scrape_program_t program;
    if (!source || !mulib_elvi_scrape_compile(source,strlen(source),&program,NULL) ||
        strcmp(program.name,provider) || !mulib_elvi_scrape_url(&program,query,(unsigned)limit,url,sizeof url)) { free(source); return 1; }
    mulib_shylock_buffer_t body={0}; char type[128]="text/html"; unsigned status=0;
    char *saved=NULL; const char *bytes=NULL; length=0;
    if (*capture) bytes=saved=shy_read(capture,SHY_PAGE_LIMIT,&length);
    else if (shy_dragnet_get(url,shy_dragnet_https,NULL,&body,&status,type,sizeof type)) { bytes=body.data; length=body.len; }
    int code=prepare(provider,query,source,bytes,length,*capture ? "saved" : "https",status,type,limit);
    free(saved); free(source); mulib_shylock_buffer_free(&body); return code;
}
int64_t shy_dragnet_count(void) { return hit_count; }
static int batch_push(mulib_json_t *out)
{
    char *serialized=out ? mulib_json_stringify(out,0) : NULL;
    size_t length=serialized ? strlen(serialized) : 0;
    int ok=serialized && !batch_failed && length<=SHY_DATA_LIMIT-batch_bytes;
    free(serialized);
    if (ok) { ok=shy_push(batch_results,out); out=NULL; batch_bytes+=length; }
    mulib_json_destroy(out);
    if (!ok) batch_failed=1;
    return ok;
}
int64_t shy_dragnet_batch_close(void)
{
    mulib_json_destroy(batch); mulib_json_destroy(batch_results); batch=NULL; batch_results=NULL;
    batch_bytes=0; batch_failed=0; return 0;
}
int64_t shy_dragnet_batch_begin(const char *job)
{
    shy_dragnet_batch_close();
    batch=mulib_json_new(MULIB_JSON_OBJECT); batch_results=mulib_json_new(MULIB_JSON_ARRAY);
    mulib_json_t *policy=mulib_json_parse(job,strlen(job),NULL);
    if (!policy || !batch_results || !shy_add(batch,"schema",mulib_json_string("shylock.discovery-batch/v1"))) {
        mulib_json_destroy(policy); shy_dragnet_batch_close(); return 2;
    }
    if (!shy_add(batch,"job",policy)) { shy_dragnet_batch_close(); return 2; }
    return 0;
}
int64_t shy_dragnet_batch_error(const char *provider)
{
    mulib_json_t *row=mulib_json_new(MULIB_JSON_OBJECT);
    if (!shy_add(row,"provider",mulib_json_string(provider)) || !shy_add(row,"status",mulib_json_string("provider_error"))) {
        mulib_json_destroy(row); batch_failed=1; return 2;
    }
    return batch_push(row) ? 0 : 2;
}
int64_t shy_dragnet_batch_finish(const char *status)
{
    if (!batch || batch_failed || !shy_add(batch,"status",mulib_json_string(status))) {
        shy_dragnet_batch_close(); return shy_error("cannot allocate complete discovery batch");
    }
    int ok=shy_add(batch,"results",batch_results);
    batch_results=NULL;
    if (!ok) { shy_dragnet_batch_close(); return shy_error("cannot attach discovery batch results"); }
    if (!seal(batch,"batch_id")) { shy_dragnet_batch_close(); return shy_error("cannot identify discovery batch"); }
    mulib_json_t *out=batch; batch=NULL; return shy_emit(out);
}
int64_t shy_dragnet_publish(const char *status,const char *diagnostic)
{
    if (!discovery || !shy_add(discovery,"status",mulib_json_string(status)) ||
        !shy_add(discovery,"diagnostic",mulib_json_string(diagnostic)) || !seal(discovery,"discovery_id")) return shy_error("cannot allocate discovery record");
    if (expected && !shy_json_equal(discovery,expected)) return shy_error("discovery replay disagrees with retained provenance/outcome");
    mulib_json_t *out=discovery; discovery=NULL;
    return batch ? (batch_push(out) ? 0 : 2) : shy_emit(out);
}
int64_t shy_dragnet_snapshot(const char *url,int64_t timestamp,const char *saved)
{
    mulib_shylock_buffer_t body={0}; unsigned status=0; char type[128]="text/html";
    size_t length=0; char *file=NULL; const char *bytes=NULL;
    if (*saved) bytes=file=shy_read(saved,SHY_PAGE_LIMIT,&length);
    else if (shy_dragnet_get(url,shy_dragnet_https,NULL,&body,&status,type,sizeof type) && status==200 && shy_dragnet_html_type(type)) { bytes=body.data; length=body.len; }
    mulib_json_t *out=bytes ? shy_snapshot_record(bytes,length,url,timestamp,*saved ? "saved" : "https",status,type) : NULL;
    free(file); mulib_shylock_buffer_free(&body);
    return out ? shy_emit(out) : shy_error("cannot capture bounded verified-HTTPS HTML snapshot");
}
int shy_snapshot_valid(const mulib_json_t *snapshot)
{
    size_t length;
    const char *bytes=mulib_json_text(mulib_json_get(snapshot,"html"),&length);
    const char *mode=shy_text(snapshot,"acquisition");
    int saved=!strcmp(mode,"saved");
    int64_t status=shy_integer(snapshot,"http_status",599);
    if (!bytes || (!saved && strcmp(mode,"https")) || (saved ? mulib_json_typeof(mulib_json_get(snapshot,"http_status"))!=MULIB_JSON_NULL : status!=200)) return 0;
    mulib_json_t *rebuilt=shy_snapshot_record(bytes,length,shy_text(snapshot,"source_url"),
        shy_integer(snapshot,"captured_at",INT64_C(253402300799)),mode,saved ? 0 : 200,shy_text(snapshot,"content_type"));
    int ok=rebuilt && shy_dragnet_html_type(shy_text(snapshot,"content_type")) && shy_json_equal(snapshot,rebuilt);
    mulib_json_destroy(rebuilt); return ok;
}
int64_t shy_dragnet_extract_snapshot(const char *path,const char *rules,const char *model,const char *plugin)
{
    size_t length;
    char *bytes=shy_read(path,SHY_DATA_LIMIT,&length);
    mulib_json_t *snapshot=bytes ? mulib_json_parse(bytes,length,NULL) : NULL; free(bytes);
    if (!shy_snapshot_valid(snapshot)) { mulib_json_destroy(snapshot); return shy_error("snapshot digest, metadata or source is invalid"); }
    char *rule=*rules ? shy_read(rules,16384,&length) : NULL;
    char *reply=*model ? shy_read(model,65536,&length) : NULL;
    const char *html=shy_text(snapshot,"html");
    mulib_json_t *out=(!*rules || rule) ? shy_capture_record(html,strlen(html),shy_text(snapshot,"source_url"),
        shy_integer(snapshot,"captured_at",INT64_C(253402300799)),rule ? rule : "",reply ? reply : (*model ? "unreadable model reply" : ""),plugin) : NULL;
    free(rule); free(reply); mulib_json_destroy(snapshot);
    return out ? shy_emit(out) : shy_error("snapshot extractor failed");
}
int64_t shy_dragnet_replay(const char *path)
{
    shy_dragnet_close(); size_t length;
    char *bytes=shy_read(path,SHY_DATA_LIMIT,&length);
    mulib_json_t *record=bytes ? mulib_json_parse(bytes,length,NULL) : NULL; free(bytes);
    const mulib_json_t *snapshot=mulib_json_get(record,"snapshot");
    if (!shy_snapshot_valid(snapshot) || shy_integer(record,"error_code",5)!=0) { mulib_json_destroy(record); return -1; }
    int64_t limit=shy_integer(record,"requested_limit",64);
    int code=limit>=1 ? prepare(shy_text(record,"provider"),shy_text(record,"query"),shy_text(record,"provider_source"),
        shy_text(snapshot,"html"),strlen(shy_text(snapshot,"html")),shy_text(snapshot,"acquisition"),
        !strcmp(shy_text(snapshot,"acquisition"),"https") ? 200 : 0,shy_text(snapshot,"content_type"),limit) : -1;
    int ok=code==0 && discovery;
    if (ok) expected=record;
    else mulib_json_destroy(record);
    if (!ok) { shy_dragnet_close(); return -1; }
    return 0;
}
