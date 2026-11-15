#ifndef SHYLOCK_POINDEXTER_H
#define SHYLOCK_POINDEXTER_H
#include "shylock.h"
int64_t shy_store_open(const char *path);
int64_t shy_store_close(void);
int64_t shy_store_select(const char *cohort);
/* Copy the current selection; caller owns the copy across store_close(). */
mulib_json_t *shy_store_rows(void);
int64_t shy_revision_load_data(const char *observation, const char *expected, const mulib_json_t *context);
#endif
