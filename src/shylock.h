#ifndef SHYLOCK_H
#define SHYLOCK_H
#include <stddef.h>
#include <stdint.h>
#include "mulib-json.h"

enum { SHY_PAGE_LIMIT = 2 * 1024 * 1024 };
char *shy_read(const char *path, size_t limit, size_t *length);
void shy_digest(const char *data, size_t length, char out[65]);
int64_t shy_error(const char *message);
int64_t shy_emit(mulib_json_t *record);
int shy_add(mulib_json_t *object, const char *key, mulib_json_t *value);

/* v1 snapshot extractor. The host owns the immutable capture and the
   resulting JSON; plugins append candidates to the supplied array. */
typedef struct {
    const char *html;
    size_t length;
    const char *url;
    const char *rules_path;
    const char *model_reply_path;
    const char *digest;
} shy_capture;
typedef struct {
    unsigned abi;
    const char *name;
    int (*extract)(const shy_capture *, mulib_json_t *);
} shy_plugin;
extern const shy_plugin shy_dragnet;
#endif
