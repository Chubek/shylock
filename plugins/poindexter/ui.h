#ifndef SHYLOCK_UI_H
#define SHYLOCK_UI_H
#include "sheet.h"
typedef struct {
    const char *action;
    char text[1025], column[64];
    int64_t row;
} shy_ui_event;

/* Shared editor/input state; frontends never write observations. */
int shy_ui_key(shy_ui_event *event, const char *key, const char *text);
const char *shy_ui_prompt(void);
const char *shy_ui_editor(void);
int shy_ui_editing(void);
int shy_tui_open(int scripted);
int shy_tui_render(void);
int shy_tui_next(shy_ui_event *event);
int shy_tui_resize(size_t width, size_t height);
void shy_tui_hash(char out[65]);
void shy_tui_close(void);
int shy_gui_open(int scripted);
int shy_gui_render(shy_ui_event *event);
int shy_gui_next(shy_ui_event *event);
int shy_gui_resize(size_t width, size_t height);
void shy_gui_pointer(int x, int y, int down);
int shy_gui_frame(const char *path);
void shy_gui_hash(char out[65]);
void shy_gui_close(void);
#endif
