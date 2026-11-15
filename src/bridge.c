#include "shylock.h"
#include "mulib-cryptohash.h"
#include "mulib-histprice.h"
#include "mulib-mune.h"
#include "mulib-priceopt.h"
#include "mulib-statsprice.h"
#include "mulib-uri.h"
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
    data[n] = 0;
    *length = n;
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

int64_t shy_emit(mulib_json_t *record)
{
    char *text = record ? mulib_json_stringify(record, 0) : NULL;
    mulib_json_destroy(record);
    if (!text) return shy_error("could not serialize result");
    int ok = puts(text) >= 0 && fflush(stdout) == 0;
    free(text);
    return ok ? 0 : 2;
}

int64_t shy_extract(const char *path, const char *url, int64_t timestamp, const char *rules,
                    const char *model_reply)
{
    mulib_uri_t uri;
    if (timestamp < 0 || timestamp > INT64_C(253402300799) || strlen(url) > 4096 ||
        (strncmp(url, "https://", 8) && strncmp(url, "http://", 7)) ||
        !mulib_uri_parse(url, strlen(url), &uri) || !uri.has_authority ||
        uri.has_userinfo || !uri.host.length)
        return shy_error("expected an HTTP(S) source URL and valid capture time");
    size_t length;
    char *data = shy_read(path, SHY_PAGE_LIMIT, &length);
    if (!data) return shy_error("cannot read bounded HTML capture");
    const shy_plugin *plugin = &shy_dragnet;
    mulib_json_t *record = mulib_json_new(MULIB_JSON_OBJECT);
    mulib_json_t *items = mulib_json_new(MULIB_JSON_ARRAY);
    char digest[65];
    shy_digest(data, length, digest);
    shy_capture capture = { data, length, url, rules, model_reply, digest };
    int ok = plugin->abi == 1 && record && items && plugin->extract(&capture, items);
    free(data);
    if (!ok || !mulib_json_set(record, "candidates", items)) {
        mulib_json_destroy(items);
        mulib_json_destroy(record);
        return shy_error("extractor failed; check site rules and snapshot limits");
    }
    if (!shy_add(record, "plugin", mulib_json_string(plugin->name)) ||
        !shy_add(record, "source_url", mulib_json_string(url)) ||
        !shy_add(record, "captured_at", mulib_json_number((double)timestamp)) ||
        !shy_add(record, "blake3", mulib_json_string(digest))) {
        mulib_json_destroy(record);
        return shy_error("could not allocate capture record");
    }
    return shy_emit(record);
}

/* Command processes carry one forecast into a subsequent decision. */
static struct {
    char digest[65];
    int64_t horizon;
    double point;
} forecast_context;

static int64_t estimate_error(const char *message, int64_t emit)
{
    shy_error(message);
    return emit ? 2 : -1;
}

int64_t shy_estimate(const char *path, int64_t horizon, int64_t emit)
{
    if (horizon < 1 || horizon > 365) return estimate_error("horizon must be in 1..365", emit);
    size_t length;
    char *data = shy_read(path, SHY_PAGE_LIMIT, &length);
    if (!data) return estimate_error("cannot read series", emit);
    shy_digest(data, length, forecast_context.digest);
    mulib_json_t *input = mulib_json_parse(data, length, NULL);
    free(data);
    size_t count = mulib_json_size(input);
    double values[4096];
    int ok = mulib_json_typeof(input) == MULIB_JSON_ARRAY && count >= 2 && count <= 4096;
    for (size_t i = 0; ok && i < count; ++i) {
        const mulib_json_t *item = mulib_json_at(input, i);
        values[i] = mulib_json_numeric(item);
        ok = mulib_json_typeof(item) == MULIB_JSON_NUMBER && values[i] >= 0 &&
             values[i] <= 1e12 && trunc(values[i]) == values[i];
    }
    mulib_json_destroy(input);
    mulib_statsprice_summary_t market;
    mulib_histprice_forecast_t forecast;
    if (!ok || !mulib_statsprice_summarize(values, count, &market) ||
        !mulib_histprice_holt(values, count, 0.4, 0.2, 0.9, (size_t)horizon, &forecast) ||
        !isfinite(forecast.point) || forecast.point < 0 || forecast.point > 1e12)
        return estimate_error("expected an equally spaced cohort of 2..4096 integer minor-unit samples", emit);
    forecast_context.horizon = horizon;
    forecast_context.point = forecast.point;
    if (!emit) return (int64_t)llround(forecast.point);
    mulib_json_t *out = mulib_json_new(MULIB_JSON_OBJECT);
    if (!shy_add(out, "reference_minor", mulib_json_number(market.median)) ||
        !shy_add(out, "forecast_minor", mulib_json_number(forecast.point)) ||
        !shy_add(out, "samples", mulib_json_number((double)count)) ||
        !shy_add(out, "horizon", mulib_json_number((double)horizon)) ||
        !shy_add(out, "series_blake3", mulib_json_string(forecast_context.digest))) {
        mulib_json_destroy(out);
        return shy_error("could not allocate estimate");
    }
    return shy_emit(out);
}

