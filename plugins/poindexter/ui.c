#define _POSIX_C_SOURCE 200809L
#include "ui.h"
#include "mulib-utf8.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static mulib_json_t *script, *results;
static size_t event_index, frames;
static int graphical, scripted, action_failures, editor_mode, opened;
static char editor[1025];
static shy_ui_event current;
static char *frame_path;

int shy_ui_editing(void) { return editor_mode != 0; }
const char *shy_ui_editor(void) { return editor; }
const char *shy_ui_prompt(void)
{
    return editor_mode == 1 ? shy_sheet_key() : editor_mode == 2 ? "Review reason" : editor_mode == 3 ? "Filter" : "";
}

int shy_ui_key(shy_ui_event *event, const char *key, const char *text)
{
    event->action = "";
    if (editor_mode) {
        size_t n = strlen(editor);
        if (!strcmp(key, "escape")) { editor_mode = 0; *editor = 0; return 1; }
        if (!strcmp(key, "backspace")) {
            if (n) { do { --n; } while (n && ((unsigned char)editor[n] & 0xc0) == 0x80); editor[n] = 0; }
            return 1;
        }
        if (!strcmp(key, "enter")) {
            event->action = editor_mode == 1 ? "edit" : editor_mode == 2 ? "reason" : "filter";
            snprintf(event->text, sizeof event->text, "%s", editor); editor_mode = 0; return 1;
        }
        if (text && *text) {
            size_t m = strlen(text);
            int safe = mulib_utf8_validate((const unsigned char *)text, m);
            for (size_t i = 0; safe && i < m; ++i) if ((unsigned char)text[i] < 32 || (unsigned char)text[i] == 127) safe = 0;
            if (safe && n+m < sizeof editor) memcpy(editor+n, text, m+1);
        }
        return 1;
    }
    if (!strcmp(key, "enter") || !strcmp(key, "edit")) {
        if (!shy_sheet_columns[shy_sheet_column_index()].editable) {
            shy_sheet_notify("Evidence and audit cells are read-only"); return 1;
        }
        shy_sheet_text(shy_sheet_row(), shy_sheet_column_index(), editor, sizeof editor);
        if (!strcmp(editor, "?")) *editor = 0;
        editor_mode = 1; return 1;
    }
    if (!strcmp(key, "reason") || !strcmp(key, "n")) {
        snprintf(editor, sizeof editor, "%s", shy_sheet_reason()); editor_mode = 2; return 1;
    }
    if (!strcmp(key, "filter") || !strcmp(key, "/")) {
        if (shy_sheet_dirty()) { shy_sheet_notify("Save or discard staged edits before filtering"); return 1; }
        *editor = 0; editor_mode = 3; return 1;
    }
    static const struct { const char *key, *action; } keys[] = {
        { "up", "up" }, { "k", "up" }, { "down", "down" }, { "j", "down" },
        { "left", "left" }, { "h", "left" }, { "right", "right" }, { "l", "right" },
        { "page_up", "page_up" }, { "page_down", "page_down" }, { "home", "home" },
        { "end", "last" }, { "tab", "right" }, { "s", "save" }, { "save", "save" },
        { "r", "refresh" }, { "refresh", "refresh" }, { "d", "discard" }, { "discard", "discard" },
        { "o", "sort" }, { "sort", "sort" }, { "q", "quit" }, { "quit", "quit" }, { "escape", "quit" }
    };
    for (size_t i = 0; i < sizeof keys / sizeof *keys; ++i)
        if (!strcmp(key, keys[i].key)) { event->action = keys[i].action; return 1; }
    if (!strcmp(key, "?")) shy_sheet_notify("Arrows: cells | Enter: edit | n: reason | s: save | /: filter | o: sort | r: refresh | d: discard | q: quit");
    return 1;
}

static int render(void)
{
    int ok = graphical ? shy_gui_render(&current) : shy_tui_render();
    if (ok) ++frames;
    return ok;
}

int64_t shy_ui_open(const char *kind, const char *events_path, const char *snapshot_path)
{
    graphical = !strcmp(kind, "gui"); scripted = *events_path != 0;
    if (strcmp(kind, "gui") && strcmp(kind, "tui")) return 0;
    if (*snapshot_path && (!graphical || !scripted)) return 0;
    if (scripted) {
        size_t n; char *text = shy_read(events_path, SHY_PAGE_LIMIT, &n);
        script = text ? mulib_json_parse(text, n, NULL) : NULL; free(text);
        const mulib_json_t *events = mulib_json_get(script, "events");
        if (!script || strcmp(shy_text(script, "schema"), "shylock.ui-events/v1") ||
            mulib_json_typeof(events) != MULIB_JSON_ARRAY || mulib_json_size(events) > 4096) return 0;
        results = mulib_json_new(MULIB_JSON_ARRAY);
        if (!results) return 0;
    }
    if (*snapshot_path) { frame_path = strdup(snapshot_path); if (!frame_path) return 0; }
    opened = graphical ? shy_gui_open(scripted) : shy_tui_open(scripted);
    return opened;
}

