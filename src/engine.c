#include "engine.h"
#include "document.h"
#include "extensions.h"
#include "mulib-histprice.h"
#include "mulib-statsprice.h"
#include "mulib-priceopt.h"
#include "mulib-money.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

enum { MAX_MODELS = 16, MAX_FOLDS = 64 };
typedef struct {
    size_t attempted, evaluated;
    long double absolute, squared, bias;
    double residuals[MAX_FOLDS];
    mulib_json_t *folds;
} assessment;
typedef struct {
    mulib_json_t *parameters;
    assessment selection, validation;
} candidate;
static struct {
    double values[4096], point, market, variance;
    size_t count, models, window;
    int failed;
    char digest[65];
    mulib_json_t *lineage, *policy, *forecast, *pricing, *demand, *optimization;
    candidate candidates[MAX_MODELS];
} engine;

int64_t shy_engine_close(void)
{
    for (size_t i = 0; i < engine.models; ++i) {
        mulib_json_destroy(engine.candidates[i].parameters);
        mulib_json_destroy(engine.candidates[i].selection.folds);
        mulib_json_destroy(engine.candidates[i].validation.folds);
    }
    mulib_json_destroy(engine.lineage); mulib_json_destroy(engine.policy);
    mulib_json_destroy(engine.forecast); mulib_json_destroy(engine.pricing);
    mulib_json_destroy(engine.demand); mulib_json_destroy(engine.optimization);
    memset(&engine, 0, sizeof engine); engine.point = -1; return 0;
}

int64_t shy_engine_open(const char *path)
{
    shy_engine_close(); engine.point = -1;
    if (!shy_series_load(path, engine.values, &engine.count, engine.digest, &engine.lineage)) return -1;
    return (int64_t)engine.count;
}

static mulib_json_t *parse_text(const char *text)
{
    size_t n = strlen(text);
    return n <= 65536 ? mulib_json_parse(text, n, NULL) : NULL;
}

int64_t shy_engine_policy(const char *text)
{
    mulib_json_destroy(engine.policy); engine.policy = parse_text(text);
    int64_t window = shy_integer(engine.policy, "window", 1024);
    engine.window = window >= 2 ? (size_t)window : 0;
    return engine.policy && engine.window ? 1 : 0;
}

int64_t shy_engine_candidate(const char *text)
{
    if (engine.models == MAX_MODELS) return -1;
    candidate *c = &engine.candidates[engine.models++];
    c->parameters = parse_text(text);
    c->selection.folds = mulib_json_new(MULIB_JSON_ARRAY);
    c->validation.folds = mulib_json_new(MULIB_JSON_ARRAY);
    if (!c->parameters || !c->selection.folds || !c->validation.folds) { engine.failed = 1; return -1; }
    return (int64_t)(engine.models-1);
}

static int predict(const candidate *c, size_t training, size_t horizon, double *point, double *variance)
{
    if (!training || training > engine.count || !engine.window) return 0;
    size_t n = training < engine.window ? training : engine.window;
    const double *values = engine.values + training-n;
    const mulib_json_t *p = c->parameters;
    const char *model = shy_text(p, "model");
    double a = (double)shy_integer(p, "alpha_bp", 10000)/10000.0;
    double b = (double)shy_integer(p, "beta_bp", 10000)/10000.0;
    double phi = (double)shy_integer(p, "phi_bp", 10000)/10000.0;
    mulib_histprice_forecast_t result = { .point = 0, .variance = NAN };
    int ok = 0;
    if (!strcmp(model, "last")) { result.point = values[n-1]; ok = 1; }
    else if (!strcmp(model, "median")) ok = mulib_statsprice_estimate(values, n, MULIB_STATSPRICE_MEDIAN, 0, &result.point);
    else if (!strcmp(model, "ema")) ok = mulib_histprice_ema(values, n, a, horizon, &result);
    else if (!strcmp(model, "holt")) ok = mulib_histprice_holt(values, n, a, b, phi, horizon, &result);
    else if (!strcmp(model, "seasonal")) {
        int64_t season = shy_integer(p, "season", 512);
        double gamma = (double)shy_integer(p, "gamma_bp", 10000)/10000.0;
        if (season >= 2) ok = mulib_histprice_seasonal(values, n, (size_t)season, a, b, gamma, horizon, &result);
    } else if (!strcmp(model, "kalman")) {
        int64_t q = shy_integer(p, "process_variance", INT64_C(1000000000000));
        int64_t r = shy_integer(p, "measurement_variance", INT64_C(1000000000000));
        if (q >= 0 && r >= 0) ok = mulib_histprice_kalman(values, n, (double)q, (double)r, horizon, &result);
    } else if (!strcmp(model, "ar1")) ok = mulib_histprice_ar1(values, n, horizon, &result);
    else if (!strcmp(model, "robust-trend")) ok = mulib_histprice_robust_trend(values, n, horizon, &result);
    if (!ok || !isfinite(result.point) || result.point < 0 || result.point > 1e12) return 0;
    *point = result.point;
    if (variance) *variance = result.variance;
    return 1;
}

