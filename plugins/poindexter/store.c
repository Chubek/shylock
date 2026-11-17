#define _POSIX_C_SOURCE 200809L
#include "../../src/shylock.h"
#include "../../src/extensions.h"
#include "../../src/poindexter.h"
#include "mulib-kvdb.h"
#include "mulib-money.h"
#include "../dragnet/naive.h"
#include <sys/file.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

enum { MAX_ROWS = 16384 };
static mulib_json_t *capture, *context, *resolved, *selected, *revision_base;
static mulib_kvdb_t *db;
static char *snapshot;
static size_t snapshot_length;
static int verified, lock_fd = -1;

static int hash_valid(const char *s)
{
    return strlen(s) == 64 && strspn(s, "0123456789abcdef") == 64;
}

int64_t shy_store_close(void)
{
    mulib_json_destroy(capture); capture = NULL;
    mulib_json_destroy(context); context = NULL;
    mulib_json_destroy(resolved); resolved = NULL;
    mulib_json_destroy(selected); selected = NULL;
    mulib_json_destroy(revision_base); revision_base = NULL;
    free(snapshot); snapshot = NULL; snapshot_length = 0; verified = 0;
    mulib_kvdb_close(db); db = NULL;
    if (lock_fd >= 0) { close(lock_fd); lock_fd = -1; }
    return 0;
}

int64_t shy_store_open(const char *path)
{
    shy_store_close();
    size_t n = strlen(path);
    if (!n || n > 4096) return 0;
    char *lock = malloc(n + 6);
    if (!lock) return 0;
    memcpy(lock, path, n); memcpy(lock + n, ".lock", 6);
    lock_fd = open(lock, O_CREAT | O_RDWR | O_CLOEXEC, 0600); free(lock);
    /* The lock covers open, read/modify, commit AND close. */
    if (lock_fd < 0 || flock(lock_fd, LOCK_EX) ||
        mulib_kvdb_open(path, 1, &db) != MULIB_KVDB_OK) {
        shy_store_close(); shy_error("cannot lock or open Poindexter database"); return 0;
    }
    return 1;
}

static mulib_json_t *read_json(const char *path, size_t limit)
{
    size_t n;
    char *data = shy_read(path, limit, &n);
    mulib_json_t *value = data ? mulib_json_parse(data, n, NULL) : NULL;
    free(data);
    return value;
}

static int context_valid(void)
{
    if (mulib_json_typeof(context) != MULIB_JSON_OBJECT) return 0;
    static const char *const allowed[] = { "listing_id", "stay_start", "stay_end", "guests",
        "basis", "qualifier", "currency", "scale", "taxes", "fees", "conflict_resolution" };
    for (size_t i = 0; i < mulib_json_size(context); ++i) {
        size_t n, k;
        const char *key = mulib_json_key(context, i, &n);
        if (!key || strlen(key) != n) return 0;
        for (k = 0; k < sizeof allowed / sizeof *allowed; ++k)
            if (!strcmp(key, allowed[k])) break;
        if (k == sizeof allowed / sizeof *allowed) return 0;
        const mulib_json_t *v = mulib_json_get(context, key);
        if (!strcmp(key, "guests") || !strcmp(key, "scale")) {
            if (mulib_json_typeof(v) != MULIB_JSON_NULL &&
                shy_integer(context, key, !strcmp(key, "scale") ? 6 : 64) < 0) return 0;
        } else if (mulib_json_typeof(v) != MULIB_JSON_NULL &&
            (mulib_json_typeof(v) != MULIB_JSON_STRING || !*shy_text(context, key) ||
             strlen(shy_text(context, key)) > 256)) return 0;
    }
    return 1;
}

static int context_load(const char *path)
{
    mulib_json_destroy(context);
    context = *path ? read_json(path, 16384) : mulib_json_new(MULIB_JSON_OBJECT);
    return context_valid();
}

int64_t shy_review_load(const char *path, const char *html, const char *context_path)
{
    mulib_json_destroy(capture);
    capture = read_json(path, SHY_PAGE_LIMIT);
    if (!capture || !strcmp(shy_text(capture,"schema"),"shylock.naive-find/v1") || !hash_valid(shy_text(capture, "blake3")) ||
        !*shy_text(capture, "source_url") ||
        shy_integer(capture, "captured_at", INT64_C(253402300799)) < 0 ||
        mulib_json_typeof(mulib_json_get(capture, "candidates")) != MULIB_JSON_ARRAY ||
        mulib_json_size(mulib_json_get(capture, "candidates")) > 256 || !context_load(context_path)) {
        shy_error("invalid extraction record or review context"); return 0;
    }
    verified = 0;
    if (*html) {
        free(snapshot);
        snapshot = shy_read(html, SHY_PAGE_LIMIT, &snapshot_length);
        if (!snapshot || !shy_verify_capture(capture, snapshot, snapshot_length)) {
            shy_error("capture verification failed: replay must match the original bytes and extraction"); return 0;
        }
        verified = 1;
    }
    return 1;
}

