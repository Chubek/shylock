#ifndef SHYLOCK_H
#define SHYLOCK_H
#include <stddef.h>
#include <stdint.h>
#include "mulib-json.h"

enum { SHY_PAGE_LIMIT = 2 * 1024 * 1024, SHY_DATA_LIMIT = 16 * 1024 * 1024 };
#ifndef SHY_MULIB_REVISION
#define SHY_MULIB_REVISION "unknown"
#endif
char *shy_read(const char *path, size_t limit, size_t *length);
void shy_digest(const char *data, size_t length, char out[65]);
int64_t shy_error(const char *message);
int64_t shy_emit(mulib_json_t *record);
int shy_add(mulib_json_t *object, const char *key, mulib_json_t *value);
int shy_push(mulib_json_t *array, mulib_json_t *value);
const char *shy_text(const mulib_json_t *object, const char *key);
int64_t shy_integer(const mulib_json_t *object, const char *key, int64_t max);
mulib_json_t *shy_clone(const mulib_json_t *value);
int shy_json_digest(const mulib_json_t *value, const char *omit, char out[65]);
int shy_json_equal(const mulib_json_t *a, const mulib_json_t *b);
int shy_currency_scale(const char *currency);
int64_t shy_date(const char *text);

/* Extractor ABI v2: descriptors and their strings have static lifetime.
   The host borrows all input bytes for one call and owns the output arrays.
   Plugins append owned candidate/diagnostic objects, retain no input pointers,
   and return 1 on success. On failure the host discards ALL appended output.
   No MuLIB implementation macros belong in a plugin translation unit. */
#define SHY_EXTRACTOR_ABI 2u
typedef struct {
    const char *html;
    size_t length;
    const char *url;
    const char *rules_path;
    const char *model_reply_path;
    const char *digest;
    const char *rules_source;
    const char *model_source;
    mulib_json_t *diagnostics;
} shy_capture;
typedef struct {
    unsigned abi;
    size_t size;
    const char *name;
    const char *version;
    int (*extract)(const shy_capture *, mulib_json_t *);
} shy_plugin;
/* Registry borrows descriptors. Duplicate names, unknown ABIs, short
   descriptors and missing callbacks fail without changing the registry. */
int shy_plugin_register(const shy_plugin *plugin);
const shy_plugin *shy_plugin_find(const char *name);
const shy_plugin *shy_plugin_at(size_t index);
int shy_plugins_init(void);
int shy_plugin_extract(const shy_plugin *, const shy_capture *, mulib_json_t *);
const char *shy_plugin_configuration(const shy_plugin *);
int64_t shy_native_plugins_begin(const char *configuration);
int64_t shy_native_plugins_end(void);
extern const shy_plugin shy_dragnet;
/* Caller owns returned record. Sources are copied into replay provenance. */
mulib_json_t *shy_capture_record(const char *html, size_t length, const char *url,
    int64_t timestamp, const char *rules, const char *model, const char *plugin);
int shy_verify_capture(const mulib_json_t *record, const char *html, size_t length);
/* Numeric samples are copied; lineage, when present, is caller-owned. */
int shy_series_load(const char *path, double values[4096], size_t *count,
    char digest[65], mulib_json_t **lineage);
int shy_series_parse(const char *data,size_t length,double values[4096],size_t *count,
    char digest[65],mulib_json_t **lineage);
mulib_json_t *shy_estimator(void);
/* Borrows an already validated demand artifact for the duration of the call. */
int64_t shy_optimize_data(const mulib_json_t *json, const char *digest,
    int64_t floor, int64_t cap, int64_t tick, int64_t cost);
#endif
