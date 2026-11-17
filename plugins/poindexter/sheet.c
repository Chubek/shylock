#include "sheet.h"
#include "poindexter.h"
#include "../../src/extensions.h"
#include "mulib-utf8.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

const shy_sheet_column shy_sheet_columns[SHY_SHEET_COLUMNS] = {
    { "listing_id", "Listing", "text", 1 }, { "minor", "Amount (minor)", "integer", 0 },
    { "currency", "Currency", "text", 1 }, { "scale", "Scale", "integer", 0 },
    { "stay_start", "Stay start", "date", 1 }, { "stay_end", "Stay end", "date", 1 },
    { "guests", "Guests", "integer", 1 }, { "taxes", "Taxes", "scope", 1 },
    { "fees", "Fees", "scope", 1 }, { "basis", "Rate basis", "text", 1 },
    { "qualifier", "Qualifier", "text", 1 }, { "decision", "Decision", "decision", 1 },
    { "captured_at", "Captured at", "integer", 0 }, { "provenance", "Provenance", "text", 0 },
    { "review_id", "Review ID", "text", 0 }, { "observation_id", "Observation ID", "text", 0 },
    { "reviewer", "Reviewer", "text", 0 }, { "reason", "Review reason", "text", 0 },
    { "conflict_resolution", "Conflict resolution", "text", 1 }, { "cohort", "Cohort", "text", 0 },
    { "source_url", "Source URL", "text", 0 }, { "extraction_id", "Extraction ID", "text", 0 },
    { "capture_id", "Capture ID", "text", 0 }, { "collection_id", "Collection ID", "text", 0 },
    { "find_id", "Find ID", "text", 0 }
};
static mulib_json_t *rows, *pending;
static size_t view[SHY_SHEET_ROWS], count, active_row, active_column, sort_column;
static int descending, refresh_required;
static char filter[257], cohort_name[129], note[1025], decision[16], message[256];

static mulib_json_t *row_at(size_t index)
{
    return index < count ? mulib_json_at(rows, view[index]) : NULL;
}
static const mulib_json_t *published_cell(const mulib_json_t *row,const char *key)
{
    if (!strcmp(key,"collection_id") || !strcmp(key,"find_id")) return mulib_json_get(mulib_json_get(row,"collection"),key);
    const mulib_json_t *value=mulib_json_get(row,key);
    return value || strcmp(key,"conflict_resolution") ? value : mulib_json_get(mulib_json_get(row,"review_context"),key);
}
const char *shy_sheet_original(const char *key) { return shy_text(mulib_json_get(row_at(active_row),"evidence"),key); }
int64_t shy_sheet_text_valid(const char *text)
{
    size_t n=strlen(text),pos=0;
    while (pos<n) {
        uint32_t cp; size_t used;
        if (!mulib_utf8_decode((const unsigned char *)text+pos,n-pos,&cp,&used) || cp<32 || (cp>=127 && cp<=159)) return 0;
        pos+=used;
    }
    return 1;
}

static int fail(const char *text)
{
    snprintf(message, sizeof message, "%s", text);
    return 2;
}

int64_t shy_sheet_notify(const char *text)
{
    snprintf(message, sizeof message, "%s", text);
    return 0;
}
const char *shy_sheet_message(void) { return message; }
const char *shy_sheet_key(void) { return shy_sheet_columns[active_column].key; }
size_t shy_sheet_count(void) { return count; }
size_t shy_sheet_row(void) { return active_row; }
size_t shy_sheet_column_index(void) { return active_column; }
int64_t shy_sheet_dirty(void) { return mulib_json_size(pending) || *note || *decision; }
const char *shy_sheet_reason(void) { return note; }
const char *shy_sheet_decision(void) { return *decision ? decision : shy_text(row_at(active_row), "decision"); }

static int compare_rows(const void *a, const void *b)
{
    const mulib_json_t *x = mulib_json_at(rows, *(const size_t *)a), *y = mulib_json_at(rows, *(const size_t *)b);
    const char *key = shy_sheet_columns[sort_column].key;
    const mulib_json_t *vx = published_cell(x, key), *vy = published_cell(y, key);
    int order;
    if (mulib_json_typeof(vx) == MULIB_JSON_NUMBER && mulib_json_typeof(vy) == MULIB_JSON_NUMBER) {
        double nx = mulib_json_numeric(vx), ny = mulib_json_numeric(vy);
        order = nx < ny ? -1 : nx > ny;
    } else {
        const char *sx=mulib_json_text(vx,NULL),*sy=mulib_json_text(vy,NULL);
        order=strcmp(sx ? sx : "",sy ? sy : "");
    }
    if (order) return descending ? -order : order;
    return strcmp(shy_text(x, "observation_id"), shy_text(y, "observation_id"));
}

