#define _POSIX_C_SOURCE 200809L
#include "../src/shylock.h"
#include "../src/document.h"
#include "../src/latex.h"
#include "../src/engine.h"
#include "../plugins/dragnet/workflow.h"
#include "../plugins/dragnet/intelligence.h"
#include "../plugins/dragnet/naive.h"
#include "../plugins/dragnet/collection.h"
#include "../plugins/poindexter/sheet.h"
#include "mulib-elvi-scrape.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* Fault injection affects the selected operation, not setup or process startup.
   Each allocator fails once; all partial trees/scratch must remain closeable. */
static size_t fault, calls;
static int injected;
void *__real_malloc(size_t);
void *__real_calloc(size_t, size_t);
void *__real_realloc(void *, size_t);
static int fail(void)
{
    if (fault && ++calls == fault) { injected = 1; return 1; }
    return 0;
}
void *__wrap_malloc(size_t n) { return fail() ? NULL : __real_malloc(n); }
void *__wrap_calloc(size_t n, size_t size) { return fail() ? NULL : __real_calloc(n, size); }
void *__wrap_realloc(void *p, size_t n) { return fail() ? NULL : __real_realloc(p, n); }

/* This executable tests only document ownership, not the numeric kernel. */
int64_t shy_optimize_data(const mulib_json_t *json, const char *digest,
    int64_t floor, int64_t cap, int64_t tick, int64_t cost)
{
    (void)json; (void)digest; (void)floor; (void)cap; (void)tick; (void)cost;
    return 2;
}
static size_t publications;
static int inspect_decision, stale_optimization;
int64_t shy_emit(mulib_json_t *record)
{
    ++publications;
    if (inspect_decision) stale_optimization=mulib_json_typeof(mulib_json_get(record,"optimization"))!=MULIB_JSON_NULL;
    mulib_json_destroy(record); return 2;
}
int shy_extensions_enqueue(const char *event, const mulib_json_t *payload)
{ (void)event; (void)payload; return 1; }
int shy_extensions_event(const char *event,const mulib_json_t *payload)
{ (void)event; (void)payload; return 1; }
/* Resource-only worksheet selection; real store/revision policy is covered by
   worksheet.py and poindexter_finish.py against actual KVDB databases. */
static const char sheet_fixture[]="[{\"schema\":\"shylock.observation/v2\",\"observation_id\":\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\","
    "\"review_id\":\"bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb\",\"listing_id\":\"Cottage\",\"decision\":\"accepted\","
    "\"minor\":18000,\"currency\":\"EUR\",\"scale\":2,\"guests\":2,\"evidence\":{\"currency\":\"EUR\"}}]";
int64_t shy_store_select(const char *cohort) { (void)cohort; return 1; }
mulib_json_t *shy_store_rows(void) { return mulib_json_parse(sheet_fixture,sizeof sheet_fixture-1,NULL); }
int64_t shy_revision_load_data(const char *id,const char *expected,const mulib_json_t *data)
{ (void)id; (void)expected; return data!=NULL; }
static int unavailable_tls(void *context,const mulib_uri_t *endpoint,mulib_tsl_t *out)
{ (void)context; (void)endpoint; (void)out; return 0; }