int64_t shy_engine_fold(int64_t index, int64_t training, int64_t horizon, int64_t validation)
{
    if (index < 0 || (size_t)index >= engine.models || training < 2 || horizon < 1 || horizon > 365 ||
        (uint64_t)training+(uint64_t)horizon > engine.count) return 0;
    candidate *c = &engine.candidates[index];
    assessment *a = validation ? &c->validation : &c->selection;
    if (a->attempted == MAX_FOLDS || engine.failed) return 0;
    size_t target = (size_t)(training+horizon-1);
    double point = 0;
    int ok = predict(c, (size_t)training, (size_t)horizon, &point, NULL);
    mulib_json_t *fold = mulib_json_new(MULIB_JSON_OBJECT);
    size_t used = (size_t)training < engine.window ? (size_t)training : engine.window;
    int built = shy_add(fold, "training_samples", mulib_json_number((double)training)) &&
        shy_add(fold, "window_start_index", mulib_json_number((double)((size_t)training-used))) &&
        shy_add(fold, "window_samples", mulib_json_number((double)used)) &&
        shy_add(fold, "target_index", mulib_json_number((double)target)) &&
        shy_add(fold, "actual_minor", mulib_json_number(engine.values[target])) &&
        shy_add(fold, "status", mulib_json_string(ok ? "evaluated" : "model_error")) &&
        shy_add(fold, "forecast_minor", ok ? mulib_json_number(point) : mulib_json_new(MULIB_JSON_NULL));
    if (built && engine.lineage) {
        int64_t start = shy_integer(engine.lineage, "start", INT64_C(253402300799));
        int64_t step = shy_integer(engine.lineage, "step", INT64_C(253402300800));
        built = shy_add(fold, "cutoff", mulib_json_number((double)(start+(training-1)*step))) &&
            shy_add(fold, "target_at", mulib_json_number((double)(start+(int64_t)target*step)));
    }
    if (!built) mulib_json_destroy(fold);
    if (!built || !shy_push(a->folds, fold)) { engine.failed = 1; return 0; }
    ++a->attempted;
    if (ok) {
        long double error = point-engine.values[target];
        a->residuals[a->evaluated++] = (double)fabsl(error);
        a->absolute += fabsl(error); a->squared += error*error; a->bias += error;
    }
    return 1; /* A model failure is recorded, not disguised as an adapter failure. */
}

int64_t shy_engine_complete(int64_t index)
{
    if (index < 0 || (size_t)index >= engine.models || engine.failed) return 0;
    assessment *a = &engine.candidates[index].selection;
    return a->attempted && a->attempted == a->evaluated;
}

int64_t shy_engine_compare(int64_t x, int64_t y, const char *metric)
{
    if (x < 0 || y < 0 || (size_t)x >= engine.models || (size_t)y >= engine.models) return 0;
    assessment *a = &engine.candidates[x].selection, *b = &engine.candidates[y].selection;
    if (!a->evaluated || !b->evaluated) return 0;
    long double av = !strcmp(metric, "mae") ? a->absolute/a->evaluated : a->squared/a->evaluated;
    long double bv = !strcmp(metric, "mae") ? b->absolute/b->evaluated : b->squared/b->evaluated;
    return av < bv ? -1 : av > bv ? 1 : 0;
}