const char *shy_ui_next(void)
{
    memset(&current, 0, sizeof current); current.action = "";
    if (!render()) return "fatal";
    if (current.action && *current.action) return current.action;
    if (!scripted) {
        int ok = graphical ? shy_gui_next(&current) : shy_tui_next(&current);
        return ok ? current.action : "fatal";
    }
    const mulib_json_t *events = mulib_json_get(script, "events");
    if (event_index == mulib_json_size(events)) return "end";
    const mulib_json_t *event = mulib_json_at(events, event_index++);
    const char *action = shy_text(event, "action"), *text = shy_text(event, "text"), *column = shy_text(event, "column");
    if ((mulib_json_get(event, "text") && mulib_json_typeof(mulib_json_get(event, "text")) != MULIB_JSON_STRING) ||
        (mulib_json_get(event, "column") && mulib_json_typeof(mulib_json_get(event, "column")) != MULIB_JSON_STRING)) return "invalid";
    if (!*action || strlen(text) > 1024 || strlen(column) >= sizeof current.column) return "invalid";
    snprintf(current.text, sizeof current.text, "%s", text);
    snprintf(current.column, sizeof current.column, "%s", column);
    current.row = shy_integer(event, "row", SHY_SHEET_ROWS);
    if (!strcmp(action, "key")) {
        (void)shy_ui_key(&current, shy_text(event, "key"), text);
        return current.action;
    }
    if (!strcmp(action, "resize")) {
        int64_t w = shy_integer(event, "width", 1920), h = shy_integer(event, "height", 1200);
        int ok = w > 0 && h > 0 && (graphical ? shy_gui_resize((size_t)w, (size_t)h) : shy_tui_resize((size_t)w, (size_t)h));
        return ok ? "resize" : "invalid";
    }
    if (!strcmp(action, "pointer")) {
        int64_t x = shy_integer(event, "x", 1920), y = shy_integer(event, "y", 1200);
        const mulib_json_t *down = mulib_json_get(event, "down");
        if (!graphical || x < 0 || y < 0 || mulib_json_typeof(down) != MULIB_JSON_BOOL) return "invalid";
        shy_gui_pointer((int)x, (int)y, mulib_json_boolean(down));
        return "pointer";
    }
    current.action = action;
    return current.action;
}

const char *shy_ui_text(void) { return current.text; }
const char *shy_ui_column(void) { return current.column; }
int64_t shy_ui_row(void) { return current.row; }

int64_t shy_ui_result(const char *action, int64_t status)
{
    if (status && (scripted || !strcmp(action, "fatal"))) ++action_failures;
    if (!scripted || !*action) return 0;
    mulib_json_t *result = mulib_json_new(MULIB_JSON_OBJECT);
    int ok = shy_add(result, "action", mulib_json_string(action)) &&
        shy_add(result, "status", mulib_json_number((double)status)) &&
        shy_add(result, "message", mulib_json_string(shy_sheet_message()));
    if (!ok) mulib_json_destroy(result);
    if (!ok || !shy_push(results, result)) { ++action_failures; return 2; }
    return 0;
}

int64_t shy_ui_finish(void)
{
    int ok = opened && render();
    char digest[65] = { 0 };
    if (opened) {
        if (graphical) {
            shy_gui_hash(digest);
            if (frame_path && !shy_gui_frame(frame_path)) ok = 0;
        } else shy_tui_hash(digest);
        if (!*digest) ok = 0;
    }
    if (graphical) shy_gui_close(); else shy_tui_close();
    int64_t status = ok && !action_failures ? 0 : 2;
    if (scripted) {
        mulib_json_t *out = shy_sheet_export();
        mulib_json_t *rendered = mulib_json_new(MULIB_JSON_OBJECT);
        int built = out && shy_add(rendered, "backend", mulib_json_string(graphical ? "mulib-immgtk" : "mulib-tui")) &&
            shy_add(rendered, "frames", mulib_json_number((double)frames)) &&
            shy_add(rendered, "blake3", mulib_json_string(digest));
        if (!built) mulib_json_destroy(rendered);
        if (!built || !shy_add(out, "render", rendered)) { mulib_json_destroy(out); out = NULL; }
        mulib_json_t *audit = results; results = NULL;
        if (!out) mulib_json_destroy(audit);
        else if (!shy_add(out, "actions", audit)) { mulib_json_destroy(out); out = NULL; }
        if (!out || shy_emit(out)) status = 2;
    }
    mulib_json_destroy(script); script = NULL;
    mulib_json_destroy(results); results = NULL;
    free(frame_path); frame_path = NULL;
    event_index = frames = 0; editor_mode = action_failures = opened = scripted = graphical = 0;
    *editor = 0;
    return status;
}