static mulib_json_t *candidate(int64_t index)
{
    return index < 0 ? NULL : mulib_json_at(mulib_json_get(capture, "candidates"), (size_t)index);
}

int64_t shy_review_naive(const char *html,const char *context_path)
{
    const mulib_json_t *record=shy_naive_verified();
    if (!db || !record) return 0;
    mulib_json_destroy(capture); capture=shy_clone(record);
    free(snapshot); snapshot=NULL; snapshot_length=0; verified=0;
    const char *bytes=shy_text(mulib_json_get(record,"snapshot"),"html");
    if (*html) {
        snapshot=shy_read(html,SHY_PAGE_LIMIT,&snapshot_length);
        if (!snapshot || snapshot_length!=strlen(bytes) || memcmp(snapshot,bytes,snapshot_length)) return 0;
    } else {
        snapshot_length=strlen(bytes); snapshot=malloc(snapshot_length+1);
        if (snapshot) memcpy(snapshot,bytes,snapshot_length+1);
    }
    if (!capture || !snapshot || !context_load(context_path)) return 0;
    verified=1; return 1;
}
static const mulib_json_t *review_evidence(void)
{
    const mulib_json_t *evidence=mulib_json_get(resolved,"evidence");
    return evidence ? evidence : resolved;
}
int64_t shy_review_is_naive(void)
{ return !strcmp(shy_text(review_evidence(),"extractor"),"naive/v1"); }
int64_t shy_review_conflict(void)
{ return mulib_json_boolean(mulib_json_get(review_evidence(),"conflict")); }
const char *shy_review_source_field(const char *key)
{ return shy_text(review_evidence(),key); }

static int merge_context(mulib_json_t *row)
{
    const mulib_json_t *evidence = mulib_json_get(row, "evidence");
    if (!evidence) evidence = row;
    const char *old_currency = shy_text(evidence, "currency");
    const char *new_currency = shy_text(context, "currency");
    if (*old_currency && *new_currency && strcmp(old_currency, new_currency)) return 0;
    const char *old_basis = shy_text(evidence, "basis"), *new_basis = shy_text(context, "basis");
    if (*old_basis && strcmp(old_basis, "unknown") && *new_basis && strcmp(old_basis, new_basis)) return 0;
    if (!strcmp(shy_text(evidence, "qualifier"), "from") &&
        *shy_text(context, "qualifier") && strcmp(shy_text(context, "qualifier"), "from")) return 0;
    for (size_t i = 0; i < mulib_json_size(context); ++i) {
        size_t n;
        const char *key = mulib_json_key(context, i, &n);
        if (!shy_add(row, key, shy_clone(mulib_json_get(context, key)))) return 0;
    }
    int scale = shy_currency_scale(shy_text(row, "currency"));
    if (mulib_json_get(context, "scale") && shy_integer(context, "scale", 6) != scale) return 0;
    if (!shy_add(row, "scale", scale < 0 ? mulib_json_new(MULIB_JSON_NULL) : mulib_json_number(scale))) return 0;
    /* Missing currencies may be resolved explicitly, never guessed. The
       original candidate and the review's supplied context remain immutable. */
    if (*new_currency) {
        mulib_money_t money;
        if (scale < 0 || !mulib_money_parse(shy_text(row, "amount_text"), new_currency,
            (uint8_t)scale, &money) || money.minor < 0 || money.minor > INT64_C(1000000000000) ||
            !shy_add(row, "minor", mulib_json_number((double)money.minor))) return 0;
    }
    return 1;
}

int64_t shy_review_prepare(int64_t index)
{
    mulib_json_destroy(resolved);
    resolved = shy_clone(candidate(index));
    if (resolved && !strcmp(shy_text(capture,"schema"),"shylock.naive-find/v1") &&
        !shy_add(resolved,"evidence",shy_clone(candidate(index)))) return 0;
    return resolved && merge_context(resolved);
}

int64_t shy_review_number(const char *key)
{
    return shy_integer(resolved, key, INT64_C(1000000000000));
}

const char *shy_review_field(const char *key) { return shy_text(resolved, key); }
int64_t shy_review_verified(void) { return verified; }

/* Shape adaptation only; On1x decides whether dates/guests/fees are admissible. */
int64_t shy_review_date(const char *key)
{
    return shy_date(shy_text(resolved, key));
}

