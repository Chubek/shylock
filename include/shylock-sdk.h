#ifndef SHYLOCK_SDK_H
#define SHYLOCK_SDK_H
#include <stddef.h>
#include <stdint.h>

/* Standalone C11 ABI: no MuLIB definitions, host symbols or implementation TU.
   Native plugins are trusted code, NOT sandboxed. All calls are synchronous on
   one host thread. Strings are NUL-terminated UTF-8; extents exclude the NUL. */
#define SHY_SDK_ABI 1u
#define SHY_SDK_ENTRY "shylock_plugin_query"
#if defined(_WIN32)
#define SHY_SDK_EXPORT __declspec(dllexport)
#else
#define SHY_SDK_EXPORT __attribute__((visibility("default")))
#endif
typedef struct shy_sdk_sink shy_sdk_sink;
typedef struct {
    uint32_t abi;
    size_t size;
    const char *bytes; size_t length;
    const char *url, *blake3, *rules_json, *model_reply_json;
} shy_sdk_capture;
typedef struct {
    size_t size;
    const char *method, *amount_text, *currency, *basis, *qualifier;
    size_t byte_offset, byte_length;
    int has_minor;
    int64_t minor;
    const char *json_pointer; /* NULL if not structured evidence */
} shy_sdk_candidate;
typedef struct {
    uint32_t abi;
    size_t size;
    /* Allocations belong to the caller; free with this same table, not libc. */
    void *(*allocate)(size_t);
    void (*release)(void *);
    void (*digest)(const char *, size_t, char out[65]);
    /* Sink is borrowed during extract only. These COPY input immediately.
       A failed call poisons the transaction even if a plugin ignores failure. */
    int (*candidate)(shy_sdk_sink *, const shy_sdk_candidate *);
    int (*diagnostic)(shy_sdk_sink *, const char *code, const char *message);
} shy_sdk_host;
typedef struct {
    uint32_t abi;
    size_t size;
    const char *name, *version;
    uint64_t flags; /* must be zero in v1 */
    /* Config/host borrowed for on_load; host table itself has process lifetime.
       On failure, free all allocations and leave *state NULL. No extraction
       occurs. on_unload runs exactly once after each successful on_load. */
    int (*on_load)(const shy_sdk_host *, const char *configuration_json, void **state);
    int (*extract)(void *state, const shy_sdk_capture *, shy_sdk_sink *);
    void (*on_unload)(void *state);
} shy_sdk_plugin;
/* Export this function. Return NULL for unsupported ABI; no effects here.
   Descriptor and its strings live until unload. Never retain capture/sink views.
   Output is transactional: any callback failure or extract!=1 discards ALL rows.
   Candidate amounts are checked against canonical MuLIB money by the host.
   Optional appended fields are permitted only with a future negotiated ABI. */
typedef const shy_sdk_plugin *(*shy_sdk_query_fn)(uint32_t abi);
SHY_SDK_EXPORT const shy_sdk_plugin *shylock_plugin_query(uint32_t abi);
#endif