static int contains(const char *text, const char *needle)
{
    if (!*needle) return 1;
    for (; *text; ++text) {
        size_t i = 0;
        while (needle[i] && text[i]) {
            unsigned char a = (unsigned char)text[i], b = (unsigned char)needle[i];
            if (a >= 'A' && a <= 'Z') a += 32;
            if (b >= 'A' && b <= 'Z') b += 32;
            if (a != b) break;
            ++i;
        }
        if (!needle[i]) return 1;
    }
    return 0;
}

static void rebuild(const char *preserve)
{
    count = 0;
    for (size_t i = 0; i < mulib_json_size(rows); ++i) {
        const mulib_json_t *row = mulib_json_at(rows, i);
        int match = !*filter;
        for (size_t j = 0; !match && j < SHY_SHEET_COLUMNS; ++j) {
            const mulib_json_t *value = published_cell(row, shy_sheet_columns[j].key);
            if (mulib_json_typeof(value) == MULIB_JSON_STRING) match = contains(mulib_json_text(value,NULL), filter);
            else if (mulib_json_typeof(value) == MULIB_JSON_NUMBER) {
                char number[64]; snprintf(number, sizeof number, "%.0f", mulib_json_numeric(value));
                match = contains(number, filter);
            }
        }
        if (match) view[count++] = i;
    }
    if (count) qsort(view, count, sizeof *view, compare_rows);
    if (active_row >= count) active_row = count ? count-1 : 0;
    for (size_t i = 0; preserve && *preserve && i < count; ++i)
        if (!strcmp(shy_text(row_at(i), "observation_id"), preserve)) { active_row = i; break; }
}

int64_t shy_sheet_load(const char *cohort)
{
    if (strlen(cohort) > 128 || shy_store_select(cohort) < 0) return fail("Cannot load worksheet selection");
    mulib_json_t *copy = shy_store_rows();
    if (!copy) return fail("Cannot allocate worksheet snapshot");
    char preserve[65]; snprintf(preserve, sizeof preserve, "%s", shy_text(row_at(active_row), "observation_id"));
    mulib_json_destroy(rows); rows = copy;
    snprintf(cohort_name, sizeof cohort_name, "%s", cohort);
    rebuild(preserve);
    refresh_required=0;
    return 0;
}

int64_t shy_sheet_discard(void)
{
    mulib_json_destroy(pending); pending = NULL;
    *note = 0; *decision = 0;
    return shy_sheet_notify("Staged edits discarded; published evidence was not changed");
}
int64_t shy_sheet_committed(void)
{
    shy_sheet_discard(); refresh_required=1;
    return 0;
}

int64_t shy_sheet_close(void)
{
    mulib_json_destroy(rows); rows = NULL;
    mulib_json_destroy(pending); pending = NULL;
    count = active_row = active_column = sort_column = 0; descending = refresh_required = 0;
    *filter = *cohort_name = *note = *decision = *message = 0;
    return 0;
}

int64_t shy_sheet_move(int64_t dr, int64_t dc)
{
    if (dr && shy_sheet_dirty()) return fail("Save or discard this row's staged edits before changing rows");
    int64_t row = (int64_t)active_row+dr, column = (int64_t)active_column+dc;
    if (row < 0) row = 0;
    if ((size_t)row >= count) row = count ? (int64_t)count-1 : 0;
    if (column < 0) column = 0;
    if (column >= SHY_SHEET_COLUMNS) column = SHY_SHEET_COLUMNS-1;
    active_row = (size_t)row; active_column = (size_t)column;
    return 0;
}

int64_t shy_sheet_select_cell(int64_t row, const char *column)
{
    if (row < 0 || (size_t)row >= count) return fail("Worksheet row is outside the selection");
    if ((size_t)row != active_row && shy_sheet_dirty()) return fail("Save or discard this row's staged edits before changing rows");
    size_t i;
    for (i = 0; i < SHY_SHEET_COLUMNS; ++i) if (!strcmp(shy_sheet_columns[i].key, column)) break;
    if (i == SHY_SHEET_COLUMNS) return fail("Unknown worksheet column");
    active_row = (size_t)row; active_column = i;
    return 0;
}

