
#ifndef PANEL_H
#define PANEL_H

#include <stdint.h>
#include <stdbool.h>

typedef struct { int x, y, w, h; } Panel;

int panel_detect(const uint8_t *rgb, int w, int h, Panel *out, int max);

#endif

#ifdef PANEL_IMPLEMENTATION

#include <stdlib.h>
#include <string.h>

#define PANEL_ANALYSIS 1600

#define PANEL_TOL        46
#define PANEL_ROW_CLEAN  0.990
#define PANEL_MIN_FRAC   0.030
#define PANEL_MIN_AREA   0.010
#define PANEL_MIN_INK    0.015
#define PANEL_PAGE_INK   0.300
#define PANEL_FLAT       26

#ifndef PANEL_GUTTER_DIV
#define PANEL_GUTTER_DIV 400
#endif

#define PANEL_MAX_DEPTH  12

#define PANEL_EDGE_STEP  2
#define PANEL_EDGE_TONE  40

#ifndef PANEL_EDGE_TONE_ROW
#define PANEL_EDGE_TONE_ROW 33
#endif
#define PANEL_EDGE_FRAC  0.55
#define PANEL_EDGE_RUN   1

#define PANEL_INSET_MIN_W    0.12
#define PANEL_INSET_MIN_H    0.08
#define PANEL_INSET_MIN_AREA 0.04
#define PANEL_INSET_MAX_AREA 0.60

#define PANEL_OPEN_MIN_AREA  0.15
#define PANEL_OPEN_MAX_AREA  0.85

#define PANEL_INSET_HOST_AREA 0.25
#ifndef PANEL_MAX_INSETS
#define PANEL_MAX_INSETS     8
#endif

#define PANEL_SIDE_TOP   1
#define PANEL_SIDE_BOT   2
#define PANEL_SIDE_LEFT  4
#define PANEL_SIDE_RIGHT 8
#define PANEL_SIDE_ALL   15

typedef struct {
    uint8_t *g;
    int w, h;
    int bg;
    int gutter;
    int min_w, min_h;
    long min_area;
} PanelCtx;

static uint8_t *panel_gray(const uint8_t *rgb, int w, int h, int *aw, int *ah) {
    int step = 1;
    while ((w / step > PANEL_ANALYSIS || h / step > PANEL_ANALYSIS) && step < 64) step++;
    int ow = w / step, oh = h / step;
    if (ow < 8 || oh < 8) { ow = w; oh = h; step = 1; }

    uint8_t *g = (uint8_t *)malloc((size_t)ow * oh);
    if (!g) return NULL;

    for (int y = 0; y < oh; y++) {
        const uint8_t *src = rgb + (size_t)y * step * w * 3;
        uint8_t *dst = g + (size_t)y * ow;
        for (int x = 0; x < ow; x++) {
            const uint8_t *p = src + (size_t)x * step * 3;
            dst[x] = (uint8_t)((p[0] * 77 + p[1] * 150 + p[2] * 29) >> 8);
        }
    }
    *aw = ow; *ah = oh;
    return g;
}

static int panel_bg_border(const uint8_t *g, int w, int h) {
    int hist[256] = {0};
    int ring = h / 50 + 1, total = 0;
    for (int y = 0; y < h; y++) {
        bool edge_row = (y < ring || y >= h - ring);
        for (int x = 0; x < w; x++) {
            if (!edge_row && x >= ring && x < w - ring) continue;
            hist[g[(size_t)y * w + x]]++;
            total++;
        }
    }
    int half = total / 2, acc = 0;
    for (int v = 0; v < 256; v++) { acc += hist[v]; if (acc >= half) return v; }
    return 255;
}

