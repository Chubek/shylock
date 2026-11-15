#include "ui.h"
#include "mulib-immgtk.h"
#include "mulib-fontdraw.h"
#include "mulib-utf8.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef SHY_HAVE_SDL2
#define SHY_HAVE_SDL2 0
#endif
#if SHY_HAVE_SDL2
#include <SDL.h>
#endif

static mulib_glx_t *surface;
static mulib_immgtk_t *gui;
static mulib_fontdraw_t *font;
static mulib_mune_value_t cell_function;
static int interactive, mouse_x, mouse_y, mouse_down;
static size_t row_offset, column_offset;
typedef struct { uint32_t scalar; int used; mulib_fontdraw_bitmap_t bitmap; unsigned char *alpha; } glyph;
static glyph glyphs[256];
#if SHY_HAVE_SDL2
static SDL_Window *window;
static SDL_Renderer *renderer;
static SDL_Texture *texture;
static int video_initialized;
#endif

/* Small, application-authored 5x7 fallback. Headless renders always use it for
   reproducibility. Interactive FreeType uses the MuLIB fontdraw adapter. */
static const unsigned char bitmap[128][7] = {
    ['A']={14,17,17,31,17,17,17}, ['B']={30,17,17,30,17,17,30}, ['C']={14,17,16,16,16,17,14},
    ['D']={30,17,17,17,17,17,30}, ['E']={31,16,16,30,16,16,31}, ['F']={31,16,16,30,16,16,16},
    ['G']={14,17,16,23,17,17,15}, ['H']={17,17,17,31,17,17,17}, ['I']={14,4,4,4,4,4,14},
    ['J']={7,2,2,2,18,18,12}, ['K']={17,18,20,24,20,18,17}, ['L']={16,16,16,16,16,16,31},
    ['M']={17,27,21,21,17,17,17}, ['N']={17,25,21,19,17,17,17}, ['O']={14,17,17,17,17,17,14},
    ['P']={30,17,17,30,16,16,16}, ['Q']={14,17,17,17,21,18,13}, ['R']={30,17,17,30,20,18,17},
    ['S']={15,16,16,14,1,1,30}, ['T']={31,4,4,4,4,4,4}, ['U']={17,17,17,17,17,17,14},
    ['V']={17,17,17,17,17,10,4}, ['W']={17,17,17,21,21,21,10}, ['X']={17,17,10,4,10,17,17},
    ['Y']={17,17,10,4,4,4,4}, ['Z']={31,1,2,4,8,16,31},
    ['0']={14,17,19,21,25,17,14}, ['1']={4,12,4,4,4,4,14}, ['2']={14,17,1,2,4,8,31},
    ['3']={30,1,1,14,1,1,30}, ['4']={2,6,10,18,31,2,2}, ['5']={31,16,16,30,1,1,30},
    ['6']={14,16,16,30,17,17,14}, ['7']={31,1,2,4,8,8,8}, ['8']={14,17,17,14,17,17,14},
    ['9']={14,17,17,15,1,1,14}, ['?']={14,17,1,2,4,0,4}, ['.']={0,0,0,0,0,0,4},
    [',']={0,0,0,0,0,4,8}, [':']={0,4,0,0,4,0,0}, [';']={0,4,0,0,4,4,8},
    ['-']={0,0,0,31,0,0,0}, ['_']={0,0,0,0,0,0,31}, ['/']={1,2,2,4,8,8,16},
    ['(']={2,4,8,8,8,4,2}, [')']={8,4,2,2,2,4,8}, ['[']={14,8,8,8,8,8,14},
    [']']={14,2,2,2,2,2,14}, ['=']={0,0,31,0,31,0,0}, ['+']={0,4,4,31,4,4,0},
    ['!']={4,4,4,4,4,0,4}, ['*']={0,21,14,31,14,21,0}, ['|']={4,4,4,4,4,4,4},
    ['<']={1,2,4,8,4,2,1}, ['>']={16,8,4,2,4,8,16}, ['\'']={4,4,8,0,0,0,0},
    ['"']={10,10,0,0,0,0,0}, ['#']={10,31,10,10,31,10,0}, ['%']={17,2,4,8,17,0,0}
};