static mulib_json_t *assessment_json(const assessment *a)
{
    mulib_json_t *out = mulib_json_new(MULIB_JSON_OBJECT);
    if (!shy_add(out, "attempted", mulib_json_number((double)a->attempted)) ||
        !shy_add(out, "evaluated", mulib_json_number((double)a->evaluated)) ||
        !shy_add(out, "coverage", mulib_json_number(a->attempted ? (double)a->evaluated/(double)a->attempted : 0)) ||
        !shy_add(out, "mae_minor", a->evaluated ? mulib_json_number((double)(a->absolute/a->evaluated)) : mulib_json_new(MULIB_JSON_NULL)) ||
        !shy_add(out, "rmse_minor", a->evaluated ? mulib_json_number(sqrt((double)(a->squared/a->evaluated))) : mulib_json_new(MULIB_JSON_NULL)) ||
        !shy_add(out, "bias_minor", a->evaluated ? mulib_json_number((double)(a->bias/a->evaluated)) : mulib_json_new(MULIB_JSON_NULL)) ||
        !shy_add(out, "folds", shy_clone(a->folds))) { mulib_json_destroy(out); return NULL; }
    return out;
}

static int seal(mulib_json_t *out, const char *key)
{
    char digest[65];
    return shy_json_digest(out, key, digest) && shy_add(out, key, mulib_json_string(digest));
}

static int market_reference(double *out)
{
    const char *name = shy_text(engine.policy, "market_method");
    static const char *const names[] = { "median", "trimmed", "winsorized", "huber", "quantile", "kde" };
    for (size_t i = 0; i < sizeof names/sizeof *names; ++i)
        if (!strcmp(name, names[i])) return mulib_statsprice_estimate(engine.values, engine.count,
            (mulib_statsprice_method_t)i, (double)shy_integer(engine.policy, "market_parameter_ppm", INT64_C(1000000000000))/1000000.0, out);
    return 0;
}