/* Separate formula state. Clear the actual globals table using raw operations
   before running any user code. Re-start iteration after each removal. */
static int restrict_formula(mulib_mune_state_t *state)
{
    mulib_mune_value_t globals, key, value;
    if (mulib_mune_get_global(state, "_G", &globals) != MULIB_MUNE_OK ||
        globals.type != MULIB_MUNE_TABLE) return 0;
    /* Keep a host root after removing _G. The VM also roots its globals. */
    for (;;) {
        size_t cursor = 0;
        int found = mulib_mune_table_next(state, globals, &cursor, &key, &value);
        if (found <= 0) return found == 0;
        if (mulib_mune_raw_set(state, globals, key, mulib_mune_nil()) != MULIB_MUNE_OK)
            return 0;
    }
}

static char formula_digest[65];

int64_t shy_formula(const char *path, int64_t reference, int64_t weekend, int64_t premium)
{
    size_t length;
    formula_digest[0] = 0;
    char *source = shy_read(path, 16384, &length);
    if (!source) { shy_error("cannot read formula"); return -1; }
    shy_digest(source, length, formula_digest);
    mulib_mune_error_t error = { 0 };
    const mulib_mune_parse_options_t parse = { 16384, 4096, 4096, 8192, 100000 };
    const mulib_mune_runtime_options_t runtime = { 1024 * 1024, 50000, 32, 32 };
    mulib_mune_chunk_t *chunk = mulib_mune_parse(source, length, &parse, &error);
    free(source);
    mulib_mune_state_t *state = chunk ? mulib_mune_state_create(&runtime) : NULL;
    mulib_mune_result_t *loaded = NULL, *result = NULL;
    int64_t rate = -1;
    if (state && restrict_formula(state) &&
        mulib_mune_run(state, chunk, &loaded, &error) == MULIB_MUNE_OK &&
        mulib_mune_result_count(loaded) == 1) {
        mulib_mune_value_t fn = mulib_mune_result_get(loaded, 0);
        mulib_mune_value_t args[] = {
            mulib_mune_integer(reference), mulib_mune_boolean(weekend != 0),
            mulib_mune_integer(premium)
        };
        if (fn.type == MULIB_MUNE_FUNCTION &&
            mulib_mune_call(state, fn, args, 3, &result, &error) == MULIB_MUNE_OK &&
            mulib_mune_result_count(result) == 1) {
            mulib_mune_value_t value = mulib_mune_result_get(result, 0);
            if (value.type == MULIB_MUNE_INTEGER && value.as.integer >= 0 &&
                value.as.integer <= INT64_C(1000000000000)) rate = value.as.integer;
        }
    }
    if (rate < 0) shy_error(error.message ? error.message : "formula must return one bounded integer");
    if (result) mulib_mune_result_destroy(result);
    if (loaded) mulib_mune_result_destroy(loaded);
    if (state) mulib_mune_state_destroy(state);
    if (chunk) mulib_mune_chunk_destroy(chunk);
    return rate;
}

