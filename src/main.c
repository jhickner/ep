/**
 * ep - a terminal epub reader
 *
 * One chapter of the spine is laid out at a time into a fixed-width column of
 * wrapped lines; reading position is a (spine, block) pair, so it survives
 * resizing and column-width changes.
 */

#define _GNU_SOURCE
#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#define TERM_IMPLEMENTATION
#include "term.h"
#define KITTY_IMPLEMENTATION
#include "kitty.h"
#define SCREEN_KITTY
#define SCREEN_IMPLEMENTATION
#include "screen.h"
#define IMAGE_IMPLEMENTATION
#include "image.h"
#define ZIP_IMPLEMENTATION
#include "zip.h"
#define XML_IMPLEMENTATION
#include "xml.h"
#define EPUB_IMPLEMENTATION
#include "epub.h"
#define DOC_IMPLEMENTATION
#include "doc.h"
#define LAYOUT_IMPLEMENTATION
#include "layout.h"
#define STATE_IMPLEMENTATION
#include "state.h"
#define PICK_IMPLEMENTATION
#include "pick.h"
#include "pdf.h"    /* implemented in pdf.c: its CoreGraphics headers and
                       screen.h both want to own the name "Style" */
#include "type.h"   /* implemented in type.c, for the same reason */

#include <time.h>

/* A printed page, near enough: mass-market paperbacks run about this many
   characters, and counting characters keeps the number stable across window
   sizes and column widths. */
#define CHARS_PER_PAGE 2000

#define C_BG   COLOR_DEFAULT_BG
#define C_FG   COLOR_DEFAULT_FG
static const Color C_DIM = { 0x80, 0x82, 0x8a };
static const Color C_ACC = { 0xc9, 0x8a, 0x2b };
static const Color C_SEL = { 0x28, 0x2c, 0x38 };

/* An image block's cell rectangle, resolved once per layout. */
typedef struct {
    int      block;
    uint32_t id;
    int      cols, rows;
    bool     sent;
    char     file[PATH_MAX];
} ImgPlace;

typedef struct {
    Epub   book;
    char   path[PATH_MAX];

    int    spine;          /* current chapter */
    Doc    doc;
    Layout lay;
    int    top;            /* first visible line */

    int    width;          /* preferred text column width */
    int    cols;           /* width actually used, clamped to the terminal */
    Term  *term;

    bool   toc_open;
    int    toc_sel, toc_top;

    ImgPlace *img; int nimg;
    int    page_h;

    int   *pg;             /* line each page starts on */
    int    npg;

    int   *cprefix;        /* characters before each chapter */
    int    ctotal;
} Reader;

#define KG_PROBE_MS 300

static bool g_graphics;
static int  g_cell_w = 10, g_cell_h = 20;

/* What the terminal said about its own colours, asked for at the same time as
   the graphics probe and for the same reason: both read stdin directly. */
static bool    g_bg_known, g_fg_known, g_bg_light;
static uint8_t g_term_fg[3];
static char g_tmpdir[PATH_MAX];

/* ------------------------------------------------------------- progress -- */

/* Chapters are parsed once at open to size the book in characters, which is
   what turns a position into a page number. The whole of a novel parses in
   about ten milliseconds, so this is not worth deferring. */
static void measure_book(Reader *r) {
    r->cprefix = calloc((size_t)r->book.nspine + 1, sizeof *r->cprefix);
    if (!r->cprefix) return;
    for (int i = 0; i < r->book.nspine; i++) {
        char *xhtml = epub_read(&r->book, r->book.spine[i], NULL);
        int chars = 0;
        if (xhtml) {
            char base[512];
            snprintf(base, sizeof base, "%s", r->book.spine[i]);
            char *slash = strrchr(base, '/');
            if (slash) slash[1] = 0; else base[0] = 0;
            Doc d = doc_parse(xhtml, base);
            for (int b = 0; b < d.nblocks; b++) chars += d.blocks[b].len + 1;
            doc_free(&d);
            free(xhtml);
        }
        r->cprefix[i + 1] = r->cprefix[i] + chars;
    }
    r->ctotal = r->cprefix[r->book.nspine];
}

static int page_total(const Reader *r) {
    return r->ctotal / CHARS_PER_PAGE + 1;
}

static int page_at(const Reader *r, int block) {
    if (!r->cprefix) return 1;
    int chars = r->cprefix[r->spine];
    for (int i = 0; i < block && i < r->doc.nblocks; i++)
        chars += r->doc.blocks[i].len + 1;
    return chars / CHARS_PER_PAGE + 1;
}

/* The first line at or below `from` that comes from a real block; blank
   spacers carry no position of their own. */
static int first_real_line(const Reader *r, int from) {
    for (int i = from < 0 ? 0 : from; i < r->lay.n; i++)
        if (r->lay.lines[i].block >= 0) return i;
    return -1;
}

/* Byte offset of a line's text within its block. Wrapping changes which
   offsets start a line, but not the offsets themselves, so this is the part
   of the position that survives a change of column width. */
static int line_off(const Reader *r, const Line *ln) {
    if (ln->block < 0 || ln->type != BLK_TEXT || !ln->text) return 0;
    const Block *b = &r->doc.blocks[ln->block];
    if (!b->text || ln->text < b->text || ln->text > b->text + b->len) return 0;
    return (int)(ln->text - b->text);
}

/* Where the top of the window sits, as a (block, offset) pair. */
static void top_pos(const Reader *r, int *block, int *off) {
    int i = first_real_line(r, r->top);
    if (i < 0) { *block = 0; *off = 0; return; }
    *block = r->lay.lines[i].block;
    *off   = line_off(r, &r->lay.lines[i]);
}

static int top_block(const Reader *r) {
    int i = first_real_line(r, r->top);
    return i < 0 ? 0 : r->lay.lines[i].block;
}

static void progress_save(Reader *r) {
    int block, off;
    top_pos(r, &block, &off);
    state_save(r->path, r->spine, block, off, page_at(r, block), page_total(r), 0);
}

/* ---------------------------------------------------------------- images -- */

/* Images have to reach image.h as files, so each one is unpacked once into a
   private temp directory that goes away with the process. */
typedef struct { char *zip_path; char file[PATH_MAX]; } Extracted;
static Extracted *g_extracted;
static int        g_nextracted;

static const char *extract(Epub *e, const char *zip_path) {
    for (int i = 0; i < g_nextracted; i++)
        if (strcmp(g_extracted[i].zip_path, zip_path) == 0)
            return g_extracted[i].file[0] ? g_extracted[i].file : NULL;

    g_extracted = realloc(g_extracted, (size_t)(g_nextracted + 1) * sizeof *g_extracted);
    Extracted *x = &g_extracted[g_nextracted++];
    x->zip_path = strdup(zip_path);
    x->file[0] = 0;

    size_t len = 0;
    void *data = epub_read(e, zip_path, &len);
    if (!data) return NULL;

    const char *dot = strrchr(zip_path, '.');
    snprintf(x->file, sizeof x->file, "%s/%d%s", g_tmpdir, g_nextracted, dot ? dot : ".img");
    FILE *f = fopen(x->file, "wb");
    if (!f) { free(data); x->file[0] = 0; return NULL; }
    fwrite(data, 1, len, f);
    fclose(f);
    free(data);
    return x->file;
}

static void tmpdir_cleanup(void) {
    for (int i = 0; i < g_nextracted; i++)
        if (g_extracted[i].file[0]) unlink(g_extracted[i].file);
    if (g_tmpdir[0]) rmdir(g_tmpdir);
}

static uint8_t *pad_to_cells(const uint8_t *rgb, int w, int h, int pw, int ph) {
    uint8_t *out = calloc((size_t)pw * (size_t)ph, 4);   /* alpha 0 = terminal bg */
    if (!out) return NULL;
    int ox = (pw - w) / 2, oy = (ph - h) / 2;
    for (int y = 0; y < h; y++) {
        int dy = oy + y;
        if (dy < 0 || dy >= ph) continue;
        const uint8_t *src = rgb + (size_t)y * (size_t)w * 3;
        uint8_t *dst = out + ((size_t)dy * (size_t)pw + (size_t)ox) * 4;
        for (int x = 0; x < w; x++) {
            if (ox + x < 0 || ox + x >= pw) continue;
            dst[x * 4 + 0] = src[x * 3 + 0];
            dst[x * 4 + 1] = src[x * 3 + 1];
            dst[x * 4 + 2] = src[x * 3 + 2];
            dst[x * 4 + 3] = 255;
        }
    }
    return out;
}

static ImgPlace *place_for(Reader *r, int block) {
    for (int i = 0; i < r->nimg; i++)
        if (r->img[i].block == block) return &r->img[i];
    return NULL;
}

static void places_clear(Reader *r) {
    for (int i = 0; i < r->nimg; i++)
        if (r->img[i].sent) kg_delete(r->img[i].id);
    free(r->img);
    r->img = NULL;
    r->nimg = 0;
}