static int put_immutable(const char *key, const void *value, size_t length)
{
    const void *old; size_t n;
    int rc = mulib_kvdb_get(db, key, strlen(key), &old, &n);
    if (rc == MULIB_KVDB_OK) return n == length && !memcmp(old, value, n) ? 0 : 3;
    if (rc != MULIB_KVDB_ENOTFOUND) return 2;
    return mulib_kvdb_set(db, key, strlen(key), value, length) == MULIB_KVDB_OK ? 0 : 2;
}

static mulib_json_t *get_json(const void *key, size_t length)
{
    const void *value; size_t n;
    return mulib_kvdb_get(db, key, length, &value, &n) == MULIB_KVDB_OK ?
        mulib_json_parse(value, n, NULL) : NULL;
}

static int review_valid(const mulib_json_t *row)
{
    char digest[65];
    int64_t revision = shy_integer(row, "revision", 1000000);
    const char *decision = shy_text(row, "decision"), *parent = shy_text(row, "parent_review_id");
    return !strcmp(shy_text(row, "schema"), "shylock.observation/v2") &&
        hash_valid(shy_text(row, "observation_id")) && hash_valid(shy_text(row, "review_id")) &&
        revision >= 1 && (revision == 1 ? !*parent : hash_valid(parent)) &&
        ((!strcmp(decision, "accepted") && !strcmp(shy_text(row, "status"), "reviewed")) ||
         (!strcmp(decision, "rejected") && !strcmp(shy_text(row, "status"), "rejected"))) &&
        shy_json_digest(row, "review_id", digest) && !strcmp(digest, shy_text(row, "review_id"));
}

/* 0: absent, 1: valid owned row, -1: malformed/I/O/allocation failure.
   A failed read must never be mistaken for permission to replace a head. */
static int load_head(const char *id, mulib_json_t **row)
{
    char key[80];
    snprintf(key, sizeof key, "head/%s", id);
    const void *value; size_t n;
    *row = NULL;
    int rc = mulib_kvdb_get(db, key, strlen(key), &value, &n);
    if (rc == MULIB_KVDB_ENOTFOUND) return 0;
    if (rc != MULIB_KVDB_OK) return -1;
    *row = get_json(value, n);
    if (!review_valid(*row) || strcmp(shy_text(*row, "observation_id"), id)) {
        mulib_json_destroy(*row); *row = NULL; return -1;
    }
    char expected[160];
    snprintf(expected, sizeof expected, "review/%s/%s", id, shy_text(*row, "review_id"));
    if (strlen(expected) != n || memcmp(value, expected, n)) {
        mulib_json_destroy(*row); *row = NULL; return -1;
    }
    return 1;
}

static mulib_json_t *cohort_context(const mulib_json_t *row)
{
    static const char *const fields[] = { "stay_start", "stay_end", "guests", "basis",
        "qualifier", "taxes", "fees", "currency", "scale" };
    mulib_json_t *out = mulib_json_new(MULIB_JSON_OBJECT);
    for (size_t i = 0; i < sizeof fields / sizeof *fields; ++i) {
        const mulib_json_t *v = mulib_json_get(row, fields[i]);
        if (!shy_add(out, fields[i], v ? shy_clone(v) : mulib_json_new(MULIB_JSON_NULL))) {
            mulib_json_destroy(out); return NULL;
        }
    }
    return out;
}

static int identify_context(mulib_json_t *row)
{
    mulib_json_t *ctx = cohort_context(row);
    char digest[65];
    int ok = ctx && shy_json_digest(ctx, NULL, digest) &&
        shy_add(row, "context_id", mulib_json_string(digest));
    mulib_json_destroy(ctx);
    return ok;
}

static int save_revision(mulib_json_t *row, const char *parent)
{
    char id[65], key[160], head_key[80];
    if (!shy_json_digest(row, "review_id", id) || !shy_add(row, "review_id", mulib_json_string(id))) return 2;
    const char *observation = shy_text(row, "observation_id");
    mulib_json_t *current;
    int found = load_head(observation, &current);
    if (found < 0) return 2;
    if (current) {
        int same = !strcmp(shy_text(current, "review_id"), id);
        int valid_parent = !strcmp(shy_text(current, "review_id"), parent);
        mulib_json_destroy(current);
        if (same) {
            (void)shy_extensions_enqueue("observation.reviewed", row);
            return 0; /* retry after a successful or uncertain commit */
        }
        if (!valid_parent) return 3;
    } else if (*parent) return 3;
    snprintf(key, sizeof key, "review/%s/%s", observation, id);
    snprintf(head_key, sizeof head_key, "head/%s", observation);
    char *encoded = mulib_json_stringify(row, 0);
    if (!encoded) return 2;
    int rc = put_immutable(key, encoded, strlen(encoded)); free(encoded);
    if (rc) return rc;
    if (mulib_kvdb_set(db, head_key, strlen(head_key), key, strlen(key)) != MULIB_KVDB_OK) return 2;
    if (mulib_kvdb_commit(db) != MULIB_KVDB_OK) return 2;
    (void)shy_extensions_enqueue("observation.reviewed", row);
    return 0;
}