int64_t shy_report_rate(int64_t reference, int64_t proposed, int64_t adjusted,
                        int64_t weekend, int64_t premium, int64_t floor, int64_t cap,
                        int64_t tick)
{
    mulib_json_t *out = mulib_json_new(MULIB_JSON_OBJECT);
    int ok = adjusted >= 0;
    if (!shy_add(out, "contract", mulib_json_string("adjust_rate/v1")) ||
        !shy_add(out, "mune_api", mulib_json_number(mulib_mune_version())) ||
        !shy_add(out, "formula_blake3", mulib_json_string(formula_digest)) ||
        !shy_add(out, "status", mulib_json_string(ok ? "adjusted" : "formula_error")) ||
        !shy_add(out, "reference_minor", mulib_json_number((double)reference)) ||
        !shy_add(out, "weekend", mulib_json_bool(weekend != 0)) ||
        !shy_add(out, "premium_bp", mulib_json_number((double)premium)) ||
        !shy_add(out, "floor_minor", mulib_json_number((double)floor)) ||
        !shy_add(out, "cap_minor", mulib_json_number((double)cap)) ||
        !shy_add(out, "tick_minor", mulib_json_number((double)tick)) ||
        !shy_add(out, "proposed_minor", ok ? mulib_json_number((double)proposed) : mulib_json_new(MULIB_JSON_NULL)) ||
        !shy_add(out, "adjusted_minor", ok ? mulib_json_number((double)adjusted) : mulib_json_new(MULIB_JSON_NULL))) {
        mulib_json_destroy(out);
        return shy_error("could not allocate decision");
    }
    if (forecast_context.horizon &&
        (!shy_add(out, "forecast_minor", mulib_json_number(forecast_context.point)) ||
         !shy_add(out, "horizon", mulib_json_number((double)forecast_context.horizon)) ||
         !shy_add(out, "series_blake3", mulib_json_string(forecast_context.digest)))) {
        mulib_json_destroy(out);
        return shy_error("could not attach forecast");
    }
    int64_t status = shy_emit(out);
    return status ? status : (ok ? 0 : 2);
}

int64_t shy_optimize(const char *path, int64_t floor, int64_t cap, int64_t tick,
                     int64_t cost)
{
    if (floor < 0 || cap < floor || cap > INT64_C(1000000000000) ||
        tick <= 0 || tick > INT64_C(1000000000000) || cost < 0 ||
        cost > INT64_C(1000000000000)) return shy_error("invalid optimization constraints");
    size_t n;
    char *data = shy_read(path, SHY_PAGE_LIMIT, &n);
    if (!data) return shy_error("cannot read demand file");
    char digest[65];
    shy_digest(data, n, digest);
    mulib_json_t *json = mulib_json_parse(data, n, NULL);
    free(data);
    size_t count = mulib_json_size(json);
    int ok = mulib_json_typeof(json) == MULIB_JSON_ARRAY && count > 0 && count <= 4096;
    double prices[4096], demand[4096];
    size_t feasible = 0;
    for (size_t i = 0; ok && i < count; ++i) {
        const mulib_json_t *row = mulib_json_at(json, i);
        const mulib_json_t *price = mulib_json_get(row, "rate_minor");
        const mulib_json_t *units = mulib_json_get(row, "expected_units");
        double p = mulib_json_numeric(price), u = mulib_json_numeric(units);
        ok = mulib_json_typeof(price) == MULIB_JSON_NUMBER &&
             mulib_json_typeof(units) == MULIB_JSON_NUMBER &&
             p >= 0 && p <= 1e12 && trunc(p) == p && u >= 0 && u <= 1e12;
        if (ok && p >= (double)floor && p <= (double)cap && (int64_t)p % tick == 0) {
            prices[feasible] = p;
            demand[feasible++] = u;
        }
    }
    mulib_json_destroy(json);
    if (!ok || !feasible) return shy_error("expected supplied demand estimates on a feasible rate grid");
    const mulib_priceopt_constraints_t limits = { .min_price = (double)floor,
        .max_price = (double)cap, .unit_cost = (double)cost };
    mulib_priceopt_result_t result;
    if (!mulib_priceopt_candidates(prices, demand, feasible, &limits, MULIB_PRICEOPT_PROFIT, &result))
        return shy_error("no feasible rate meets the price and cost constraints");
    mulib_json_t *out = mulib_json_new(MULIB_JSON_OBJECT);
    if (!shy_add(out, "objective", mulib_json_string("expected_profit")) ||
        !shy_add(out, "demand_blake3", mulib_json_string(digest)) ||
        !shy_add(out, "recommended_minor", mulib_json_number(result.price)) ||
        !shy_add(out, "expected_units", mulib_json_number(result.expected_demand)) ||
        !shy_add(out, "expected_profit_minor", mulib_json_number(result.score))) {
        mulib_json_destroy(out);
        return shy_error("could not allocate optimization result");
    }
    return shy_emit(out);
}
