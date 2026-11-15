#include "shylock.h"
#include "mulib-cryptohash.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

char *shy_read(const char *path, size_t limit, size_t *length)
{
    FILE *stream = fopen(path, "rb");
    if (!stream) return NULL;
    char *data = malloc(limit + 1);
    size_t n = data ? fread(data, 1, limit + 1, stream) : 0;
    int ok = data && !ferror(stream) && n <= limit && !memchr(data, 0, n);
    fclose(stream);
    if (!ok) { free(data); return NULL; }
    data[n] = 0; *length = n;
    return data;
}

void shy_digest(const char *data, size_t length, char out[65])
{
    unsigned char digest[32];
    (void)mulib_blake3(data, length, digest, sizeof digest);
    for (size_t i = 0; i < sizeof digest; ++i)
        snprintf(out + i * 2, 3, "%02x", (unsigned)digest[i]);
}

int64_t shy_error(const char *message)
{
    fprintf(stderr, "shylock: %s\n", message);
    return 2;
}

int shy_add(mulib_json_t *object, const char *key, mulib_json_t *value)
{
    if (value && mulib_json_set(object, key, value)) return 1;
    mulib_json_destroy(value);
    return 0;
}

int shy_push(mulib_json_t *array, mulib_json_t *value)
{
    if (value && mulib_json_append(array, value)) return 1;
    mulib_json_destroy(value);
    return 0;
}

const char *shy_text(const mulib_json_t *object, const char *key)
{
    size_t n;
    const char *s = mulib_json_text(mulib_json_get(object, key), &n);
    return s && strlen(s) == n ? s : "";
}

int64_t shy_integer(const mulib_json_t *object, const char *key, int64_t max)
{
    const mulib_json_t *v = mulib_json_get(object, key);
    double n = mulib_json_numeric(v);
    return mulib_json_typeof(v) == MULIB_JSON_NUMBER && n >= 0 &&
        n <= (double)max && isfinite(n) && floor(n) == n ? (int64_t)n : -1;
}

mulib_json_t *shy_clone(const mulib_json_t *value)
{
    if (!value) return NULL;
    char *s = mulib_json_stringify(value, 0);
    mulib_json_t *copy = s ? mulib_json_parse(s, strlen(s), NULL) : NULL;
    free(s);
    return copy;
}

static int compare_keys(const void *a, const void *b)
{
    return strcmp(*(const char *const *)a, *(const char *const *)b);
}

/* Canonical identifiers: sorted UTF-8 object keys, original array order and
   the pinned MuLIB numeric serialization. omit applies to the root only. */
static mulib_json_t *canonical(const mulib_json_t *value, const char *omit, unsigned depth)
{
    if (!value || depth > 64) return NULL;
    mulib_json_type type = mulib_json_typeof(value);
    if (type != MULIB_JSON_ARRAY && type != MULIB_JSON_OBJECT) return shy_clone(value);
    mulib_json_t *out = mulib_json_new(type);
    size_t n = mulib_json_size(value);
    const char **keys = type == MULIB_JSON_OBJECT && n ? malloc(n * sizeof *keys) : NULL;
    if (!out || (type == MULIB_JSON_OBJECT && n && !keys)) goto fail;
    if (type == MULIB_JSON_OBJECT) {
        for (size_t i = 0; i < n; ++i) {
            size_t length;
            keys[i] = mulib_json_key(value, i, &length);
            if (!keys[i] || strlen(keys[i]) != length) goto fail;
        }
        if (n) qsort(keys, n, sizeof *keys, compare_keys);
    }
    for (size_t i = 0; i < n; ++i) {
        if (keys && omit && !strcmp(keys[i], omit)) continue;
        const mulib_json_t *child = keys ? mulib_json_get(value, keys[i]) : mulib_json_at(value, i);
        mulib_json_t *copy = canonical(child, NULL, depth + 1);
        if (!(keys ? shy_add(out, keys[i], copy) : shy_push(out, copy))) goto fail;
    }
    free(keys);
    return out;
fail:
    free(keys); mulib_json_destroy(out); return NULL;
}

int shy_json_digest(const mulib_json_t *value, const char *omit, char out[65])
{
    mulib_json_t *copy = canonical(value, omit, 0);
    char *s = copy ? mulib_json_stringify(copy, 0) : NULL;
    mulib_json_destroy(copy);
    if (!s) return 0;
    shy_digest(s, strlen(s), out); free(s); return 1;
}

int shy_json_equal(const mulib_json_t *a, const mulib_json_t *b)
{
    char x[65], y[65];
    return shy_json_digest(a, NULL, x) && shy_json_digest(b, NULL, y) && !strcmp(x, y);
}