int64_t shy_review_save(int64_t index, const char *cohort, const char *reviewer,
    const char *decision, const char *reason)
{
    if (!db || !resolved || !candidate(index) || !*cohort || strlen(cohort) > 128 ||
        !*reviewer || strlen(reviewer) > 128 || !*reason || strlen(reason) > 1024 ||
        (strcmp(decision, "accepted") && strcmp(decision, "rejected"))) return 2;
    int accepted = !strcmp(decision, "accepted");
    mulib_json_t *row = mulib_json_new(MULIB_JSON_OBJECT);
    char id[65], extraction_id[65];
    if (verified) snprintf(id, sizeof id, "%s", shy_text(candidate(index), "evidence_id"));
    else {
        char legacy[128];
        snprintf(legacy, sizeof legacy, "legacy/%s/%lld", shy_text(capture, "blake3"), (long long)index);
        shy_digest(legacy, strlen(legacy), id);
    }
    int ok = row && shy_json_digest(capture, NULL, extraction_id);
    static const char *const fields[] = { "minor", "currency", "scale", "basis", "qualifier",
        "listing_id", "stay_start", "stay_end", "guests", "taxes", "fees", "amount_text" };
    for (size_t i = 0; ok && i < sizeof fields / sizeof *fields; ++i) {
        const mulib_json_t *v = mulib_json_get(resolved, fields[i]);
        ok = shy_add(row, fields[i], v ? shy_clone(v) : mulib_json_new(MULIB_JSON_NULL));
    }
    ok = ok && shy_add(row, "schema", mulib_json_string("shylock.observation/v2")) &&
        shy_add(row, "observation_id", mulib_json_string(id)) &&
        shy_add(row, "cohort", mulib_json_string(cohort)) &&
        shy_add(row, "reviewer", mulib_json_string(reviewer)) &&
        shy_add(row, "reason", mulib_json_string(reason)) &&
        shy_add(row, "decision", mulib_json_string(decision)) &&
        shy_add(row, "status", mulib_json_string(accepted ? "reviewed" : "rejected")) &&
        shy_add(row, "revision", mulib_json_number(1)) &&
        shy_add(row, "parent_review_id", mulib_json_string("")) &&
        shy_add(row, "provenance", mulib_json_string(verified ? "verified-replay" : "legacy-unverified")) &&
        shy_add(row, "admission", mulib_json_string(!accepted ? "rejected/v1" : (verified ? "comparable/v1" : "legacy/v1"))) &&
        shy_add(row, "review_context", shy_clone(context)) &&
        shy_add(row, "evidence", shy_clone(candidate(index))) &&
        shy_add(row, "source_url", mulib_json_string(shy_text(capture, "source_url"))) &&
        shy_add(row, "captured_at", shy_clone(mulib_json_get(capture, "captured_at"))) &&
        shy_add(row, "capture_blake3", mulib_json_string(shy_text(capture, "blake3"))) &&
        shy_add(row, "capture_id", mulib_json_string(shy_text(capture, "capture_id"))) &&
        shy_add(row, "extraction_id", mulib_json_string(extraction_id)) &&
        shy_add(row, "candidate_index", mulib_json_number((double)index)) && identify_context(row);
    int rc = 2;
    if (ok && verified) {
        char key[96];
        snprintf(key, sizeof key, "snapshot/%s", shy_text(capture, "blake3"));
        rc = put_immutable(key, snapshot, snapshot_length);
        char *encoded = mulib_json_stringify(capture, 0);
        snprintf(key, sizeof key, "extraction/%s", extraction_id);
        if (!rc) rc = encoded ? put_immutable(key, encoded, strlen(encoded)) : 2;
        free(encoded);
        if (!rc) rc = save_revision(row, "");
    } else if (ok) rc = save_revision(row, "");
    mulib_json_destroy(row);
    if (rc == 3) shy_error("conflicting review; use revise with the current review ID");
    return rc;
}

