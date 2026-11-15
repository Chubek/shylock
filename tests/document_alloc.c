#define _POSIX_C_SOURCE 200809L
#include "../src/shylock.h"
#include "../src/document.h"
#include "../src/latex.h"
#include "../src/engine.h"
#include "../plugins/dragnet/workflow.h"
#include "../plugins/dragnet/intelligence.h"
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
int64_t shy_emit(mulib_json_t *record) { mulib_json_destroy(record); return 2; }
int shy_extensions_enqueue(const char *event, const mulib_json_t *payload)
{ (void)event; (void)payload; return 1; }
static int unavailable_tls(void *context,const mulib_uri_t *endpoint,mulib_tsl_t *out)
{ (void)context; (void)endpoint; (void)out; return 0; }

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
    int reached = 0;
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
    for (size_t at = 1; ok && at <= 768; ++at) {
        calls = 0; injected = 0; fault = at;
        int success = shy_engine_open("tests/fixtures/series.json") == 5 && shy_engine_policy(policy) == 1 &&
            shy_engine_candidate(candidate) == 0 && shy_engine_fold(0, 3, 1, 0) == 1 &&
            shy_engine_complete(0) == 1 && shy_engine_fold(0, 4, 1, 1) == 1 &&
            shy_engine_finish(0, 1, 1, 9000) == 0;
        fault = 0;
        reached += injected;
        if ((injected && success) || (!injected && !success)) ok = 0;
        if (success && (shy_engine_reference() != 20000 || shy_engine_emit() != 2)) ok = 0;
        shy_engine_close(); shy_engine_close();
    }
    for (size_t at=1;ok && at<=1536;++at) {
        calls=0; injected=0; fault=at;
        int64_t prepared=shy_dragnet_prepare("google","lake cottage","tests/fixtures/google.html",
            "third_party/mulib/library/elvi-scrape",10);
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
    if (saved) { setenv("SHY_FAULT_TEST_OPENAI_KEY",saved,1); free(saved); } else unsetenv("SHY_FAULT_TEST_OPENAI_KEY");
    mulib_json_destroy(snapshot); mulib_json_destroy(ir); mulib_json_destroy(profile);
    const char job[]="{\"start_url\":\"https://example.org/search\",\"captured_at\":1000,\"allow_network\":false,\"pages\":[{\"url\":\"https://example.org/search\",\"path\":\"tests/fixtures/browser-start.html\"}]}";
    for (size_t at=1;ok && at<=512;++at) {
        calls=0; injected=0; fault=at;
        if (!shy_browser_begin(job,".")) (void)shy_browser_step("assert","h1","Saved listings");
        (void)shy_browser_finish(0);
        fault=0; reached+=injected; shy_browser_close(); shy_browser_close();
    }
    return ok && reached > 50;
}
