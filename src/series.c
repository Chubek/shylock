#include "shylock.h"
#include "mulib-histprice.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

mulib_json_t *shy_estimator(void)
{
    mulib_json_t *out = mulib_json_new(MULIB_JSON_OBJECT);
    if (!shy_add(out, "name", mulib_json_string("damped-holt/v1")) ||
        !shy_add(out, "alpha", mulib_json_number(0.4)) ||
        !shy_add(out, "beta", mulib_json_number(0.2)) ||
        !shy_add(out, "phi", mulib_json_number(0.9)) ||
        !shy_add(out, "mulib_revision", mulib_json_string(SHY_MULIB_REVISION))) {
        mulib_json_destroy(out); return NULL;
    }
    return out;
}

static int hash_valid(const char *s)
{
    return strlen(s) == 64 && strspn(s, "0123456789abcdef") == 64;
}

typedef struct {
    const char *listing;
    int64_t minor;
} cohort_ref;

static int ref_order(const void *a, const void *b)
{
    const cohort_ref *x = a, *y = b;
    return strcmp(x->listing, y->listing);
}

static int bucket_valid(const mulib_json_t *bucket, const mulib_json_t *sample,
    int64_t start, int64_t step)
{
    const mulib_json_t *refs = mulib_json_get(bucket, "evidence");
    size_t count = mulib_json_size(refs);
    if (shy_integer(bucket, "start", INT64_C(253402300799)) != start ||
        mulib_json_typeof(refs) != MULIB_JSON_ARRAY || !count || count > 16384 ||
        mulib_json_typeof(sample) != MULIB_JSON_NUMBER) return 0;
    cohort_ref *claims = calloc(count, sizeof *claims);
    if (!claims) return 0;
    int ok = 1;
    for (size_t i = 0; ok && i < count; ++i) {
        const mulib_json_t *ref = mulib_json_at(refs, i);
        static const char *const ids[] = { "observation_id", "review_id", "capture_id", "extraction_id", "capture_blake3" };
        for (size_t k = 0; k < sizeof ids / sizeof *ids; ++k)
            if (!hash_valid(shy_text(ref, ids[k]))) ok = 0;
        claims[i].listing = shy_text(ref, "listing_id");
        claims[i].minor = shy_integer(ref, "minor", INT64_C(1000000000000));
        int64_t time = shy_integer(ref, "captured_at", INT64_C(253402300799));
        if (!*claims[i].listing || strlen(claims[i].listing) > 256 || claims[i].minor < 0 ||
            time < start || time >= start+step) ok = 0;
    }
    if (ok) {
        qsort(claims, count, sizeof *claims, ref_order);
        int64_t sum = 0, distinct = 0;
        for (size_t i = 0; ok && i < count; ++i) {
            if (i && !strcmp(claims[i-1].listing, claims[i].listing)) {
                if (claims[i-1].minor != claims[i].minor) ok = 0;
            } else { sum += claims[i].minor; ++distinct; }
        }
        ok = ok && shy_integer(bucket, "distinct_listings", 16384) == distinct &&
            mulib_json_numeric(sample) == (double)((sum + distinct/2)/distinct);
    }
    free(claims);
    return ok;
}

int shy_series_load(const char *path, double values[4096], size_t *count,
    char digest[65], mulib_json_t **lineage)
{
    *lineage = NULL;
    size_t length;
    char *data = shy_read(path, SHY_DATA_LIMIT, &length);
    if (!data) return 0;
    shy_digest(data, length, digest);
    mulib_json_t *input = mulib_json_parse(data, length, NULL);
    free(data);
    const mulib_json_t *items = input;
    int rich = mulib_json_typeof(input) == MULIB_JSON_OBJECT;
    if (rich) {
        char id[65];
        const char *currency = shy_text(input, "currency");
        int64_t start = shy_integer(input, "start", INT64_C(253402300799));
        int64_t step = shy_integer(input, "step", INT64_C(253402300800));
        int64_t n = shy_integer(input, "count", 4096);
        if (strcmp(shy_text(input, "schema"), "shylock.cohort/v1") ||
            strcmp(shy_text(input, "status"), "reviewed") ||
            strcmp(shy_text(input, "basis"), "nightly") ||
            strcmp(shy_text(input, "aggregation"), "distinct-listing-mean/v1") ||
            strcmp(shy_text(input, "missing"), "reject/v1") || !*shy_text(input, "cohort") ||
            shy_currency_scale(currency) < 0 || shy_integer(input, "scale", 6) != shy_currency_scale(currency) ||
            start < 0 || step < 1 || n < 2 || step > (INT64_C(253402300800)-start)/n ||
            !shy_json_digest(input, "cohort_id", id) || strcmp(shy_text(input, "cohort_id"), id)) goto fail;
        items = mulib_json_get(input, "values");
        const mulib_json_t *buckets = mulib_json_get(input, "buckets");
        const mulib_json_t *context = mulib_json_get(input, "context");
        const char *taxes = shy_text(context, "taxes"), *fees = shy_text(context, "fees");
        int64_t stay = shy_date(shy_text(context, "stay_start"));
        if (mulib_json_typeof(context) != MULIB_JSON_OBJECT || stay < 0 ||
            shy_date(shy_text(context, "stay_end")) <= stay || shy_integer(context, "guests", 64) < 1 ||
            strcmp(shy_text(context, "basis"), "nightly") || strcmp(shy_text(context, "qualifier"), "unspecified") ||
            strcmp(shy_text(context, "currency"), currency) || shy_integer(context, "scale", 6) != shy_currency_scale(currency) ||
            (strcmp(taxes, "included") && strcmp(taxes, "excluded")) ||
            (strcmp(fees, "included") && strcmp(fees, "excluded"))) goto fail;
        if (mulib_json_size(items) != (size_t)n ||
            mulib_json_typeof(buckets) != MULIB_JSON_ARRAY || mulib_json_size(buckets) != (size_t)n) goto fail;
        for (int64_t i = 0; i < n; ++i) {
            const mulib_json_t *bucket = mulib_json_at(buckets, (size_t)i);
            if (!bucket_valid(bucket, mulib_json_at(items, (size_t)i), start+i*step, step)) goto fail;
        }
    }
    *count = mulib_json_size(items);
    if (mulib_json_typeof(items) != MULIB_JSON_ARRAY || *count < 2 || *count > 4096) goto fail;
    for (size_t i = 0; i < *count; ++i) {
        const mulib_json_t *value = mulib_json_at(items, i);
        double n = mulib_json_numeric(value);
        if (mulib_json_typeof(value) != MULIB_JSON_NUMBER || !isfinite(n) || n < 0 || n > 1e12 || floor(n) != n) goto fail;
        values[i] = n;
    }
    if (rich) *lineage = input;
    else mulib_json_destroy(input);
    return 1;
fail:
    mulib_json_destroy(input);
    return 0;
}