static int64_t revision_load(const char *observation, const char *expected)
{
    if (!hash_valid(observation) || !hash_valid(expected)) return 0;
    char key[160];
    snprintf(key, sizeof key, "review/%s/%s", observation, expected);
    mulib_json_destroy(revision_base);
    revision_base = get_json(key, strlen(key));
    mulib_json_destroy(resolved);
    resolved = shy_clone(revision_base);
    if (!review_valid(resolved) || strcmp(shy_text(resolved, "observation_id"), observation) ||
        strcmp(shy_text(resolved, "review_id"), expected)) return 0;
    verified = !strcmp(shy_text(resolved, "provenance"), "verified-replay");
    return merge_context(resolved);
}

int64_t shy_revision_load(const char *observation, const char *expected, const char *context_path)
{
    return context_load(context_path) && revision_load(observation, expected);
}

int64_t shy_revision_load_data(const char *observation, const char *expected, const mulib_json_t *data)
{
    mulib_json_destroy(context);
    context = shy_clone(data);
    return context_valid() && revision_load(observation, expected);
}

int64_t shy_revision_save(const char *decision, const char *reviewer, const char *reason)
{
    if (!resolved || !revision_base || !*reviewer || strlen(reviewer) > 128 ||
        !*reason || strlen(reason) > 1024 || (strcmp(decision, "accepted") && strcmp(decision, "rejected"))) return 2;
    int64_t revision = shy_integer(revision_base, "revision", 1000000);
    if (revision < 1 || revision == 1000000 ||
        !shy_add(resolved, "revision", mulib_json_number((double)(revision + 1))) ||
        !shy_add(resolved, "parent_review_id", mulib_json_string(shy_text(revision_base, "review_id"))) ||
        !shy_add(resolved, "reviewer", mulib_json_string(reviewer)) ||
        !shy_add(resolved, "reason", mulib_json_string(reason)) ||
        !shy_add(resolved, "decision", mulib_json_string(decision)) ||
        !shy_add(resolved, "status", mulib_json_string(!strcmp(decision, "accepted") ? "reviewed" : "rejected")) ||
        !shy_add(resolved, "admission", mulib_json_string(!strcmp(decision, "rejected") ? "rejected/v1" :
            (verified ? "comparable/v1" : "legacy/v1"))) ||
        !shy_add(resolved, "review_context", shy_clone(context)) || !identify_context(resolved)) return 2;
    int rc = save_revision(resolved, shy_text(revision_base, "review_id"));
    if (rc == 3) shy_error("stale review ID; published reviews were not changed");
    return rc;
}

/* Existing v1 KVDB rows remain readable but never become verified by migration. */
static int adapt_legacy(mulib_json_t *row, const void *key, size_t length)
{
    char id[65], review_id[65];
    shy_digest(key, length, id);
    int scale = shy_currency_scale(shy_text(row, "currency"));
    return shy_json_digest(row, NULL, review_id) &&
        shy_add(row, "observation_id", mulib_json_string(id)) &&
        shy_add(row, "review_id", mulib_json_string(review_id)) &&
        shy_add(row, "decision", mulib_json_string("accepted")) &&
        shy_add(row, "status", mulib_json_string("reviewed")) &&
        shy_add(row, "provenance", mulib_json_string("legacy-unverified")) &&
        shy_add(row, "scale", scale < 0 ? mulib_json_new(MULIB_JSON_NULL) : mulib_json_number(scale));
}

int64_t shy_store_select(const char *cohort)
{
    mulib_json_destroy(selected);
    selected = mulib_json_new(MULIB_JSON_ARRAY);
    if (!selected || !db) return -1;
    for (size_t i = 0; i < mulib_kvdb_size(db); ++i) {
        const void *key, *value; size_t kn, vn;
        if (mulib_kvdb_at(db, i, &key, &kn, &value, &vn) != MULIB_KVDB_OK) return -1;
        mulib_json_t *row;
        if (kn > 5 && !memcmp(key, "head/", 5)) {
            if (kn != 69) return -1;
            char id[65]; memcpy(id, (const char *)key + 5, 64); id[64] = 0;
            if (!hash_valid(id) || load_head(id, &row) != 1) return -1;
        }
        else if (kn >= 66 && ((const char *)key)[64] == '/') {
            /* v1 keys were lowercase capture hashes followed by /INDEX. */
            const char *s = key;
            int legacy = 1;
            for (size_t j = 0; j < 64; ++j)
                if (!s[j] || !strchr("0123456789abcdef", s[j])) legacy = 0;
            if (!legacy) continue;
            row = mulib_json_parse(value, vn, NULL);
            if (row && !adapt_legacy(row, key, kn)) { mulib_json_destroy(row); return -1; }
        } else continue;
        if (!row) return -1;
        if (*cohort && strcmp(shy_text(row, "cohort"), cohort)) { mulib_json_destroy(row); continue; }
        if (mulib_json_size(selected) >= MAX_ROWS) { mulib_json_destroy(row); return -1; }
        if (!shy_push(selected, row)) return -1;
    }
    return (int64_t)mulib_json_size(selected);
}

