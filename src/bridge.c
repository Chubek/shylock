#include "shylock.h"
#include "extensions.h"
#include "mune_vm.h"
#include "sandbox.h"
#include "mulib-histprice.h"
#include "mulib-mune.h"
#include "mulib-priceopt.h"
#include "mulib-statsprice.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int64_t shy_emit(mulib_json_t *record)
{
    const char *schema = shy_text(record, "schema"), *event = NULL;
    if (!strcmp(schema, "shylock.capture/v2")) event = "capture.extracted";
    else if (!strcmp(schema, "shylock.cohort/v1")) event = "cohort.created";
    else if (!strcmp(schema, "shylock.forecast/v1") || !strcmp(schema, "shylock.forecast/v2")) event = "forecast.created";
    else if (!strcmp(schema, "shylock.decision/v1") || !strcmp(schema, "shylock.pricing-decision/v1")) event = "decision.created";
    else if (!strcmp(schema, "shylock.optimization/v1") || !strcmp(schema, "shylock.optimization/v2")) event = "optimization.created";
    else if (!strcmp(schema, "shylock.demand/v1")) event = "demand.created";
    if (event) (void)shy_extensions_enqueue(event, record);
    char *text = record ? mulib_json_stringify(record, 0) : NULL;
    mulib_json_destroy(record);
    if (!text) return shy_error("could not serialize result");
    int ok = puts(text) >= 0 && fflush(stdout) == 0;
    free(text);
    return ok ? 0 : 2;
}

int64_t shy_extract_plugin(const char *plugin, const char *path, const char *url, int64_t timestamp, const char *rules,
                    const char *model_reply)
{
    size_t length;
    char *data = shy_read(path, SHY_PAGE_LIMIT, &length);
    if (!data) return shy_error("cannot read bounded HTML capture");
    size_t ignored;
    char *rule = *rules ? shy_read(rules, 16384, &ignored) : NULL;
    char *model = *model_reply ? shy_read(model_reply, 65536, &ignored) : NULL;
    /* A missing optional model file is represented as invalid model input so
       extraction can retain its deterministic results and a diagnostic. */
    mulib_json_t *record = (!*rules || rule) ? shy_capture_record(data, length, url, timestamp,
        rule ? rule : "", model ? model : (*model_reply ? "unreadable model reply" : ""), plugin) : NULL;
    free(data);
    free(rule);
    free(model);
    if (!record) return shy_error("extractor failed; check URL, capture time, site rules and snapshot limits");
    return shy_emit(record);
}

int64_t shy_extract(const char *path, const char *url, int64_t timestamp, const char *rules, const char *model)
{ return shy_extract_plugin("dragnet",path,url,timestamp,rules,model); }

int64_t shy_plugins(void)
{
    if (!shy_plugins_init()) return shy_error("plugin ABI negotiation failed");
    mulib_json_t *out = mulib_json_new(MULIB_JSON_ARRAY);
    const shy_plugin *plugin;
    for (size_t i = 0; (plugin = shy_plugin_at(i)); ++i) {
        mulib_json_t *row = mulib_json_new(MULIB_JSON_OBJECT);
        int ok = shy_add(row, "name", mulib_json_string(plugin->name)) &&
            shy_add(row, "version", mulib_json_string(plugin->version)) &&
            shy_add(row, "abi", mulib_json_number(plugin->abi));
        if (ok && shy_plugin_configuration(plugin)) ok=shy_add(row,"sdk_abi",mulib_json_number(1));
        if (!ok) mulib_json_destroy(row);
        if (!ok || !shy_push(out, row)) { mulib_json_destroy(out); return 2; }
    }
    return shy_emit(out);
}

/* Command processes carry one forecast into a subsequent decision. */
static struct {
    char digest[65];
    int64_t horizon;
    double point;
    size_t samples;
    mulib_json_t *cohort;
} forecast_context;

static int64_t estimate_error(const char *message, int64_t emit)
{
    shy_error(message);
    return emit ? 2 : -1;
}