static int img_rows(const Block *b, int width, void *ctx) {
    Reader *r = ctx;
    if (!g_graphics || !b->src) return 1;

    const char *file = extract(&r->book, b->src);
    int iw, ih;
    if (!file || !image_probe(file, &iw, &ih)) return 1;

    int box_rows = r->page_h - 2;
    if (box_rows < 3) box_rows = 3;
    int cols, rows, px_w, px_h;
    kg_fit_cells(iw, ih, g_cell_w, g_cell_h, width, box_rows, &cols, &rows,
                 &px_w, &px_h);
    if (cols < 1 || rows < 1) return 1;

    static uint32_t next_id = 1;
    r->img = realloc(r->img, (size_t)(r->nimg + 1) * sizeof *r->img);
    ImgPlace *p = &r->img[r->nimg++];
    memset(p, 0, sizeof *p);
    p->block = (int)(b - r->doc.blocks);
    p->id    = next_id++;
    p->cols  = cols;
    p->rows  = rows;
    snprintf(p->file, sizeof p->file, "%s", file);
    return rows;
}

/* Decode and hand the pixels to the terminal; the cells that show it are
   written per visible row by draw_line(). */
static bool img_send(ImgPlace *p) {
    if (p->sent) return true;
    int px_w = p->cols * g_cell_w, px_h = p->rows * g_cell_h;
    static const uint8_t bg[3] = { 0, 0, 0 };
    Image im = {0};
    if (!image_load_fit(p->file, bg, px_w, px_h, &im)) return false;
    uint8_t *padded = pad_to_cells(im.rgb, im.w, im.h, px_w, px_h);
    image_free(&im);
    if (!padded) return false;
    kg_transmit_ex(p->id, padded, px_w, px_h, 4);
    kg_virtual_place(p->id, p->cols, p->rows);
    free(padded);
    p->sent = true;
    return true;
}

/* Page breaks are computed once per layout rather than derived from the top
   line, so that turning back lands on the same boundary it was turned from.
   A break never falls inside an image, and never strands a heading at the foot
   of a page; blank lines at a break are swallowed. */
static void repaginate(Reader *r) {
    free(r->pg);
    r->pg = NULL;
    r->npg = 0;
    int h = r->page_h > 0 ? r->page_h : 1;

    int i = 0;
    while (i < r->lay.n) {
        r->pg = realloc(r->pg, (size_t)(r->npg + 1) * sizeof *r->pg);
        r->pg[r->npg++] = i;

        int end = i + h;
        if (end < r->lay.n) {
            int e = end;

            /* Back up out of an image that the break would cut in half. Images
               are laid out no taller than a page, so this always terminates
               above the break. */
            while (e > i && r->lay.lines[e].type == BLK_IMG && r->lay.lines[e].img_row > 0) e--;

            /* Back up off a heading whose first lines would sit alone at the
               foot of the page. */
            int b = r->lay.lines[e - 1].block;
            if (b >= 0 && b < r->doc.nblocks && r->doc.blocks[b].heading) {
                int k = e - 1;
                while (k > i && r->lay.lines[k - 1].block == b) k--;
                e = k;
            }

            if (e > i) end = e;
        }
        i = end;
        while (i < r->lay.n && r->lay.lines[i].block < 0) i++;   /* blanks */
    }
    if (r->npg == 0) {
        r->pg = realloc(r->pg, sizeof *r->pg);
        r->pg[r->npg++] = 0;
    }
}

/* The page `line` sits on. */
static int page_of(const Reader *r, int line) {
    int p = 0;
    for (int i = 0; i < r->npg && r->pg[i] <= line; i++) p = i;
    return p;
}

static void relayout(Reader *r, int keep_block, int keep_off) {
    layout_free(&r->lay);
    places_clear(r);
    r->cols = r->width;
    if (r->term && r->cols > r->term->width - 4) r->cols = r->term->width - 4;
    if (r->cols < 20) r->cols = 20;
    r->lay = layout_doc(&r->doc, r->cols, img_rows, r);
    repaginate(r);
    r->top = 0;
    if (keep_block > 0 || keep_off > 0) {
        /* The last line of the block that starts at or before the saved
           offset - that is the line the offset falls on at this width. */
        for (int i = 0; i < r->lay.n; i++) {
            if (r->lay.lines[i].block != keep_block) continue;
            if (line_off(r, &r->lay.lines[i]) > keep_off) break;
            r->top = i;
        }
    }
    /* Reading always starts at the top of a page, so a resumed or jumped-to
       position never opens mid-page. */
    r->top = r->pg[page_of(r, r->top)];
}

static void load_chapter(Reader *r, int spine, int block, int off) {
    if (spine < 0) spine = 0;
    if (spine >= r->book.nspine) spine = r->book.nspine - 1;
    r->spine = spine;

    doc_free(&r->doc);
    layout_free(&r->lay);

    char *xhtml = epub_read(&r->book, r->book.spine[spine], NULL);
    char base[512];
    snprintf(base, sizeof base, "%s", r->book.spine[spine]);
    char *slash = strrchr(base, '/');
    if (slash) slash[1] = 0; else base[0] = 0;

    if (xhtml) {
        r->doc = doc_parse(xhtml, base);
        free(xhtml);
    }
    relayout(r, block, off);
}

/* Re-wrap at the current width, holding the reader's place. */
static void relayout_keeping(Reader *r) {
    int block, off;
    top_pos(r, &block, &off);
    relayout(r, block, off);
}

static const char *chapter_title(Reader *r) {
    const char *best = NULL;
    for (int i = 0; i < r->book.ntoc; i++)
        if (r->book.toc[i].spine == r->spine && r->book.toc[i].title) {
            best = r->book.toc[i].title;
            break;
        }
    if (best && *best) return best;
    const char *p = strrchr(r->book.spine[r->spine], '/');
    return p ? p + 1 : r->book.spine[r->spine];
}

/* ------------------------------------------------------------- drawing -- */

static void draw_line(Reader *r, Screen *s, int x, int y, const Line *ln, int maxw) {
    if (ln->type == BLK_RULE) {
        int w = maxw / 3;
        for (int i = 0; i < w; i++)
            screen_set(s, x + (maxw - w) / 2 + i, y,
                       (Glyph){ .codepoint = 0x2500, .fg = C_DIM, .bg = C_BG });
        return;
    }
    if (ln->type == BLK_IMG) {
        ImgPlace *p = place_for(r, ln->block);
        if (p && img_send(p)) {
            int cx = x + (maxw - p->cols) / 2;
            for (int c = 0; c < p->cols; c++)
                screen_set(s, cx + c, y, glyph_placeholder(p->id, ln->img_row, c));
            return;
        }
        const char *name = ln->text ? strrchr(ln->text, '/') : NULL;
        char label[128];
        snprintf(label, sizeof label, "[ %s ]", name ? name + 1 : "image");
        int w = u8_cols(label, (int)strlen(label));
        screen_print(s, x + (maxw - w) / 2, y, label, C_DIM, C_BG);
        return;
    }

    int cols = u8_cols(ln->text, ln->len);
    int cx = x + (ln->center ? (maxw - cols) / 2 : ln->indent);
    if (cx < x) cx = x;

    for (int i = 0; i < ln->len; ) {
        unsigned char c = (unsigned char)ln->text[i];
        int adv = 1;
        uint32_t cp = c;
        if ((c & 0xF8) == 0xF0)      { adv = 4; cp = c & 0x07; }
        else if ((c & 0xF0) == 0xE0) { adv = 3; cp = c & 0x0F; }
        else if ((c & 0xE0) == 0xC0) { adv = 2; cp = c & 0x1F; }
        for (int k = 1; k < adv && i + k < ln->len; k++)
            cp = cp << 6 | ((unsigned char)ln->text[i + k] & 0x3F);

        unsigned char st = ln->style ? ln->style[i] : 0;
        Glyph g = { .codepoint = cp, .fg = (st & DS_BOLD) ? C_ACC : C_FG, .bg = C_BG };
        g.style = (uint8_t)(((st & DS_BOLD) ? STYLE_BOLD : 0) |
                            ((st & DS_ITALIC) ? STYLE_ITALIC : 0));
        screen_set(s, cx++, y, g);
        i += adv;
    }
}

static void draw_status(Reader *r, Screen *s) {
    int y = s->height - 1;
    for (int x = 0; x < s->width; x++)
        screen_put(s, x, y, ' ', C_DIM, C_BG);

    char left[256];
    snprintf(left, sizeof left, "%s", chapter_title(r));
    screen_print(s, 1, y, left, C_DIM, C_BG);

    int total = page_total(r);
    int page  = page_at(r, top_block(r));
    if (page > total) page = total;
    char right[128];
    snprintf(right, sizeof right, "%d/%d  %d%%   ? help",
             page, total, total ? page * 100 / total : 100);
    int rw = (int)strlen(right);
    screen_print(s, s->width - rw - 1, y, right, C_DIM, C_BG);
}