int64_t shy_sheet_filter(const char *text)
{
    if (shy_sheet_dirty()) return fail("Save or discard staged edits before filtering");
    if (strlen(text) > 256) return fail("Filter exceeds 256 bytes");
    snprintf(filter, sizeof filter, "%s", text); active_row = 0;
    rebuild(NULL);
    return 0;
}

int64_t shy_sheet_sort(void)
{
    if (shy_sheet_dirty()) return fail("Save or discard staged edits before sorting");
    char preserve[65]; snprintf(preserve, sizeof preserve, "%s", shy_text(row_at(active_row), "observation_id"));
    if (sort_column == active_column) descending = !descending;
    else { sort_column = active_column; descending = 0; }
    rebuild(preserve);
    return 0;
}

int64_t shy_sheet_stage_value(const char *key, const char *json)
{
    if (refresh_required) return fail("Review committed; refresh successfully before staging another edit");
    if (!row_at(active_row)) return fail("The worksheet has no selected row");
    if (strcmp(shy_text(row_at(active_row), "schema"), "shylock.observation/v2"))
        return fail("Legacy flat records are read-only; re-import evidence for revision support");
    if (strlen(json)>8192) return fail("Staged scalar exceeds native bound");
    mulib_json_t *value=mulib_json_parse(json,strlen(json),NULL);
    size_t length=0; const char *text=mulib_json_text(value,&length);
    if (!strcmp(key,"$reason") || !strcmp(key,"decision")) {
        char *target=!strcmp(key,"$reason") ? note : decision;
        size_t capacity=!strcmp(key,"$reason") ? sizeof note : sizeof decision;
        int valid=text && length && strlen(text)==length && length<capacity && shy_sheet_text_valid(text);
        if (valid) memcpy(target,text,length+1);
        mulib_json_destroy(value);
        return valid ? shy_sheet_notify("Review value staged; save explicitly to publish") : fail("Invalid staged text scalar");
    }
    size_t i;
    for (i = 0; i < SHY_SHEET_COLUMNS; ++i) if (!strcmp(shy_sheet_columns[i].key, key)) break;
    if (i == SHY_SHEET_COLUMNS || !shy_sheet_columns[i].editable) { mulib_json_destroy(value); return fail("Evidence and audit cells are read-only"); }
    int valid=!strcmp(key,"guests") ? mulib_json_typeof(value)==MULIB_JSON_NUMBER :
        text && length && strlen(text)==length && length<=256 && shy_sheet_text_valid(text);
    if (!valid) { mulib_json_destroy(value); return fail("Invalid staged scalar"); }
    if (!pending) pending = mulib_json_new(MULIB_JSON_OBJECT);
    if (!shy_add(pending, key, value)) return fail("Cannot allocate staged edit");
    return shy_sheet_notify("Edit staged; add a reason with n, then save with s");
}

int64_t shy_sheet_prepare_revision(void)
{
    if (refresh_required) return 0;
    if (!shy_sheet_dirty() || !*note) return fail("Staged edits require an explicit review reason (n) before saving");
    const mulib_json_t *row = row_at(active_row);
    mulib_json_t *empty = pending ? NULL : mulib_json_new(MULIB_JSON_OBJECT);
    int ok = row && shy_revision_load_data(shy_text(row, "observation_id"), shy_text(row, "review_id"), pending ? pending : empty);
    mulib_json_destroy(empty);
    return ok ? 1 : 0;
}

const mulib_json_t *shy_sheet_cell(size_t row, size_t column)
{
    if (column >= SHY_SHEET_COLUMNS) return NULL;
    const char *key = shy_sheet_columns[column].key;
    if (row == active_row && strcmp(key, "decision")) {
        const mulib_json_t *value = mulib_json_get(pending, key);
        if (value) return value;
    }
    return published_cell(row_at(row), key);
}

void shy_sheet_text(size_t row, size_t column, char *out, size_t capacity)
{
    if (!capacity) return;
    const mulib_json_t *value = shy_sheet_cell(row, column);
    const char *text = NULL; char number[64];
    if (column == 11 && row == active_row && *decision) text = decision;
    else if (mulib_json_typeof(value) == MULIB_JSON_STRING) text = mulib_json_text(value, NULL);
    else if (mulib_json_typeof(value) == MULIB_JSON_NUMBER) {
        snprintf(number, sizeof number, "%.0f", mulib_json_numeric(value)); text = number;
    }
    if (!text) text = "?";
    size_t n = strlen(text), used = 0, pos = 0;
    while (pos < n) {
        uint32_t cp; size_t length;
        if (!mulib_utf8_decode((const unsigned char *)text+pos, n-pos, &cp, &length)) break;
        if (cp < 32 || (cp >= 127 && cp <= 159)) {
            if (used+1 >= capacity) break;
            out[used++] = ' ';
        } else {
            if (used+length >= capacity) break;
            memcpy(out+used, text+pos, length); used += length;
        }
        pos += length;
    }
    out[used] = 0;
}