int64_t shy_estimate(const char *path, int64_t horizon, int64_t emit)
{
    mulib_json_destroy(forecast_context.cohort);
    memset(&forecast_context, 0, sizeof forecast_context);
    if (horizon < 1 || horizon > 365) return estimate_error("horizon must be in 1..365", emit);
    size_t count;
    double values[4096];
    int ok = shy_series_load(path, values, &count, forecast_context.digest, &forecast_context.cohort);
    mulib_statsprice_summary_t market;
    mulib_histprice_forecast_t forecast;
    if (!ok || !mulib_statsprice_summarize(values, count, &market) ||
        !mulib_histprice_holt(values, count, 0.4, 0.2, 0.9, (size_t)horizon, &forecast) ||
        !isfinite(forecast.point) || forecast.point < 0 || forecast.point > 1e12)
        return estimate_error("expected an equally spaced cohort of 2..4096 integer minor-unit samples", emit);
    forecast_context.horizon = horizon;
    forecast_context.point = forecast.point;
    forecast_context.samples = count;
    mulib_json_t *out = mulib_json_new(MULIB_JSON_OBJECT);
    if (!shy_add(out, "schema", mulib_json_string("shylock.forecast/v1")) ||
        !shy_add(out, "kind", mulib_json_string("forecast")) ||
        !shy_add(out, "estimator", shy_estimator()) ||
        !shy_add(out, "reference_minor", mulib_json_number(market.median)) ||
        !shy_add(out, "forecast_minor", mulib_json_number(forecast.point)) ||
        !shy_add(out, "samples", mulib_json_number((double)count)) ||
        !shy_add(out, "horizon", mulib_json_number((double)horizon)) ||
        !shy_add(out, "series_blake3", mulib_json_string(forecast_context.digest))) {
        mulib_json_destroy(out);
        return estimate_error("could not allocate estimate", emit);
    }
    if (forecast_context.cohort && !shy_add(out, "cohort_lineage", shy_clone(forecast_context.cohort))) {
        mulib_json_destroy(out); return estimate_error("could not attach forecast lineage", emit);
    }
    if (!emit) {
        (void)shy_extensions_enqueue("forecast.created", out);
        mulib_json_destroy(out);
        return (int64_t)llround(forecast.point);
    }
    return shy_emit(out);
}

static char formula_digest[65];
static char formula_error[SHY_VM_ERROR];
static int formula_isolation, formula_sandbox_active;

/* Borrowed audit views; digest/diagnostic/backend/profile describe the most
   recent evaluation and must be copied before evaluating another formula. */
const char *shy_formula_digest(void) { return formula_digest; }
const char *shy_formula_diagnostic(void) { return formula_error; }
const char *shy_formula_backend(void) { return formula_isolation < 0 ? "invalid" : formula_isolation ? "process" : "off"; }
const char *shy_formula_profile(void) { return formula_sandbox_active ? SHY_SANDBOX_PROFILE : "none"; }
const char *shy_runtime_revision(void) { return SHY_MULIB_REVISION; }
int64_t shy_mune_api(void) { return mulib_mune_version(); }

int64_t shy_formula(const char *path, int64_t reference, int64_t weekend, int64_t premium)
{
    size_t length;
    formula_digest[0] = 0; *formula_error = 0; formula_sandbox_active = 0;
    formula_isolation = shy_sandbox_mode(getenv("SHY_FORMULA_SANDBOX"));
    char *source = shy_read(path, 16384, &length);
    if (!source) { shy_vm_error(formula_error, "cannot read formula"); shy_error(formula_error); return -1; }
    shy_digest(source, length, formula_digest);
    int64_t rate = -1;
    if (formula_isolation < 0) shy_vm_error(formula_error, "sandbox mode must be off or process");
    else if (!formula_isolation) (void)shy_formula_eval(source, length, reference, weekend, premium, &rate, formula_error);
    else {
        shy_process *worker = shy_process_start(0, formula_error);
        formula_sandbox_active = worker != NULL;
        mulib_json_t *request = mulib_json_new(MULIB_JSON_OBJECT);
        int built = shy_add(request, "op", mulib_json_string("formula")) &&
            shy_add(request, "source", mulib_json_string(source)) &&
            shy_add(request, "reference", mulib_json_number((double)reference)) &&
            shy_add(request, "weekend", mulib_json_bool(weekend != 0)) &&
            shy_add(request, "premium", mulib_json_number((double)premium));
        mulib_json_t *response = worker && built ? shy_process_request(worker, request, NULL, NULL, formula_error) : NULL;
        if (response && mulib_json_boolean(mulib_json_get(response, "ok"))) rate = shy_integer(response, "minor", INT64_C(1000000000000));
        if (rate < 0 && !*formula_error) shy_vm_error(formula_error, response && *shy_text(response, "error") ?
            shy_text(response, "error") : "invalid or unallocated sandbox formula result");
        mulib_json_destroy(response); mulib_json_destroy(request); shy_process_destroy(worker);
    }
    free(source);
    if (rate < 0) shy_error(formula_error);
    return rate;
}

