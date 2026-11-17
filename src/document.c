#include "shylock.h"
#include "document.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Scalar ABI for a single lexical document session. The adapter owns artifact;
   root and string lookups borrow it until close. JSON/quote scratch lasts until
   the next JSON/quote call; the caller immediately copies it into a buffer.
   No domain admission, grouping or estimation policy lives here. */
static mulib_json_t *artifact;
static const mulib_json_t *root;
static char *scratch;
static char input_digest[65];
static int failed;

int64_t shy_document_close(void)
{
    mulib_json_destroy(artifact); artifact = NULL; root = NULL;
    free(scratch); scratch = NULL;
    failed = 0;
    return 0;
}

int64_t shy_document_open(const char *path)
{
    shy_document_close();
    size_t length;
    char *data = shy_read(path, SHY_DATA_LIMIT, &length);
    if (!data) return 0;
    shy_digest(data, length, input_digest);
    artifact = mulib_json_parse(data, length, NULL);
    free(data); root = artifact;
    return artifact != NULL;
}

int64_t shy_document_parse(const char *text)
{
    size_t length = strlen(text);
    shy_document_close();
    if (length > SHY_DATA_LIMIT) return 0;
    artifact = mulib_json_parse(text, length, NULL); root = artifact;
    shy_digest(text, length, input_digest); return artifact != NULL;
}

const mulib_json_t *shy_document_borrow(void) { return artifact; }
int64_t shy_document_ok(void) { return !failed; }

static const mulib_json_t *lookup(const char *section, int64_t index, const char *key)
{
    const mulib_json_t *value = root;
    if (*section) value = mulib_json_get(value, section);
    if (index >= 0) value = mulib_json_at(value, (size_t)index);
    if (*key) value = mulib_json_get(value, key);
    return value;
}

int64_t shy_document_has(const char *section, int64_t index, const char *key, const char *kind)
{
    const mulib_json_t *v = lookup(section, index, key);
    mulib_json_type type = mulib_json_typeof(v);
    if (!v) return 0;
    if (!strcmp(kind, "object")) return type == MULIB_JSON_OBJECT;
    if (!strcmp(kind, "array")) return type == MULIB_JSON_ARRAY;
    if (!strcmp(kind, "boolean")) return type == MULIB_JSON_BOOL;
    if (!strcmp(kind, "number")) return type == MULIB_JSON_NUMBER;
    if (!strcmp(kind, "null")) return type == MULIB_JSON_NULL;
    if (!strcmp(kind, "string")) {
        size_t length;
        const char *text = mulib_json_text(v, &length);
        return text && length <= SHY_PAGE_LIMIT && strlen(text) == length;
    }
    if (!strcmp(kind, "integer")) {
        double n = mulib_json_numeric(v);
        return type == MULIB_JSON_NUMBER && isfinite(n) && n >= 0 &&
            n <= 1e12 && trunc(n) == n;
    }
    return 0;
}

int64_t shy_document_size(const char *section, int64_t index, const char *key)
{
    return (int64_t)mulib_json_size(lookup(section, index, key));
}

const char *shy_document_text(const char *section, int64_t index, const char *key)
{
    size_t length;
    const char *s = mulib_json_text(lookup(section, index, key), &length);
    return s && length <= SHY_PAGE_LIMIT && strlen(s) == length ? s : "";
}

int64_t shy_document_number(const char *section, int64_t index, const char *key)
{
    if (!shy_document_has(section, index, key, "integer")) return -1;
    return (int64_t)mulib_json_numeric(lookup(section, index, key));
}

int64_t shy_document_child(const char *key)
{
    const mulib_json_t *child = mulib_json_get(root, key);
    if (!child) return 0;
    root = child;
    return 1;
}

int64_t shy_document_reset(void) { root = artifact; return root != NULL; }

int64_t shy_document_index(int64_t index)
{
    if (index < 0 || mulib_json_typeof(root) != MULIB_JSON_ARRAY) return 0;
    const mulib_json_t *value = mulib_json_at(root, (size_t)index);
    if (!value) return 0;
    root = value; return 1;
}

int64_t shy_document_boolean(const char *section, int64_t index, const char *key)
{
    const mulib_json_t *value = lookup(section, index, key);
    return mulib_json_typeof(value) == MULIB_JSON_BOOL ? mulib_json_boolean(value) : -1;
}

const char *shy_document_json_at(const char *section, int64_t index, const char *key)
{
    const mulib_json_t *value = lookup(section, index, key);
    free(scratch); scratch = value ? mulib_json_stringify(value, 0) : NULL;
    /* Leave room for the generated record within On1x's 4 MiB text bound. */
    if (scratch && strlen(scratch) > SHY_PAGE_LIMIT) { free(scratch); scratch = NULL; }
    if (!scratch) failed = 1;
    return scratch ? scratch : "";
}

const char *shy_document_json(void) { return shy_document_json_at("", -1, ""); }

const char *shy_document_id(void)
{
    static char digest[65];
    if (!shy_json_digest(root, NULL, digest)) { failed = 1; return ""; }
    return digest;
}

const char *shy_json_quote(const char *text)
{
    mulib_json_t *value = mulib_json_string(text);
    free(scratch); scratch = value ? mulib_json_stringify(value, 0) : NULL;
    mulib_json_destroy(value);
    if (!scratch) failed = 1;
    return scratch ? scratch : "";
}

const char *shy_json_ratio(int64_t numerator, int64_t denominator)
{
    static char number[64];
    if (numerator < 0 || denominator <= 0) return "";
    snprintf(number, sizeof number, "%.17g", (double)numerator / (double)denominator);
    return number;
}

/* Seal an On1x-built record. Verification compares the whole replay, not just
   an attacker-replaceable digest. The caller retains the session on failure. */
int64_t shy_document_seal(const char *text, const char *field, int64_t emit)
{
    if (failed) return shy_error("document serialization/allocation failure");
    mulib_json_t *record = mulib_json_parse(text, strlen(text), NULL);
    char digest[65];
    if (!record || !shy_json_digest(record, field, digest) ||
        !shy_add(record, field, mulib_json_string(digest))) {
        mulib_json_destroy(record); return shy_error("cannot seal document JSON");
    }
    if (emit) return shy_emit(record);
    int same = shy_json_equal(record, artifact);
    mulib_json_destroy(record);
    return same ? 0 : shy_error("demand artifact differs from its booking-outcome replay");
}

int64_t shy_document_optimize(int64_t floor, int64_t cap, int64_t tick, int64_t cost)
{
    return shy_optimize_data(artifact, input_digest, floor, cap, tick, cost);
}