mulib_json_t *shy_sheet_export(void)
{
    mulib_json_t *out = mulib_json_new(MULIB_JSON_OBJECT), *columns = mulib_json_new(MULIB_JSON_ARRAY), *data = mulib_json_new(MULIB_JSON_ARRAY);
    for (size_t j = 0; j < SHY_SHEET_COLUMNS; ++j) {
        mulib_json_t *column = mulib_json_new(MULIB_JSON_OBJECT);
        int ok = shy_add(column, "key", mulib_json_string(shy_sheet_columns[j].key)) &&
            shy_add(column, "label", mulib_json_string(shy_sheet_columns[j].label)) &&
            shy_add(column, "type", mulib_json_string(shy_sheet_columns[j].type)) &&
            shy_add(column, "editable", mulib_json_bool(shy_sheet_columns[j].editable));
        if (!ok) mulib_json_destroy(column);
        if (!ok || !shy_push(columns, column)) goto fail;
    }
    for (size_t i = 0; i < count; ++i) {
        mulib_json_t *row = mulib_json_new(MULIB_JSON_OBJECT);
        for (size_t j = 0; j < SHY_SHEET_COLUMNS; ++j) {
            const mulib_json_t *value = published_cell(row_at(i), shy_sheet_columns[j].key);
            if (!shy_add(row, shy_sheet_columns[j].key, value ? shy_clone(value) : mulib_json_new(MULIB_JSON_NULL))) {
                mulib_json_destroy(row); goto fail;
            }
        }
        if (!shy_push(data, row)) goto fail;
    }
    if (!shy_add(out, "schema", mulib_json_string("shylock.worksheet/v1")) ||
        !shy_add(out, "cohort", mulib_json_string(cohort_name)) ||
        !shy_add(out, "filter", mulib_json_string(filter)) ||
        !shy_add(out, "sort_column", mulib_json_string(shy_sheet_columns[sort_column].key)) ||
        !shy_add(out, "descending", mulib_json_bool(descending)) ||
        !shy_add(out, "selected_row", mulib_json_number((double)active_row)) ||
        !shy_add(out, "selected_column", mulib_json_string(shy_sheet_key())) ||
        !shy_add(out, "pending", pending ? shy_clone(pending) : mulib_json_new(MULIB_JSON_OBJECT)) ||
        !shy_add(out, "pending_decision", mulib_json_string(decision)) ||
        !shy_add(out, "pending_reason", mulib_json_string(note)) ||
        !shy_add(out, "refresh_required", mulib_json_bool(refresh_required)) ||
        !shy_add(out, "message", mulib_json_string(message))) goto fail;
    if (!mulib_json_set(out, "columns", columns)) goto fail;
    columns = NULL;
    if (!mulib_json_set(out, "rows", data)) goto fail;
    return out;
fail:
    mulib_json_destroy(out); mulib_json_destroy(columns); mulib_json_destroy(data); return NULL;
}

int64_t shy_sheet_emit(void) { return shy_emit(shy_sheet_export()); }

uint64_t shy_sheet_cell_id(size_t row, size_t column)
{
    const char *id = shy_text(row_at(row), "observation_id");
    uint64_t prefix = 0;
    for (size_t i = 0; i < 12 && id[i]; ++i) {
        unsigned digit = id[i] >= '0' && id[i] <= '9' ? (unsigned)(id[i]-'0') : (unsigned)(id[i]-'a'+10);
        prefix = (prefix<<4) | (digit & 15u);
    }
    return 8+(prefix<<5)+column;
}

int64_t shy_sheet_event(const char *name)
{
    mulib_json_t *payload = mulib_json_new(MULIB_JSON_OBJECT);
    const mulib_json_t *row = row_at(active_row);
    int ok = shy_add(payload, "cohort", mulib_json_string(cohort_name)) &&
        shy_add(payload, "rows", mulib_json_number((double)count)) &&
        shy_add(payload, "observation_id", mulib_json_string(shy_text(row, "observation_id"))) &&
        shy_add(payload, "review_id", mulib_json_string(shy_text(row, "review_id")));
    if (ok) ok = shy_extensions_event(name, payload);
    mulib_json_destroy(payload);
    return ok ? 0 : 2;
}
