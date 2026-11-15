#define _POSIX_C_SOURCE 200809L
#include "ui.h"
#include "mulib-tui.h"
#include "mulib-termkey.h"
#include "mulib-strbuf.h"
#include <sys/ioctl.h>
#include <termios.h>
#include <poll.h>
#include <signal.h>
#include <unistd.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

static mulib_tui_t terminal;
static mulib_termkey_t decoder;
static struct termios previous;
static int interactive, raw_mode, screen_mode;
static size_t row_offset, column_offset;
static volatile sig_atomic_t interrupted;
static struct sigaction old_int, old_term, old_hup;
static int handlers;
static unsigned char paste_tail[4];
static size_t paste_length;

static void stop(int signal_number) { interrupted = signal_number; }
static int write_all(const char *text, size_t length)
{
    while (length) {
        ssize_t n = write(STDOUT_FILENO, text, length);
        if (n < 0 && errno == EINTR) { if (interrupted) return 0; continue; }
        if (n <= 0) return 0;
        text += n; length -= (size_t)n;
    }
    return 1;
}

int shy_tui_resize(size_t width, size_t height)
{
    if (width < 40 || width > 240 || height < 12 || height > 80) return 0;
    return mulib_tui_resize(&terminal, width, height);
}

int shy_tui_open(int scripted)
{
    interactive = !scripted;
    if (interactive && (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO))) {
        shy_error("tui requires a terminal, or an explicit EVENTS_JSON script"); return 0;
    }
    if (!mulib_tui_init(&terminal, 128, 32)) return 0;
    mulib_termkey_init(&decoder);
    if (!interactive) return 1;
    if (tcgetattr(STDIN_FILENO, &previous)) return 0;
    struct termios raw = previous;
    raw.c_lflag &= (tcflag_t)~(ICANON | ECHO | IEXTEN);
    raw.c_iflag &= (tcflag_t)~(IXON | ICRNL);
    raw.c_cc[VMIN] = 0; raw.c_cc[VTIME] = 0;
    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw)) return 0;
    raw_mode = 1;
    struct sigaction action;
    memset(&action, 0, sizeof action); action.sa_handler = stop; sigemptyset(&action.sa_mask);
    if (sigaction(SIGINT, &action, &old_int)) return 0;
    handlers |= 1;
    if (sigaction(SIGTERM, &action, &old_term)) return 0;
    handlers |= 2;
    if (sigaction(SIGHUP, &action, &old_hup)) return 0;
    handlers |= 4;
    static const char enter[] = "\033[?1049h\033[?25l\033[?2004h";
    screen_mode = 1;
    return write_all(enter, sizeof enter-1);
}

static int text(size_t x, size_t y, size_t width, const char *value, mulib_term_style_t style)
{
    return mulib_tui_text(&terminal, (mulib_tui_rect_t){ x, y, width, 1 }, value, strlen(value), style);
}