static char *omit(const mulib_json_t *input,const char *a,const char *b)
{
    mulib_json_t *out=mulib_json_new(MULIB_JSON_OBJECT);
    for (size_t i=0;i<mulib_json_size(input);++i) {
        const char *key=mulib_json_key(input,i,NULL);
        if (strcmp(key,a) && strcmp(key,b) && !shy_add(out,key,shy_clone(mulib_json_get(input,key)))) {
            mulib_json_destroy(out); return NULL;
        }
    }
    char *s=mulib_json_stringify(out,0); mulib_json_destroy(out); return s;
}
static int collection_allocations(void)
{
    const char *path=getenv("SHY_COLLECTION_ALLOCATION_FIXTURE"); size_t n;
    char *text=path ? shy_read(path,SHY_DATA_LIMIT,&n) : NULL;
    mulib_json_t *fixture=text ? mulib_json_parse(text,n,NULL) : NULL; free(text);
    const mulib_json_t *entry=mulib_json_at(mulib_json_get(fixture,"entries"),0);
    char *metadata=omit(fixture,"entries","collection_id"),*row=omit(entry,"discovery","extraction");
    char *extraction=mulib_json_stringify(mulib_json_get(entry,"extraction"),0);
    int ok=metadata && row && extraction;
    size_t limit=0;
    for (size_t at=0;ok && (at==0 || at<=limit);++at) {
        calls=0; injected=0; fault=at ? at : SIZE_MAX;
        int success=shy_collection_begin(path)==1 && shy_collection_source(0,"","","")==1 &&
            shy_collection_check("results","")==1 && shy_naive_finish(extraction)==1 &&
            *shy_collection_view("summary") && shy_collection_append(row)==1;
        int quote_failed=0;
        if (success) {
            /* On1x uses NaiveFinder's sticky JSON-quote primitive while
               assembling collection metadata after the last source closes. */
            (void)shy_naive_quote("collection metadata");
            quote_failed=shy_nf.failed;
            success=shy_collection_finish(metadata,0)==1;
        }
        if (quote_failed && success) ok=0;
        if (!at) { limit=calls; if (!success || limit<50) ok=0; }
        if (at && !injected) ok=0;
        fault=0;
        if (success!=shy_collection_ready()) ok=0;
        if (success && (!shy_collection_review_begin(0) || shy_collection_review_begin(99))) ok=0;
        /* The injected sink always rejects output. Publication failure poisons
           verified access, even after a successful reconstruction. */
        if (success && (shy_collection_finish(metadata,1)!=0 || shy_collection_ready())) ok=0;
        shy_collection_close(); shy_collection_close();
        if (shy_collection_ready() || shy_collection_verified() || shy_naive_verified()) ok=0;
        if (!ok) fprintf(stderr,"collection allocation check failed at %zu/%zu (success=%d injected=%d calls=%zu)\n",at,limit,success,injected,calls);
    }
    free(metadata); free(row); free(extraction); mulib_json_destroy(fixture); return ok;
}
static int sheet_allocations(void)
{
    int ok=shy_sheet_text_valid("caf\xc3\xa9")==1 && !shy_sheet_text_valid("\x1b[31m") &&
        !shy_sheet_text_valid("\xff") && !shy_sheet_text_valid("\xc2\x85");
    size_t limit=0;
    for (size_t at=0;ok && (at==0 || at<=limit);++at) {
        calls=0; injected=0; fault=at ? at : SIZE_MAX;
        int success=shy_sheet_load("market")==0 && shy_sheet_stage_value("guests","3")==0 &&
            shy_sheet_stage_value("$reason","\"Checked\"")==0;
        mulib_json_t *out=success ? shy_sheet_export() : NULL;
        success=success && out;
        if (!at) { limit=calls; if (!success || limit<50) ok=0; }
        if ((at && !injected) || (injected && success)) ok=0;
        fault=0; mulib_json_destroy(out); shy_sheet_close(); shy_sheet_close();
    }
    if (!ok || shy_sheet_load("market")!=0 || shy_sheet_stage_value("guests","3")!=0) return 0;
    shy_sheet_committed(); /* Acknowledged commit, then an allocation-failed reload. */
    calls=0; fault=1;
    int failed_reload=shy_sheet_load("market")!=0;
    fault=0;
    mulib_json_t *out=shy_sheet_export();
    ok=failed_reload && out && mulib_json_boolean(mulib_json_get(out,"refresh_required")) &&
        shy_sheet_stage_value("guests","4")==2 && !shy_sheet_prepare_revision();
    mulib_json_destroy(out);
    if (shy_sheet_load("market")!=0 || shy_sheet_stage_value("guests","4")!=0) ok=0;
    shy_sheet_close(); shy_sheet_close(); return ok;
}