static int panel_bg_flat(const uint8_t *g, int w, int h, int edge) {
    int hist[256] = {0};
    long total = 0;

    for (int y = 0; y < h; y++) {
        const uint8_t *row = g + (size_t)y * w;
        int lo = 255, hi = 0;
        long sum = 0;
        for (int x = 0; x < w; x++) {
            int v = row[x];
            if (v < lo) lo = v;
            if (v > hi) hi = v;
            sum += v;
        }
        if (hi - lo <= PANEL_FLAT) { hist[(int)(sum / w)] += w; total += w; }
    }
    for (int x = 0; x < w; x++) {
        int lo = 255, hi = 0;
        long sum = 0;
        for (int y = 0; y < h; y++) {
            int v = g[(size_t)y * w + x];
            if (v < lo) lo = v;
            if (v > hi) hi = v;
            sum += v;
        }
        if (hi - lo <= PANEL_FLAT) { hist[(int)(sum / h)] += h; total += h; }
    }
    if (total < w + h) return -1;

    int best = -1;
    long best_score = 0;
    for (int v = 0; v < 256; v++) {
        long score = 0;
        for (int d = -3; d <= 3; d++)
            if (v + d >= 0 && v + d < 256) score += hist[v + d];
        if (score > best_score) { best_score = score; best = v; }
    }

    int gap = best - edge;
    bool border_agrees = (gap < 0 ? -gap : gap) <= PANEL_TOL;

    if (best > 45 && best < 210 && !border_agrees) {
        for (int v = 0; v < 256; v++) {
            if (v > 45 && v < 210) continue;
            long score = 0;
            for (int d = -3; d <= 3; d++)
                if (v + d >= 0 && v + d < 256) score += hist[v + d];
            if (score * 4 >= best_score) return v;
        }
    }
    return best;
}

static int panel_bg(const uint8_t *g, int w, int h) {
    int edge = panel_bg_border(g, w, h);
    int v = panel_bg_flat(g, w, h, edge);
    return v >= 0 ? v : edge;
}

static inline bool panel_is_bg(const PanelCtx *c, int v) {
    int d = v - c->bg;
    return (d < 0 ? -d : d) <= PANEL_TOL;
}

static int panel_inset_cut(const PanelCtx *c, int x, int y, int w, int h,
                           int sides, int depth, Panel *out, int *n, int max);

static void panel_trim(const PanelCtx *c, int *x, int *y, int *w, int *h) {
    int need = (int)(*w * PANEL_ROW_CLEAN);
    while (*h > 1) {
        int n = 0;
        for (int i = 0; i < *w; i++) n += panel_is_bg(c, c->g[(size_t)*y * c->w + *x + i]);
        if (n < need) break;
        (*y)++; (*h)--;
    }
    while (*h > 1) {
        int n = 0, row = *y + *h - 1;
        for (int i = 0; i < *w; i++) n += panel_is_bg(c, c->g[(size_t)row * c->w + *x + i]);
        if (n < need) break;
        (*h)--;
    }
    need = (int)(*h * PANEL_ROW_CLEAN);
    while (*w > 1) {
        int n = 0;
        for (int i = 0; i < *h; i++) n += panel_is_bg(c, c->g[(size_t)(*y + i) * c->w + *x]);
        if (n < need) break;
        (*x)++; (*w)--;
    }
    while (*w > 1) {
        int n = 0, col = *x + *w - 1;
        for (int i = 0; i < *h; i++) n += panel_is_bg(c, c->g[(size_t)(*y + i) * c->w + col]);
        if (n < need) break;
        (*w)--;
    }
}

static int panel_segments(const bool *clean, int lo, int len, int gutter,
                          int *seg_lo, int *seg_len, int max_seg) {
    int n = 0, start = lo, i = lo, end = lo + len;
    while (i < end) {
        if (!clean[i]) { i++; continue; }
        int run = i;
        while (run < end && clean[run]) run++;
        bool interior = (i > start) && (run < end);
        if (interior && run - i >= gutter && n < max_seg - 1) {
            seg_lo[n] = start; seg_len[n] = i - start; n++;
            start = run;
        }
        i = run;
    }
    if (n == 0) return 1;
    seg_lo[n] = start; seg_len[n] = end - start; n++;
    return n;
}

static bool panel_has_ink_frac(const PanelCtx *c, int x, int y, int w, int h,
                               double frac) {
    long ink = 0, total = (long)w * h;
    for (int j = 0; j < h; j++) {
        const uint8_t *row = c->g + (size_t)(y + j) * c->w + x;
        for (int i = 0; i < w; i++) if (!panel_is_bg(c, row[i])) ink++;
    }
    return ink * 1000 >= (long)(frac * 1000) * total;
}