static void draw_toc(Reader *r, Screen *s) {
    int w = s->width - 8 < 70 ? s->width - 8 : 70;
    if (w < 20) w = s->width - 2;
    int h = s->height - 6;
    if (h < 5) h = s->height - 2;
    int x0 = (s->width - w) / 2, y0 = (s->height - h) / 2;

    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++)
            screen_put(s, x0 + x, y0 + y, ' ', C_FG, C_SEL);

    screen_print(s, x0 + 2, y0, " Contents ", C_ACC, C_SEL);

    int rows = h - 2;
    if (r->toc_sel < r->toc_top) r->toc_top = r->toc_sel;
    if (r->toc_sel >= r->toc_top + rows) r->toc_top = r->toc_sel - rows + 1;

    for (int i = 0; i < rows; i++) {
        int idx = r->toc_top + i;
        if (idx >= r->book.ntoc) break;
        EpubTocEntry *t = &r->book.toc[idx];
        char buf[512];
        snprintf(buf, sizeof buf, "%*s%s", t->depth * 2, "", t->title ? t->title : "?");
        bool sel = idx == r->toc_sel;
        Color bg = sel ? C_ACC : C_SEL;
        Color fg = sel ? C_SEL : (t->spine == r->spine ? C_ACC : C_FG);
        for (int x = 1; x < w - 1; x++) screen_put(s, x0 + x, y0 + 1 + i, ' ', fg, bg);
        screen_print(s, x0 + 2, y0 + 1 + i, buf, fg, bg);
    }
}

static const char *HELP[] = {
    "  \xe2\x86\x92 / space / f     next page",
    "  \xe2\x86\x90 / b             previous page",
    "  j k / \xe2\x86\x91 \xe2\x86\x93         nudge a line",
    "  n / p              next / previous chapter",
    "  g / G              chapter start / end",
    "  t                  table of contents",
    "  - / +              narrower / wider column",
    "  q                  quit (position is saved)",
};

static void draw_help(Screen *s) {
    int n = (int)(sizeof HELP / sizeof *HELP);
    int w = 40, h = n + 2;
    int x0 = (s->width - w) / 2, y0 = (s->height - h) / 2;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++)
            screen_put(s, x0 + x, y0 + y, ' ', C_FG, C_SEL);
    screen_print(s, x0 + 2, y0, " Keys ", C_ACC, C_SEL);
    for (int i = 0; i < n; i++)
        screen_print(s, x0 + 1, y0 + 1 + i, HELP[i], C_FG, C_SEL);
}

/* ---------------------------------------------------------------- input -- */

static void scroll_by(Reader *r, int delta, int page_h) {
    (void)page_h;
    int top = r->top + delta;

    while (top >= r->lay.n) {                 /* run on into later chapters */
        if (r->spine >= r->book.nspine - 1) { top = r->lay.n - 1; break; }
        int over = top - r->lay.n;
        load_chapter(r, r->spine + 1, 0, 0);
        top = over;
    }
    while (top < 0) {                         /* and back into earlier ones */
        if (r->spine <= 0) { top = 0; break; }
        int under = -top;
        load_chapter(r, r->spine - 1, 0, 0);
        top = r->lay.n - under;
    }

    if (top < 0) top = 0;
    if (r->lay.n > 0 && top > r->lay.n - 1) top = r->lay.n - 1;
    r->top = top;
}

/* One page forward or back, running on into the next or previous chapter. A
   chapter always begins at its own first page, so nothing is skipped at the
   join and nothing is shown twice. */
static void turn_page(Reader *r, int dir) {
    int p = page_of(r, r->top);
    if (dir > 0) {
        if (p + 1 < r->npg) { r->top = r->pg[p + 1]; return; }
        if (r->spine < r->book.nspine - 1) { load_chapter(r, r->spine + 1, 0, 0); r->top = 0; }
        return;
    }
    if (r->top > r->pg[p]) { r->top = r->pg[p]; return; }   /* nudged off-grid */
    if (p > 0) { r->top = r->pg[p - 1]; return; }
    if (r->spine > 0) {
        load_chapter(r, r->spine - 1, 0, 0);
        r->top = r->pg[r->npg - 1];
    }
}

static void goto_toc(Reader *r, int idx) {
    if (idx < 0 || idx >= r->book.ntoc) return;
    EpubTocEntry *t = &r->book.toc[idx];
    int spine = t->spine;
    if (spine < 0) {
        for (int i = 0; i < r->book.nspine; i++)
            if (strcmp(r->book.spine[i], t->href) == 0) { spine = i; break; }
    }
    if (spine < 0) return;
    load_chapter(r, spine, 0, 0);
    if (t->anchor) {
        int block = doc_anchor_block(&r->doc, t->anchor);
        if (block >= 0)
            for (int i = 0; i < r->lay.n; i++)
                if (r->lay.lines[i].block >= block) { r->top = i; break; }
        r->top = r->pg[page_of(r, r->top)];
    }
}

/* -------------------------------------------------------------- pickers -- */

static bool is_epub(const char *path) {
    const char *dot = strrchr(path, '.');
    return dot && strcasecmp(dot, ".epub") == 0;
}

static bool is_pdf(const char *path) {
    const char *dot = strrchr(path, '.');
    return dot && strcasecmp(dot, ".pdf") == 0;
}

static bool is_readable_book(const char *path) {
    return is_epub(path) || is_pdf(path);
}

static bool book_accept(const char *path, const char *name, bool is_dir, void *ctx) {
    (void)name; (void)ctx;
    return is_dir || is_readable_book(path);
}

#define RESUME_MAX 40

/* A typed filter over a picker's rows. Entries that do not match drop out, and
   what is left is ordered best match first. */
typedef struct { int idx, score, order; } FilterRow;

static int filter_cmp(const void *x, const void *y) {
    const FilterRow *a = x, *b = y;
    if (a->score != b->score) return b->score - a->score;
    return a->order - b->order;
}


static void resume_ago(char *out, size_t cap, time_t then, time_t now) {
    if (then <= 0) { snprintf(out, cap, "-"); return; }
    long d = (long)(now - then);
    if (d < 0) d = 0;
    if (d < 60)              snprintf(out, cap, "just now");
    else if (d < 3600)       snprintf(out, cap, "%ldm ago", d / 60);
    else if (d < 86400)      snprintf(out, cap, "%ldh ago", d / 3600);
    else if (d < 172800)     snprintf(out, cap, "yesterday");
    else if (d < 7 * 86400)  snprintf(out, cap, "%ldd ago", d / 86400);
    else if (d < 60 * 86400) snprintf(out, cap, "%ldw ago", d / (7 * 86400));
    else {
        struct tm tm;
        localtime_r(&then, &tm);
        strftime(out, cap, "%b %d", &tm);
    }
}

/* Writes the chosen path to `out`. Returns 0 on a pick, 1 if cancelled, -1 if
   there is nothing to resume. */
static int resume_pick(char *out, size_t cap) {
    RecentBook *b = malloc(sizeof *b * RESUME_MAX);
    if (!b) return -1;
    int n = state_recent(b, RESUME_MAX);
    if (n == 0) { free(b); return -1; }

    /* With no keyboard to drive the menu, resuming means the newest book. */
    if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO)) {
        snprintf(out, cap, "%s", b[0].path);
        free(b);
        return 0;
    }

    int cols, rows;
    pick_size(&cols, &rows);
    int visible = rows - 2;
    if (visible > n) visible = n;
    if (visible < 1) visible = 1;
    int linew = cols - 4;
    if (linew < 20) linew = 20;
    int namew = linew - 32;
    if (namew < 8) namew = 8;
    if (namew > 52) namew = 52;

    int view[RESUME_MAX];
    FilterRow rank[RESUME_MAX];
    int nview = 0;
    for (int i = 0; i < n; i++) view[nview++] = i;

    char query[64] = "";
    int  nq = 0;
    bool filtering = false;

    Pick p;
    pick_begin(&p, visible + 1);

    time_t now = time(NULL);
    int sel = 0, first = 0, chosen = -1;
    for (bool initial = true;; initial = false) {
        if (!initial) pick_home(&p);

        char hint[160];
        if (filtering)
            snprintf(hint, sizeof hint, "/%s\xe2\x96\x8f  \xc2\xb7  %d of %d",
                     query, nview ? sel + 1 : 0, nview);
        else {
            int used = snprintf(hint, sizeof hint,
                                "\xe2\x86\x91/\xe2\x86\x93 move \xc2\xb7 "
                                "enter read \xc2\xb7 / find \xc2\xb7 x forget \xc2\xb7 q cancel");
            if (nview > visible)
                snprintf(hint + used, sizeof hint - (size_t)used,
                         "  \xc2\xb7  %d of %d", sel + 1, nview);
        }
        pick_header("Resume", NULL, hint, cols);

        for (int i = 0; i < visible; i++) {
            int vi = first + i;
            if (vi >= nview) { printf("\r\n\x1b[2K"); continue; }
            int idx = view[vi];

            char name[400], when[24], at[24], line[600];
            pick_fit(name, sizeof name, b[idx].title, namew);
            resume_ago(when, sizeof when, b[idx].when, now);
            bool done = b[idx].total > 0 && b[idx].page >= b[idx].total;
            if (b[idx].total > 0) snprintf(at, sizeof at, "%d/%d", b[idx].page, b[idx].total);
            else                  snprintf(at, sizeof at, "%d/?", b[idx].page);
            snprintf(line, sizeof line, "%2d  %-*s  %-10s  %s",
                     idx + 1, namew, name, when, at);
            pick_row(vi, sel, line, linew, done);
        }
        if (nview == 0) {
            pick_home(&p);
            printf("\r\n\x1b[2K   \x1b[2m(nothing matches)\x1b[0m");
            for (int i = 1; i < visible; i++) printf("\r\n\x1b[2K");
        }
        fflush(stdout);

        unsigned k = pick_key();
        bool refilter = false, done_key = false;

        if (filtering && k < 0x100 && k >= ' ' && k != 0x7f) {
            if (nq < (int)sizeof query - 1) { query[nq++] = (char)k; query[nq] = 0; }
            refilter = true;
        } else if (filtering && (k == 0x7f || k == 8)) {
            if (nq > 0) query[--nq] = 0;
            else filtering = false;
            refilter = true;
        } else if (k == '/' && !filtering) {
            filtering = true; nq = 0; query[0] = 0;
            refilter = true;
        } else if (k == PICK_ESC) {
            if (filtering || nq) { filtering = false; nq = 0; query[0] = 0; refilter = true; }
            else done_key = true;
        } else if (k == PICK_UP || (!filtering && k == 'k')) sel--;
        else if (k == PICK_DOWN || (!filtering && k == 'j')) sel++;
        else if (!filtering && k == 'g') sel = 0;
        else if (!filtering && k == 'G') sel = nview - 1;
        else if ((k == '\r' || k == '\n' || k == PICK_RIGHT || (!filtering && k == ' '))
                 && nview > 0) { chosen = view[sel]; break; }
        else if (!filtering && k >= '1' && k <= '9' && (int)k - '1' < nview) {
            chosen = view[k - '1'];
            break;
        }
        else if (!filtering && k == 'x' && nview > 0) {
            /* Forgetting a book drops the place it was left, not the book. */
            int idx = view[sel];
            state_forget(b[idx].path);
            memmove(&b[idx], &b[idx + 1], (size_t)(n - idx - 1) * sizeof *b);
            if (--n == 0) break;
            refilter = true;
        }
        else if (k == 0 || (!filtering && k == 'q') || k == PICK_INTR || k == 4) done_key = true;

        if (done_key) break;

        if (refilter) {
            int m = 0;
            for (int i = 0; i < n; i++) {
                int sc;
                if (!pick_fuzzy(b[i].title, query, &sc)) continue;
                rank[m].idx = i; rank[m].score = sc; rank[m].order = i;
                m++;
            }
            if (*query) qsort(rank, (size_t)m, sizeof *rank, filter_cmp);
            nview = 0;
            for (int i = 0; i < m; i++) view[nview++] = rank[i].idx;
            sel = first = 0;
        }

        if (sel < 0) sel = 0;
        if (sel > nview - 1) sel = nview - 1;
        if (sel < 0) sel = 0;
        if (sel < first) first = sel;
        if (sel >= first + visible) first = sel - visible + 1;
    }

    pick_end(&p);
    if (chosen >= 0) snprintf(out, cap, "%s", b[chosen].path);
    free(b);
    return chosen >= 0 ? 0 : 1;
}

