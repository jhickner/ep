
#ifndef SCREEN_H
#define SCREEN_H

#include <stdint.h>
#include <stdbool.h>

#ifndef TERM_H
#include "term.h"
#endif

#ifdef SCREEN_KITTY
#ifndef KITTY_H
#include "kitty.h"
#endif
#endif

typedef struct {
    uint8_t r, g, b;
} Color;

#define COLOR_DEFAULT_BG ((Color){ 0x01, 0x00, 0x01 })

#define COLOR_DEFAULT_FG ((Color){ 0x01, 0x01, 0x00 })

typedef enum {
    STYLE_NONE      = 0,
    STYLE_BOLD      = (1 << 0),
    STYLE_DIM       = (1 << 1),
    STYLE_ITALIC    = (1 << 2),
    STYLE_UNDERLINE = (1 << 3),
    STYLE_REVERSE   = (1 << 4),
} Style;

typedef enum {

    GLYPH_PLACEHOLDER = (1 << 0),
} GlyphFlags;

typedef struct {
    uint32_t codepoint;
    Color fg;
    Color bg;
    uint8_t style;
    uint8_t flags;
} Glyph;

typedef struct {
    Glyph *front;
    Glyph *back;
    bool *dirty;
    int width;
    int height;
    bool force_redraw;
} Screen;

Screen screen_create(int width, int height);
void screen_destroy(Screen *s);

void screen_resize(Screen *s, int width, int height);

void screen_clear(Screen *s, Glyph fill);

void screen_set(Screen *s, int x, int y, Glyph g);

void screen_put(Screen *s, int x, int y, uint32_t ch, Color fg, Color bg);

void screen_print(Screen *s, int x, int y, const char *str, Color fg, Color bg);

void screen_render(Screen *s);

void screen_flush_region(Screen *s, int rx, int ry, int rw, int rh);

void screen_swap(Screen *s);

Glyph glyph_make(uint32_t ch, Color fg, Color bg);

Glyph glyph_placeholder(uint32_t id, int row, int col);

bool glyph_is_placeholder(const Glyph *g);
uint32_t glyph_placeholder_id(const Glyph *g);
int glyph_placeholder_row(const Glyph *g);
int glyph_placeholder_col(const Glyph *g);

bool glyph_eq(const Glyph *a, const Glyph *b);

Color color_dim(Color c, float factor);
Color color_lerp(Color a, Color b, float t);
Glyph glyph_dim(Glyph g, float factor);

#endif

#ifdef SCREEN_IMPLEMENTATION

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <wchar.h>

Screen screen_create(int width, int height) {
    Screen s = {0};
    if (width <= 0 || height <= 0) return s;
    s.width = width;
    s.height = height;

    size_t size = (size_t)width * (size_t)height;
    s.front = calloc(size, sizeof(Glyph));
    s.back = calloc(size, sizeof(Glyph));
    s.dirty = calloc(size, sizeof(bool));
    if (!s.front || !s.back || !s.dirty) {
        free(s.front); free(s.back); free(s.dirty);
        return (Screen){0};
    }
    s.force_redraw = true;

    Glyph blank = glyph_make(' ', (Color){255,255,255}, (Color){0,0,0});
    for (size_t i = 0; i < size; i++) {
        s.front[i] = blank;
        s.back[i] = blank;
    }

    return s;
}

void screen_destroy(Screen *s) {
    free(s->front);
    free(s->back);
    free(s->dirty);
    s->front = NULL;
    s->back = NULL;
    s->dirty = NULL;
}

void screen_resize(Screen *s, int width, int height) {
    screen_destroy(s);
    *s = screen_create(width, height);
}

void screen_clear(Screen *s, Glyph fill) {
    if (!s || !s->back) return;
    size_t size = (size_t)s->width * (size_t)s->height;
    for (size_t i = 0; i < size; i++) {
        s->back[i] = fill;
    }
}

void screen_set(Screen *s, int x, int y, Glyph g) {
    if (!s || x < 0 || x >= s->width || y < 0 || y >= s->height) {
        return;
    }
    int idx = y * s->width + x;
    s->back[idx] = g;
}

void screen_put(Screen *s, int x, int y, uint32_t ch, Color fg, Color bg) {
    if (!s) return;
    screen_set(s, x, y, glyph_make(ch, fg, bg));
}

