
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdbool.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <limits.h>
#include <time.h>
#include <pthread.h>
#include <sys/stat.h>
#include <dirent.h>

#include "term.h"
#define SCREEN_KITTY
#include "kitty.h"
#include "screen.h"
#include "image.h"
#define DISKCACHE_IMPLEMENTATION
#include "diskcache.h"
#define CACHE_IMPLEMENTATION
#include "cache.h"
#define PANEL_IMPLEMENTATION
#include "panel.h"
#define PAGE_IMPLEMENTATION
#include "page.h"
#define BOOK_IMPLEMENTATION
#include "book.h"
#include "state.h"
#include "comic.h"

extern int g_cell_w, g_cell_h;
void ui_cell_size(void);

#define C_BG COLOR_DEFAULT_BG
#define C_FG COLOR_DEFAULT_FG

static const Color C_DIM   = { 0x82, 0x88, 0x94 };
static const Color C_SEL   = { 0x7a, 0xa2, 0xf7 };
static const Color C_PANEL = { 0x1b, 0x1d, 0x25 };
static const Color C_PTXT  = { 0xc8, 0xcc, 0xd4 };
static const Color C_ACC   = { 0xc9, 0x8a, 0x2b };
static const Color C_ERR   = { 0xd6, 0x45, 0x5f };

static const uint8_t BG_RGB[3] = { 0x11, 0x12, 0x16 };

typedef enum { VIEW_PAGE = 0, VIEW_THUMBS } View;
typedef enum { FIT_PAGE = 0, FIT_WIDTH } Fit;

static char **g_books;
static int    g_nbooks, g_book_idx;

static Book       g_book;
static PageStore  g_pages;
static Cache      g_thumbs;
static bool       g_thumbs_ready;

static int  g_page;
static int  g_panel;
static bool g_panel_mode;
static bool g_mode_forced;
static View g_view = VIEW_PAGE;
static Fit  g_fit = FIT_PAGE;
static int  g_scroll;
static int  g_thumb_size = 12;
static int  g_thumb_scroll;
static bool g_show_help;
static char g_status[256];

static Term   *g_tty;
static Screen *g_scr;

#define PAGE_MAX_DIM 3000
#define PAGE_SLOTS   5

#define MAIN_ID_BASE   0x00B14100u
#define MAIN_ID_SLOTS  4
#define THUMB_ID_BASE  0x00C25000u
#define THUMB_RESIDENT 48

#define KG_PROBE_MS 300

#define BOOK_CACHE_BUDGET  (2048ull * 1024 * 1024)
#define THUMB_CACHE_BUDGET (256u   * 1024 * 1024)

#define KEY_INTR 0x03

static uint32_t g_main_id;
static unsigned g_main_seq;

typedef struct { uint32_t id; int col, row, cols, rows; } Place;

static Place *g_places, *g_prev_places;
static int g_nplaces, g_nprev_places, g_places_cap;

static void push_place(uint32_t id, int col, int row, int cols, int rows) {
    if (g_nplaces == g_places_cap) {
        int cap = g_places_cap ? g_places_cap * 2 : 64;
        Place *a = (Place *)realloc(g_places, (size_t)cap * sizeof *a);
        Place *b = (Place *)realloc(g_prev_places, (size_t)cap * sizeof *b);
        if (!a || !b) { free(a); free(b); return; }
        g_places = a; g_prev_places = b; g_places_cap = cap;
    }
    g_places[g_nplaces++] = (Place){ id, col, row, cols, rows };
}

static void stamp_placeholders(void) {
    bool changed = (g_nplaces != g_nprev_places) ||
                   memcmp(g_places, g_prev_places, (size_t)g_nplaces * sizeof(Place)) != 0;
    if (changed) {
        for (int i = 0; i < g_nplaces; i++)
            kg_virtual_place(g_places[i].id, g_places[i].cols, g_places[i].rows);
        memcpy(g_prev_places, g_places, (size_t)g_nplaces * sizeof(Place));
        g_nprev_places = g_nplaces;
    }
    for (int i = 0; i < g_nplaces; i++) {
        const Place *p = &g_places[i];
        for (int r = 0; r < p->rows; r++)
            for (int c = 0; c < p->cols; c++)
                screen_set(g_scr, p->col + c, p->row + r, glyph_placeholder(p->id, r, c));
    }
}