/* ----------------------------------------------------------------- dump -- */

static int dump(Reader *r) {
    printf("%s — %s\n", r->book.title, r->book.author);
    printf("%d chapters, %d toc entries, cover: %s\n\n",
           r->book.nspine, r->book.ntoc, r->book.cover ? r->book.cover : "(none)");
    for (int i = 0; i < r->book.nspine; i++) {
        load_chapter(r, i, 0, 0);
        printf("\n===== [%d] %s (%s) =====\n\n", i + 1, chapter_title(r), r->book.spine[i]);
        for (int j = 0; j < r->lay.n; j++) {
            Line *ln = &r->lay.lines[j];
            if (ln->type == BLK_RULE) { printf("%*s* * *\n", r->width / 2 - 2, ""); continue; }
            if (ln->type == BLK_IMG)  { printf("[image: %s]\n", ln->text ? ln->text : "?"); continue; }
            int pad = ln->center ? (r->width - u8_cols(ln->text, ln->len)) / 2 : ln->indent;
            printf("%*s%.*s\n", pad > 0 ? pad : 0, "", ln->len, ln->text);
        }
    }
    return 0;
}

/* ----------------------------------------------------------------- main -- */

static void usage(void) {
    fprintf(stderr,
        "usage: ep [-w cols] book.epub|book.pdf|dir\n"
        "       ep --resume\n"
        "\n"
        "  -w cols       text column width (default 76)\n"
        "  --resume      pick from the books you have been reading\n"
        "  --dump        print the book as wrapped text and exit\n"
        "  --typeset     draw pages with a real typesetter (macOS, prototype)\n"
        "\n"
        "A directory is browsed; only epubs, PDFs and directories are listed.\n"
        "PDFs are shown as page images and need a kitty-graphics terminal.\n");
}

static Term g_tm;
static bool g_cleaned;

static void cleanup(void) {
    if (g_cleaned) return;
    g_cleaned = true;
    term_cleanup(&g_tm);
}

static int dump_epub(const char *path, int width) {
    Reader r = {0};
    r.width = width;
    snprintf(r.path, sizeof r.path, "%s", path);
    if (!epub_open(&r.book, r.path)) {
        fprintf(stderr, "ep: cannot read epub: %s\n", path);
        return 1;
    }
    measure_book(&r);
    int rc = dump(&r);
    doc_free(&r.doc);
    layout_free(&r.lay);
    free(r.cprefix);
    epub_close(&r.book);
    return rc;
}

/* ------------------------------------------------------------------- ui -- */

/* Everything that has to ask the terminal a question, done once before the
   terminal is set up. These queries read stdin themselves, and term_init
   starts a thread that owns it from then on - after that the thread wins the
   race and the answers come back as keystrokes. */
static void ui_detect(void) {
    kg_init();
    kg_sweep_stale_tempfiles(3600);   /* whatever a crash left behind */
    if (getenv("TMUX") && !kg_tmux_allow_passthrough())
        fprintf(stderr, "ep: warning: could not set tmux allow-passthrough\n");

    /* Under tmux a terminal that stays silent yields -1 rather than 0, because
       the trick that tells the two apart is one tmux answers itself. What the
       environment says is the only thing left to go on. */
    int gfx = kg_probe(KG_PROBE_MS);
    if (gfx == -1) gfx = kg_supported() ? 1 : 0;
    /* EP_FORCE_GFX and EP_CELL stand in for a terminal that cannot answer the
       probe - a capture harness, mainly. */
    g_graphics = gfx > 0 || getenv("EP_FORCE_GFX") != NULL;

    uint8_t bg[3];
    if (term_query_background_color(&bg[0], &bg[1], &bg[2], 150)) {
        g_bg_known = true;
        g_bg_light = term_color_is_light(bg[0], bg[1], bg[2]);
    }
    g_fg_known = term_query_foreground_color(&g_term_fg[0], &g_term_fg[1],
                                             &g_term_fg[2], 150);
}

static void ui_graphics_error(const char *what) {
    fprintf(stderr, "ep: %s needs a terminal that speaks kitty graphics "
                    "(kitty, Ghostty)\n", what);
    if (getenv("TMUX"))
        fprintf(stderr, "    inside tmux this also needs:  "
                        "tmux set -g allow-passthrough all\n");
}

static bool ui_start(Screen *s) {
    if (!term_init(&g_tm)) {
        fprintf(stderr, "ep: needs a terminal\n");
        return false;
    }
    atexit(cleanup);
    atexit(tmpdir_cleanup);
    term_install_signal_restore();
    term_enter_alt_screen(&g_tm);
    term_hide_cursor();
    term_enable_mouse();

    if (g_graphics) {
        int cw, ch;
        const char *cell = getenv("EP_CELL");
        if (cell && sscanf(cell, "%dx%d", &cw, &ch) == 2 && cw > 1 && ch > 1) {
            g_cell_w = cw; g_cell_h = ch;
        } else if (term_cell_size(&g_tm, &cw, &ch) && cw > 1 && ch > 1) {
            g_cell_w = cw; g_cell_h = ch;
        }
        snprintf(g_tmpdir, sizeof g_tmpdir, "/tmp/ep-XXXXXX");
        if (!mkdtemp(g_tmpdir)) { g_tmpdir[0] = 0; }
    }

    *s = screen_create(g_tm.width, g_tm.height);
    return true;
}

static void ui_stop(Screen *s) {
    term_disable_mouse();
    term_show_cursor();
    term_leave_alt_screen(&g_tm);
    cleanup();
    kg_delete_all();
    kg_cleanup_tempfiles();
    screen_destroy(s);
}

static void ui_cell_size(void) {
    int cw, ch;
    if (getenv("EP_CELL")) return;
    if (g_graphics && term_cell_size(&g_tm, &cw, &ch) && cw > 1 && ch > 1) {
        g_cell_w = cw; g_cell_h = ch;
    }
}

/* ------------------------------------------------------------ read epub -- */

