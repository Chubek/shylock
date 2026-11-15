#define _POSIX_C_SOURCE 200809L
#include "../src/shylock.h"
#include "../src/document.h"
#include "../src/latex.h"
#include "../src/engine.h"
#include "../plugins/dragnet/dragnet.h"
#include "mulib-kvdb.h"
#include "../plugins/poindexter/ui.h"
#if SHY_HAVE_SDL2
#include <SDL.h>
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

static int fixture_extract(const shy_capture *capture, mulib_json_t *items)
{
    if (capture->length != 18 || memcmp(capture->html, "EUR 1.00 per night", 18)) return 0;
    mulib_json_t *row = mulib_json_new(MULIB_JSON_OBJECT);
    int ok = shy_add(row, "method", mulib_json_string("fixture")) &&
        shy_add(row, "amount_text", mulib_json_string("1.00")) &&
        shy_add(row, "currency", mulib_json_string("EUR")) &&
        shy_add(row, "minor", mulib_json_number(100)) &&
        shy_add(row, "basis", mulib_json_string("nightly")) &&
        shy_add(row, "qualifier", mulib_json_string("unspecified")) &&
        shy_add(row, "byte_offset", mulib_json_number(0)) &&
        shy_add(row, "byte_length", mulib_json_number(18));
    if (!ok) { mulib_json_destroy(row); return 0; }
    return shy_push(items, row);
}

static int failed_extract(const shy_capture *capture, mulib_json_t *items)
{
    (void)capture;
    (void)shy_push(items, mulib_json_new(MULIB_JSON_OBJECT));
    return 0;
}

static int bad_span_extract(const shy_capture *capture, mulib_json_t *items)
{
    if (!fixture_extract(capture, items)) return 0;
    return shy_add(mulib_json_at(items, 0), "byte_length", mulib_json_number(1000));
}

int64_t shy_native_registry_test(void)
{
    static const shy_plugin second = { SHY_EXTRACTOR_ABI, sizeof(shy_plugin), "fixture", "fixture/v1", fixture_extract };
    static const shy_plugin failed = { SHY_EXTRACTOR_ABI, sizeof(shy_plugin), "failed", "failed/v1", failed_extract };
    static const shy_plugin bad_span = { SHY_EXTRACTOR_ABI, sizeof(shy_plugin), "bad-span", "bad-span/v1", bad_span_extract };
    shy_plugin invalid = second;
    if (!shy_plugins_init()) return 0;
    invalid.abi += 1;
    if (shy_plugin_register(&invalid)) return 0;
    invalid = second; invalid.size -= 1;
    if (shy_plugin_register(&invalid)) return 0;
    invalid = second; invalid.extract = NULL;
    if (shy_plugin_register(&invalid)) return 0;
    if (!shy_plugin_register(&second) || shy_plugin_register(&second) ||
        !shy_plugin_register(&failed) || !shy_plugin_register(&bad_span) || shy_plugin_find("fixture") != &second) return 0;
    const char *html = "EUR 1.00 per night";
    mulib_json_t *record = shy_capture_record(html, strlen(html), "https://example.org/", 1000, "", "", "fixture");
    int ok = record && shy_verify_capture(record, html, strlen(html)) &&
        mulib_json_size(mulib_json_get(record, "candidates")) == 1;
    mulib_json_destroy(record);
    record = shy_capture_record(html, strlen(html), "https://example.org/", 1000, "", "", "failed");
    if (record) { mulib_json_destroy(record); return 0; }
    record = shy_capture_record(html, strlen(html), "https://example.org/", 1000, "", "", "bad-span");
    if (record) { mulib_json_destroy(record); return 0; }
    return ok;
}

extern int64_t shy_store_open(const char *path);
extern int64_t shy_store_close(void);
extern int64_t shy_store_select(const char *cohort);
extern const char *shy_stored_field(int64_t index, const char *key);
extern int64_t shy_stored_number(int64_t index, const char *key);