int64_t shy_engine_finish(int64_t selected, int64_t horizon, int64_t rank, int64_t coverage)
{
    if (engine.failed || !engine.policy || horizon < 1 || horizon > 365 || !market_reference(&engine.market)) return 2;
    char policy_id[65];
    if (!shy_json_digest(engine.policy, NULL, policy_id)) return 2;
    candidate *chosen = selected >= 0 && (size_t)selected < engine.models ? &engine.candidates[selected] : NULL;
    assessment *validation = chosen ? &chosen->validation : NULL;
    int complete = validation && validation->attempted && validation->evaluated == validation->attempted;
    double radius = 0;
    int ok = complete && rank >= 1 && (size_t)rank <= validation->evaluated &&
        predict(chosen, engine.count, (size_t)horizon, &engine.point, &engine.variance) &&
        mulib_statsproc_quantile(validation->residuals, validation->evaluated,
            validation->evaluated == 1 ? 0 : (double)(rank-1)/(double)(validation->evaluated-1), &radius);
    if (!ok) engine.point = -1;
    mulib_json_t *out = mulib_json_new(MULIB_JSON_OBJECT), *models = mulib_json_new(MULIB_JSON_ARRAY);
    for (size_t i = 0; i < engine.models; ++i) {
        mulib_json_t *row = mulib_json_new(MULIB_JSON_OBJECT);
        int built = shy_add(row, "candidate", shy_clone(engine.candidates[i].parameters)) &&
            shy_add(row, "selection", assessment_json(&engine.candidates[i].selection));
        if (!built) mulib_json_destroy(row);
        if (!built || !shy_push(models, row)) goto fail;
    }
    mulib_json_t *interval = mulib_json_new(MULIB_JSON_OBJECT);
    int built = shy_add(interval, "method", mulib_json_string("holdout-absolute-error-quantile/v1")) &&
        shy_add(interval, "coverage_bp", mulib_json_number((double)coverage)) &&
        shy_add(interval, "calibration_samples", mulib_json_number(validation ? (double)validation->evaluated : 0)) &&
        shy_add(interval, "rank", mulib_json_number((double)rank)) &&
        shy_add(interval, "radius_minor", ok ? mulib_json_number(radius) : mulib_json_new(MULIB_JSON_NULL)) &&
        shy_add(interval, "lower_minor", ok ? mulib_json_number(fmax(0, engine.point-radius)) : mulib_json_new(MULIB_JSON_NULL)) &&
        shy_add(interval, "upper_minor", ok ? mulib_json_number(fmin(1e12, engine.point+radius)) : mulib_json_new(MULIB_JSON_NULL));
    if (!built) { mulib_json_destroy(interval); goto fail; }
    if (!shy_add(out, "interval", interval)) goto fail;
    if (!shy_add(out, "schema", mulib_json_string("shylock.forecast/v2")) ||
        !shy_add(out, "kind", mulib_json_string("evaluated_forecast")) ||
        !shy_add(out, "status", mulib_json_string(ok ? "forecast" : !chosen ? "no_eligible_model" : "forecast_error")) ||
        !shy_add(out, "selection_policy", mulib_json_string("complete-mae-rmse-order/v1")) ||
        !shy_add(out, "mulib_revision", mulib_json_string(SHY_MULIB_REVISION)) ||
        !shy_add(out, "series_blake3", mulib_json_string(engine.digest)) ||
        !shy_add(out, "policy", shy_clone(engine.policy)) ||
        !shy_add(out, "policy_id", mulib_json_string(policy_id)) ||
        !shy_add(out, "samples", mulib_json_number((double)engine.count)) ||
        !shy_add(out, "horizon", mulib_json_number((double)horizon)) ||
        !shy_add(out, "market_reference_minor", mulib_json_number(engine.market)) ||
        !shy_add(out, "selected_estimator", chosen ? shy_clone(chosen->parameters) : mulib_json_new(MULIB_JSON_NULL)) ||
        !shy_add(out, "validation", chosen ? assessment_json(validation) : mulib_json_new(MULIB_JSON_NULL)) ||
        !shy_add(out, "forecast_minor", ok ? mulib_json_number(engine.point) : mulib_json_new(MULIB_JSON_NULL)) ||
        !shy_add(out, "model_variance_minor_squared", ok && isfinite(engine.variance) ? mulib_json_number(engine.variance) : mulib_json_new(MULIB_JSON_NULL)) ||
        !shy_add(out, "reference_minor", ok ? mulib_json_number((double)llround(engine.point)) : mulib_json_new(MULIB_JSON_NULL))) goto fail;
    if (!mulib_json_set(out, "candidates", models)) goto fail;
    models = NULL;
    if (engine.lineage && !shy_add(out, "cohort_lineage", shy_clone(engine.lineage))) goto fail;
    if (!seal(out, "forecast_id")) goto fail;
    mulib_json_destroy(engine.forecast); engine.forecast = out;
    return ok ? 0 : 2;
fail:
    mulib_json_destroy(out); mulib_json_destroy(models); engine.failed = 1; return 2;
}

int64_t shy_engine_emit(void)
{
    if (!engine.forecast || engine.failed) return shy_error("cannot allocate evaluated forecast");
    return shy_emit(shy_clone(engine.forecast));
}

int64_t shy_engine_reference(void) { return engine.point < 0 ? -1 : (int64_t)llround(engine.point); }

const char *shy_engine_field(const char *key)
{
    const mulib_json_t *context = mulib_json_get(engine.lineage, "context");
    if (!strcmp(key, "cohort") || !strcmp(key, "currency")) return shy_text(engine.lineage, key);
    return shy_text(context, key);
}