static int read_epub(const char *path, int width) {
    Reader r = {0};
    r.width = width;
    snprintf(r.path, sizeof r.path, "%s", path);
    if (!epub_open(&r.book, r.path)) {
        fprintf(stderr, "ep: cannot read epub: %s\n", path);
        return 1;
    }
    measure_book(&r);

    int spine = 0, block = 0, off = 0;
    StateLine saved;
    if (state_lookup(r.path, &saved)) {
        spine = saved.spine; block = saved.block; off = saved.off;
    }

    Screen scr;
    if (!ui_start(&scr)) { epub_close(&r.book); return 1; }
    Screen *s = &scr;
    r.term = &g_tm;

    r.page_h = s->height - 1;
    load_chapter(&r, spine, block, off);

    bool running = true, help = false, dirty = true;
    while (running) {
        int page_h = s->height - 1;
        if (page_h < 1) page_h = 1;
        r.page_h = page_h;

        if (dirty) {
            screen_clear(s, glyph_make(' ', C_FG, C_BG));
            int col = r.cols;
            int x0 = (s->width - col) / 2;

            /* A page can be shorter than the window - a break pulled back off
               an image or a heading leaves the rest of the window empty rather
               than bleeding the next page's first lines into it. */
            int pnum = page_of(&r, r.top);
            int end  = pnum + 1 < r.npg ? r.pg[pnum + 1] : r.lay.n;
            if (end <= r.top || end > r.top + page_h) end = r.top + page_h;
            if (end > r.lay.n) end = r.lay.n;

            for (int y = 0; r.top + y < end; y++)
                draw_line(&r, s, x0, y, &r.lay.lines[r.top + y], col);
            draw_status(&r, s);
            if (r.toc_open) draw_toc(&r, s);
            if (help) draw_help(s);
            if (g_graphics) kg_placeholder_redraw_begin();
            screen_render(s);
            if (g_graphics) kg_placeholder_redraw_end();
            screen_swap(s);
            term_flush();
            dirty = false;
        }

        InputEvent ev;
        if (!term_wait_event(&g_tm, &ev, 500)) continue;

        if (ev.code == KEY_RESIZE) {
            term_get_size(&g_tm);
            screen_resize(s, g_tm.width, g_tm.height);
            r.page_h = s->height - 1 > 0 ? s->height - 1 : 1;
            int cw, ch;
            if (g_graphics && term_cell_size(&g_tm, &cw, &ch) && cw > 1 && ch > 1) {
                g_cell_w = cw; g_cell_h = ch;
            }
            relayout_keeping(&r);
            dirty = true;
            continue;
        }

        if (help) {
            if (ev.code != KEY_NONE) { help = false; dirty = true; }
            continue;
        }

        if (r.toc_open) {
            switch (ev.code) {
                case KEY_UP:   if (r.toc_sel > 0) r.toc_sel--; break;
                case KEY_DOWN: if (r.toc_sel < r.book.ntoc - 1) r.toc_sel++; break;
                case KEY_MOUSE_WHEEL_UP:   if (r.toc_sel > 0) r.toc_sel--; break;
                case KEY_MOUSE_WHEEL_DOWN: if (r.toc_sel < r.book.ntoc - 1) r.toc_sel++; break;
                case KEY_ENTER:
                    goto_toc(&r, r.toc_sel);
                    r.toc_open = false;
                    break;
                case KEY_ESCAPE: r.toc_open = false; break;
                case KEY_CHAR:
                    if (ev.ch == 'j' && r.toc_sel < r.book.ntoc - 1) r.toc_sel++;
                    else if (ev.ch == 'k' && r.toc_sel > 0) r.toc_sel--;
                    else if (ev.ch == 'q' || ev.ch == 't') r.toc_open = false;
                    break;
                default: break;
            }
            dirty = true;
            continue;
        }

        switch (ev.code) {
            case KEY_RIGHT: case KEY_SPACE: case KEY_PAGE_DOWN: turn_page(&r, +1); break;
            case KEY_LEFT:  case KEY_PAGE_UP:                   turn_page(&r, -1); break;
            case KEY_DOWN:  case KEY_MOUSE_WHEEL_DOWN: scroll_by(&r, 3, page_h); break;
            case KEY_UP:    case KEY_MOUSE_WHEEL_UP:   scroll_by(&r, -3, page_h); break;
            case KEY_HOME:  r.top = 0; break;
            case KEY_END:   r.top = r.pg[r.npg - 1]; break;
            case KEY_ESCAPE: break;
            case KEY_CHAR:
                switch (ev.ch) {
                    case 'q': running = false; break;
                    case 'f': turn_page(&r, +1); break;
                    case 'b': turn_page(&r, -1); break;
                    case 'j': scroll_by(&r, 1, page_h); break;
                    case 'k': scroll_by(&r, -1, page_h); break;
                    case 'd': scroll_by(&r, page_h / 2, page_h); break;
                    case 'u': scroll_by(&r, -page_h / 2, page_h); break;
                    case 'n': case ']': load_chapter(&r, r.spine + 1, 0, 0); break;
                    case 'p': case '[': load_chapter(&r, r.spine - 1, 0, 0); break;
                    case 'g': r.top = 0; break;
                    case 'G': r.top = r.pg[r.npg - 1]; break;
                    case 't': case 'c':
                        r.toc_open = r.book.ntoc > 0;
                        r.toc_sel = 0;
                        for (int i = 0; i < r.book.ntoc; i++)
                            if (r.book.toc[i].spine == r.spine) { r.toc_sel = i; break; }
                        break;
                    case '?': help = true; break;
                    case '-': case '_':
                        if (r.width > 30) { r.width -= 4; relayout_keeping(&r); }
                        break;
                    case '+': case '=':
                        if (r.width < 200) { r.width += 4; relayout_keeping(&r); }
                        break;
                    default: break;
                }
                break;
            default: break;
        }
        dirty = true;
    }

    progress_save(&r);
    ui_stop(s);

    places_clear(&r);
    doc_free(&r.doc);
    layout_free(&r.lay);
    free(r.cprefix);
    free(r.pg);
    epub_close(&r.book);
    return 0;
}

/* --------------------------------------------------------- typeset epub -- */

/* The typeset reading mode: instead of wrapping a chapter onto the character
   grid, hand it to type.h and show the pages it draws. A chapter is built once
   and paginated, so turning either way is a step between pages that are
   already known - and a page carries the same (block, offset) position the
   wrapped reader uses, so the two modes can hand a book to each other. */

#define TY_ID_BASE  8192
#define TY_ID_SLOTS 4

/* Used only for a painted page: one asked for, or one forced by a terminal
   that would not say what colours it uses. */
/* Serif faces that ship with macOS and hold up as book text, roughly in the
   order they are worth trying. Any that a system turns out not to have is
   passed over. */
static const char *TY_FONTS[] = {
    "Iowan Old Style", "Charter", "Palatino", "Athelas", "Hoefler Text",
    "Baskerville", "Georgia", "PT Serif", "STIX Two Text",
};
#define TY_NFONTS ((int)(sizeof TY_FONTS / sizeof TY_FONTS[0]))

static const uint8_t TY_LIGHT_PAPER[3] = { 0xfa, 0xf7, 0xf0 };
static const uint8_t TY_LIGHT_INK[3]   = { 0x1c, 0x1a, 0x18 };
static const uint8_t TY_DARK_PAPER[3]  = { 0x18, 0x18, 0x1b };
static const uint8_t TY_DARK_INK[3]    = { 0xd2, 0xcc, 0xc0 };

typedef struct {
    Epub        *bk;
    Doc          doc;
    TypeChapter *tc;
    TypeStyle    st;
    char         font[128];
    bool         paper;       /* paint a page rather than sit on the terminal */
    bool         fill;        /* fill the pane rather than hold a page shape */

    /* What the terminal said about its own colours. The background is only
       wanted for its polarity - whether a page can sit on it - since a page
       that does sit on it is drawn clear rather than painted to match. */
    bool         bg_known, fg_known;
    bool         bg_light;
    uint8_t      term_fg[3];

    int          spine, page, npages;
    int          cols, rows, w, h;
} Ty;

/* -------- settings, kept beside the reading history in ~/.config/ep -------- */

static void ty_conf_path(char *out, size_t n) {
    char dir[PATH_MAX];
    state_dir(dir, sizeof dir);
    snprintf(out, n, "%s/typeset", dir);
}

static void ty_conf_load(Ty *t) {
    char path[PATH_MAX];
    ty_conf_path(path, sizeof path);
    FILE *f = fopen(path, "r");
    if (!f) return;
    char line[256];
    while (fgets(line, sizeof line, f)) {
        char *nl = strchr(line, '\n');
        if (nl) *nl = 0;
        char *sp = strchr(line, ' ');
        if (!sp) continue;
        *sp++ = 0;
        if (!strcmp(line, "size") && atof(sp) >= 6) t->st.size = atof(sp);
        else if (!strcmp(line, "paper")) t->paper = atoi(sp) != 0;
        else if (!strcmp(line, "fill")) t->fill = atoi(sp) != 0;
        else if (!strcmp(line, "justify")) t->st.justify = atoi(sp) != 0;
        else if (!strcmp(line, "hyphenate")) t->st.hyphenation = atoi(sp) ? 1 : 0;
        else if (!strcmp(line, "font") && *sp) snprintf(t->font, sizeof t->font, "%s", sp);
    }
    fclose(f);
}