int shy_tui_render(void)
{
    if (interrupted) return 1;
    if (interactive) {
        struct winsize size;
        if (!ioctl(STDOUT_FILENO, TIOCGWINSZ, &size) && size.ws_col && size.ws_row) {
            size_t w = size.ws_col < 40 ? 40 : size.ws_col > 240 ? 240 : size.ws_col;
            size_t h = size.ws_row < 12 ? 12 : size.ws_row > 80 ? 80 : size.ws_row;
            if ((w != terminal.back.grid.width || h != terminal.back.grid.height) && !shy_tui_resize(w, h)) return 0;
        }
    }
    size_t width = terminal.back.grid.width, height = terminal.back.grid.height;
    size_t visible_columns = (width-6)/17, visible_rows = height-10;
    if (visible_columns > SHY_SHEET_COLUMNS) visible_columns = SHY_SHEET_COLUMNS;
    size_t selected_row = shy_sheet_row(), selected_column = shy_sheet_column_index();
    if (selected_row < row_offset) row_offset = selected_row;
    if (selected_row >= row_offset+visible_rows) row_offset = selected_row-visible_rows+1;
    if (selected_column < column_offset) column_offset = selected_column;
    if (selected_column >= column_offset+visible_columns) column_offset = selected_column-visible_columns+1;
    mulib_term_style_t normal = mulib_term_style_default(), selected = normal, heading = normal;
    selected.attributes |= MULIB_TERM_INVERSE;
    heading.attributes |= MULIB_TERM_BOLD;
    int ok = mulib_tui_clear(&terminal, normal);
    char title[256]; snprintf(title, sizeof title, "POINDEXTER | %zu rows | %s | staged: %s", shy_sheet_count(), shy_sheet_key(), shy_sheet_dirty() ? "yes" : "no");
    ok = ok && text(1, 0, width-2, title, heading) &&
        text(1, 1, width-2, "Arrows: cells  Enter: edit  n: reason  s: save  /: filter  o: sort  r: refresh  d: discard  q: quit", normal);
    for (size_t c = 0; ok && c < visible_columns; ++c)
        ok = text(6+c*17, 3, 16, shy_sheet_columns[column_offset+c].label, heading);
    for (size_t r = 0; ok && r < visible_rows && row_offset+r < shy_sheet_count(); ++r) {
        char index[16]; snprintf(index, sizeof index, "%zu", row_offset+r+1);
        ok = text(0, 4+r, 5, index, normal);
        for (size_t c = 0; ok && c < visible_columns; ++c) {
            char cell[1100]; shy_sheet_text(row_offset+r, column_offset+c, cell, sizeof cell);
            ok = text(6+c*17, 4+r, 16, cell, row_offset+r == selected_row && column_offset+c == selected_column ? selected : normal);
        }
    }
    char value[1100]; shy_sheet_text(selected_row, selected_column, value, sizeof value);
    ok = ok && text(1, height-5, width-2, value, heading) && text(1, height-4, width-2, shy_sheet_message(), normal);
    if (shy_ui_editing()) {
        ok = ok && text(1, height-3, width-2, shy_ui_prompt(), heading) && text(1, height-2, width-2, shy_ui_editor(), normal);
    } else ok = ok && text(1, height-2, width-2, "Changes publish new review revisions. Amount, currency, scale and provenance are read-only.", normal);
    if (!ok) return 0;
    mulib_strbuf_t output; mulib_strbuf_init(&output);
    /* Extension audit lines or a write failure may have changed the terminal. */
    if (interactive) mulib_tui_invalidate(&terminal);
    ok = mulib_tui_render(&terminal, NULL, &output);
    if (ok && interactive) ok = write_all(output.data, output.length);
    mulib_strbuf_free(&output);
    return ok;
}

static int key(shy_ui_event *out, const mulib_termkey_event_t *event)
{
    const char *name = ""; char character[5] = { 0 };
    if (event->type == MULIB_TERMKEY_PASTE_TEXT) {
        /* Paste never executes shortcuts. Keep split UTF-8 scalars between
           decoder chunks, and never interpret control bytes as UI commands. */
        if (shy_ui_editing()) {
            unsigned char data[MULIB_TERMKEY_CAPACITY+4];
            memcpy(data, paste_tail, paste_length);
            memcpy(data+paste_length, event->raw, event->length);
            size_t length = paste_length+event->length, pos = 0;
            paste_length = 0;
            while (pos < length) {
                uint32_t scalar; size_t used;
                if (!mulib_utf8_decode(data+pos, length-pos, &scalar, &used)) {
                    size_t need = data[pos] >= 0xf0 ? 4 : data[pos] >= 0xe0 ? 3 : data[pos] >= 0xc2 ? 2 : 1;
                    if (need > length-pos && length-pos < sizeof paste_tail) {
                        paste_length = length-pos; memcpy(paste_tail, data+pos, paste_length); break;
                    }
                    ++pos; continue;
                }
                if (scalar >= 32 && scalar != 127) {
                    char text[5]; memcpy(text, data+pos, used); text[used] = 0;
                    (void)shy_ui_key(out, "text", text);
                }
                pos += used;
            }
        }
        return 1;
    }
    if (event->type == MULIB_TERMKEY_PASTE_BEGIN || event->type == MULIB_TERMKEY_PASTE_END) {
        paste_length = 0; return 1;
    }
    if (event->type != MULIB_TERMKEY_KEY) return 1;
    switch (event->key) {
    case MULIB_TERMKEY_UP: name = "up"; break;
    case MULIB_TERMKEY_DOWN: name = "down"; break;
    case MULIB_TERMKEY_LEFT: name = "left"; break;
    case MULIB_TERMKEY_RIGHT: name = "right"; break;
    case MULIB_TERMKEY_PAGE_UP: name = "page_up"; break;
    case MULIB_TERMKEY_PAGE_DOWN: name = "page_down"; break;
    case MULIB_TERMKEY_HOME: name = "home"; break;
    case MULIB_TERMKEY_END: name = "end"; break;
    case MULIB_TERMKEY_ENTER: name = "enter"; break;
    case MULIB_TERMKEY_ESCAPE: name = "escape"; break;
    case MULIB_TERMKEY_BACKSPACE: name = "backspace"; break;
    case MULIB_TERMKEY_TAB: name = "tab"; break;
    case MULIB_TERMKEY_CHAR: {
        size_t n = mulib_utf8_encode(event->codepoint, (unsigned char *)character, 4);
        if (!n) return 1;
        character[n] = 0; name = character;
        if ((event->modifiers & MULIB_TERMKEY_CTRL) && event->codepoint == 's') name = "save";
        if ((event->modifiers & MULIB_TERMKEY_CTRL) && event->codepoint == 'q') name = "quit";
        break;
    }
    default: return 1;
    }
    return shy_ui_key(out, name, event->modifiers & MULIB_TERMKEY_CTRL ? "" : character);
}