static uint8_t *pad_to_cells(const uint8_t *rgb, int w, int h,
                             int ox, int oy, int pw, int ph) {
    if (pw <= 0 || ph <= 0) return NULL;
    uint8_t *out = (uint8_t *)calloc((size_t)pw * (size_t)ph, 4);
    if (!out) return NULL;
    for (int y = 0; y < h; y++) {
        int dy = oy + y;
        if (dy < 0 || dy >= ph) continue;
        const uint8_t *src = rgb + (size_t)y * (size_t)w * 3;
        uint8_t *dst = out + ((size_t)dy * (size_t)pw) * 4;
        for (int x = 0; x < w; x++) {
            int dx = ox + x;
            if (dx < 0 || dx >= pw) continue;
            dst[dx * 4 + 0] = src[x * 3 + 0];
            dst[dx * 4 + 1] = src[x * 3 + 1];
            dst[dx * 4 + 2] = src[x * 3 + 2];
            dst[dx * 4 + 3] = 0xFF;
        }
    }
    return out;
}

static uint8_t *crop_scale(const uint8_t *src, int sw, int sh,
                           int cx, int cy, int cw, int ch, int dw, int dh) {
    if (cx < 0) cx = 0;
    if (cy < 0) cy = 0;
    if (cx + cw > sw) cw = sw - cx;
    if (cy + ch > sh) ch = sh - cy;
    if (cw <= 0 || ch <= 0 || dw <= 0 || dh <= 0) return NULL;

    const uint8_t *base = src;
    uint8_t *cut = NULL;
    if (cx != 0 || cy != 0 || cw != sw || ch != sh) {
        cut = (uint8_t *)malloc((size_t)cw * ch * 3);
        if (!cut) return NULL;
        for (int y = 0; y < ch; y++)
            memcpy(cut + (size_t)y * cw * 3,
                   src + ((size_t)(cy + y) * sw + cx) * 3, (size_t)cw * 3);
        base = cut;
    }
    if (cw == dw && ch == dh) {
        if (cut) return cut;
        uint8_t *copy = (uint8_t *)malloc((size_t)cw * ch * 3);
        if (copy) memcpy(copy, base, (size_t)cw * ch * 3);
        return copy;
    }
    uint8_t *out = image_scale(base, cw, ch, dw, dh);
    free(cut);
    return out;
}

static int npages(void) { return g_book.npages; }

static int page_panel_count(int page) {
    const Panel *p = NULL;
    int n = 0, w, h;
    if (pages_borrow(&g_pages, page, &w, &h, &p, &n)) pages_release(&g_pages, page);
    return n;
}

static void clamp_page(void) {
    if (g_page < 0) g_page = 0;
    if (g_page >= npages()) g_page = npages() - 1;
    if (g_page < 0) g_page = 0;
}

static void goto_page(int page, bool from_end) {
    clamp_page();
    if (page < 0 || page >= npages()) return;
    g_page = page;
    g_scroll = from_end ? INT_MAX : 0;

    g_panel = from_end ? INT_MAX : 0;
    pages_set_focus(&g_pages, g_page);
}

static void step_forward(void) {
    if (g_panel_mode) {
        int n = page_panel_count(g_page);
        if (n > 0 && g_panel < n - 1) { g_panel++; return; }
    }
    if (g_page + 1 < npages()) goto_page(g_page + 1, false);
}

static void step_back(void) {
    if (g_panel_mode && g_panel > 0) { g_panel--; return; }
    if (g_page > 0) goto_page(g_page - 1, true);
}

#define MODE_PANEL 1
#define MODE_WIDTH 2

static int reading_mode(void) {
    return (g_panel_mode ? MODE_PANEL : 0) | (g_fit == FIT_WIDTH ? MODE_WIDTH : 0);
}

static void set_reading_mode(int mode) {
    g_panel_mode = (mode & MODE_PANEL) != 0;
    g_fit = (mode & MODE_WIDTH) ? FIT_WIDTH : FIT_PAGE;
}

static void save_place(void) {
    state_save(g_book.path, g_page, g_panel, 0,
               g_page + 1, g_book.npages, reading_mode());
}