int64_t shy_report_rate(int64_t reference, int64_t proposed, int64_t adjusted,
                        int64_t weekend, int64_t premium, int64_t floor, int64_t cap,
                        int64_t tick)
{
    mulib_json_t *out = mulib_json_new(MULIB_JSON_OBJECT);
    int ok = adjusted >= 0;
    if (!shy_add(out, "schema", mulib_json_string("shylock.decision/v1")) ||
        !shy_add(out, "kind", mulib_json_string("formula_adjusted")) ||
        !shy_add(out, "contract", mulib_json_string("adjust_rate/v1")) ||
        !shy_add(out, "mulib_revision", mulib_json_string(SHY_MULIB_REVISION)) ||
        !shy_add(out, "mune_api", mulib_json_number(mulib_mune_version())) ||
        !shy_add(out, "formula_blake3", mulib_json_string(formula_digest)) ||
        !shy_add(out, "formula_isolation", mulib_json_string(formula_isolation < 0 ? "invalid" : formula_isolation ? "process" : "off")) ||
        !shy_add(out, "formula_sandbox_profile", mulib_json_string(formula_sandbox_active ? SHY_SANDBOX_PROFILE : "none")) ||
        !shy_add(out, "formula_error", ok ? mulib_json_new(MULIB_JSON_NULL) : mulib_json_string(formula_error)) ||
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
          !shy_add(out, "samples", mulib_json_number((double)forecast_context.samples)) ||
          !shy_add(out, "estimator", shy_estimator()) ||
         !shy_add(out, "series_blake3", mulib_json_string(forecast_context.digest)))) {
        mulib_json_destroy(out);
        return shy_error("could not attach forecast");
    }
    if (forecast_context.cohort && !shy_add(out, "cohort_lineage", shy_clone(forecast_context.cohort))) {
        mulib_json_destroy(out); return 2;
    }
    int64_t status = shy_emit(out);
    return status ? status : (ok ? 0 : 2);
}

int64_t shy_optimize_data(const mulib_json_t *json, const char *digest,
                         int64_t floor, int64_t cap, int64_t tick, int64_t cost)
{
    if (floor < 0 || cap < floor || cap > INT64_C(1000000000000) ||
        tick <= 0 || tick > INT64_C(1000000000000) || cost < 0 ||
        cost > INT64_C(1000000000000)) return shy_error("invalid optimization constraints");
    int rich = mulib_json_typeof(json) == MULIB_JSON_OBJECT;
    const mulib_json_t *scenarios = rich ? mulib_json_get(json, "scenarios") : json;
    size_t count = mulib_json_size(scenarios);
    int ok = mulib_json_typeof(scenarios) == MULIB_JSON_ARRAY && count > 0 && count <= 4096;
    double prices[4096], demand[4096];
    size_t feasible = 0;
    for (size_t i = 0; ok && i < count; ++i) {
        const mulib_json_t *row = mulib_json_at(scenarios, i);
        const mulib_json_t *price = mulib_json_get(row, "rate_minor");
        const mulib_json_t *units = mulib_json_get(row, "expected_units");
        double p = mulib_json_numeric(price), u = mulib_json_numeric(units);
        ok = mulib_json_typeof(price) == MULIB_JSON_NUMBER &&
             mulib_json_typeof(units) == MULIB_JSON_NUMBER &&
             isfinite(p) && isfinite(u) &&
             p >= 0 && p <= 1e12 && trunc(p) == p && u >= 0 && u <= 1e12;
        if (ok && p >= (double)floor && p <= (double)cap && (int64_t)p % tick == 0) {
            prices[feasible] = p;
            demand[feasible++] = u;
        }
    }
    if (!ok || !feasible) return shy_error("expected supplied demand estimates on a feasible rate grid");
    const mulib_priceopt_constraints_t limits = { .min_price = (double)floor,
        .max_price = (double)cap, .unit_cost = (double)cost };
    mulib_priceopt_result_t result;
    if (!mulib_priceopt_candidates(prices, demand, feasible, &limits, MULIB_PRICEOPT_PROFIT, &result))
        return shy_error("no feasible rate meets the price and cost constraints");
    mulib_json_t *out = mulib_json_new(MULIB_JSON_OBJECT);
    if (!shy_add(out, "schema", mulib_json_string("shylock.optimization/v1")) ||
        !shy_add(out, "kind", mulib_json_string("demand_optimized")) ||
        !shy_add(out, "demand_origin", mulib_json_string(rich ? "empirical_booking_outcomes" : "externally_supplied")) ||
        !shy_add(out, "floor_minor", mulib_json_number((double)floor)) ||
        !shy_add(out, "cap_minor", mulib_json_number((double)cap)) ||
        !shy_add(out, "tick_minor", mulib_json_number((double)tick)) ||
        !shy_add(out, "unit_cost_minor", mulib_json_number((double)cost)) ||
        !shy_add(out, "objective", mulib_json_string("expected_profit")) ||
        !shy_add(out, "demand_blake3", mulib_json_string(digest)) ||
        !shy_add(out, "recommended_minor", mulib_json_number(result.price)) ||
        !shy_add(out, "expected_units", mulib_json_number(result.expected_demand)) ||
        !shy_add(out, "expected_profit_minor", mulib_json_number(result.score))) {
        mulib_json_destroy(out);
        return shy_error("could not allocate optimization result");
    }
    if (rich && !shy_add(out, "demand_lineage", shy_clone(json))) {
        mulib_json_destroy(out); return 2;
    }
    return shy_emit(out);
}
