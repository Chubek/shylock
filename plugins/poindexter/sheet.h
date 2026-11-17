#ifndef SHYLOCK_SHEET_H
#define SHYLOCK_SHEET_H
#include "../../src/shylock.h"
enum { SHY_SHEET_COLUMNS = 25, SHY_SHEET_ROWS = 16384 };
typedef struct { const char *key, *label, *type; int editable; } shy_sheet_column;
extern const shy_sheet_column shy_sheet_columns[SHY_SHEET_COLUMNS];
int64_t shy_sheet_load(const char *cohort);
int64_t shy_sheet_close(void);
int64_t shy_sheet_move(int64_t rows, int64_t columns);
int64_t shy_sheet_select_cell(int64_t row, const char *column);
/* On1x supplies a validated JSON scalar; native code owns the staged copy. */
int64_t shy_sheet_stage_value(const char *key, const char *json);
int64_t shy_sheet_text_valid(const char *text);
const char *shy_sheet_original(const char *key);
int64_t shy_sheet_committed(void);
int64_t shy_sheet_discard(void);
int64_t shy_sheet_dirty(void);
int64_t shy_sheet_filter(const char *text);
int64_t shy_sheet_sort(void);
int64_t shy_sheet_prepare_revision(void);
int64_t shy_sheet_notify(const char *text);
int64_t shy_sheet_event(const char *name);
const char *shy_sheet_key(void);
const char *shy_sheet_decision(void);
const char *shy_sheet_reason(void);
const char *shy_sheet_message(void);
size_t shy_sheet_count(void);
size_t shy_sheet_row(void);
size_t shy_sheet_column_index(void);
/* Borrowed typed cells remain valid until stage, reload or close. */
const mulib_json_t *shy_sheet_cell(size_t row, size_t column);
void shy_sheet_text(size_t row, size_t column, char *out, size_t capacity);
mulib_json_t *shy_sheet_export(void);
uint64_t shy_sheet_cell_id(size_t row, size_t column);
#endif