static void ty_conf_save(const Ty *t) {
    char dir[PATH_MAX], path[PATH_MAX];
    state_dir(dir, sizeof dir);
    mkdir(dir, 0700);
    ty_conf_path(path, sizeof path);
    FILE *f = fopen(path, "w");
    if (!f) return;
    fprintf(f, "size %.1f\npaper %d\nfill %d\njustify %d\nhyphenate %d\nfont %s\n",
            t->st.size, t->paper, t->fill, t->st.justify,
            t->st.hyphenation > 0 ? 1 : 0, t->font);
    fclose(f);
}

/* A page reads as part of the window rather than a slide laid over it, so by
   default nothing is painted at all: the paper is left clear and what shows
   through is the terminal's own background - exactly, with no colour to get
   wrong, and still exact after a theme change, through transparency or over a
   background image. The ink is the terminal's foreground, which contrasts with
   that background by construction, so the background never has to be known.
 *
 * Painting is for asking for something the terminal is not: a cream page on a
   dark theme. That, and a terminal that would not say what its foreground is. */
/* The next face along that this system actually has. */
static void ty_font_cycle(Ty *t, int dir) {
    int at = 0;
    for (int i = 0; i < TY_NFONTS; i++)
        if (!strcmp(TY_FONTS[i], t->font)) { at = i; break; }
    for (int n = 1; n <= TY_NFONTS; n++) {
        const char *cand = TY_FONTS[((at + dir * n) % TY_NFONTS + TY_NFONTS) % TY_NFONTS];
        if (type_font_exists(cand)) {
            snprintf(t->font, sizeof t->font, "%s", cand);
            return;
        }
    }
}

static void ty_palette(Ty *t) {
    if (!t->paper && t->fg_known) {
        t->st.transparent = true;
        memcpy(t->st.ink, t->term_fg, 3);
        return;
    }
    t->st.transparent = false;
    bool term_light = t->bg_known && t->bg_light;
    bool light = t->paper ? !term_light : term_light;
    memcpy(t->st.paper, light ? TY_LIGHT_PAPER : TY_DARK_PAPER, 3);
    memcpy(t->st.ink,   light ? TY_LIGHT_INK   : TY_DARK_INK,   3);
}

/* ------------------------------------------------------------- chapters -- */

static const char *ty_title(const Epub *bk, int spine) {
    for (int i = 0; i < bk->ntoc; i++)
        if (bk->toc[i].spine == spine && bk->toc[i].title && *bk->toc[i].title)
            return bk->toc[i].title;
    return "";
}

static Doc ty_parse(Epub *bk, int spine) {
    Doc d = {0};
    if (spine < 0 || spine >= bk->nspine) return d;
    char *xhtml = epub_read(bk, bk->spine[spine], NULL);
    if (!xhtml) return d;
    char base[512];
    snprintf(base, sizeof base, "%s", bk->spine[spine]);
    char *slash = strrchr(base, '/');
    if (slash) slash[1] = 0; else base[0] = 0;
    d = doc_parse(xhtml, base);
    free(xhtml);
    return d;
}

/* Build and paginate the current chapter, opening at (block, off). */
static void ty_build(Ty *t, int block, int off) {
    if (t->tc) { type_close(t->tc); t->tc = NULL; }
    t->st.family = t->font;
    t->st.margin = t->st.size * 2.6;
    ty_palette(t);
    t->tc = type_open(&t->doc, &t->st);
    t->npages = t->w > 0 && t->h > 0 ? type_paginate(t->tc, t->w, t->h) : 0;
    t->page   = t->npages ? type_page_of(t->tc, block, off) : 0;
}

/* Move to a chapter, opening at (block, off). A chapter that sets no pages -
   one that is nothing but a cover image, say - is stepped over in whichever
   direction the reader was already going. */
static void ty_chapter(Ty *t, int spine, int block, int off, int dir) {
    while (spine >= 0 && spine < t->bk->nspine) {
        doc_free(&t->doc);
        t->doc   = ty_parse(t->bk, spine);
        t->spine = spine;
        ty_build(t, block, off);
        if (t->npages > 0 || dir == 0) return;
        spine += dir;
        block = off = 0;
    }
    /* Nothing further to show; stay where the last attempt left us. */
}

/* Where the reader is, which is what survives a change of size or style. */
static void ty_here(const Ty *t, int *block, int *off) {
    *block = 0;
    *off   = 0;
    if (t->npages > 0) type_page_start(t->tc, t->page, block, off);
}

static void ty_restyle(Ty *t) {
    int block, off;
    ty_here(t, &block, &off);
    ty_build(t, block, off);
}

static void draw_ty_status(Screen *s, const Ty *t) {
    int y = s->height - 1;
    for (int x = 0; x < s->width; x++) screen_put(s, x, y, ' ', C_DIM, C_BG);
    screen_print(s, 1, y, ty_title(t->bk, t->spine), C_DIM, C_BG);

    char right[200];
    snprintf(right, sizeof right, "%s %.0f %s %s   %d/%d   ch %d/%d   ? help",
             t->font, t->st.size, t->st.transparent ? "term" : "paper",
             t->fill ? "fill" : "page",
             t->npages ? t->page + 1 : 0, t->npages,
             t->spine + 1, t->bk->nspine);
    int rw = (int)strlen(right);
    if (rw < s->width - 2)
        screen_print(s, s->width - rw - 1, y, right, C_DIM, C_BG);
}

static void draw_ty_help(Screen *s) {
    static const char *rows[] = {
        "  space / f / right   next page",
        "  b / left            previous page",
        "  n p  or  ] [        next / previous chapter",
        "  + -                 larger / smaller type",
        "  d                   painted page / terminal colours",
        "  F                   next serif face",
        "  w                   fill the pane",
        "  J                   justified text",
        "  H                   hyphenation",
        "  q                   quit",
    };
    int n = (int)(sizeof rows / sizeof rows[0]);
    int w = 40, h = n + 2;
    int x0 = (s->width - w) / 2, y0 = (s->height - h) / 2;
    if (x0 < 0 || y0 < 0) return;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++)
            screen_put(s, x0 + x, y0 + y, ' ', C_FG, C_SEL);
    for (int i = 0; i < n; i++) screen_print(s, x0, y0 + 1 + i, rows[i], C_FG, C_SEL);
}