int64_t shy_engine_number(const char *key)
{
    if (!strcmp(key, "verified")) return engine.lineage != NULL;
    if (!strcmp(key, "scale")) return shy_integer(engine.lineage, key, 6);
    if (!strcmp(key, "guests")) return shy_integer(mulib_json_get(engine.lineage, "context"), key, 64);
    if (!strcmp(key, "latest_capture")) {
        int64_t latest = -1;
        const mulib_json_t *buckets = mulib_json_get(engine.lineage, "buckets");
        for (size_t i = 0; i < mulib_json_size(buckets); ++i) {
            const mulib_json_t *refs = mulib_json_get(mulib_json_at(buckets, i), "evidence");
            for (size_t j = 0; j < mulib_json_size(refs); ++j) {
                int64_t time = shy_integer(mulib_json_at(refs, j), "captured_at", INT64_C(253402300799));
                if (time > latest) latest = time;
            }
        }
        return latest;
    }
    return -1;
}

int64_t shy_money_operation(const char *operation, int64_t a, int64_t b,
    int64_t denominator, const char *currency, int64_t scale)
{
    mulib_money_t x, y, result;
    if (a < 0 || b < 0 || scale < 0 || scale != shy_currency_scale(currency) ||
        !mulib_money_make(&x, a, currency, (uint8_t)scale)) return -1;
    int ok = 0;
    if (!strcmp(operation, "scale")) ok = mulib_money_scale(x, b, denominator, &result);
    else if (mulib_money_make(&y, b, currency, (uint8_t)scale)) {
        if (!strcmp(operation, "add")) ok = mulib_money_add(x, y, &result);
        if (!strcmp(operation, "sub")) ok = mulib_money_sub(x, y, &result);
    }
    return ok && result.minor >= 0 && result.minor <= INT64_C(1000000000000) ? result.minor : -1;
}

int64_t shy_engine_pricing_policy(const char *text)
{
    mulib_json_destroy(engine.pricing); engine.pricing = parse_text(text);
    return engine.pricing != NULL;
}

int shy_engine_demand(const mulib_json_t *demand)
{
    mulib_json_destroy(engine.demand); engine.demand = shy_clone(demand);
    return engine.demand != NULL;
}

int64_t shy_engine_take_demand(void) { return shy_engine_demand(shy_document_borrow()); }

int64_t shy_engine_optimize(int64_t low, int64_t high, int64_t tick, int64_t cost, const char *objective)
{
    if (low < 0 || high < low || high > INT64_C(1000000000000) || tick < 1 || cost < 0) return -1;
    const mulib_json_t *rows = mulib_json_get(engine.demand, "scenarios");
    size_t n = mulib_json_size(rows);
    if (!n || n > 4096) return -1;
    double rates[4096], units[4096];
    for (size_t i = 0; i < n; ++i) {
        const mulib_json_t *row = mulib_json_at(rows, i);
        int64_t rate = shy_integer(row, "rate_minor", INT64_C(1000000000000));
        const mulib_json_t *demand = mulib_json_get(row, "expected_units");
        if (rate < 0 || mulib_json_typeof(demand) != MULIB_JSON_NUMBER) return -1;
        rates[i] = (double)rate; units[i] = mulib_json_numeric(demand);
    }
    mulib_priceopt_constraints_t constraints = { .min_price = (double)low, .max_price = (double)high,
        .unit_cost = (double)cost, .tick = (double)tick };
    mulib_priceopt_result_t result;
    int revenue = !strcmp(objective, "revenue");
    if ((!revenue && strcmp(objective, "profit")) ||
        !mulib_priceopt_candidates(rates, units, n, &constraints, revenue ? MULIB_PRICEOPT_REVENUE : MULIB_PRICEOPT_PROFIT, &result) ||
        result.price < low || result.price > high || trunc(result.price) != result.price || (int64_t)result.price%tick) return -1;
    mulib_json_t *out = mulib_json_new(MULIB_JSON_OBJECT);
    if (!shy_add(out, "kind", mulib_json_string("demand_optimized")) ||
        !shy_add(out, "objective", mulib_json_string(objective)) ||
        !shy_add(out, "objective_basis", mulib_json_string("quoted-nightly-rate/v1")) ||
        !shy_add(out, "recommended_minor", mulib_json_number(result.price)) ||
        !shy_add(out, "expected_units", mulib_json_number(result.expected_demand)) ||
        !shy_add(out, "objective_minor", mulib_json_number(result.score)) ||
        !shy_add(out, "evaluations", mulib_json_number((double)result.evaluations)) ||
        !shy_add(out, "effective_floor_minor", mulib_json_number((double)low)) ||
        !shy_add(out, "effective_cap_minor", mulib_json_number((double)high))) { mulib_json_destroy(out); return -1; }
    mulib_json_destroy(engine.optimization); engine.optimization = out;
    return (int64_t)result.price;
}

