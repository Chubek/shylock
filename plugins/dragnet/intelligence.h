#ifndef SHYLOCK_DRAGNET_INTELLIGENCE_H
#define SHYLOCK_DRAGNET_INTELLIGENCE_H
#include "../../src/shylock.h"
#include "mulib-https.h"
/* Owned results. Input trees/views borrowed only during synchronous calls. */
mulib_json_t *shy_flatten(const mulib_json_t *snapshot);
mulib_json_t *shy_flat_source(const char *path);
mulib_json_t *shy_find_information(const mulib_json_t *ir,const char *target);
int shy_intelligence_seal(mulib_json_t *,const char *);
int64_t shy_flatten_command(const char *);
int64_t shy_find_command(const char *,const char *);
int64_t shy_openai_account(const char *);
/* Connector injection is C-only for tests/embedding, never an env bypass.
   NULL connector selects authenticated, public-address-pinned TLS. */
mulib_json_t *shy_model_information(const mulib_json_t *,const mulib_json_t *,const char *,
    mulib_https_connect_fn,void *,int *);
int64_t shy_model_command(const char *,const char *,const char *);
typedef struct {
    char base_url[4097],host[256];
    unsigned port;
} shy_openai_endpoint;
/* Copy/validate OPENAI_BASE_URL once per operation. Empty/unset selects OpenAI.
   No DNS or network here; failure leaves the caller's output unchanged. */
int shy_openai_endpoint_parse(const char *,shy_openai_endpoint *);
int shy_openai_endpoint_env(shy_openai_endpoint *);
int shy_openai_connect(void *,const mulib_uri_t *,mulib_tsl_t *);
int64_t shy_browser_begin(const char *job,const char *directory);
int64_t shy_browser_step(const char *action,const char *selector,const char *value);
int64_t shy_browser_finish(int64_t success);
int64_t shy_browser_close(void);
#endif