static int read_typeset(const char *path) {
    if (!type_available()) {
        fprintf(stderr, "ep: --typeset needs the CoreText backend (macOS)\n");
        return 1;
    }
    Epub bk;
    if (!epub_open(&bk, path)) {
        fprintf(stderr, "ep: cannot read epub: %s\n", path);
        return 1;
    }

    Screen scr;
    if (!ui_start(&scr)) { epub_close(&bk); return 1; }
    Screen *s = &scr;
    if (!g_graphics) {
        ui_stop(s);
        epub_close(&bk);
        ui_graphics_error("--typeset");
        return 1;
    }

    Ty t = {0};
    t.bk = &bk;
    t.bg_known = g_bg_known;
    t.fg_known = g_fg_known;
    t.bg_light = g_bg_light;
    memcpy(t.term_fg, g_term_fg, 3);
    type_style_default(&t.st);
    snprintf(t.font, sizeof t.font, "%s", t.st.family);
    t.st.size = g_cell_h * 0.9;
    ty_conf_load(&t);
    const char *env = getenv("EP_FONT");
    if (env && *env) snprintf(t.font, sizeof t.font, "%s", env);
    /* A name nothing answers to would quietly come back as Helvetica. */
    if (!type_font_exists(t.font)) {
        t.font[0] = 0;
        ty_font_cycle(&t, +1);
    }

    int spine = 0, block = 0, off = 0;
    StateLine saved;
    int keep_page = 0, keep_total = 0;
    if (state_lookup(path, &saved)) {
        spine = saved.spine;
        block = saved.block;
        off   = saved.off;
        keep_page  = saved.page;
        keep_total = saved.total;
        if (spine < 0 || spine >= bk.nspine) { spine = 0; block = off = 0; }
    }

    uint32_t id = 0, seq = 0;
    bool have = false, running = true, dirty = true, help = false;
    bool opened = false;

    while (running) {
        int box_cols = s->width, box_rows = s->height - 1;

        if (dirty && box_cols >= 8 && box_rows >= 4) {
            /* A page shaped like a page: the column is sized off the window's
               height so the measure stays readable on a wide terminal. Filling
               the pane gives that up for the room, which is what a narrow
               window or a split wants. */
            int rows = box_rows;
            int cols = box_cols;
            if (!t.fill) {
                cols = (int)(rows * g_cell_h * 0.68) / g_cell_w;
                if (cols > box_cols || cols < 8) cols = box_cols;
            }
            int w = cols * g_cell_w, h = rows * g_cell_h;

            if (!opened) {
                t.w = w; t.h = h; t.cols = cols; t.rows = rows;
                ty_chapter(&t, spine, block, off, +1);
                opened = true;
            } else if (w != t.w || h != t.h) {
                int b, o;
                ty_here(&t, &b, &o);
                t.w = w; t.h = h; t.cols = cols; t.rows = rows;
                t.npages = type_paginate(t.tc, w, h);
                t.page   = t.npages ? type_page_of(t.tc, b, o) : 0;
            }

            uint8_t *px = malloc((size_t)w * (size_t)h * 4);
            if (px && type_draw(t.tc, t.page, px)) {
                /* Rotating slots retires the old placement and, because the id
                   rides in the cell's colour, makes the cells themselves
                   differ - which is what gets them redrawn. */
                uint32_t slot = TY_ID_BASE + (seq++ % TY_ID_SLOTS);
                kg_delete(slot);
                kg_transmit_ex(slot, px, w, h, 4);
                kg_virtual_place(slot, cols, rows);
                id = slot;
                have = true;
            }
            free(px);

            int x0 = (s->width - cols) / 2;
            screen_clear(s, glyph_make(' ', C_FG, C_BG));
            if (have)
                for (int rr = 0; rr < rows; rr++)
                    for (int cc = 0; cc < cols; cc++)
                        screen_set(s, x0 + cc, rr, glyph_placeholder(id, rr, cc));
            draw_ty_status(s, &t);
            if (help) draw_ty_help(s);

            kg_placeholder_redraw_begin();
            screen_render(s);
            kg_placeholder_redraw_end();
            screen_swap(s);
            term_flush();
            dirty = false;
        }

        InputEvent ev;
        if (!term_wait_event(&g_tm, &ev, 500)) continue;

        if (ev.code == KEY_RESIZE) {
            term_get_size(&g_tm);
            screen_resize(s, g_tm.width, g_tm.height);
            ui_cell_size();
            dirty = true;
            continue;
        }
        if (help) {
            if (ev.code != KEY_NONE) { help = false; dirty = true; }
            continue;
        }

        bool fwd = false, back = false;
        switch (ev.code) {
            case KEY_RIGHT: case KEY_SPACE: case KEY_PAGE_DOWN:
            case KEY_DOWN:  case KEY_MOUSE_WHEEL_DOWN: fwd = true; break;
            case KEY_LEFT:  case KEY_PAGE_UP:
            case KEY_UP:    case KEY_MOUSE_WHEEL_UP:   back = true; break;
            case KEY_HOME:  t.page = 0; dirty = true; break;
            case KEY_END:   t.page = t.npages ? t.npages - 1 : 0; dirty = true; break;
            case KEY_CHAR:
                switch (ev.ch) {
                    case 'q': running = false; break;
                    case 'f': case 'j': fwd = true; break;
                    case 'b': case 'k': back = true; break;
                    case 'g': t.page = 0; dirty = true; break;
                    case 'G': t.page = t.npages ? t.npages - 1 : 0; dirty = true; break;
                    case 'n': case ']':
                        if (t.spine < bk.nspine - 1) {
                            ty_chapter(&t, t.spine + 1, 0, 0, +1);
                            dirty = true;
                        }
                        break;
                    case 'p': case '[':
                        if (t.spine > 0) {
                            ty_chapter(&t, t.spine - 1, 0, 0, -1);
                            dirty = true;
                        }
                        break;
                    case '+': case '=':
                        if (t.st.size < 72) { t.st.size += 1; ty_restyle(&t); dirty = true; }
                        break;
                    case '-': case '_':
                        if (t.st.size > 8) { t.st.size -= 1; ty_restyle(&t); dirty = true; }
                        break;
                    case 'd': t.paper = !t.paper; ty_restyle(&t); dirty = true; break;
                    case 'F': ty_font_cycle(&t, +1); ty_restyle(&t); dirty = true; break;
                    case 'w': t.fill = !t.fill; dirty = true; break;
                    case 'J': t.st.justify = !t.st.justify; ty_restyle(&t); dirty = true; break;
                    case 'H':
                        t.st.hyphenation = t.st.hyphenation > 0 ? 0 : 1;
                        ty_restyle(&t);
                        dirty = true;
                        break;
                    case '?': help = true; dirty = true; break;
                    default: break;
                }
                break;
            default: break;
        }

        if (fwd) {
            if (t.page + 1 < t.npages) { t.page++; dirty = true; }
            else if (t.spine < bk.nspine - 1) {
                ty_chapter(&t, t.spine + 1, 0, 0, +1);
                dirty = true;
            }
        } else if (back) {
            if (t.page > 0) { t.page--; dirty = true; }
            else if (t.spine > 0) {
                /* Back off the top of a chapter and the one before opens at
                   its end, which is what going back through it means. */
                ty_chapter(&t, t.spine - 1, 0, 0, -1);
                t.page = t.npages ? t.npages - 1 : 0;
                dirty = true;
            }
        }
    }

    int b, o;
    ty_here(&t, &b, &o);
    /* The page estimate belongs to the wrapped reader, which measures the
       whole book; keep whatever it left, and fall back to chapters so a book
       only ever read here still shows progress in --resume. */
    if (keep_total <= 0) { keep_page = t.spine + 1; keep_total = bk.nspine; }
    state_save(path, t.spine, b, o, keep_page, keep_total, 0);
    ty_conf_save(&t);

    ui_stop(s);
    if (t.tc) type_close(t.tc);
    doc_free(&t.doc);
    epub_close(&bk);
    return 0;
}

/* -------------------------------------------------------------- read pdf -- */

/* A PDF is pages of pixels, not text to reflow, so it is read the way cbr
   reads a comic: the page fitted to the window, or fitted to its width and
   scrolled, which is the only way body text is legible in a terminal. */

typedef enum { FIT_PAGE = 0, FIT_WIDTH = 1 } Fit;

typedef struct {
    int page, scroll, cols, rows, cell_w, cell_h;
    int fit;
} PageKey;

static const char *PDF_HELP[] = {
    "  \xe2\x86\x92 / space / f     next page",
    "  \xe2\x86\x90 / b             previous page",
    "  j k / \xe2\x86\x91 \xe2\x86\x93         nudge a line",
    "  n / p              next / previous sheet",
    "  w                  fit page / fit width",
    "  g / G              first / last page",
    "  123 then enter     go to a page",
    "  q                  quit (position is saved)",
};

static void draw_pdf_help(Screen *s) {
    int n = (int)(sizeof PDF_HELP / sizeof *PDF_HELP);
    int w = 40, h = n + 2;
    int x0 = (s->width - w) / 2, y0 = (s->height - h) / 2;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++)
            screen_put(s, x0 + x, y0 + y, ' ', C_FG, C_SEL);
    screen_print(s, x0 + 2, y0, " Keys ", C_ACC, C_SEL);
    for (int i = 0; i < n; i++)
        screen_print(s, x0 + 1, y0 + 1 + i, PDF_HELP[i], C_FG, C_SEL);
}

static void draw_pdf_status(Screen *s, const char *path, int page, int npages,
                            Fit fit, const char *jump) {
    int y = s->height - 1;
    for (int x = 0; x < s->width; x++) screen_put(s, x, y, ' ', C_DIM, C_BG);

    const char *base = strrchr(path, '/');
    char left[256];
    snprintf(left, sizeof left, "%s", base ? base + 1 : path);
    char *dot = strrchr(left, '.');
    if (dot && dot != left) *dot = 0;
    screen_print(s, 1, y, left, C_DIM, C_BG);

    char right[128];
    if (*jump)
        snprintf(right, sizeof right, "go to page %s_", jump);
    else
        snprintf(right, sizeof right, "%d/%d  %d%%  %s   ? help",
                 page + 1, npages, npages ? (page + 1) * 100 / npages : 100,
                 fit == FIT_WIDTH ? "width" : "page");
    int rw = (int)strlen(right);
    screen_print(s, s->width - rw - 1, y, right, *jump ? C_ACC : C_DIM, C_BG);
}

/* How far a width-fitted page can scroll before its foot is on screen. */
static int pdf_max_scroll(PdfDoc *doc, int page, int cols, int rows) {
    double pw, ph;
    if (!pdf_page_size(doc, page, &pw, &ph) || pw <= 0) return 0;
    double scale = (double)(cols * g_cell_w) / pw;
    int m = (int)(ph * scale + 0.5) - rows * g_cell_h;
    return m > 0 ? m : 0;
}

#define PDF_ID_BASE 4096
#define PDF_ID_SLOTS 4
#define SCROLL_END (1 << 28)

