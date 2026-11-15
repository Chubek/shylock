#define _POSIX_C_SOURCE 200809L
#include "../src/shylock.h"
#include "mulib-kvdb.h"
#include "../plugins/poindexter/ui.h"
#if SHY_HAVE_SDL2
#include <SDL.h>
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

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
