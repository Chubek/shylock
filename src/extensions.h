#ifndef SHYLOCK_EXTENSIONS_H
#define SHYLOCK_EXTENSIONS_H
#include "shylock.h"

/* Explicit opt-in host. Events borrow JSON for the call. Queueing copies it.
   Each extension owns a separate Mune state, never shared with formulas/UI.
   Hooks cannot change the event, a price, a review, or a host permission. */
int64_t shy_extensions_begin(const char *configuration);
int shy_extensions_event(const char *event, const mulib_json_t *payload);
int shy_extensions_enqueue(const char *event, const mulib_json_t *payload);
int64_t shy_extensions_drain(void);
int64_t shy_extensions_end(void);
int64_t shy_extension_command(const char *configuration, const char *event, const char *payload_path);
#endif
