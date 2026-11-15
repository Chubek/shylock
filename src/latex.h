#ifndef SHYLOCK_LATEX_H
#define SHYLOCK_LATEX_H
#include <stdint.h>
/* One owning, non-reentrant document. Append borrows NUL-free text for the call;
   MuLIB copies/escapes it. Level 0 means paragraph, 1..3 mean section headings.
   Errors poison publication until close/begin. No output precedes publish.
   Digest storage is borrowed until its next call. close is idempotent. */
int64_t shy_latex_begin(const char *title);
int64_t shy_latex_append(int64_t level, const char *text);
int64_t shy_latex_finish(void);
const char *shy_latex_digest(void);
int64_t shy_latex_publish(void);
int64_t shy_latex_close(void);
#endif