int shy_tui_next(shy_ui_event *event)
{
    event->action = "";
    if (interrupted) { event->action = "stop"; return 1; }
    mulib_termkey_event_t decoded;
    int got = mulib_termkey_next(&decoder, &decoded, 0);
    if (got > 0) return key(event, &decoded);
    struct pollfd fd = { STDIN_FILENO, POLLIN, 0 };
    int ready = poll(&fd, 1, 80);
    if (ready < 0) return errno == EINTR;
    if (!ready) {
        got = mulib_termkey_next(&decoder, &decoded, 1);
        return got > 0 ? key(event, &decoded) : got == 0;
    }
    if (fd.revents & (POLLHUP | POLLERR | POLLNVAL)) { event->action = "stop"; return 1; }
    unsigned char bytes[128];
    ssize_t n = read(STDIN_FILENO, bytes, sizeof bytes-decoder.length);
    if (n < 0) return errno == EINTR || errno == EAGAIN;
    if (!n) { event->action = "stop"; return 1; }
    if (mulib_termkey_feed(&decoder, bytes, (size_t)n) != (size_t)n) return 0;
    got = mulib_termkey_next(&decoder, &decoded, 0);
    return got > 0 ? key(event, &decoded) : got == 0;
}

void shy_tui_hash(char out[65])
{
    mulib_strbuf_t screen; mulib_strbuf_init(&screen);
    int ok = 1;
    for (size_t y = 0; y < terminal.back.grid.height; ++y)
        for (size_t x = 0; x < terminal.back.grid.width; ++x) {
            const mulib_term_cell_t *cell = mulib_tui_cell(&terminal, x, y);
            unsigned char bytes[4]; size_t n = cell ? mulib_utf8_encode(cell->codepoint, bytes, sizeof bytes) : 0;
            if (n && !mulib_strbuf_append_n(&screen, (const char *)bytes, n)) ok = 0;
        }
    if (ok) shy_digest(screen.data ? screen.data : "", screen.length, out);
    else *out = 0;
    mulib_strbuf_free(&screen);
}

void shy_tui_close(void)
{
    if (raw_mode) (void)tcsetattr(STDIN_FILENO, TCSAFLUSH, &previous);
    if (screen_mode) {
        static const char leave[] = "\033[0m\033[?25h\033[?2004l\033[?1049l";
        (void)write_all(leave, sizeof leave-1);
    }
    if (handlers & 1) (void)sigaction(SIGINT, &old_int, NULL);
    if (handlers & 2) (void)sigaction(SIGTERM, &old_term, NULL);
    if (handlers & 4) (void)sigaction(SIGHUP, &old_hup, NULL);
    mulib_tui_free(&terminal);
    interactive = raw_mode = screen_mode = handlers = 0; row_offset = column_offset = 0; interrupted = 0;
    paste_length = 0;
}