static int read_pdf(const char *path) {
    PdfDoc *doc = pdf_open(path);
    if (!doc) {
        fprintf(stderr, "ep: cannot read pdf: %s\n", path);
        return 1;
    }
    int npages = pdf_pages(doc);

    int page = 0, scroll = 0;
    Fit fit = FIT_PAGE;
    StateLine saved;
    if (state_lookup(path, &saved)) {
        page   = saved.spine;
        scroll = saved.block;
        fit    = saved.mode ? FIT_WIDTH : FIT_PAGE;
        if (page < 0 || page >= npages) { page = 0; scroll = 0; }
    }

    Screen scr;
    if (!ui_start(&scr)) { pdf_close(doc); return 1; }
    Screen *s = &scr;

    if (!g_graphics) {
        ui_stop(s);
        pdf_close(doc);
        ui_graphics_error("reading a PDF");
        return 1;
    }

    uint32_t id = 0, seq = 0;
    PageKey shown = {0};
    bool have = false;
    int cols = 0, rows = 0;

    char jump[8] = "";
    int  njump = 0;
    bool running = true, help = false, dirty = true;

    while (running) {
        int box_cols = s->width, box_rows = s->height - 1;
        if (box_cols < 4 || box_rows < 2) { dirty = false; }

        if (dirty && box_cols >= 4 && box_rows >= 2) {
            double pw = 612, ph = 792;
            pdf_page_size(doc, page, &pw, &ph);

            double scale;
            int off_x, off_y;
            if (fit == FIT_WIDTH) {
                cols = box_cols;
                rows = box_rows;
                scale = (double)(cols * g_cell_w) / pw;
                int full_h = (int)(ph * scale + 0.5);
                int max_scroll = full_h - rows * g_cell_h;
                if (max_scroll < 0) max_scroll = 0;
                if (scroll > max_scroll) scroll = max_scroll;
                if (scroll < 0) scroll = 0;
                off_x = 0;
                off_y = scroll;
            } else {
                int px_w, px_h;
                kg_fit_cells((int)(pw * 10), (int)(ph * 10), g_cell_w, g_cell_h,
                             box_cols, box_rows, &cols, &rows, &px_w, &px_h);
                if (cols < 1) cols = 1;
                if (rows < 1) rows = 1;
                scale = px_w / pw;
                off_x = -(cols * g_cell_w - px_w) / 2;
                off_y = -(rows * g_cell_h - px_h) / 2;
                scroll = 0;
            }

            PageKey key = { page, scroll, cols, rows, g_cell_w, g_cell_h, (int)fit };
            if (!have || memcmp(&key, &shown, sizeof key) != 0) {
                int w = cols * g_cell_w, h = rows * g_cell_h;
                uint8_t *px = malloc((size_t)w * (size_t)h * 4);
                if (px && pdf_render(doc, page, scale, off_x, off_y, w, h, px)) {
                    /* Rotating slots retires the old placement and, because the
                       id rides in the cell's colour, makes the cells themselves
                       differ - which is what gets them redrawn. */
                    uint32_t next = PDF_ID_BASE + (seq++ % PDF_ID_SLOTS);
                    kg_delete(next);
                    kg_transmit_ex(next, px, w, h, 4);
                    kg_virtual_place(next, cols, rows);
                    id = next;
                    shown = key;
                    have = true;
                }
                free(px);
            }

            int x0 = (s->width - cols) / 2;
            int y0 = (box_rows - rows) / 2;
            if (y0 < 0) y0 = 0;

            screen_clear(s, glyph_make(' ', C_FG, C_BG));
            if (have)
                for (int rr = 0; rr < rows; rr++)
                    for (int cc = 0; cc < cols; cc++)
                        screen_set(s, x0 + cc, y0 + rr, glyph_placeholder(id, rr, cc));
            draw_pdf_status(s, path, page, npages, fit, jump);
            if (help) draw_pdf_help(s);

            kg_placeholder_redraw_begin();
            screen_render(s);
            kg_placeholder_redraw_end();
            screen_swap(s);
            term_flush();
            dirty = false;
        }

        InputEvent ev;
        if (!term_wait_event(&g_tm, &ev, 500)) continue;

        if (ev.code == KEY_RESIZE) {
            term_get_size(&g_tm);
            screen_resize(s, g_tm.width, g_tm.height);
            ui_cell_size();
            dirty = true;
            continue;
        }
        if (help) {
            if (ev.code != KEY_NONE) { help = false; dirty = true; }
            continue;
        }

        int line = g_cell_h;
        /* A turn moves a whole window, so nothing is shown twice and the bands
           of a page read like pages of their own. */
        int win  = box_rows * g_cell_h;
        if (win < line) win = line;
        int maxs = fit == FIT_WIDTH ? pdf_max_scroll(doc, page, box_cols, box_rows) : 0;

        /* Bands sit on a fixed grid down the sheet, with the last one resting
           against its foot, so turning back retraces the same boundaries the
           way forward crossed them. */
        int fwd = ((scroll / win) + 1) * win;
        if (fwd > maxs) fwd = maxs;
        int back = (((scroll + win - 1) / win) - 1) * win;
        if (back < 0) back = 0;

        switch (ev.code) {
            case KEY_RIGHT: case KEY_SPACE: case KEY_PAGE_DOWN:
            case KEY_MOUSE_WHEEL_DOWN:
                if (fit == FIT_WIDTH && fwd > scroll) scroll = fwd;
                else if (page < npages - 1) { page++; scroll = 0; }
                break;
            case KEY_LEFT: case KEY_PAGE_UP: case KEY_MOUSE_WHEEL_UP:
                if (fit == FIT_WIDTH && scroll > 0 && back < scroll) scroll = back;
                else if (page > 0) { page--; scroll = SCROLL_END; }
                break;
            case KEY_DOWN:
                if (fit == FIT_WIDTH) scroll += line;
                else if (page < npages - 1) page++;
                break;
            case KEY_UP:
                if (fit == FIT_WIDTH) scroll -= line;
                else if (page > 0) page--;
                break;
            case KEY_HOME:  page = 0; scroll = 0; break;
            case KEY_END:   page = npages - 1; scroll = 0; break;
            case KEY_ENTER:
                if (njump) {
                    int want = atoi(jump);
                    if (want >= 1 && want <= npages) { page = want - 1; scroll = 0; }
                    jump[0] = 0;
                    njump = 0;
                }
                break;
            case KEY_ESCAPE: jump[0] = 0; njump = 0; break;
            case KEY_CHAR:
                if (ev.ch >= '0' && ev.ch <= '9' && njump < (int)sizeof jump - 1) {
                    jump[njump++] = ev.ch;
                    jump[njump] = 0;
                    break;
                }
                switch (ev.ch) {
                    case 'q': running = false; break;
                    case 'f':
                        if (fit == FIT_WIDTH && fwd > scroll) scroll = fwd;
                        else if (page < npages - 1) { page++; scroll = 0; }
                        break;
                    case 'b':
                        if (fit == FIT_WIDTH && scroll > 0 && back < scroll) scroll = back;
                        else if (page > 0) { page--; scroll = SCROLL_END; }
                        break;
                    case 'j': if (fit == FIT_WIDTH) scroll += line; else if (page < npages - 1) page++; break;
                    case 'k': if (fit == FIT_WIDTH) scroll -= line; else if (page > 0) page--; break;
                    case 'd': scroll += win / 2; break;
                    case 'u': scroll -= win / 2; break;
                    case 'n': case ']': if (page < npages - 1) { page++; scroll = 0; } break;
                    case 'p': case '[': if (page > 0) { page--; scroll = 0; } break;
                    case 'w':
                        fit = fit == FIT_WIDTH ? FIT_PAGE : FIT_WIDTH;
                        scroll = 0;
                        break;
                    case 'g': page = 0; scroll = 0; break;
                    case 'G': page = npages - 1; scroll = 0; break;
                    case '?': help = true; break;
                    default: break;
                }
                break;
            default: break;
        }

        if (page < 0) page = 0;
        if (page > npages - 1) page = npages - 1;
        if (fit != FIT_WIDTH) {
            scroll = 0;
        } else {
            /* Scrolling off either end of a page carries on into the next one,
               so a long document reads as one continuous strip. */
            if (scroll < 0) {
                if (page > 0) { page--; scroll = pdf_max_scroll(doc, page, box_cols, box_rows); }
                else scroll = 0;
            } else {
                int m = pdf_max_scroll(doc, page, box_cols, box_rows);
                if (scroll > m) scroll = m;
            }
        }
        dirty = true;
    }

    state_save(path, page, scroll, 0, page + 1, npages, fit == FIT_WIDTH);
    ui_stop(s);
    pdf_close(doc);
    return 0;
}

/* ----------------------------------------------------------------- main -- */

int main(int argc, char **argv) {
    const char *file = NULL;
    bool want_dump = false, want_resume = false, want_type = false;
    int  width = 76;

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "--dump") || !strcmp(a, "-d")) want_dump = true;
        else if (!strcmp(a, "--resume")) want_resume = true;
        else if (!strcmp(a, "--typeset")) want_type = true;
        else if (!strcmp(a, "-h") || !strcmp(a, "--help")) { usage(); return 0; }
        else if (!strcmp(a, "-w") && i + 1 < argc) width = atoi(argv[++i]);
        else if (a[0] != '-') file = a;
        else { usage(); return 2; }
    }

    /* Not for --dump, whose output is the stdout the probe would write to. */
    if (!want_dump) ui_detect();

    char picked[PATH_MAX];
    if (want_resume && !file) {
        int rc = resume_pick(picked, sizeof picked);
        if (rc < 0) { fprintf(stderr, "ep: nothing to resume yet - read a book first\n"); return 1; }
        if (rc > 0) return 0;                       /* cancelled */
        file = picked;
    }
    if (!file) { usage(); return 2; }

    struct stat st;
    if (stat(file, &st) == 0 && S_ISDIR(st.st_mode)) {
        PickDir opts = { .accept = book_accept };
        if (pick_dir(file, picked, sizeof picked, &opts) != 0) return 0;
        file = picked;
    }

    char full[PATH_MAX];
    if (!realpath(file, full)) snprintf(full, sizeof full, "%s", file);

    if (is_pdf(full)) {
        if (want_dump) { fprintf(stderr, "ep: --dump only works on epubs\n"); return 2; }
        return read_pdf(full);
    }
    if (want_dump) return dump_epub(full, width);
    return want_type ? read_typeset(full) : read_epub(full, width);
}