static bool panel_has_ink(const PanelCtx *c, int x, int y, int w, int h) {
    return panel_has_ink_frac(c, x, y, w, h, PANEL_MIN_INK);
}

static void panel_emit(const PanelCtx *c, int x, int y, int w, int h,
                       Panel *out, int *n, int max) {
    if (*n >= max) return;
    if (w < c->min_w || h < c->min_h) return;
    if ((long)w * h < c->min_area) return;
    if (!panel_has_ink(c, x, y, w, h)) return;
    out[*n].x = x; out[*n].y = y; out[*n].w = w; out[*n].h = h;
    (*n)++;
}

static bool panel_leaf(PanelCtx *c, int x, int y, int w, int h,
                       Panel *out, int *n, int max) {
    if ((long)w * h < (long)((long)c->w * c->h * PANEL_INSET_HOST_AREA)) return false;

    Panel boxes[64];
    int nb = 0;
    int framed = panel_inset_cut(c, x, y, w, h, 0, 0, boxes, &nb, 64);

    if (nb < 2 || nb > PANEL_MAX_INSETS || framed < 1) return false;

    for (int i = 0; i < nb && *n < max; i++) {
        int bx = boxes[i].x, by = boxes[i].y, bw = boxes[i].w, bh = boxes[i].h;
        panel_trim(c, &bx, &by, &bw, &bh);
        if (bw > 1 && bh > 1) { boxes[i].x = bx; boxes[i].y = by; boxes[i].w = bw; boxes[i].h = bh; }
        out[(*n)++] = boxes[i];
    }
    return true;
}

static void panel_cut(PanelCtx *c, int x, int y, int w, int h, int depth,
                      Panel *out, int *n, int max) {
    if (w <= 0 || h <= 0 || *n >= max) return;
    panel_trim(c, &x, &y, &w, &h);
    if (w < c->min_w || h < c->min_h || depth >= PANEL_MAX_DEPTH) {
        panel_emit(c, x, y, w, h, out, n, max);
        return;
    }

    bool *clean = (bool *)malloc((size_t)(c->w > c->h ? c->w : c->h));
    if (!clean) { panel_emit(c, x, y, w, h, out, n, max); return; }

    int *seg_lo = (int *)malloc(sizeof(int) * 64);
    int *seg_len = (int *)malloc(sizeof(int) * 64);
    if (!seg_lo || !seg_len) {
        free(clean); free(seg_lo); free(seg_len);
        panel_emit(c, x, y, w, h, out, n, max);
        return;
    }

    int need = (int)(w * PANEL_ROW_CLEAN);
    for (int j = y; j < y + h; j++) {
        int cnt = 0;
        const uint8_t *row = c->g + (size_t)j * c->w + x;
        for (int i = 0; i < w; i++) cnt += panel_is_bg(c, row[i]);
        clean[j] = cnt >= need;
    }
    int nseg = panel_segments(clean, y, h, c->gutter, seg_lo, seg_len, 64);
    if (nseg > 1) {
        for (int s = 0; s < nseg; s++)
            panel_cut(c, x, seg_lo[s], w, seg_len[s], depth + 1, out, n, max);
        free(clean); free(seg_lo); free(seg_len);
        return;
    }

    need = (int)(h * PANEL_ROW_CLEAN);
    for (int i = x; i < x + w; i++) {
        int cnt = 0;
        for (int j = 0; j < h; j++) cnt += panel_is_bg(c, c->g[(size_t)(y + j) * c->w + i]);
        clean[i] = cnt >= need;
    }
    nseg = panel_segments(clean, x, w, c->gutter, seg_lo, seg_len, 64);
    if (nseg > 1) {
        for (int s = 0; s < nseg; s++)
            panel_cut(c, seg_lo[s], y, seg_len[s], h, depth + 1, out, n, max);
    } else if (!panel_leaf(c, x, y, w, h, out, n, max)) {
        panel_emit(c, x, y, w, h, out, n, max);
    }
    free(clean); free(seg_lo); free(seg_len);
}