int64_t shy_native_document_test(void)
{
    int ok = 1;
    for (int i = 0; i < 3; ++i) {
        ok = ok && shy_document_open("tests/fixtures/booking-outcomes.json") == 1 &&
            shy_document_has("", -1, "rows", "array") == 1 &&
            shy_document_size("", -1, "rows") == 6 &&
            shy_document_number("rows", 0, "rate_minor") == 10000 &&
            shy_document_number("rows", 0, "missing") == -1 &&
            shy_document_has("rows", 0, "rate_minor", "string") == 0 &&
            !strcmp(shy_document_text("rows", 0, "outcome"), "booked") &&
            strlen(shy_document_id()) == 64 && *shy_document_json() &&
            !strcmp(shy_json_quote("a\"b"), "\"a\\\"b\"") &&
            !strcmp(shy_json_ratio(3, 2), "1.5") && !*shy_json_ratio(1, 0) &&
            shy_document_child("missing") == 0 && shy_document_child("context") == 1 &&
            shy_document_number("", -1, "guests") == 2;
        /* Reopening drops the previous tree and invalidates borrowed views. */
        ok = ok && shy_document_open("tests/fixtures/booking-outcomes.json") == 1 &&
            shy_document_has("", -1, "rows", "array") == 1;
        shy_document_close(); shy_document_close();
        ok = ok && shy_document_has("", -1, "", "object") == 0 &&
            shy_document_open("tests/fixtures/rental.html") == 0 &&
            shy_document_number("", -1, "scale") == -1;
        shy_document_close();
    }
    return ok;
}

int64_t shy_native_latex_test(void)
{
    int ok = 1;
    for (int i = 0; i < 3; ++i) {
        ok = ok && shy_latex_begin("A&B") == 1 && shy_latex_append(1, "Scope") == 1 &&
            shy_latex_append(0, "x_1 % \\input{not-code}") == 1 && shy_latex_finish() == 1 &&
            shy_latex_finish() == 1 && strlen(shy_latex_digest()) == 64;
        shy_latex_close(); shy_latex_close();
        ok = ok && shy_latex_begin("Failure must not publish") == 1 &&
            shy_latex_append(4, "invalid level") == 0 && shy_latex_finish() == 0 && !*shy_latex_digest();
        shy_latex_close();
        ok = ok && shy_latex_begin("Control bytes") == 1 && shy_latex_append(0, "bad\001text") == 0 && shy_latex_finish() == 0;
        shy_latex_close();
    }
    return ok;
}

int64_t shy_native_engine_test(void)
{
    int ok = shy_engine_open("tests/fixtures/series.json") == 5 &&
        shy_engine_policy("{\"window\":4,\"market_method\":\"median\",\"market_parameter_ppm\":0}") == 1 &&
        shy_engine_candidate("{\"name\":\"last\",\"model\":\"last\"}") == 0 &&
        shy_engine_fold(0, 4, 2, 0) == 0 && shy_engine_fold(0, 3, 1, 0) == 1 &&
        shy_engine_fold(0, 4, 1, 1) == 1 && shy_engine_finish(0, 1, 1, 9000) == 0 &&
        shy_engine_reference() == 20000 && shy_engine_number("verified") == 0;
    shy_engine_close(); shy_engine_close();
    ok = ok && shy_money_operation("scale", 1, 5000, 10000, "EUR", 2) == 1 &&
        shy_money_operation("scale", 1000000000000LL, 365, 1, "EUR", 2) == -1 &&
        shy_money_operation("add", 1, 2, 1, "JPY", 0) == 3 &&
        shy_money_operation("sub", 1, 2, 1, "KWD", 3) == -1 &&
        shy_money_operation("add", 1, 2, 1, "JPY", 2) == -1;
    return ok;
}