void comic_import_state(void) {
    const char *home = getenv("HOME");
    if (!home) return;

    char dir[PATH_MAX], marker[PATH_MAX];
    state_dir(dir, sizeof dir);
    snprintf(marker, sizeof marker, "%s/cbr-imported", dir);
    struct stat st;
    if (stat(marker, &st) == 0) return;

    char old[PATH_MAX];
    snprintf(old, sizeof old, "%s/.config/cbr/state", home);
    FILE *f = fopen(old, "r");
    if (!f) return;

    char (*lines)[PATH_MAX + 64] = malloc(sizeof(*lines) * 500);
    int n = 0;
    if (lines) while (n < 500 && fgets(lines[n], sizeof lines[0], f)) n++;
    fclose(f);

    for (int i = n - 1; i >= 0; i--) {
        char *p = lines[i];
        char *nl = strchr(p, '\n');
        if (nl) *nl = '\0';

        long long num[5] = { 0, 0, 0, 0, 0 };
        int nnum = 0;
        while (nnum < 5) {
            char *tab = strchr(p, '\t');
            if (!tab || tab == p) break;
            bool digits = true;
            for (char *q = p; q < tab && digits; q++)
                if (*q < '0' || *q > '9') digits = false;
            if (!digits) break;
            *tab = '\0';
            num[nnum++] = atoll(p);
            p = tab + 1;
        }
        if (nnum < 1 || !*p || stat(p, &st) != 0) continue;
        state_save(p, (int)num[0], nnum > 4 ? (int)num[4] : 0, 0,
                   (int)num[0] + 1, nnum > 2 ? (int)num[2] : 0,
                   nnum > 3 ? (int)num[3] : 0);
    }
    free(lines);

    mkdir(dir, 0755);
    FILE *m = fopen(marker, "w");
    if (m) fclose(m);
}

bool comic_accept(const char *path, const char *name, bool is_dir, void *ctx) {
    (void)name; (void)ctx;
    return is_dir || book_is_comic(path);
}

bool comic_dir_is_book(const char *dir) {
    DIR *d = opendir(dir);
    if (!d) return false;
    struct dirent *de;
    bool found = false;
    while (!found && (de = readdir(d)))
        if (de->d_name[0] != '.' && book_is_image(de->d_name)) found = true;
    closedir(d);
    return found;
}

static bool browse_has_content(const char *dir) {
    DIR *d = opendir(dir);
    if (!d) return false;
    struct dirent *de;
    bool any = false;
    while (!any && (de = readdir(d))) {
        if (de->d_name[0] == '.') continue;
        if (book_is_image(de->d_name) || book_is_comic(de->d_name)) any = true;
    }
    closedir(d);
    return any;
}

#define BROWSE_LOOKAHEAD 64