static bool panel_inset_ok(const PanelCtx *c, int x, int y, int w, int h,
                           bool framed) {
    if (w < c->w * PANEL_INSET_MIN_W || h < c->h * PANEL_INSET_MIN_H) return false;
    long area = (long)w * h, page = (long)c->w * c->h;
    double lo = framed ? PANEL_INSET_MIN_AREA : PANEL_OPEN_MIN_AREA;
    double hi = framed ? PANEL_INSET_MAX_AREA : PANEL_OPEN_MAX_AREA;
    if (area < (long)(page * lo) || area > (long)(page * hi)) return false;
    return framed || panel_has_ink(c, x, y, w, h);
}

static bool panel_edge_row(const PanelCtx *c, int x, int y, int w) {
    if (y < PANEL_EDGE_STEP) return false;
    const uint8_t *a = c->g + (size_t)y * c->w + x;
    const uint8_t *b = c->g + (size_t)(y - PANEL_EDGE_STEP) * c->w + x;
    int n = 0;
    for (int i = 0; i < w; i++) {
        int d = a[i] - b[i];
        if (d < 0) d = -d;
        if (d > PANEL_EDGE_TONE_ROW) n++;
    }
    return n >= (int)(w * PANEL_EDGE_FRAC);
}

static bool panel_edge_col(const PanelCtx *c, int x, int y, int h) {
    if (x < PANEL_EDGE_STEP) return false;
    int n = 0;
    for (int j = 0; j < h; j++) {
        const uint8_t *row = c->g + (size_t)(y + j) * c->w;
        int d = row[x] - row[x - PANEL_EDGE_STEP];
        if (d < 0) d = -d;
        if (d > PANEL_EDGE_TONE) n++;
    }
    return n >= (int)(h * PANEL_EDGE_FRAC);
}

static int panel_inset_cut(const PanelCtx *c, int x, int y, int w, int h,
                           int sides, int depth, Panel *out, int *n, int max) {
    if (w <= 0 || h <= 0 || *n >= max) return 0;

    while (h > 2 && panel_edge_row(c, x, y, w))         { y++; h--; sides |= PANEL_SIDE_TOP; }
    while (h > 2 && panel_edge_row(c, x, y + h - 1, w)) { h--;      sides |= PANEL_SIDE_BOT; }
    while (w > 2 && panel_edge_col(c, x, y, h))         { x++; w--; sides |= PANEL_SIDE_LEFT; }
    while (w > 2 && panel_edge_col(c, x + w - 1, y, h)) { w--;      sides |= PANEL_SIDE_RIGHT; }

    if (w < c->min_w || h < c->min_h) return 0;

    if (depth < PANEL_MAX_DEPTH) {
        bool *line = (bool *)malloc((size_t)(c->w > c->h ? c->w : c->h));
        int *seg_lo = (int *)malloc(sizeof(int) * 64);
        int *seg_len = (int *)malloc(sizeof(int) * 64);
        Panel *kids = (Panel *)malloc(sizeof(Panel) * 64);
        if (line && seg_lo && seg_len && kids) {
            for (int pass = 0; pass < 2; pass++) {
                int nseg;
                if (pass == 0) {
                    for (int j = y; j < y + h; j++) line[j] = panel_edge_row(c, x, j, w);
                    nseg = panel_segments(line, y, h, PANEL_EDGE_RUN, seg_lo, seg_len, 64);
                } else {
                    for (int i = x; i < x + w; i++) line[i] = panel_edge_col(c, i, y, h);
                    nseg = panel_segments(line, x, w, PANEL_EDGE_RUN, seg_lo, seg_len, 64);
                }
                if (nseg <= 1) continue;

                int nk = 0, framed = 0, pend = -1, last_first = 0;
                for (int t = 0; t < nseg; t++) {
                    int sub, first = (t == 0), last = (t == nseg - 1);
                    int before = nk;
                    if (pass == 0) {
                        sub  = sides & (PANEL_SIDE_LEFT | PANEL_SIDE_RIGHT);
                        sub |= first ? (sides & PANEL_SIDE_TOP) : PANEL_SIDE_TOP;
                        sub |= last  ? (sides & PANEL_SIDE_BOT) : PANEL_SIDE_BOT;
                        framed += panel_inset_cut(c, x, seg_lo[t], w, seg_len[t],
                                                  sub, depth + 1, kids, &nk, 64);
                    } else {
                        sub  = sides & (PANEL_SIDE_TOP | PANEL_SIDE_BOT);
                        sub |= first ? (sides & PANEL_SIDE_LEFT) : PANEL_SIDE_LEFT;
                        sub |= last  ? (sides & PANEL_SIDE_RIGHT) : PANEL_SIDE_RIGHT;
                        framed += panel_inset_cut(c, seg_lo[t], y, seg_len[t], h,
                                                  sub, depth + 1, kids, &nk, 64);
                    }

                    if (nk == before) {
                        if (pend < 0) pend = seg_lo[t];
                    } else {
                        if (pend >= 0) {
                            for (int i = before; i < nk; i++) {
                                Panel *k = &kids[i];
                                if (pass == 0) { k->h += k->y - pend; k->y = pend; }
                                else           { k->w += k->x - pend; k->x = pend; }
                            }
                            pend = -1;
                        }
                        last_first = before;
                    }
                }
                if (pend >= 0 && nk > 0) {
                    for (int i = last_first; i < nk; i++) {
                        Panel *k = &kids[i];
                        if (pass == 0) k->h = y + h - k->y;
                        else           k->w = x + w - k->x;
                    }
                }

                if (nk > 0 && framed > 0) {
                    for (int i = 0; i < nk && *n < max; i++) out[(*n)++] = kids[i];
                    free(line); free(seg_lo); free(seg_len); free(kids);
                    return framed;
                }
            }
        }
        free(line); free(seg_lo); free(seg_len); free(kids);
    }

    bool framed = (sides == PANEL_SIDE_ALL);
    if (panel_inset_ok(c, x, y, w, h, framed)) {
        out[*n].x = x; out[*n].y = y; out[*n].w = w; out[*n].h = h;
        (*n)++;
        return framed ? 1 : 0;
    }
    return 0;
}