static mulib_json_t *selected_row(int64_t index)
{
    return index < 0 ? NULL : mulib_json_at(selected, (size_t)index);
}
mulib_json_t *shy_store_rows(void) { return shy_clone(selected); }
const char *shy_stored_field(int64_t index, const char *key) { return shy_text(selected_row(index), key); }
int64_t shy_stored_number(int64_t index, const char *key)
{
    return shy_integer(selected_row(index), key, INT64_C(1000000000000));
}

int64_t shy_store_list(const char *cohort)
{
    if (shy_store_select(cohort) < 0) return shy_error("cannot read ledger");
    return shy_emit(shy_clone(selected));
}

int64_t shy_store_history(const char *id)
{
    if (!hash_valid(id)) return 2;
    mulib_json_t *row, *reverse = mulib_json_new(MULIB_JSON_ARRAY);
    if (load_head(id, &row) != 1 || !reverse) { mulib_json_destroy(row); mulib_json_destroy(reverse); return 2; }
    int64_t last = 1000001;
    while (row) {
        int64_t revision = shy_integer(row, "revision", 1000000);
        const char *parent = shy_text(row, "parent_review_id");
        char key[160];
        if (!review_valid(row) || strcmp(shy_text(row, "observation_id"), id) ||
            revision < 1 || revision >= last || (last != 1000001 && revision != last-1) ||
            (revision > 1 && !hash_valid(parent))) goto fail;
        last = revision;
        snprintf(key, sizeof key, "review/%s/%s", id, parent);
        int more = *parent != 0;
        if (mulib_json_size(reverse) == MAX_ROWS || !mulib_json_append(reverse, row)) goto fail;
        row = NULL;
        if (more && !(row = get_json(key, strlen(key)))) goto fail;
    }
    mulib_json_t *out = mulib_json_new(MULIB_JSON_ARRAY);
    for (size_t i = mulib_json_size(reverse); i; --i) {
        if (!shy_push(out, shy_clone(mulib_json_at(reverse, i - 1)))) {
            mulib_json_destroy(out); goto fail;
        }
    }
    mulib_json_destroy(reverse);
    return shy_emit(out);
fail:
    mulib_json_destroy(row); mulib_json_destroy(reverse); return 2;
}

int64_t shy_store_replay(const char *id)
{
    if (!hash_valid(id)) return 2;
    char key[96], digest[65];
    snprintf(key, sizeof key, "extraction/%s", id);
    mulib_json_t *record = get_json(key, strlen(key));
    const void *bytes; size_t n;
    if (!record || !shy_json_digest(record, NULL, digest) || strcmp(id, digest)) goto fail;
    snprintf(key, sizeof key, "snapshot/%s", shy_text(record, "blake3"));
    if (mulib_kvdb_get(db, key, strlen(key), &bytes, &n) != MULIB_KVDB_OK ||
        !shy_verify_capture(record, bytes, n)) goto fail;
    return shy_emit(record);
fail:
    mulib_json_destroy(record); return shy_error("cannot replay a verified stored extraction");
}

int64_t shy_store_naive_begin(const char *id)
{
    if (!hash_valid(id)) return -1;
    char key[96],digest[65];
    snprintf(key,sizeof key,"extraction/%s",id);
    mulib_json_t *record=get_json(key,strlen(key));
    if (!record || !shy_json_digest(record,NULL,digest) || strcmp(id,digest)) { mulib_json_destroy(record); return -1; }
    if (strcmp(shy_text(record,"schema"),"shylock.naive-find/v1")) { mulib_json_destroy(record); return 0; }
    const void *bytes; size_t n;
    snprintf(key,sizeof key,"snapshot/%s",shy_text(record,"blake3"));
    const char *expected=shy_text(mulib_json_get(record,"snapshot"),"html");
    int ok=mulib_kvdb_get(db,key,strlen(key),&bytes,&n)==MULIB_KVDB_OK && n==strlen(expected) &&
        !memcmp(bytes,expected,n) && shy_naive_begin_record(record);
    mulib_json_destroy(record); return ok ? 1 : -1;
}