static int fixture_resolve(void *context,const char *host,char address[16])
{ (void)host; strcpy(address,context); return 1; }
int64_t shy_native_dragnet_test(void)
{
    char host[256];
    int ok=shy_dragnet_url_host("https://example.org/search?q=hello%20world",host) && !strcmp(host,"example.org");
    static const char *const bad[]={"http://example.org/","https://localhost/","https://127.0.0.1/",
        "https://[::1]/","https://user@example.org/","https://example.org:8443/","https://example.org/%0a",
        "https://example.org/\\evil","https://example.org/%5c","https://example.org/%ZZ"};
    for (size_t i=0;i<sizeof bad/sizeof *bad;++i) if (shy_dragnet_url_host(bad[i],host)) ok=0;
    static const unsigned char private[][4]={{0,0,0,0},{10,0,0,1},{127,0,0,1},{169,254,0,1},{172,16,0,1},
        {192,168,0,1},{100,64,0,1},{198,18,0,1},{192,0,2,1},{198,51,100,1},{203,0,113,1},{224,0,0,1}};
    for (size_t i=0;i<sizeof private/sizeof *private;++i) if (shy_dragnet_public_ipv4(private[i])) ok=0;
    char directory[]="/tmp/opencode/shylock-transport-XXXXXX";
    if (!mkdtemp(directory)) return 0;
    char script[512],path[8192]; snprintf(script,sizeof script,"%s/curl",directory);
    const char *old=getenv("PATH"); char *saved=old ? strdup(old) : NULL;
    snprintf(path,sizeof path,"%s:%s",directory,old ? old : "");
    if (setenv("PATH",path,1)) ok=0;
    static const char *const sources[]={
        "#!/bin/sh\nprintf '<html>ok</html>\\n200\\ntext/html\\n'\n",
        "#!/bin/sh\nprintf '<html>redirect</html>\\n302\\ntext/html\\n'\n",
        "#!/bin/sh\nprintf 'broken trailer'\n",
        "#!/bin/sh\nsleep 5\n",
        "#!/bin/sh\nprintf '<html>ok</html>\\n200\\ntext/html\\n'\nexec 1>&-\nsleep 5\n"
    };
    shy_dragnet_transport transport={fixture_resolve,"93.184.216.34",200};
    for (size_t i=0;ok && i<sizeof sources/sizeof *sources;++i) {
        FILE *f=fopen(script,"w"); if (!f) { ok=0; break; }
        int written=fputs(sources[i],f)>=0; if (fclose(f)) written=0;
        if (!written || chmod(script,0700)) { ok=0; break; }
        mulib_shylock_buffer_t body={0}; unsigned status=777; char type[128]="unchanged";
        int got=shy_dragnet_https(&transport,"https://example.org/",&body,&status,type,sizeof type);
        if (i<2) ok=got && status==(i==0 ? 200u : 302u) && !strcmp(type,"text/html");
        else ok=!got && status==777 && !strcmp(type,"unchanged") && !body.len;
        mulib_shylock_buffer_free(&body);
    }
    transport.context="127.0.0.1";
    mulib_shylock_buffer_t body={0}; unsigned status=0; char type[128];
    if (shy_dragnet_https(&transport,"https://example.org/",&body,&status,type,sizeof type)) ok=0;
    mulib_shylock_buffer_free(&body);
    if (saved) { setenv("PATH",saved,1); free(saved); } else unsetenv("PATH");
    unlink(script); rmdir(directory); return ok;
}

int64_t shy_native_storage_test(void)
{
    char path[4096], lock[4102], key[67];
    const char *temp = getenv("TMPDIR");
    int length = snprintf(path, sizeof path, "%s/shylock-migration-XXXXXX", temp && *temp ? temp : "/tmp");
    if (length < 0 || (size_t)length >= sizeof path) return 0;
    int fd = mkstemp(path);
    if (fd < 0) return 0;
    close(fd);
    snprintf(lock, sizeof lock, "%s.lock", path);
    memset(key, 'a', 64); memcpy(key + 64, "/1", 3);
    const char *legacy = "{\"cohort\":\"legacy\",\"reviewer\":\"alice\",\"basis\":\"nightly\","
        "\"currency\":\"EUR\",\"minor\":18000,\"captured_at\":1000,\"source_url\":\"https://example.org/\"}";
    mulib_kvdb_t *db = mulib_kvdb_create();
    int ok = db && mulib_kvdb_set(db, key, strlen(key), legacy, strlen(legacy)) == MULIB_KVDB_OK &&
        mulib_kvdb_save(db, path) == MULIB_KVDB_OK;
    mulib_kvdb_close(db); db = NULL;
    ok = ok && shy_store_open(path) == 1 && shy_store_select("legacy") == 1 &&
        !strcmp(shy_stored_field(0, "provenance"), "legacy-unverified") && shy_stored_number(0, "scale") == 2;
    shy_store_close();
    if (ok && mulib_kvdb_open(path, 0, &db) == MULIB_KVDB_OK) {
        const void *value; size_t n;
        ok = mulib_kvdb_size(db) == 1 && mulib_kvdb_get(db, key, strlen(key), &value, &n) == MULIB_KVDB_OK &&
            n == strlen(legacy) && !memcmp(value, legacy, n);
    } else ok = 0;
    mulib_kvdb_close(db);
    unlink(path); unlink(lock);
    return ok;
}

int64_t shy_native_gui_test(void)
{
#if SHY_HAVE_SDL2
    if (!shy_gui_open(0)) { shy_gui_close(); return -1; }
    shy_ui_event event = { 0 };
    event.action = "";
    int ok = shy_gui_render(&event);
    SDL_Event quit;
    memset(&quit, 0, sizeof quit); quit.type = SDL_QUIT;
    ok = ok && SDL_PushEvent(&quit) == 1;
    int received = 0;
    for (size_t i = 0; ok && !received && i < 32; ++i) {
        ok = shy_gui_next(&event);
        received = event.action && !strcmp(event.action, "quit");
    }
    shy_gui_close();
    return ok && received ? 1 : -1;
#else
    return 0; /* Headless GUI rendering is still tested without SDL2. */
#endif
}