static void fallback(mulib_glx_t *target, int x, int y, uint32_t scalar, uint32_t color)
{
    if (scalar == ' ') return;
    if (scalar >= 'a' && scalar <= 'z') scalar -= 32;
    if (scalar >= 128 || !bitmap[scalar][0]) {
        int any = 0;
        if (scalar < 128) for (size_t i = 0; i < 7; ++i) any |= bitmap[scalar][i];
        if (!any) scalar = '?';
    }
    for (int row = 0; row < 7; ++row)
        for (int col = 0; col < 5; ++col)
            if (bitmap[scalar][row] & (1u << (4-col)))
                mulib_glx_fill_rect(target, x+col, y+row*2+2, 1, 2, color);
}

static glyph *cached_glyph(uint32_t scalar)
{
    glyph *slot = NULL;
    for (size_t i = 0; i < sizeof glyphs / sizeof *glyphs; ++i) {
        if (glyphs[i].used && glyphs[i].scalar == scalar) return &glyphs[i];
        if (!glyphs[i].used && !slot) slot = &glyphs[i];
    }
    if (!slot) slot = &glyphs[255];
    if (slot->used) { mulib_fontdraw_bitmap_destroy(&slot->bitmap); free(slot->alpha); memset(slot, 0, sizeof *slot); }
    unsigned id = mulib_fontdraw_glyph(font, scalar);
    if (!id || mulib_fontdraw_render(font, id, 0, 0, &slot->bitmap) || mulib_fontdraw_alpha(&slot->bitmap, &slot->alpha)) {
        mulib_fontdraw_bitmap_destroy(&slot->bitmap); free(slot->alpha); memset(slot, 0, sizeof *slot); return NULL;
    }
    slot->used = 1; slot->scalar = scalar;
    return slot;
}

static int draw_text(mulib_glx_t *target, float x, float y, const char *text, size_t n, uint32_t color, void *user)
{
    (void)user;
    int pen = (int)x;
    for (size_t pos = 0; pos < n;) {
        uint32_t scalar; size_t bytes;
        if (!mulib_utf8_decode((const unsigned char *)text+pos, n-pos, &scalar, &bytes)) return 0;
        pos += bytes;
        if (scalar < 32 || (scalar >= 127 && scalar <= 159)) scalar = ' ';
        glyph *g = font ? cached_glyph(scalar) : NULL;
        if (g) {
            for (size_t row = 0; row < g->bitmap.height; ++row)
                for (size_t col = 0; col < g->bitmap.width; ++col) {
                    unsigned alpha = g->alpha[row*g->bitmap.width+col];
                    uint32_t pixel = (color & UINT32_C(0x00ffffff)) | (uint32_t)(alpha*((color>>24)&255)/255)<<24;
                    if (alpha) mulib_glx_pixel(target, pen+g->bitmap.left+(int)col, (int)y+17-g->bitmap.top+(int)row, pixel);
                }
            pen += (int)(g->bitmap.advance_x/64);
        } else { fallback(target, pen, (int)y, scalar, color); pen += 8; }
        if (pen > (int)mulib_glx_width(target)+64) break;
    }
    return 1;
}

static const char widget_source[] =
    "function worksheet_cell(id, text, x, y, w, h, selected)\n"
    "  local hot, held, clicked = imm_interact(id,x,y,w,h)\n"
    "  local color = 0xff182638\n"
    "  if selected then color=0xff215c68 elseif held then color=0xff344e68 elseif hot then color=0xff263b50 end\n"
    "  imm_rect(x,y,w,h,color)\n"
    "  imm_text(text,x+7,y+4,0xffe5edf5)\n"
    "  return clicked\n"
    "end\n";

int shy_gui_resize(size_t width, size_t height)
{
    if (width < 640 || width > 1920 || height < 400 || height > 1200) return 0;
    mulib_glx_t *next = mulib_glx_create((unsigned)width, (unsigned)height);
    if (!next) return 0;
#if SHY_HAVE_SDL2
    SDL_Texture *next_texture = NULL;
    if (renderer) {
        next_texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, (int)width, (int)height);
        if (!next_texture) { mulib_glx_destroy(next); return 0; }
    }
    SDL_DestroyTexture(texture); texture = next_texture;
#endif
    mulib_glx_destroy(surface); surface = next;
    return 1;
}