int64_t shy_store_incompatible(int64_t index, const char *field, int64_t reference, const char *expected)
{
    const mulib_json_t *row = selected_row(index), *other = selected_row(reference);
    const mulib_json_t *value = mulib_json_get(row, field);
    mulib_json_t *out = mulib_json_new(MULIB_JSON_OBJECT);
    mulib_json_t *want = other ? shy_clone(mulib_json_get(other, field)) :
        (!strcmp(field, "scale") ? mulib_json_number(shy_currency_scale(expected)) : mulib_json_string(expected));
    if (!shy_add(out, "expected", want) ||
        !shy_add(out, "status", mulib_json_string("incompatible_cohort")) ||
        !shy_add(out, "field", mulib_json_string(field)) ||
        !shy_add(out, "observation_id", mulib_json_string(shy_text(row, "observation_id"))) ||
        !shy_add(out, "actual", value ? shy_clone(value) : mulib_json_new(MULIB_JSON_NULL))) {
        mulib_json_destroy(out); return 2;
    }
    return shy_emit(out) ? 2 : 3;
}

typedef struct {
    char group[65];
    size_t row, bucket;
    int64_t minor;
} sample;

static int sample_order(const void *a, const void *b)
{
    const sample *x = a, *y = b;
    if (x->bucket != y->bucket) return x->bucket < y->bucket ? -1 : 1;
    int order = strcmp(x->group, y->group);
    if (order) return order;
    return x->row < y->row ? -1 : x->row > y->row;
}

static int64_t conflict(const sample *samples, size_t begin, size_t end)
{
    mulib_json_t *out = mulib_json_new(MULIB_JSON_OBJECT), *claims = mulib_json_new(MULIB_JSON_ARRAY);
    for (size_t i = begin; i < end; ++i) {
        mulib_json_t *row = selected_row((int64_t)samples[i].row);
        mulib_json_t *claim = mulib_json_new(MULIB_JSON_OBJECT);
        int ok = shy_add(claim, "observation_id", mulib_json_string(shy_text(row, "observation_id"))) &&
            shy_add(claim, "minor", mulib_json_number((double)samples[i].minor));
        if (!ok) mulib_json_destroy(claim);
        if (!ok || !shy_push(claims, claim)) { mulib_json_destroy(claims); mulib_json_destroy(out); return 2; }
    }
    int ok = shy_add(out, "status", mulib_json_string("review_conflict")) &&
        shy_add(out, "field", mulib_json_string("minor")) &&
        shy_add(out, "bucket", mulib_json_number((double)samples[begin].bucket));
    if (!ok) { mulib_json_destroy(out); mulib_json_destroy(claims); return 2; }
    if (!shy_add(out, "claims", claims)) { mulib_json_destroy(out); return 2; }
    return shy_emit(out) ? 2 : 3;
}