int64_t shy_document_allocation_test(void)
{
    char path[4096], digest[65];
    const char *temp = getenv("TMPDIR");
    int n = snprintf(path, sizeof path, "%s/shylock-document-XXXXXX", temp && *temp ? temp : "/tmp");
    if (n < 0 || (size_t)n >= sizeof path) return 0;
    int fd = mkstemp(path);
    FILE *file = fd < 0 ? NULL : fdopen(fd, "w");
    if (!file) { if (fd >= 0) { close(fd); unlink(path); } return 0; }
    const char *body = "{\"value\":1}";
    mulib_json_t *record = mulib_json_parse(body, strlen(body), NULL);
    int ok = record && shy_json_digest(record, NULL, digest);
    mulib_json_destroy(record);
    if (ok) ok = fprintf(file, "{\"value\":1,\"id\":\"%s\"}", digest) > 0;
    if (fclose(file)) ok = 0;
    int reached = 0, elvi_parse_failures=0;
    for (size_t at = 1; ok && at <= 256; ++at) {
        calls = 0; injected = 0; fault = at;
        (void)shy_document_open(path);
        fault = 0;
        reached += injected;
        shy_document_close(); shy_document_close();

        if (!shy_document_open(path)) { ok = 0; break; }
        calls = 0; injected = 0; fault = at;
        (void)shy_document_json();
        (void)shy_json_quote("a\"b");
        (void)shy_document_id();
        int64_t result = shy_document_seal(body, "id", 0);
        fault = 0;
        reached += injected;
        if ((injected && result != 2) || (!injected && result != 0)) ok = 0;
        shy_document_close(); shy_document_close();
    }
    fault = 0; shy_document_close(); unlink(path);
    for (size_t at = 1; ok && at <= 128; ++at) {
        calls = 0; injected = 0; fault = at;
        int success = shy_latex_begin("A&B") && shy_latex_append(1, "Calculation") &&
            shy_latex_append(0, "x_1 % \\input{not-code}") && shy_latex_finish();
        fault = 0;
        reached += injected;
        if ((injected && success) || (!injected && !success)) ok = 0;
        shy_latex_close(); shy_latex_close();
    }
    const char *policy = "{\"window\":4,\"market_method\":\"median\",\"market_parameter_ppm\":0}";
    const char *candidate = "{\"name\":\"last\",\"model\":\"last\"}";
    mulib_json_t *in_memory=mulib_json_parse("[1,2,3,4,5]",11,NULL);
    if (!in_memory) ok=0;
    for (size_t at=1;ok && at<=128;++at) {
        calls=0; injected=0; fault=at;
        int success=shy_engine_open_record(in_memory)==5 && shy_engine_selection("{\"selection\":{}}")==1;
        fault=0; reached+=injected;
        if ((injected && success) || (!injected && !success)) ok=0;
        shy_engine_close(); shy_engine_close();
    }
    mulib_json_destroy(in_memory);
    size_t engine_allocations=0;
    for (size_t at = 0; ok && (at==0 || at<=engine_allocations); ++at) {
        calls = 0; injected = 0; fault = at ? at : SIZE_MAX;
        int success = shy_engine_open("tests/fixtures/series.json") == 5 && shy_engine_policy(policy) == 1 &&
            shy_engine_candidate(candidate) == 0 && shy_engine_fold(0, 3, 1, 0) == 1 &&
            shy_engine_complete(0) == 1 && shy_engine_fold(0, 4, 1, 1) == 1 &&
            shy_engine_finish(0, 1, 1, 9000) == 0;
        fault = 0;
        if (!at) engine_allocations=calls;
        reached += injected;
        if ((injected && success) || (!injected && !success)) ok = 0;
        if (!success && shy_engine_reference()!=-1) ok=0;
        if (success && (shy_engine_reference() != 20000 || shy_engine_emit() != 2 || shy_engine_reference()!=-1)) ok = 0;
        shy_engine_close(); shy_engine_close();
    }
    /* A failed optional quote operation can leave valid-looking empty metadata.
       The decision sink must honor sticky document failure, not publish it. */
    if (ok && shy_document_parse("{}") && shy_engine_open("tests/fixtures/series.json")==5 &&
        shy_engine_policy(policy)==1 && shy_engine_candidate(candidate)==0 && shy_engine_fold(0,3,1,0)==1 &&
        shy_engine_fold(0,4,1,1)==1 && shy_engine_finish(0,1,1,9000)==0 && shy_engine_pricing_policy("{}")) {
        calls=0; fault=1; (void)shy_json_quote("formula audit"); fault=0;
        size_t before=publications;
        if (shy_document_ok() || shy_engine_decision("priced",20000,20000,20000,"{}","{}")!=2 || publications!=before) ok=0;
    } else ok=0;
    shy_document_close(); shy_engine_close();
    if (ok && shy_engine_open("tests/fixtures/series.json")==5 && shy_engine_policy(policy)==1 &&
        shy_engine_candidate(candidate)==0 && shy_engine_fold(0,3,1,0)==1 && shy_engine_fold(0,4,1,1)==1 &&
        shy_engine_finish(0,1,1,9000)==0 && shy_engine_pricing_policy("{}")) {
        const char text[]="{\"scenarios\":[{\"rate_minor\":20000,\"expected_units\":1}]}";
        mulib_json_t *demand=mulib_json_parse(text,sizeof text-1,NULL);
        ok=demand && shy_engine_demand(demand) && shy_engine_optimize(10000,30000,100,0,"profit")==20000 &&
            shy_engine_optimize(25000,30000,100,0,"profit")==-1;
        inspect_decision=1; size_t before=publications;
        ok=ok && shy_engine_decision("demand_error",20000,20000,-1,"{}","")==2 && publications==before+1 &&
            !stale_optimization && shy_engine_reference()==-1;
        inspect_decision=0; mulib_json_destroy(demand);
    } else ok=0;
    shy_engine_close();
    mulib_elvi_scrape_program_t program;
    if (!mulib_elvi_scrape_load("third_party/mulib/library/elvi-scrape/google.elvs",&program,NULL)) ok=0;
    for (size_t at=0;ok && at<=2;++at) {
        mulib_elvi_scrape_result_t rows[2],before[2];
        memset(rows,0xa5,sizeof rows); memcpy(before,rows,sizeof rows); size_t count=123;
        const char page[]="<a href='https://example.org/'><h3>Safe</h3></a>";
        calls=0; injected=0; fault=at;
        int success=mulib_elvi_scrape_parse(&program,page,sizeof page-1,rows,2,&count);
        fault=0; reached+=injected;
        if (at ? (!injected || success || count!=123 || memcmp(rows,before,sizeof rows)) : (!success || count!=1)) ok=0;
    }
    for (size_t at=1;ok && at<=1536;++at) {
        calls=0; injected=0; fault=at;
        int64_t prepared=shy_dragnet_prepare("google","lake cottage","tests/fixtures/google.html",
            "third_party/mulib/library/elvi-scrape",10);
        if (prepared==5) {
            ++elvi_parse_failures;
            if (shy_dragnet_count()!=0) ok=0;
        }
        int success=prepared==0 && shy_dragnet_publish("results","")==2; /* injected failing output callback */
        fault=0; reached+=injected;
        if (!injected && !success) ok=0;
        shy_dragnet_close(); shy_dragnet_close();
    }
    for (size_t at=1;ok && at<=512;++at) {
        calls=0; injected=0; fault=at;
        if (!shy_dragnet_batch_begin("{\"schema\":\"test\"}")) {
            (void)shy_dragnet_batch_error("fixture");
            (void)shy_dragnet_batch_finish("partial_failure");
        }
        fault=0; reached+=injected;
        shy_dragnet_batch_close(); shy_dragnet_batch_close();
    }
    const char html[]="<main><p>EUR 180.00 per night</p></main>";
    mulib_json_t *snapshot=shy_snapshot_record(html,strlen(html),"https://example.org/cottage",1000,"saved",0,"text/html");
    mulib_json_t *ir=snapshot ? shy_flatten(snapshot) : NULL;
    const char settings[]="{\"model\":\"fixture\",\"api_key_env\":\"SHY_FAULT_TEST_OPENAI_KEY\",\"organization\":\"\",\"project\":\"\",\"input\":\"ir\",\"max_output_tokens\":1024}";
    mulib_json_t *profile=mulib_json_parse(settings,strlen(settings),NULL);
    const char *prior=getenv("SHY_FAULT_TEST_OPENAI_KEY"); char *saved=prior ? strdup(prior) : NULL;
    if (setenv("SHY_FAULT_TEST_OPENAI_KEY","fake-key-no-network",1)) ok=0;
    if (!snapshot || !ir || !profile) ok=0;
    for (size_t at=1;ok && at<=1536;++at) {
        calls=0; injected=0; fault=at;
        mulib_json_t *result=shy_flatten(snapshot);
        fault=0; reached+=injected; mulib_json_destroy(result);
        calls=0; injected=0; fault=at;
        result=shy_find_information(ir,"prices");
        fault=0; reached+=injected; mulib_json_destroy(result);
        calls=0; injected=0; fault=at;
        int success=0;
        result=shy_model_information(ir,profile,"prices",unavailable_tls,NULL,&success);
        fault=0; reached+=injected;
        if (success) ok=0; /* Never invent successful model work after failure. */
        mulib_json_destroy(result);
    }
    mulib_json_t *naive_record=mulib_json_new(MULIB_JSON_OBJECT);
    if (!shy_add(naive_record,"schema",mulib_json_string("shylock.naive-find/v1")) ||
        !shy_add(naive_record,"snapshot",shy_clone(snapshot)) ||
        !shy_add(naive_record,"rule_source",mulib_json_string("{}"))) ok=0;
    for (size_t at=1;ok && at<=2048;++at) {
        calls=0; injected=0; fault=at;
        int success=shy_naive_begin_record(naive_record);
        if (success) {
            int64_t node=shy_naive_query(0,"p",0);
            success=node>0 && shy_naive_scan(node,"","EUR [0-9][0-9.]*")==1;
            if (success) {
                (void)shy_naive_span_digest(shy_naive_hit_number(0,"offset"),shy_naive_hit_number(0,"span"));
                (void)shy_naive_quote("quoted \"value\"");
                (void)shy_naive_decode("\"key\"");
                (void)shy_naive_identify("{\"value\":1}","id");
            }
        }
        fault=0; reached+=injected;
        if ((!injected && !success) || shy_naive_verified()) ok=0;
        shy_naive_close(); shy_naive_close();
    }
    mulib_json_destroy(naive_record);
    if (saved) { setenv("SHY_FAULT_TEST_OPENAI_KEY",saved,1); free(saved); } else unsetenv("SHY_FAULT_TEST_OPENAI_KEY");
    mulib_json_destroy(snapshot); mulib_json_destroy(ir); mulib_json_destroy(profile);
    const char job[]="{\"start_url\":\"https://example.org/search\",\"captured_at\":1000,\"allow_network\":false,\"pages\":[{\"url\":\"https://example.org/search\",\"path\":\"tests/fixtures/browser-start.html\"}]}";
    for (size_t at=1;ok && at<=512;++at) {
        calls=0; injected=0; fault=at;
        if (!shy_browser_begin(job,".")) (void)shy_browser_step("assert","h1","Saved listings");
        (void)shy_browser_finish(0);
        fault=0; reached+=injected; shy_browser_close(); shy_browser_close();
    }
    return ok && reached > 50 && elvi_parse_failures>0 && collection_allocations() && sheet_allocations();
}