int64_t shy_backtest(const char *path, int64_t horizon, int64_t training)
{
    if (horizon < 1 || horizon > 365 || training < 2 || training > 4096)
        return shy_error("backtest needs horizon 1..365 and at least two training samples");
    double values[4096]; size_t count;
    char digest[65]; mulib_json_t *lineage = NULL;
    if (!shy_series_load(path, values, &count, digest, &lineage) || (size_t)(training+horizon) > count) {
        mulib_json_destroy(lineage);
        return shy_error("not enough valid, equally spaced samples for a chronological backtest");
    }
    mulib_json_t *out = mulib_json_new(MULIB_JSON_OBJECT), *folds = mulib_json_new(MULIB_JSON_ARRAY);
    size_t evaluated = 0, attempted = 0;
    long double absolute = 0, squared = 0, bias = 0;
    for (size_t n = (size_t)training; n + (size_t)horizon <= count; ++n) {
        mulib_histprice_forecast_t forecast;
        size_t target = n + (size_t)horizon - 1;
        int ok = mulib_histprice_holt(values, n, 0.4, 0.2, 0.9, (size_t)horizon, &forecast) &&
            isfinite(forecast.point) && forecast.point >= 0 && forecast.point <= 1e12;
        mulib_json_t *fold = mulib_json_new(MULIB_JSON_OBJECT);
        if (!shy_add(fold, "training_samples", mulib_json_number((double)n)) ||
            !shy_add(fold, "target_index", mulib_json_number((double)target)) ||
            !shy_add(fold, "actual_minor", mulib_json_number(values[target])) ||
            !shy_add(fold, "status", mulib_json_string(ok ? "evaluated" : "forecast_error")) ||
            !shy_add(fold, "forecast_minor", ok ? mulib_json_number(forecast.point) : mulib_json_new(MULIB_JSON_NULL))) {
            mulib_json_destroy(fold); goto fail;
        }
        if (lineage) {
            int64_t start = shy_integer(lineage, "start", INT64_C(253402300799));
            int64_t step = shy_integer(lineage, "step", INT64_C(253402300800));
            if (!shy_add(fold, "cutoff", mulib_json_number((double)(start + (int64_t)(n-1)*step))) ||
                !shy_add(fold, "target_at", mulib_json_number((double)(start + (int64_t)target*step)))) {
                mulib_json_destroy(fold); goto fail;
            }
        }
        if (!shy_push(folds, fold)) goto fail;
        ++attempted;
        if (ok) {
            long double error = forecast.point - values[target];
            absolute += fabsl(error); squared += error*error; bias += error; ++evaluated;
        }
    }
    if (!shy_add(out, "schema", mulib_json_string("shylock.backtest/v1")) ||
        !shy_add(out, "kind", mulib_json_string("chronological_backtest")) ||
        !shy_add(out, "estimator", shy_estimator()) ||
        !shy_add(out, "series_blake3", mulib_json_string(digest)) ||
        !shy_add(out, "horizon", mulib_json_number((double)horizon)) ||
        !shy_add(out, "minimum_training_samples", mulib_json_number((double)training)) ||
        !shy_add(out, "samples", mulib_json_number((double)count)) ||
        !shy_add(out, "attempted", mulib_json_number((double)attempted)) ||
        !shy_add(out, "evaluated", mulib_json_number((double)evaluated)) ||
        !shy_add(out, "coverage", mulib_json_number((double)evaluated/(double)attempted)) ||
        !shy_add(out, "mae_minor", evaluated ? mulib_json_number((double)(absolute/evaluated)) : mulib_json_new(MULIB_JSON_NULL)) ||
        !shy_add(out, "rmse_minor", evaluated ? mulib_json_number(sqrt((double)(squared/evaluated))) : mulib_json_new(MULIB_JSON_NULL)) ||
        !shy_add(out, "bias_minor", evaluated ? mulib_json_number((double)(bias/evaluated)) : mulib_json_new(MULIB_JSON_NULL))) goto fail;
    if (!mulib_json_set(out, "folds", folds)) goto fail;
    folds = NULL;
    if (lineage && !mulib_json_set(out, "cohort_lineage", lineage)) goto fail;
    lineage = NULL;
    int64_t rc = shy_emit(out);
    return rc ? rc : (evaluated ? 0 : 2);
fail:
    mulib_json_destroy(out); mulib_json_destroy(folds); mulib_json_destroy(lineage); return 2;
}