void screen_print(Screen *s, int x, int y, const char *str, Color fg, Color bg) {
    if (!s || !str) return;
    while (*str && x < s->width) {
        uint32_t cp;
        uint8_t c = (uint8_t)*str;

        if (c < 0x80) {
            cp = c;
            str++;
        } else if ((c & 0xE0) == 0xC0 && (str[1] & 0xC0) == 0x80) {
            cp = (c & 0x1F) << 6 | (str[1] & 0x3F);
            str += 2;
        } else if ((c & 0xF0) == 0xE0 && (str[1] & 0xC0) == 0x80 && (str[2] & 0xC0) == 0x80) {
            cp = (c & 0x0F) << 12 | (str[1] & 0x3F) << 6 | (str[2] & 0x3F);
            str += 3;
        } else if ((c & 0xF8) == 0xF0 && (str[1] & 0xC0) == 0x80 && (str[2] & 0xC0) == 0x80 && (str[3] & 0xC0) == 0x80) {
            cp = (c & 0x07) << 18 | (str[1] & 0x3F) << 12 | (str[2] & 0x3F) << 6 | (str[3] & 0x3F);
            str += 4;
        } else {
            cp = 0xFFFD;
            str++;
        }
        screen_put(s, x, y, cp, fg, bg);
        x++;
    }
}

static void output_codepoint(uint32_t cp) {
    char buf[5];
    int len = 0;

    if (cp < 0x80) {
        buf[len++] = cp;
    } else if (cp < 0x800) {
        buf[len++] = 0xC0 | (cp >> 6);
        buf[len++] = 0x80 | (cp & 0x3F);
    } else if (cp < U'𐀀') {
        buf[len++] = 0xE0 | (cp >> 12);
        buf[len++] = 0x80 | ((cp >> 6) & 0x3F);
        buf[len++] = 0x80 | (cp & 0x3F);
    } else {
        buf[len++] = 0xF0 | (cp >> 18);
        buf[len++] = 0x80 | ((cp >> 12) & 0x3F);
        buf[len++] = 0x80 | ((cp >> 6) & 0x3F);
        buf[len++] = 0x80 | (cp & 0x3F);
    }
    buf[len] = '\0';
    term_write(buf);
}

static inline bool color_eq(Color a, Color b) {
    return a.r == b.r && a.g == b.g && a.b == b.b;
}

#define GLYPH_PH_MAX_ROWCOL 4096

Glyph glyph_placeholder(uint32_t id, int row, int col) {
    if (id == 0 || id > 0xFFFFFFu ||
        row < 0 || col < 0 ||
        row >= GLYPH_PH_MAX_ROWCOL || col >= GLYPH_PH_MAX_ROWCOL) {
        return glyph_make(' ', (Color){255,255,255}, COLOR_DEFAULT_BG);
    }
    Glyph g = {0};
    g.codepoint = 0x10EEEEu;
    g.fg = (Color){ (uint8_t)(id >> 16), (uint8_t)(id >> 8), (uint8_t)id };
    g.bg = (Color){ (uint8_t)row, (uint8_t)col,
                    (uint8_t)(((row >> 8) & 0x0F) | ((col >> 8) << 4)) };
    g.style = STYLE_NONE;
    g.flags = GLYPH_PLACEHOLDER;
    return g;
}

bool glyph_is_placeholder(const Glyph *g) {
    return g && (g->flags & GLYPH_PLACEHOLDER);
}

uint32_t glyph_placeholder_id(const Glyph *g) {
    return ((uint32_t)g->fg.r << 16) | ((uint32_t)g->fg.g << 8) | g->fg.b;
}

int glyph_placeholder_row(const Glyph *g) {
    return g->bg.r | ((g->bg.b & 0x0F) << 8);
}

int glyph_placeholder_col(const Glyph *g) {
    return g->bg.g | ((g->bg.b >> 4) << 8);
}

static inline bool ph_run_continues(const Glyph *a, const Glyph *b) {
    return glyph_is_placeholder(b)
        && color_eq(a->fg, b->fg)
        && a->style == b->style
        && glyph_placeholder_row(a) == glyph_placeholder_row(b)
        && glyph_placeholder_col(a) + 1 == glyph_placeholder_col(b);
}