int64_t shy_engine_decision(const char *status, int64_t proposed, int64_t formula_rate,
    int64_t final_rate, const char *formula_audit, const char *quote)
{
    if (engine.failed) return shy_error("pricing audit allocation failed");
    mulib_json_t *out = mulib_json_new(MULIB_JSON_OBJECT);
    char policy_id[65];
    mulib_json_t *calculation = *formula_audit ? parse_text(formula_audit) : mulib_json_new(MULIB_JSON_NULL);
    mulib_json_t *stay = *quote ? parse_text(quote) : mulib_json_new(MULIB_JSON_NULL);
    if (*quote && (!stay || !seal(stay, "quote_id"))) { mulib_json_destroy(stay); stay = NULL; }
    int built = shy_json_digest(engine.pricing, NULL, policy_id) &&
        shy_add(out, "schema", mulib_json_string("shylock.pricing-decision/v1")) &&
        shy_add(out, "kind", mulib_json_string("pricing_decision")) &&
        shy_add(out, "status", mulib_json_string(status)) &&
        shy_add(out, "information_cutoff", mulib_json_string("capture-and-finalization/v1")) &&
        shy_add(out, "mulib_revision", mulib_json_string(SHY_MULIB_REVISION)) &&
        shy_add(out, "policy", shy_clone(engine.pricing)) &&
        shy_add(out, "policy_id", mulib_json_string(policy_id)) &&
        shy_add(out, "forecast", engine.forecast ? shy_clone(engine.forecast) : mulib_json_new(MULIB_JSON_NULL)) &&
        shy_add(out, "reference_minor", engine.point >= 0 ? mulib_json_number((double)llround(engine.point)) : mulib_json_new(MULIB_JSON_NULL)) &&
        shy_add(out, "proposed_minor", proposed >= 0 ? mulib_json_number((double)proposed) : mulib_json_new(MULIB_JSON_NULL)) &&
        shy_add(out, "formula_adjusted_minor", formula_rate >= 0 ? mulib_json_number((double)formula_rate) : mulib_json_new(MULIB_JSON_NULL)) &&
        shy_add(out, "recommended_minor", final_rate >= 0 ? mulib_json_number((double)final_rate) : mulib_json_new(MULIB_JSON_NULL)) &&
        shy_add(out, "demand_lineage", engine.demand ? shy_clone(engine.demand) : mulib_json_new(MULIB_JSON_NULL)) &&
        shy_add(out, "optimization", engine.optimization ? shy_clone(engine.optimization) : mulib_json_new(MULIB_JSON_NULL));
    if (built) { built = shy_add(out, "formula", calculation); calculation = NULL; }
    if (built) { built = shy_add(out, "quote", stay); stay = NULL; }
    mulib_json_destroy(calculation); mulib_json_destroy(stay);
    if (!built || !seal(out, "decision_id")) { mulib_json_destroy(out); return 2; }
    if (engine.forecast) (void)shy_extensions_enqueue("forecast.created", engine.forecast);
    int64_t emitted = shy_emit(out);
    return emitted ? emitted : strcmp(status, "priced") ? 2 : 0;
}

