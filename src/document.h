#ifndef SHYLOCK_DOCUMENT_H
#define SHYLOCK_DOCUMENT_H
#include "shylock.h"

/* Single-threaded scalar ABI session: open owns one immutable artifact and
   replaces any prior session. child selects a borrowed subtree. close is
   idempotent. Text views live until close; json/quote scratch lives until the
   next json/quote call. id/ratio have separate, overwritten static storage.
   Copy scratch into On1x buffers immediately. Missing/wrong types are recoverable.
   An index of -1 denotes the section itself, and an empty key denotes the value. */
int64_t shy_document_open(const char *path);
/* parse borrows independent caller text, never a view into the current session. */
int64_t shy_document_parse(const char *text);
/* C-only borrowed immutable artifact; valid until the session is closed/replaced. */
const mulib_json_t *shy_document_borrow(void);
int64_t shy_document_close(void);
/* Sticky serialization status, reset only by close/open/parse. Check before
   replacing a document whose borrowed data contributed to a decision. */
int64_t shy_document_ok(void);
int64_t shy_document_has(const char *section, int64_t index, const char *key, const char *kind);
int64_t shy_document_size(const char *section, int64_t index, const char *key);
const char *shy_document_text(const char *section, int64_t index, const char *key);
int64_t shy_document_number(const char *section, int64_t index, const char *key);
int64_t shy_document_child(const char *key);
int64_t shy_document_reset(void);
int64_t shy_document_index(int64_t index);
int64_t shy_document_boolean(const char *section, int64_t index, const char *key);
const char *shy_document_json_at(const char *section, int64_t index, const char *key);
const char *shy_document_json(void);
const char *shy_document_id(void);
const char *shy_json_quote(const char *text);
const char *shy_json_ratio(int64_t numerator, int64_t denominator);
int64_t shy_document_seal(const char *text, const char *field, int64_t emit);
int64_t shy_document_optimize(int64_t floor, int64_t cap, int64_t tick, int64_t cost);
#endif