static bool output_placeholder_run(const Glyph *g, int count) {
#ifdef SCREEN_KITTY

    term_write("\x1b[49m");
    kg_placeholder_cell(glyph_placeholder_id(g),
                        glyph_placeholder_row(g), glyph_placeholder_col(g));

    static const char bare[] = "\xf4\x8e\xbb\xae";
    for (int i = 1; i < count; i++) term_write_n(bare, 4);
    return true;
#else
    (void)g;
    for (int i = 0; i < count; i++) term_write(" ");
    return false;
#endif
}

static inline int u8_to_dec(char *dst, uint8_t v) {
    if (v >= 100) {
        dst[0] = '0' + v / 100;
        dst[1] = '0' + (v / 10) % 10;
        dst[2] = '0' + v % 10;
        return 3;
    }
    if (v >= 10) {
        dst[0] = '0' + v / 10;
        dst[1] = '0' + v % 10;
        return 2;
    }
    dst[0] = '0' + v;
    return 1;
}

static void set_colors(Color fg, Color bg) {
    char buf[64];
    char *p = buf;
    if (color_eq(fg, COLOR_DEFAULT_FG)) {
        memcpy(p, "\x1b[39m", 5); p += 5;
    } else {
        memcpy(p, "\x1b[38;2;", 7); p += 7;
        p += u8_to_dec(p, fg.r); *p++ = ';';
        p += u8_to_dec(p, fg.g); *p++ = ';';
        p += u8_to_dec(p, fg.b); *p++ = 'm';
    }
    if (color_eq(bg, COLOR_DEFAULT_BG)) {
        memcpy(p, "\x1b[49m", 5); p += 5;
    } else {
        memcpy(p, "\x1b[48;2;", 7); p += 7;
        p += u8_to_dec(p, bg.r); *p++ = ';';
        p += u8_to_dec(p, bg.g); *p++ = ';';
        p += u8_to_dec(p, bg.b); *p++ = 'm';
    }
    *p = '\0';
    term_write(buf);
}

static void set_style(uint8_t style) {

    term_write("\x1b[22;23;24;25;27m");
    if (style & STYLE_BOLD)      term_write("\x1b[1m");
    if (style & STYLE_DIM)       term_write("\x1b[2m");
    if (style & STYLE_ITALIC)    term_write("\x1b[3m");
    if (style & STYLE_UNDERLINE) term_write("\x1b[4:3m");
    if (style & STYLE_REVERSE)   term_write("\x1b[7m");
}

void screen_render(Screen *s) {
    if (!s || !s->front || !s->back || !s->dirty) return;
    size_t size = (size_t)s->width * (size_t)s->height;

    for (size_t i = 0; i < size; i++) {
        s->dirty[i] = s->force_redraw || !glyph_eq(&s->front[i], &s->back[i]);
    }

    Color last_fg = {0, 0, 0};
    Color last_bg = {0, 0, 0};
    uint8_t last_style = STYLE_NONE;
    bool colors_set = false;
    bool style_set = false;

    for (int y = 0; y < s->height; y++) {
        int x = 0;
        while (x < s->width) {
            int idx = y * s->width + x;
            if (!s->dirty[idx]) {
                x++;
                continue;
            }

            Glyph *g = &s->back[idx];

            if (glyph_is_placeholder(g)) {
                int ph_end = x + 1;
                while (ph_end < s->width) {
                    int next_idx = y * s->width + ph_end;
                    if (!s->dirty[next_idx]) break;
                    if (!ph_run_continues(&s->back[next_idx - 1], &s->back[next_idx])) break;
                    ph_end++;
                }

                term_move_cursor(x, y);
                if (!style_set || g->style != last_style) {
                    set_style(g->style);
                    last_style = g->style;
                    style_set = true;
                }
                output_placeholder_run(g, ph_end - x);
                colors_set = false;
                x = ph_end;
                continue;
            }

            int run_start = x;
            int run_end = x + 1;

            while (run_end < s->width) {
                int next_idx = y * s->width + run_end;
                if (!s->dirty[next_idx]) break;
                Glyph *next = &s->back[next_idx];
                if (glyph_is_placeholder(next)) break;
                if (!color_eq(g->fg, next->fg) || !color_eq(g->bg, next->bg) || g->style != next->style) break;
                run_end++;
            }

            term_move_cursor(run_start, y);

            if (!style_set || g->style != last_style) {
                set_style(g->style);
                last_style = g->style;
                style_set = true;
                colors_set = false;
            }

            if (!colors_set || !color_eq(g->fg, last_fg) || !color_eq(g->bg, last_bg)) {
                set_colors(g->fg, g->bg);
                last_fg = g->fg;
                last_bg = g->bg;
                colors_set = true;
            }

            bool cursor_uncertain = false;
            for (int rx = run_start; rx < run_end; rx++) {
                Glyph *rg = &s->back[y * s->width + rx];
                if (rg->codepoint == 0 || rg->codepoint < 0x80) {
                    if (cursor_uncertain) {
                        term_move_cursor(rx, y);
                        cursor_uncertain = false;
                    }
                    if (rg->codepoint == 0)
                        term_write(" ");
                    else
                        output_codepoint(rg->codepoint);
                } else {
                    term_move_cursor(rx, y);
                    output_codepoint(rg->codepoint);
                    cursor_uncertain = true;
                    if (wcwidth(rg->codepoint) == 2 && rx + 1 < run_end)
                        rx++;
                }
            }

            x = run_end;
        }
    }

    term_write("\x1b[0m");
    term_flush();

    s->force_redraw = false;
}