int64_t shy_store_series(const char *cohort, const char *currency, int64_t start,
    int64_t step, int64_t count, int64_t rich)
{
    if (!db || !selected || !*cohort || shy_currency_scale(currency) < 0 || start < 0 ||
        start > INT64_C(253402300799) || step < 1 || count < 2 || count > 4096 ||
        step > (INT64_C(253402300800)-start)/count) return 2;
    size_t total = mulib_json_size(selected), used = 0;
    sample *samples = total ? calloc(total, sizeof *samples) : NULL;
    if (total && !samples) return 2;
    for (size_t i = 0; i < total; ++i) {
        mulib_json_t *row = selected_row((int64_t)i);
        if (strcmp(shy_text(row, "decision"), "accepted")) continue;
        int64_t time = shy_integer(row, "captured_at", INT64_C(253402300799));
        if (time < start || time >= start + step * count) continue;
        sample *s = &samples[used++];
        s->row = i; s->bucket = (size_t)((time-start)/step);
        s->minor = shy_integer(row, "minor", INT64_C(1000000000000));
        if (s->minor < 0) { free(samples); return 2; }
        mulib_json_t *identity = mulib_json_new(MULIB_JSON_ARRAY);
        const char *listing = shy_text(row, "listing_id");
        /* Unknown identities must NOT be guessed from URL or correlated. */
        int ok = shy_push(identity, mulib_json_string(*listing ? listing : shy_text(row, "observation_id"))) &&
            shy_push(identity, mulib_json_string(shy_text(row, "context_id"))) &&
            shy_json_digest(identity, NULL, s->group);
        mulib_json_destroy(identity);
        if (!ok) { free(samples); return 2; }
    }
    if (used) qsort(samples, used, sizeof *samples, sample_order);
    int64_t sums[4096] = { 0 };
    size_t totals[4096] = { 0 };
    for (size_t i = 0; i < used;) {
        size_t j = i + 1;
        int disagree = 0;
        while (j < used && samples[j].bucket == samples[i].bucket && !strcmp(samples[j].group, samples[i].group)) {
            if (samples[j].minor != samples[i].minor) disagree = 1;
            ++j;
        }
        if (disagree) { int64_t rc = conflict(samples, i, j); free(samples); return rc; }
        sums[samples[i].bucket] += samples[i].minor;
        ++totals[samples[i].bucket];
        i = j;
    }
    mulib_json_t *values = mulib_json_new(MULIB_JSON_ARRAY), *buckets = mulib_json_new(MULIB_JSON_ARRAY);
    mulib_json_t *out = NULL;
    for (int64_t i = 0; i < count; ++i) {
        if (!totals[i]) { free(samples); mulib_json_destroy(values); mulib_json_destroy(buckets);
            shy_error("missing cohort bucket; no interpolation was performed"); return 4; }
        int64_t mean = (sums[i] + (int64_t)totals[i]/2) / (int64_t)totals[i];
        if (!shy_push(values, mulib_json_number((double)mean))) goto fail;
        if (!rich) continue;
        mulib_json_t *bucket = mulib_json_new(MULIB_JSON_OBJECT), *refs = mulib_json_new(MULIB_JSON_ARRAY);
        int ok = shy_add(bucket, "start", mulib_json_number((double)(start + i*step))) &&
            shy_add(bucket, "distinct_listings", mulib_json_number((double)totals[i]));
        for (size_t j = 0; ok && j < used; ++j) {
            if (samples[j].bucket != (size_t)i) continue;
            const mulib_json_t *row = selected_row((int64_t)samples[j].row);
            mulib_json_t *ref = mulib_json_new(MULIB_JSON_OBJECT);
            static const char *const keys[] = { "observation_id", "review_id", "capture_id", "extraction_id", "capture_blake3" };
            for (size_t k = 0; ok && k < sizeof keys / sizeof *keys; ++k)
                ok = shy_add(ref, keys[k], mulib_json_string(shy_text(row, keys[k])));
            ok = ok && shy_add(ref, "listing_id", mulib_json_string(shy_text(row, "listing_id"))) &&
                shy_add(ref, "captured_at", shy_clone(mulib_json_get(row, "captured_at"))) &&
                shy_add(ref, "minor", mulib_json_number((double)samples[j].minor));
            if (!ok) mulib_json_destroy(ref);
            else ok = shy_push(refs, ref);
        }
        if (!ok) { mulib_json_destroy(bucket); mulib_json_destroy(refs); goto fail; }
        if (!shy_add(bucket, "evidence", refs)) { mulib_json_destroy(bucket); goto fail; }
        if (!shy_push(buckets, bucket)) goto fail;
    }
    free(samples); samples = NULL;
    if (!rich) { mulib_json_destroy(buckets); return shy_emit(values); }
    out = mulib_json_new(MULIB_JSON_OBJECT);
    if (!shy_add(out, "schema", mulib_json_string("shylock.cohort/v1")) ||
        !shy_add(out, "status", mulib_json_string("reviewed")) ||
        !shy_add(out, "cohort", mulib_json_string(cohort)) ||
        !shy_add(out, "currency", mulib_json_string(currency)) ||
        !shy_add(out, "scale", mulib_json_number(shy_currency_scale(currency))) ||
        !shy_add(out, "basis", mulib_json_string("nightly")) ||
        !shy_add(out, "start", mulib_json_number((double)start)) ||
        !shy_add(out, "step", mulib_json_number((double)step)) ||
        !shy_add(out, "count", mulib_json_number((double)count)) ||
        !shy_add(out, "aggregation", mulib_json_string("distinct-listing-mean/v1")) ||
        !shy_add(out, "missing", mulib_json_string("reject/v1"))) goto fail;
    /* Find an in-window accepted row, not an unrelated/rejected ledger entry. */
    for (size_t i = 0; i < total; ++i) {
        const mulib_json_t *row = selected_row((int64_t)i);
        int64_t time = shy_integer(row, "captured_at", INT64_C(253402300799));
        if (!strcmp(shy_text(row, "decision"), "accepted") && time >= start && time < start+step*count) {
            if (!shy_add(out, "context", cohort_context(row))) goto fail;
            break;
        }
    }
    if (!mulib_json_set(out, "values", values)) goto fail;
    values = NULL;
    if (!mulib_json_set(out, "buckets", buckets)) goto fail;
    buckets = NULL;
    char digest[65];
    if (!shy_json_digest(out, NULL, digest) || !shy_add(out, "cohort_id", mulib_json_string(digest))) goto fail;
    return shy_emit(out);
fail:
    free(samples); mulib_json_destroy(out); mulib_json_destroy(values); mulib_json_destroy(buckets); return 2;
}
