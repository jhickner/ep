#ifndef HELP_H
#define HELP_H

#include "screen.h"

typedef struct {
    const char *keys;
    const char *what;
} HelpRow;

void help_draw(Screen *s, const char *title, const HelpRow *rows, int n,
               Color fg, Color bg, Color accent);

#endif

#ifdef HELP_IMPLEMENTATION

#include <stdbool.h>

#define HP_GAP 2

static uint32_t hp_decode(const char *p, int *adv) {
    uint8_t c = (uint8_t)*p;
    if (c < 0x80) { *adv = 1; return c; }
    if ((c & 0xE0) == 0xC0 && (p[1] & 0xC0) == 0x80) {
        *adv = 2;
        return (uint32_t)(c & 0x1F) << 6 | (uint32_t)(p[1] & 0x3F);
    }
    if ((c & 0xF0) == 0xE0 && (p[1] & 0xC0) == 0x80 && (p[2] & 0xC0) == 0x80) {
        *adv = 3;
        return (uint32_t)(c & 0x0F) << 12 | (uint32_t)(p[1] & 0x3F) << 6 |
               (uint32_t)(p[2] & 0x3F);
    }
    if ((c & 0xF8) == 0xF0 && (p[1] & 0xC0) == 0x80 && (p[2] & 0xC0) == 0x80 &&
        (p[3] & 0xC0) == 0x80) {
        *adv = 4;
        return (uint32_t)(c & 0x07) << 18 | (uint32_t)(p[1] & 0x3F) << 12 |
               (uint32_t)(p[2] & 0x3F) << 6 | (uint32_t)(p[3] & 0x3F);
    }
    *adv = 1;
    return 0xFFFD;
}

static int hp_cols(const char *s) {
    int n = 0;
    for (const char *p = s; *p; p++)
        if (((unsigned char)*p & 0xC0) != 0x80) n++;
    return n;
}

static void hp_print(Screen *s, int x, int y, const char *str, int maxw,
                     Color fg, Color bg) {
    if (!str || maxw <= 0) return;
    int used = 0;
    const char *p = str;
    while (*p && used < maxw) {
        int adv = 1;
        uint32_t cp = hp_decode(p, &adv);
        screen_put(s, x + used, y, cp, fg, bg);
        p += adv;
        used++;
    }
    if (*p && used > 0) screen_put(s, x + used - 1, y, 0x2026, fg, bg);
}

static void hp_frame(Screen *s, int x, int y, int w, int h, Color fg, Color bg) {
    if (w < 2 || h < 2) return;
    screen_put(s, x, y, 0x256D, fg, bg);
    screen_put(s, x + w - 1, y, 0x256E, fg, bg);
    screen_put(s, x, y + h - 1, 0x2570, fg, bg);
    screen_put(s, x + w - 1, y + h - 1, 0x256F, fg, bg);
    for (int i = 1; i < w - 1; i++) {
        screen_put(s, x + i, y, 0x2500, fg, bg);
        screen_put(s, x + i, y + h - 1, 0x2500, fg, bg);
    }
    for (int i = 1; i < h - 1; i++) {
        screen_put(s, x, y + i, 0x2502, fg, bg);
        screen_put(s, x + w - 1, y + i, 0x2502, fg, bg);
    }
}

void help_draw(Screen *s, const char *title, const HelpRow *rows, int n,
               Color fg, Color bg, Color accent) {
    if (!s || !rows || n <= 0 || s->width < 6 || s->height < 3) return;

    int kw = 0, dw = 0, nw = 0;
    for (int i = 0; i < n; i++) {
        if (rows[i].keys) {
            int a = hp_cols(rows[i].keys);
            if (a > kw) kw = a;
            if (rows[i].what) {
                int b = hp_cols(rows[i].what);
                if (b > dw) dw = b;
            }
        } else if (rows[i].what) {
            int b = hp_cols(rows[i].what);
            if (b > nw) nw = b;
        }
    }

    int max_w = s->width - 4;
    int max_h = s->height - 2;
    if (max_w < 1) max_w = 1;
    if (max_h < 1) max_h = 1;

    int wide = kw + HP_GAP + dw;
    if (nw > wide) wide = nw;

    bool stacked = wide > max_w;
    int inner_w = wide, lines = n;

    if (stacked) {
        int st = kw;
        if (HP_GAP + dw > st) st = HP_GAP + dw;
        if (nw > st) st = nw;
        lines = 0;
        for (int i = 0; i < n; i++)
            lines += (rows[i].keys && rows[i].what) ? 2 : 1;
        if (lines > max_h) {
            stacked = false;
            inner_w = max_w;
            lines = n;
        } else {
            inner_w = st;
        }
    }
    if (inner_w > max_w) inner_w = max_w;

    bool clipped = lines > max_h;
    if (clipped) lines = max_h;

    int w = inner_w + 4, h = lines + 2;
    int x0 = (s->width - w) / 2, y0 = (s->height - h) / 2;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;

    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++)
            screen_put(s, x0 + x, y0 + y, ' ', fg, bg);
    hp_frame(s, x0, y0, w, h, fg, bg);
    if (title && w > 6) hp_print(s, x0 + 2, y0, title, w - 4, accent, bg);

    int tx = x0 + 2, avail = lines - (clipped ? 1 : 0), row = 0;

    for (int i = 0; i < n && row < avail; i++) {
        int y = y0 + 1 + row;
        if (!rows[i].keys) {
            if (rows[i].what) hp_print(s, tx, y, rows[i].what, inner_w, fg, bg);
            row++;
            continue;
        }
        if (stacked) {
            hp_print(s, tx, y, rows[i].keys, inner_w, fg, bg);
            row++;
            if (rows[i].what && row < avail)
                hp_print(s, tx + HP_GAP, y0 + 1 + row, rows[i].what,
                         inner_w - HP_GAP, fg, bg);
            else if (rows[i].what)
                break;
            row++;
        } else {
            hp_print(s, tx, y, rows[i].keys, inner_w, fg, bg);
            if (rows[i].what && inner_w - kw - HP_GAP >= 4)
                hp_print(s, tx + kw + HP_GAP, y, rows[i].what,
                         inner_w - kw - HP_GAP, fg, bg);
            row++;
        }
    }

    if (clipped) hp_print(s, tx, y0 + lines, "\xe2\x80\xa6", inner_w, fg, bg);
}

#endif