void screen_flush_region(Screen *s, int rx, int ry, int rw, int rh) {
    if (!s || !s->front || !s->back) return;

    if (rx < 0) { rw += rx; rx = 0; }
    if (ry < 0) { rh += ry; ry = 0; }
    if (rx + rw > s->width) rw = s->width - rx;
    if (ry + rh > s->height) rh = s->height - ry;
    if (rw <= 0 || rh <= 0) return;

    Color last_fg = {0, 0, 0};
    Color last_bg = {0, 0, 0};
    bool colors_set = false;

    for (int y = ry; y < ry + rh; y++) {
        term_move_cursor(rx, y);
        for (int x = rx; x < rx + rw; x++) {
            int idx = y * s->width + x;
            Glyph *g = &s->back[idx];

            if (glyph_is_placeholder(g)) {
                int ph_end = x + 1;
                while (ph_end < rx + rw &&
                       ph_run_continues(&s->back[y * s->width + ph_end - 1],
                                        &s->back[y * s->width + ph_end])) {
                    ph_end++;
                }
                output_placeholder_run(g, ph_end - x);
                colors_set = false;
                for (int px = x; px < ph_end; px++)
                    s->front[y * s->width + px] = s->back[y * s->width + px];
                x = ph_end - 1;
                continue;
            }

            if (!colors_set || !color_eq(g->fg, last_fg) || !color_eq(g->bg, last_bg)) {
                set_colors(g->fg, g->bg);
                last_fg = g->fg;
                last_bg = g->bg;
                colors_set = true;
            }

            if (g->codepoint == 0) {
                term_write(" ");
            } else {
                output_codepoint(g->codepoint);
                if (wcwidth(g->codepoint) == 2) x++;
            }

            s->front[idx] = *g;
        }
    }

    term_write("\x1b[0m");
    term_flush();
}

void screen_swap(Screen *s) {
    if (!s || !s->front || !s->back || !s->dirty) return;

    size_t size = (size_t)s->width * (size_t)s->height;
    memcpy(s->front, s->back, size * sizeof(Glyph));
    memset(s->dirty, 0, size * sizeof(bool));
}

Glyph glyph_make(uint32_t ch, Color fg, Color bg) {
    return (Glyph){
        .codepoint = ch,
        .fg = fg,
        .bg = bg,
        .style = STYLE_NONE,
        .flags = 0
    };
}

bool glyph_eq(const Glyph *a, const Glyph *b) {

    return a->codepoint == b->codepoint
        && color_eq(a->fg, b->fg)
        && color_eq(a->bg, b->bg)
        && a->style == b->style
        && a->flags == b->flags;
}

static inline uint8_t clamp_u8(float v) {
    if (v < 0) return 0;
    if (v > 255) return 255;
    return (uint8_t)v;
}

Color color_dim(Color c, float factor) {
    return (Color){
        clamp_u8(c.r * factor),
        clamp_u8(c.g * factor),
        clamp_u8(c.b * factor)
    };
}

Color color_lerp(Color a, Color b, float t) {
    return (Color){
        clamp_u8(a.r + (b.r - a.r) * t),
        clamp_u8(a.g + (b.g - a.g) * t),
        clamp_u8(a.b + (b.b - a.b) * t)
    };
}

Glyph glyph_dim(Glyph g, float factor) {

    if (glyph_is_placeholder(&g)) return g;
    g.fg = color_dim(g.fg, factor);
    g.bg = color_dim(g.bg, factor);
    return g;
}

#endif