int panel_detect(const uint8_t *rgb, int w, int h, Panel *out, int max) {
    Panel whole = { 0, 0, w, h };
    if (max < 1) return 0;
    out[0] = whole;
    if (!rgb || w < 16 || h < 16) return 1;

    PanelCtx c;
    memset(&c, 0, sizeof c);
    c.g = panel_gray(rgb, w, h, &c.w, &c.h);
    if (!c.g) return 1;

    c.bg = panel_bg(c.g, c.w, c.h);
    c.gutter  = c.h / PANEL_GUTTER_DIV + 2;
    c.min_w   = (int)(c.w * PANEL_MIN_FRAC) + 1;
    c.min_h   = (int)(c.h * PANEL_MIN_FRAC) + 1;
    c.min_area = (long)(c.w * c.h * PANEL_MIN_AREA);

    if (c.bg >= 210 && !panel_has_ink_frac(&c, 0, 0, c.w, c.h, PANEL_PAGE_INK)) {
        free(c.g);
        return 1;
    }

    int n = 0;
    Panel found[128];
    int cap = max < 128 ? max : 128;
    panel_cut(&c, 0, 0, c.w, c.h, 0, found, &n, cap);

    bool sane = (n >= 1 && n <= 40);
    if (sane && n == 1) {

        long area = (long)found[0].w * found[0].h;
        if (area * 100 >= (long)c.w * c.h * 88) sane = false;
    }

    if (!sane) { free(c.g); return 1; }

    if (n >= 2 && n < cap) {
        memmove(found + 1, found, (size_t)n * sizeof *found);
        found[0].x = 0; found[0].y = 0; found[0].w = c.w; found[0].h = c.h;
        n++;
    }

    double sx = (double)w / c.w, sy = (double)h / c.h;
    int padx = (int)(w * 0.006) + 1, pady = (int)(h * 0.004) + 1;
    for (int i = 0; i < n; i++) {
        int x0 = (int)(found[i].x * sx) - padx;
        int y0 = (int)(found[i].y * sy) - pady;
        int x1 = (int)((found[i].x + found[i].w) * sx) + padx;
        int y1 = (int)((found[i].y + found[i].h) * sy) + pady;
        if (x0 < 0) x0 = 0;
        if (y0 < 0) y0 = 0;
        if (x1 > w) x1 = w;
        if (y1 > h) y1 = h;
        out[i].x = x0; out[i].y = y0; out[i].w = x1 - x0; out[i].h = y1 - y0;
    }
    free(c.g);
    return n;
}

#endif