bool comic_dir_is_shelf(const char *dir) {
    DIR *d = opendir(dir);
    if (!d) return false;

    bool comic = false, image = false;
    char (*subs)[PATH_MAX] = malloc(sizeof(*subs) * BROWSE_LOOKAHEAD);
    int nsubs = 0;
    struct dirent *de;
    while (!comic && (de = readdir(d))) {
        if (de->d_name[0] == '.') continue;
        char p[PATH_MAX];
        if (snprintf(p, sizeof p, "%s/%s", dir, de->d_name) >= (int)sizeof p) continue;
        struct stat st;
        if (stat(p, &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) {
            if (subs && nsubs < BROWSE_LOOKAHEAD) snprintf(subs[nsubs++], PATH_MAX, "%s", p);
        }
        else if (book_is_comic(p))          comic = true;
        else if (book_is_image(de->d_name)) image = true;
    }
    closedir(d);

    bool shelf = comic;
    if (!shelf) {
        if (!image) shelf = (nsubs > 0);
        else for (int i = 0; i < nsubs && !shelf; i++) shelf = browse_has_content(subs[i]);
    }
    free(subs);
    return shelf;
}

bool comic_leaf(const char *path, void *ctx) {
    (void)ctx;
    return comic_dir_is_book(path) && !comic_dir_is_shelf(path);
}

static void drop_terminal_images(void) {
    kg_delete_all();
    g_nprev_places = -1;
    g_main_id = 0;
}

static void close_book(void) {
    if (!g_book.npages) return;
    save_place();
    if (g_thumbs_ready) { cache_destroy(&g_thumbs); g_thumbs_ready = false; }
    pages_set_list(&g_pages, NULL, 0);
    book_close(&g_book);
    drop_terminal_images();
}

static bool open_book(int idx) {
    if (idx < 0 || idx >= g_nbooks) return false;
    close_book();

    if (!book_open(g_books[idx], &g_book)) {
        snprintf(g_status, sizeof g_status, "cannot open %s", g_books[idx]);
        return false;
    }
    g_book_idx = idx;
    pages_set_list(&g_pages, g_book.pages, g_book.npages);
    g_thumbs_ready = cache_init(&g_thumbs, g_book.npages, 3, BG_RGB, true);

    g_panel = 0;
    g_scroll = 0;

    StateLine at;
    bool known = state_lookup(g_book.path, &at);
    g_page = known ? at.spine : 0;
    if (known && !g_mode_forced) {
        set_reading_mode(at.mode);
        if (at.mode & MODE_PANEL) g_panel = at.block;
    }
    clamp_page();
    pages_set_focus(&g_pages, g_page);
    g_status[0] = '\0';
    return true;
}

typedef struct {
    int page, panel, fit, scroll;
    int cols, rows, cell_w, cell_h;
    int book;
} MainKey;

static MainKey g_shown;
static bool    g_have_shown;

static void invalidate_main(void) { g_have_shown = false; }

static bool stage_main(int box_x, int box_y, int box_cw, int box_ch) {

    int lim = kg_max_rowcolumn() + 1;
    if (box_cw > lim) { box_x += (box_cw - lim) / 2; box_cw = lim; }
    if (box_ch > lim) { box_y += (box_ch - lim) / 2; box_ch = lim; }
    if (box_cw < 2 || box_ch < 2) return false;

    int pw = 0, ph = 0, np = 0;
    const Panel *panels = NULL;
    const uint8_t *rgb = pages_borrow(&g_pages, g_page, &pw, &ph, &panels, &np);
    if (!rgb) return false;

    if (g_panel_mode && np > 0) {
        if (g_panel >= np) g_panel = np - 1;
        if (g_panel < 0) g_panel = 0;
    }

    int cx = 0, cy = 0, cw = pw, ch = ph;
    if (g_panel_mode && np > 0) {
        cx = panels[g_panel].x; cy = panels[g_panel].y;
        cw = panels[g_panel].w; ch = panels[g_panel].h;
    } else if (g_fit == FIT_WIDTH) {

        ch = (int)((int64_t)pw * (box_ch * g_cell_h) / (box_cw * g_cell_w));
        if (ch > ph) ch = ph;
        if (g_scroll > ph - ch) g_scroll = ph - ch;
        if (g_scroll < 0) g_scroll = 0;
        cy = g_scroll;
    }

    int box_px_w = box_cw * g_cell_w, box_px_h = box_ch * g_cell_h;
    int dw, dh;
    image_fit(cw, ch, box_px_w, box_px_h, &dw, &dh);

    int free_x = box_px_w - dw, free_y = box_px_h - dh;
    if (free_x < 0) free_x = 0;
    if (free_y < 0) free_y = 0;
    int left = free_x / 2, top = free_y / 2;

    int col0 = left / g_cell_w, row0 = top / g_cell_h;
    int cols = (left + dw + g_cell_w - 1) / g_cell_w - col0;
    int rows = (top + dh + g_cell_h - 1) / g_cell_h - row0;
    if (col0 + cols > box_cw) cols = box_cw - col0;
    if (row0 + rows > box_ch) rows = box_ch - row0;
    if (cols < 1 || rows < 1) { pages_release(&g_pages, g_page); return false; }

    MainKey key = { g_page, g_panel_mode ? g_panel : -1, (int)g_fit, cy,
                    box_cw, box_ch, g_cell_w, g_cell_h, g_book_idx };

    if (!g_have_shown || memcmp(&key, &g_shown, sizeof key) != 0 || !g_main_id) {
        uint8_t *scaled = crop_scale(rgb, pw, ph, cx, cy, cw, ch, dw, dh);
        if (!scaled) { pages_release(&g_pages, g_page); return false; }
        uint8_t *padded = pad_to_cells(scaled, dw, dh,
                                       left - col0 * g_cell_w, top - row0 * g_cell_h,
                                       cols * g_cell_w, rows * g_cell_h);
        free(scaled);
        if (!padded) { pages_release(&g_pages, g_page); return false; }

        uint32_t id = MAIN_ID_BASE + (g_main_seq++ % MAIN_ID_SLOTS);
        kg_delete(id);
        kg_transmit_ex(id, padded, cols * g_cell_w, rows * g_cell_h, 4);
        free(padded);
        g_main_id = id;
        g_shown = key;
        g_have_shown = true;
    }

    pages_release(&g_pages, g_page);
    push_place(g_main_id, box_x + col0, box_y + row0, cols, rows);
    return true;
}

static void draw_centered(int x, int y, int w, const char *msg, Color c) {
    int len = (int)strlen(msg);
    screen_print(g_scr, x + (w - len) / 2, y, msg, c, C_BG);
}

static void draw_page(void) {
    int box_cw = g_scr->width, box_ch = g_scr->height - 1;
    if (box_cw < 2 || box_ch < 2 || !npages()) return;

    pages_want(&g_pages, g_page);
    pages_want(&g_pages, g_page + 1);
    pages_want(&g_pages, g_page - 1);
    pages_want(&g_pages, g_page + 2);

    if (!stage_main(0, 0, box_cw, box_ch)) {
        int st = pages_state(&g_pages, g_page);
        const char *msg = (st == PAGE_FAILED) ? "cannot decode this page" : "…";
        draw_centered(0, box_ch / 2, box_cw, msg,
                      st == PAGE_FAILED ? C_ERR : C_DIM);
    }
}

typedef struct { int cols, rows, tw, th, ow, oh, x0; } Geom;

static Geom thumb_geom(void) {
    Geom g;
    g.tw = g_thumb_size;

    g.th = (int)((double)g.tw * g_cell_w * 1.5 / g_cell_h) + 1;
    g.ow = g.tw + 2;
    g.oh = g.th + 3;
    g.cols = g_scr->width / g.ow;
    if (g.cols < 1) g.cols = 1;
    g.rows = (g_scr->height - 1) / g.oh;
    if (g.rows < 1) g.rows = 1;
    g.x0 = (g_scr->width - g.cols * g.ow) / 2;
    return g;
}

static bool stage_thumb(int idx, int box_x, int box_y, int box_cw, int box_ch) {
    Slot s;
    cache_peek(&g_thumbs, idx, &s);
    if (s.state != SLOT_READY || s.w <= 0 || s.h <= 0) return false;
    if (s.w > box_cw * g_cell_w || s.h > box_ch * g_cell_h) return false;

    uint32_t id = THUMB_ID_BASE + (uint32_t)idx;
    int free_x = box_cw * g_cell_w - s.w, free_y = box_ch * g_cell_h - s.h;
    if (free_x < 0) free_x = 0;
    if (free_y < 0) free_y = 0;
    int left = free_x / 2, top = free_y / 2;

    int col0 = left / g_cell_w, row0 = top / g_cell_h;
    int cols = (left + s.w + g_cell_w - 1) / g_cell_w - col0;
    int rows = (top + s.h + g_cell_h - 1) / g_cell_h - row0;
    if (col0 + cols > box_cw) cols = box_cw - col0;
    if (row0 + rows > box_ch) rows = box_ch - row0;
    if (cols < 1 || rows < 1) return false;

    int w, h;
    uint8_t *rgb = cache_take_pixels(&g_thumbs, idx, &w, &h);
    if (rgb) {
        uint8_t *padded = pad_to_cells(rgb, w, h, left - col0 * g_cell_w,
                                       top - row0 * g_cell_h,
                                       cols * g_cell_w, rows * g_cell_h);
        free(rgb);
        if (!padded) return false;
        kg_transmit_ex(id, padded, cols * g_cell_w, rows * g_cell_h, 4);
        free(padded);
    } else {
        cache_touch(&g_thumbs, idx);
    }
    push_place(id, box_x + col0, box_y + row0, cols, rows);
    return true;
}

static void draw_box_frame(int x, int y, int w, int h, Color fg) {
    if (w < 2 || h < 2) return;
    screen_put(g_scr, x, y, 0x256D, fg, C_BG);
    screen_put(g_scr, x + w - 1, y, 0x256E, fg, C_BG);
    screen_put(g_scr, x, y + h - 1, 0x2570, fg, C_BG);
    screen_put(g_scr, x + w - 1, y + h - 1, 0x256F, fg, C_BG);
    for (int i = 1; i < w - 1; i++) {
        screen_put(g_scr, x + i, y, 0x2500, fg, C_BG);
        screen_put(g_scr, x + i, y + h - 1, 0x2500, fg, C_BG);
    }
    for (int i = 1; i < h - 1; i++) {
        screen_put(g_scr, x, y + i, 0x2502, fg, C_BG);
        screen_put(g_scr, x + w - 1, y + i, 0x2502, fg, C_BG);
    }
}

static void draw_thumbs(void) {
    if (!g_thumbs_ready || !npages()) return;
    Geom g = thumb_geom();

    int total_rows = (npages() + g.cols - 1) / g.cols;
    int sel_row = g_page / g.cols;
    if (sel_row < g_thumb_scroll) g_thumb_scroll = sel_row;
    if (sel_row >= g_thumb_scroll + g.rows) g_thumb_scroll = sel_row - g.rows + 1;
    if (g_thumb_scroll > total_rows - g.rows) g_thumb_scroll = total_rows - g.rows;
    if (g_thumb_scroll < 0) g_thumb_scroll = 0;

    int px_w = g.tw * g_cell_w, px_h = g.th * g_cell_h;
    cache_set_focus(&g_thumbs, g_page);

    int first = g_thumb_scroll * g.cols, last = (g_thumb_scroll + g.rows) * g.cols;
    cache_set_window(&g_thumbs, first - g.cols * 2, last + g.cols * 2);

    for (int r = 0; r < g.rows; r++) {
        for (int c = 0; c < g.cols; c++) {
            int idx = (g_thumb_scroll + r) * g.cols + c;
            if (idx >= npages()) continue;
            int x = g.x0 + c * g.ow, y = r * g.oh;

            cache_request(&g_thumbs, idx, g_book.pages[idx], px_w, px_h);
            if (!stage_thumb(idx, x + 1, y + 1, g.tw, g.th))
                screen_put(g_scr, x + 1 + g.tw / 2, y + 1 + g.th / 2, 0xB7, C_DIM, C_BG);

            char label[16];
            snprintf(label, sizeof label, "%d", idx + 1);
            int lw = (int)strlen(label);
            screen_print(g_scr, x + 1 + (g.tw - lw) / 2, y + g.th + 1, label,
                         idx == g_page ? C_SEL : C_DIM, C_BG);
            if (idx == g_page) draw_box_frame(x, y, g.ow, g.th + 3, C_SEL);
        }
    }

    for (int i = first - g.cols; i < last + g.cols; i++) {
        if (i < 0 || i >= npages() || (i >= first && i < last)) continue;
        cache_request(&g_thumbs, i, g_book.pages[i], px_w, px_h);
    }
}

static void draw_status(void) {
    int y = g_scr->height - 1;
    char left[512];
    int used;

    if (!npages()) {
        screen_print(g_scr, 1, y, "no pages", C_DIM, C_BG);
        return;
    }

    used = snprintf(left, sizeof left, " %s  %d/%d",
                    g_book.title, g_page + 1, npages());
    if (g_panel_mode) {
        int n = page_panel_count(g_page);
        if (n > 0)
            snprintf(left + used, sizeof left - (size_t)used,
                     "  panel %d/%d", g_panel + 1, n);
        else
            snprintf(left + used, sizeof left - (size_t)used, "  panel …");
    }
    screen_print(g_scr, 0, y, left, C_FG, C_BG);

    if (g_status[0]) {
        int x = (int)strlen(left) + 2;
        if (x < g_scr->width - 4) screen_print(g_scr, x, y, g_status, C_ACC, C_BG);
    }

    const char *mode = g_view == VIEW_THUMBS ? "thumbs"
                     : g_panel_mode          ? "panels"
                     : g_fit == FIT_WIDTH    ? "width" : "page";
    char right[96];
    snprintf(right, sizeof right, "%s  ? help ", mode);
    int rx = g_scr->width - (int)strlen(right);
    if (rx > (int)strlen(left)) screen_print(g_scr, rx, y, right, C_DIM, C_BG);
}

static void draw_help(void) {
    static const char *lines[] = {
        "",
        "  space / l / →     forward   (next panel in panel mode)",
        "  b / h / ←         back",
        "  n / p             next / previous page, always",
        "  f                 panel mode on/off",
        "  w                 fit width; j/k scroll",
        "  g / G             first / last page",
        "  tab               thumbnails; enter opens the page",
        "  ] / [             next / previous book",
        "  + / -             thumbnail size",
        "  ctrl-l            redraw from scratch",
        "  ?                 this help",
        "  q  ctrl-c        quit",
        "",
        "  The reading position is remembered per book.",
        "",
    };
    int n = (int)(sizeof lines / sizeof *lines);
    int w = 62, x = (g_scr->width - w) / 2, y = (g_scr->height - n) / 2;
    if (x < 0 || y < 0) return;
    for (int i = 0; i < n; i++) {
        for (int c = 0; c < w; c++) screen_put(g_scr, x + c, y + i, ' ', C_PTXT, C_PANEL);
        screen_print(g_scr, x, y + i, lines[i], C_PTXT, C_PANEL);
    }
}

static void evict_thumbs(void) {
    if (!g_thumbs_ready) return;
    for (int guard = 0; guard < 16; guard++) {
        int v = cache_lru_victim(&g_thumbs, THUMB_RESIDENT);
        if (v < 0) break;
        kg_delete(THUMB_ID_BASE + (uint32_t)v);
        cache_mark_evicted(&g_thumbs, v);
    }
}

static void render(void) {
    g_nplaces = 0;
    screen_clear(g_scr, glyph_make(' ', C_FG, C_BG));

    if (g_view == VIEW_THUMBS) draw_thumbs();
    else                       draw_page();

    stamp_placeholders();
    draw_status();
    if (g_show_help) draw_help();

    kg_placeholder_redraw_begin();
    screen_render(g_scr);
    kg_placeholder_redraw_end();
    screen_swap(g_scr);
    evict_thumbs();
    term_flush();
}

static bool scroll_by(int cells) {
    if (g_fit != FIT_WIDTH || g_panel_mode) return false;
    int pw = 0, ph = 0;
    const uint8_t *rgb = pages_borrow(&g_pages, g_page, &pw, &ph, NULL, NULL);
    if (!rgb) return false;
    pages_release(&g_pages, g_page);

    int box_px_w = g_scr->width * g_cell_w;
    int box_px_h = (g_scr->height - 1) * g_cell_h;
    if (box_px_w < 1 || box_px_h < 1) return false;

    int shown = (int)((int64_t)pw * box_px_h / box_px_w);
    if (shown >= ph) return false;
    int max_scroll = ph - shown;

    int per_cell = (int)((int64_t)pw * g_cell_h / box_px_w);
    if (per_cell < 1) per_cell = 1;

    int before = g_scroll;
    g_scroll += cells * per_cell;
    if (g_scroll < 0) g_scroll = 0;
    if (g_scroll > max_scroll) g_scroll = max_scroll;
    return g_scroll != before;
}

static void switch_book(int delta) {
    if (g_nbooks < 2) return;
    int next = g_book_idx + delta;
    if (next < 0 || next >= g_nbooks) return;
    if (open_book(next)) invalidate_main();
}

static void handle_key(const InputEvent *ev, bool *quit, bool *dirty) {

    if (g_show_help) {
        g_show_help = false;
        *dirty = true;
        if (ev->code == KEY_CHAR &&
            (ev->ch == '?' || ev->ch == 'q' || ev->ch == KEY_INTR)) {
            if (ev->ch != '?') *quit = true;
            return;
        }
    }

    Geom g = thumb_geom();
    int row_step = (g_view == VIEW_THUMBS) ? g.cols : 1;

    switch (ev->code) {
        case KEY_SPACE:
        case KEY_RIGHT:
            if (g_view == VIEW_THUMBS) { g_page++; clamp_page(); }
            else step_forward();
            *dirty = true;
            break;
        case KEY_LEFT:
        case KEY_BACKSPACE:
            if (g_view == VIEW_THUMBS) { g_page--; clamp_page(); }
            else step_back();
            *dirty = true;
            break;
        case KEY_DOWN:
            if (g_view == VIEW_THUMBS) { g_page += row_step; clamp_page(); }
            else if (!scroll_by(3)) step_forward();
            *dirty = true;
            break;
        case KEY_UP:
            if (g_view == VIEW_THUMBS) { g_page -= row_step; clamp_page(); }
            else if (!scroll_by(-3)) step_back();
            *dirty = true;
            break;
        case KEY_PAGE_DOWN:
            if (g_view == VIEW_THUMBS) g_page += g.cols * g.rows;
            else g_page++;
            clamp_page();
            goto_page(g_page, false);
            *dirty = true;
            break;
        case KEY_PAGE_UP:
            if (g_view == VIEW_THUMBS) g_page -= g.cols * g.rows;
            else g_page--;
            clamp_page();
            goto_page(g_page, false);
            *dirty = true;
            break;
        case KEY_MOUSE_WHEEL_DOWN:
            if (g_view == VIEW_THUMBS) { g_page += g.cols; clamp_page(); }
            else if (!scroll_by(3)) step_forward();
            *dirty = true;
            break;
        case KEY_MOUSE_WHEEL_UP:
            if (g_view == VIEW_THUMBS) { g_page -= g.cols; clamp_page(); }
            else if (!scroll_by(-3)) step_back();
            *dirty = true;
            break;
        case KEY_ENTER:
            if (g_view == VIEW_THUMBS) {
                g_view = VIEW_PAGE;
                goto_page(g_page, false);
            } else {
                step_forward();
            }
            *dirty = true;
            break;
        case KEY_TAB:
            g_view = (g_view == VIEW_THUMBS) ? VIEW_PAGE : VIEW_THUMBS;
            *dirty = true;
            break;
        case KEY_ESCAPE:
            if (g_view == VIEW_THUMBS) { g_view = VIEW_PAGE; *dirty = true; }
            else *quit = true;
            break;
        case KEY_CHAR:
            switch (ev->ch) {
                case 'q':
                case KEY_INTR:
                    *quit = true;
                    break;
                case 'l': step_forward(); *dirty = true; break;
                case 'h':
                case 'b': step_back(); *dirty = true; break;
                case 'n': goto_page(g_page + 1, false); *dirty = true; break;
                case 'p': goto_page(g_page - 1, false); *dirty = true; break;
                case 'j':
                    if (g_view == VIEW_THUMBS) { g_page += row_step; clamp_page(); }
                    else if (!scroll_by(3)) step_forward();
                    *dirty = true;
                    break;
                case 'k':
                    if (g_view == VIEW_THUMBS) { g_page -= row_step; clamp_page(); }
                    else if (!scroll_by(-3)) step_back();
                    *dirty = true;
                    break;
                case 'g': goto_page(0, false); *dirty = true; break;
                case 'G': goto_page(npages() - 1, false); *dirty = true; break;
                case 'f':

                    g_panel_mode = !g_panel_mode;
                    g_view = VIEW_PAGE;
                    if (g_panel_mode) {
                        int n = page_panel_count(g_page);
                        snprintf(g_status, sizeof g_status,
                                 n == 1 ? "panel mode - this page is one panel"
                                        : "panel mode");
                    } else {
                        g_status[0] = '\0';
                    }
                    *dirty = true;
                    break;
                case 'w':
                    g_fit = (g_fit == FIT_WIDTH) ? FIT_PAGE : FIT_WIDTH;
                    g_scroll = 0;
                    *dirty = true;
                    break;
                case ']': switch_book(1); *dirty = true; break;
                case '[': switch_book(-1); *dirty = true; break;
                case '+':
                case '=':
                    if (g_thumb_size < 40) { g_thumb_size++; *dirty = true; }
                    break;
                case '-':
                    if (g_thumb_size > 4) { g_thumb_size--; *dirty = true; }
                    break;
                case '?': g_show_help = !g_show_help; *dirty = true; break;
                case 0x0c:

                    drop_terminal_images();
                    invalidate_main();
                    if (g_thumbs_ready)
                        for (int i = 0; i < npages(); i++) cache_mark_evicted(&g_thumbs, i);
                    g_scr->force_redraw = true;
                    *dirty = true;
                    break;
                default: break;
            }
            break;
        default: break;
    }
}

bool comic_is_file(const char *path) { return book_is_comic(path); }

static void *prune_thread(void *keep) {
    dc_prune(THUMB_CACHE_BUDGET);
    book_cache_prune(BOOK_CACHE_BUDGET, (const char *)keep);
    kg_sweep_stale_tempfiles(3600);
    free(keep);
    return NULL;
}

static void start_prune(const char *keep) {
    char *copy = keep ? strdup(keep) : NULL;
    pthread_t t;
    if (pthread_create(&t, NULL, prune_thread, copy) == 0) pthread_detach(t);
    else free(copy);
}

int comic_read(char **paths, int npaths, const ComicOpts *o, Term *tm, Screen *scr) {
    g_tty = tm;
    g_scr = scr;
    g_books = paths;
    g_nbooks = npaths;
    g_panel_mode = o->panel_mode;
    g_fit = o->fit_width ? FIT_WIDTH : FIT_PAGE;
    g_mode_forced = o->forced;

    char thumbdir[PATH_MAX];
    snprintf(thumbdir, sizeof thumbdir, "%s/thumbs", book_cache_root());
    dc_init(thumbdir);

    if (!pages_init(&g_pages, BG_RGB, PAGE_SLOTS, 2, PAGE_MAX_DIM)) {
        fprintf(stderr, "ep: cannot start decoder threads\n");
        return 1;
    }

    if (!open_book(0)) {
        fprintf(stderr, "ep: cannot read %s\n", paths[0]);
        pages_destroy(&g_pages);
        return 1;
    }

    start_prune(g_book.extracted ? g_book.dir : NULL);

    bool quit = false, dirty = true;
    int prev_page = -1;

    while (!quit) {
        InputEvent ev;
        while (term_poll_event(g_tty, &ev, 0)) {
            if (ev.code == KEY_PASTE) { free(ev.paste); continue; }
            if (ev.code == KEY_RESIZE) {
                term_get_size(g_tty);
                ui_cell_size();
                screen_resize(g_scr, g_tty->width, g_tty->height);
                g_scr->force_redraw = true;
                g_nprev_places = -1;
                invalidate_main();
                dirty = true;
                continue;
            }
            handle_key(&ev, &quit, &dirty);
        }

        if (g_page != prev_page) {
            prev_page = g_page;
            pages_set_focus(&g_pages, g_page);
            g_status[0] = '\0';
            dirty = true;
        }

        if (pages_take_completions(&g_pages)) dirty = true;
        if (g_thumbs_ready && cache_take_completions(&g_thumbs)) dirty = true;

        if (dirty) { render(); dirty = false; }

        struct timespec ts = { 0, 16L * 1000 * 1000 };
        nanosleep(&ts, NULL);
    }

    close_book();
    pages_destroy(&g_pages);
    free(g_places);
    free(g_prev_places);
    g_places = g_prev_places = NULL;
    g_nplaces = g_nprev_places = g_places_cap = 0;
    return 0;
}