int shy_gui_open(int scripted)
{
    interactive = !scripted;
    if (!shy_gui_resize(1280, 800)) return 0;
#if SHY_HAVE_SDL2
    if (interactive) {
        if (SDL_InitSubSystem(SDL_INIT_VIDEO)) { shy_error(SDL_GetError()); return 0; }
        video_initialized = 1;
        window = SDL_CreateWindow("Poindexter - reviewed price worksheet", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
            1280, 800, SDL_WINDOW_RESIZABLE);
        if (!window) { shy_error(SDL_GetError()); return 0; }
        SDL_SetWindowMinimumSize(window, 640, 400); SDL_SetWindowMaximumSize(window, 1920, 1200);
        renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_SOFTWARE);
        if (!renderer || !shy_gui_resize(1280, 800)) return 0;
        SDL_StartTextInput();
    }
#else
    if (interactive) { shy_error("interactive GUI needs SDL2; rebuild with GUI=1 or supply EVENTS_JSON for headless rendering"); return 0; }
#endif
    if (interactive) {
        const char *requested = getenv("SHY_UI_FONT");
        if (requested && *requested) font = mulib_fontdraw_open(requested, 0, 15, NULL);
        else {
            static const char *const paths[] = { "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf",
                "/usr/share/fonts/noto/NotoSansMono-Regular.ttf", "/usr/share/fonts/truetype/liberation2/LiberationMono-Regular.ttf" };
            for (size_t i = 0; !font && i < sizeof paths / sizeof *paths; ++i) font = mulib_fontdraw_open(paths[i], 0, 15, NULL);
        }
    }
    mulib_mune_error_t error = { 0 };
    gui = mulib_immgtk_create(draw_text, NULL, &error);
    if (!gui || mulib_immgtk_run(gui, widget_source, sizeof widget_source-1, &error) != MULIB_MUNE_OK ||
        mulib_mune_get_global(mulib_immgtk_state(gui), "worksheet_cell", &cell_function) != MULIB_MUNE_OK) return 0;
    return cell_function.type == MULIB_MUNE_FUNCTION;
}

void shy_gui_pointer(int x, int y, int down) { mouse_x = x; mouse_y = y; mouse_down = down; }

static int cell(uint64_t id, const char *label, int x, int y, int width, int height, int selected, int *clicked)
{
    mulib_mune_state_t *state = mulib_immgtk_state(gui);
    mulib_mune_value_t text;
    if (mulib_mune_make_string(state, label, strlen(label), &text) != MULIB_MUNE_OK) return 0;
    const mulib_mune_value_t args[] = { mulib_mune_integer((int64_t)id), text,
        mulib_mune_integer(x), mulib_mune_integer(y), mulib_mune_integer(width), mulib_mune_integer(height),
        mulib_mune_boolean(selected) };
    mulib_mune_result_t *result = NULL;
    mulib_mune_error_t error = { 0 };
    mulib_glx_clip(surface, x, y, width, height);
    int ok = mulib_mune_call(state, cell_function, args, 7, &result, &error) == MULIB_MUNE_OK &&
        mulib_mune_result_count(result) == 1;
    if (ok) {
        mulib_mune_value_t value = mulib_mune_result_get(result, 0);
        ok = value.type == MULIB_MUNE_BOOLEAN;
        *clicked = ok && value.as.boolean;
    }
    mulib_mune_result_destroy(result);
    mulib_glx_reset_clip(surface);
    return ok;
}

static int label(int x, int y, const char *text, uint32_t color)
{
    return draw_text(surface, (float)x, (float)y, text, strlen(text), color, NULL);
}