int64_t shy_model_optimize(const char *text, int64_t low, int64_t high)
{
    mulib_json_t *policy = parse_text(text), *out = NULL;
    const mulib_json_t *items = mulib_json_get(policy, "models");
    const mulib_json_t *limits = mulib_json_get(policy, "constraints");
    size_t count = mulib_json_size(items);
    int64_t tick = shy_integer(limits, "tick_minor", INT64_C(1000000000000));
    int64_t cost = shy_integer(limits, "unit_cost_minor", INT64_C(1000000000000));
    int64_t risk = shy_integer(policy, "risk_bp", 100000);
    if (low < 0 || high < low || high > INT64_C(1000000000000) || tick < 1 || cost < 0 || risk < 0 ||
        !count || count > MAX_MODELS || (high-low)/tick > 99999) goto fail;
    mulib_priceopt_demand_model_t models[MAX_MODELS];
    double weights[MAX_MODELS];
    for (size_t i = 0; i < count; ++i) {
        const mulib_json_t *row = mulib_json_at(items, i);
        const char *kind = shy_text(row, "kind");
        int64_t scale = shy_integer(row, "scale_units", 1000000);
        int64_t shape = shy_integer(row, "shape_ppm", INT64_C(1000000000000));
        int64_t reference = shy_integer(row, "reference_minor", INT64_C(1000000000000));
        int64_t weight = shy_integer(row, "weight_bp", 1000000);
        int type = !strcmp(kind, "linear") ? MULIB_PRICEOPT_LINEAR : !strcmp(kind, "isoelastic") ? MULIB_PRICEOPT_ISOELASTIC :
            !strcmp(kind, "logistic") ? MULIB_PRICEOPT_LOGISTIC : -1;
        if (type < 0 || scale < 0 || shape < 0 || reference < 0 || weight < 0) goto fail;
        models[i] = (mulib_priceopt_demand_model_t){ (mulib_priceopt_demand_kind_t)type,
            (double)scale, (double)shape/1000000.0, (double)reference };
        weights[i] = (double)weight;
    }
    const char *objective = shy_text(limits, "objective");
    int revenue = !strcmp(objective, "revenue");
    if (!revenue && strcmp(objective, "profit")) goto fail;
    mulib_priceopt_constraints_t constraints = { .min_price = (double)low, .max_price = (double)high,
        .unit_cost = (double)cost, .tick = (double)tick };
    mulib_priceopt_result_t result;
    if (!mulib_priceopt_scenarios(models, weights, count, (double)risk/10000.0, &constraints,
        revenue ? MULIB_PRICEOPT_REVENUE : MULIB_PRICEOPT_PROFIT, &result) ||
        result.price < low || result.price > high || trunc(result.price) != result.price || (int64_t)result.price%tick) goto fail;
    out = mulib_json_new(MULIB_JSON_OBJECT);
    char id[65];
    if (!shy_json_digest(policy, NULL, id) ||
        !shy_add(out, "schema", mulib_json_string("shylock.optimization/v2")) ||
        !shy_add(out, "kind", mulib_json_string("scenario_risk_optimized")) ||
        !shy_add(out, "demand_origin", mulib_json_string("externally_specified_models")) ||
        !shy_add(out, "objective_basis", mulib_json_string("quoted-nightly-rate/v1")) ||
        !shy_add(out, "mulib_revision", mulib_json_string(SHY_MULIB_REVISION)) ||
        !shy_add(out, "policy_id", mulib_json_string(id)) ||
        !shy_add(out, "recommended_minor", mulib_json_number(result.price)) ||
        !shy_add(out, "expected_units", mulib_json_number(result.expected_demand)) ||
        !shy_add(out, "risk_adjusted_score_minor", mulib_json_number(result.score)) ||
        !shy_add(out, "expected_revenue_minor", mulib_json_number(result.price*result.expected_demand)) ||
        !shy_add(out, "expected_profit_minor", mulib_json_number((result.price-cost)*result.expected_demand)) ||
        !shy_add(out, "evaluations", mulib_json_number((double)result.evaluations)) ||
        !shy_add(out, "effective_floor_minor", mulib_json_number((double)low)) ||
        !shy_add(out, "effective_cap_minor", mulib_json_number((double)high))) goto fail;
    if (!mulib_json_set(out, "policy", policy)) goto fail;
    policy = NULL;
    if (!seal(out, "optimization_id")) goto fail;
    return shy_emit(out);
fail:
    mulib_json_destroy(policy); mulib_json_destroy(out);
    return shy_error("invalid, unavailable or unallocatable model optimization");
}
