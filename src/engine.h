#ifndef SHYLOCK_ENGINE_H
#define SHYLOCK_ENGINE_H
#include "shylock.h"
/* One synchronous engine session. Open copies numeric samples and owns lineage;
   policy/candidate JSON is copied on entry. All string views borrow the session.
   Close releases every candidate, fold, snapshot and decision, even on failure.
   A failed load/build/publication poisons the session until close/open. A
   reference is available only after complete forecast construction succeeds;
   policy/candidate/fold changes invalidate any previous forecast. */
int64_t shy_engine_open(const char *series);
/* Borrows input for the call; owns a validated copy. Uses compact JSON bytes
   for series_blake3 when the source is an in-memory ledger cohort. */
int64_t shy_engine_open_record(const mulib_json_t *);
int64_t shy_engine_selection(const char *audit);
int64_t shy_engine_bucket_number(int64_t bucket,int64_t evidence,const char *key);
int64_t shy_engine_close(void);
int64_t shy_engine_policy(const char *policy);
int64_t shy_engine_candidate(const char *candidate);
int64_t shy_engine_fold(int64_t candidate, int64_t training, int64_t horizon, int64_t validation);
int64_t shy_engine_complete(int64_t candidate);
int64_t shy_engine_compare(int64_t a, int64_t b, const char *metric);
int64_t shy_engine_finish(int64_t selected, int64_t horizon, int64_t rank, int64_t coverage);
int64_t shy_engine_emit(void);
int64_t shy_engine_reference(void);
const char *shy_engine_field(const char *key);
int64_t shy_engine_number(const char *key);
int64_t shy_engine_pricing_policy(const char *policy);
int shy_engine_demand(const mulib_json_t *demand);
int64_t shy_engine_take_demand(void);
int64_t shy_engine_optimize(int64_t low, int64_t high, int64_t tick, int64_t cost, const char *objective);
int64_t shy_engine_decision(const char *status, int64_t proposed, int64_t formula_rate,
    int64_t final_rate, const char *formula_audit, const char *quote);
int64_t shy_money_operation(const char *operation, int64_t a, int64_t b,
    int64_t denominator, const char *currency, int64_t scale);
int64_t shy_model_optimize(const char *policy, int64_t low, int64_t high);
#endif