int shy_gui_render(shy_ui_event *event)
{
    if (!surface || !gui) return 0;
    int width = (int)mulib_glx_width(surface), height = (int)mulib_glx_height(surface);
    size_t visible_columns = (size_t)(width-48)/150, visible_rows = (size_t)(height-260)/26;
    if (visible_rows > 25) visible_rows = 25;
    if (visible_columns > 8) visible_columns = 8;
    size_t selected_row = shy_sheet_row(), selected_column = shy_sheet_column_index();
    if (selected_row < row_offset) row_offset = selected_row;
    if (selected_row >= row_offset+visible_rows) row_offset = selected_row-visible_rows+1;
    if (selected_column < column_offset) column_offset = selected_column;
    if (selected_column >= column_offset+visible_columns) column_offset = selected_column-visible_columns+1;
    mulib_glx_clear(surface, UINT32_C(0xff101b29));
    mulib_immgtk_input_t input = { (float)mouse_x, (float)mouse_y, mouse_down, 0 };
    if (!mulib_immgtk_begin(gui, surface, input)) return 0;
    char title[160]; snprintf(title, sizeof title, "POINDEXTER  |  %zu reviewed rows  |  staged: %s", shy_sheet_count(), shy_sheet_dirty() ? "yes" : "no");
    int ok = label(24, 18, title, UINT32_C(0xff66d8ce));
    static const char *const captions[] = { "Save (s)", "Reason (n)", "Filter (/)", "Refresh (r)", "Discard (d)", "Quit (q)" };
    static const char *const actions[] = { "save", "reason", "filter", "refresh", "discard", "quit" };
    for (size_t i = 0; ok && i < 6; ++i) {
        int clicked = 0;
        ok = cell(i+1, captions[i], 24+(int)i*98, 54, 92, 30, 0, &clicked);
        if (clicked && !shy_ui_editing()) {
            if (i == 1 || i == 2) (void)shy_ui_key(event, actions[i], "");
            else event->action = actions[i];
        }
    }
    ok = ok && label(24, 98, "Arrows: navigate | Enter: edit | o: sort selected column | values retain minor-unit scale", UINT32_C(0xffa4b7cb));
    for (size_t c = 0; ok && c < visible_columns; ++c) {
        int x = 48+(int)c*150;
        mulib_glx_clip(surface, x, 130, 146, 24);
        ok = label(x+7, 133, shy_sheet_columns[column_offset+c].label, UINT32_C(0xff8ce0d6));
        mulib_glx_reset_clip(surface);
    }
    for (size_t r = 0; ok && r < visible_rows && row_offset+r < shy_sheet_count(); ++r) {
        char number[16]; snprintf(number, sizeof number, "%zu", row_offset+r+1);
        ok = label(8, 162+(int)r*26, number, UINT32_C(0xff8299b0));
        for (size_t c = 0; ok && c < visible_columns; ++c) {
            char value[1100]; shy_sheet_text(row_offset+r, column_offset+c, value, sizeof value);
            int clicked = 0;
            uint64_t id = shy_sheet_cell_id(row_offset+r, column_offset+c);
            ok = cell(id, value, 48+(int)c*150, 156+(int)r*26, 146, 25,
                row_offset+r == selected_row && column_offset+c == selected_column, &clicked);
            if (clicked && !shy_ui_editing()) {
                event->action = "select"; event->row = (int64_t)(row_offset+r);
                snprintf(event->column, sizeof event->column, "%s", shy_sheet_columns[column_offset+c].key);
            }
        }
    }
    mulib_glx_fill_rect(surface, 16, height-100, width-32, 1, UINT32_C(0xff29445b));
    char selected_value[1100]; shy_sheet_text(selected_row, selected_column, selected_value, sizeof selected_value);
    char detail[1200]; snprintf(detail, sizeof detail, "%s: %s", shy_sheet_key(), selected_value);
    ok = ok && label(24, height-90, detail, UINT32_C(0xffdbe8f2)) &&
        label(24, height-64, shy_sheet_message(), UINT32_C(0xffffcd82));
    if (shy_ui_editing()) {
        char edit[1200]; snprintf(edit, sizeof edit, "%s > %s_", shy_ui_prompt(), shy_ui_editor());
        ok = ok && label(24, height-32, edit, UINT32_C(0xff9ce8cf));
    } else ok = ok && label(24, height-32, "Evidence is immutable. Edits publish review revisions only after an explicit reason and save.", UINT32_C(0xff8299b0));
    if (!mulib_immgtk_end(gui)) ok = 0;
#if SHY_HAVE_SDL2
    if (ok && interactive) {
        ok = SDL_UpdateTexture(texture, NULL, mulib_glx_pixels(surface), (int)mulib_glx_pitch(surface)) == 0 &&
            SDL_RenderClear(renderer) == 0 && SDL_RenderCopy(renderer, texture, NULL, NULL) == 0;
        if (ok) SDL_RenderPresent(renderer);
    }
#endif
    return ok;
}

int shy_gui_next(shy_ui_event *event)
{
    event->action = "";
#if SHY_HAVE_SDL2
    SDL_Event input;
    if (!SDL_WaitEventTimeout(&input, 80)) return 1;
    if (input.type == SDL_QUIT) { event->action = "quit"; return 1; }
    if (input.type == SDL_WINDOWEVENT && input.window.event == SDL_WINDOWEVENT_SIZE_CHANGED)
        return shy_gui_resize((size_t)input.window.data1, (size_t)input.window.data2);
    if (input.type == SDL_MOUSEMOTION) { mouse_x = input.motion.x; mouse_y = input.motion.y; return 1; }
    if ((input.type == SDL_MOUSEBUTTONDOWN || input.type == SDL_MOUSEBUTTONUP) && input.button.button == SDL_BUTTON_LEFT) {
        shy_gui_pointer(input.button.x, input.button.y, input.type == SDL_MOUSEBUTTONDOWN); return 1;
    }
    if (input.type == SDL_MOUSEWHEEL && input.wheel.y) return shy_ui_key(event, input.wheel.y > 0 ? "up" : "down", "");
    if (input.type == SDL_TEXTINPUT) {
        if (shy_ui_editing()) return shy_ui_key(event, "text", input.text.text);
        return 1;
    }
    if (input.type == SDL_KEYDOWN) {
        const char *key = ""; char character[2] = { 0 };
        SDL_Keycode code = input.key.keysym.sym;
        switch (code) {
        case SDLK_UP: key = "up"; break; case SDLK_DOWN: key = "down"; break;
        case SDLK_LEFT: key = "left"; break; case SDLK_RIGHT: key = "right"; break;
        case SDLK_PAGEUP: key = "page_up"; break; case SDLK_PAGEDOWN: key = "page_down"; break;
        case SDLK_HOME: key = "home"; break; case SDLK_END: key = "end"; break;
        case SDLK_RETURN: key = "enter"; break; case SDLK_ESCAPE: key = "escape"; break;
        case SDLK_BACKSPACE: key = "backspace"; break; case SDLK_TAB: key = "tab"; break;
        default:
            if (code > 0 && code < 128) { character[0] = (char)code; key = character; }
            break;
        }
        /* Printable input arrives once via SDL_TEXTINPUT while editing. */
        if (shy_ui_editing() && *character) return 1;
        return shy_ui_key(event, key, "");
    }
    return 1;
#else
    return 0;
#endif
}

int shy_gui_frame(const char *path)
{
    if (!surface) return 0;
    FILE *file = fopen(path, "wb");
    if (!file) return 0;
    unsigned width = mulib_glx_width(surface), height = mulib_glx_height(surface);
    int ok = fprintf(file, "P6\n%u %u\n255\n", width, height) > 0;
    const uint32_t *pixels = mulib_glx_pixels(surface);
    size_t stride = mulib_glx_pitch(surface)/4;
    for (unsigned y = 0; ok && y < height; ++y)
        for (unsigned x = 0; ok && x < width; ++x) {
            uint32_t pixel = pixels[y*stride+x];
            unsigned char rgb[] = { (unsigned char)(pixel>>16), (unsigned char)(pixel>>8), (unsigned char)pixel };
            ok = fwrite(rgb, 1, sizeof rgb, file) == sizeof rgb;
        }
    if (fclose(file)) ok = 0;
    return ok;
}

void shy_gui_hash(char out[65])
{
    if (!surface) { *out = 0; return; }
    shy_digest((const char *)mulib_glx_pixels(surface), mulib_glx_pitch(surface)*mulib_glx_height(surface), out);
}

void shy_gui_close(void)
{
    mulib_immgtk_destroy(gui); gui = NULL;
    mulib_glx_destroy(surface); surface = NULL;
    mulib_fontdraw_close(font); font = NULL;
    for (size_t i = 0; i < sizeof glyphs / sizeof *glyphs; ++i) {
        mulib_fontdraw_bitmap_destroy(&glyphs[i].bitmap); free(glyphs[i].alpha); memset(&glyphs[i], 0, sizeof glyphs[i]);
    }
#if SHY_HAVE_SDL2
    SDL_DestroyTexture(texture); texture = NULL;
    SDL_DestroyRenderer(renderer); renderer = NULL;
    SDL_DestroyWindow(window); window = NULL;
    if (video_initialized) { SDL_StopTextInput(); SDL_QuitSubSystem(SDL_INIT_VIDEO); }
    video_initialized = 0;
#endif
    mouse_x = mouse_y = mouse_down = interactive = 0; row_offset = column_offset = 0;
}
