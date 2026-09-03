#ifndef PICK_H
#define PICK_H

/**
 * pick.h - line-mode menus that run on the terminal as it is.
 *
 * They draw in the normal flow and wipe themselves afterwards, so cancelling
 * one leaves the shell exactly as it was. The primitives build any such menu;
 * pick_dir() is the directory lister: folder navigation, / fuzzy search, and
 * an accept callback so each app lists only what it can open.
 *
 * In exactly one .c file:
 *
 *     #define PICK_IMPLEMENTATION
 *     #include "pick.h"
 */

#include <stdbool.h>
#include <stddef.h>
#include <termios.h>

#define PICK_UP    0x100
#define PICK_DOWN  0x101
#define PICK_LEFT  0x102
#define PICK_RIGHT 0x103
#define PICK_ESC   0x104
#define PICK_INTR  0x03

#ifndef PICK_DIR_MAX
#define PICK_DIR_MAX 4096
#endif

typedef struct {
    struct termios saved;
    bool restore;
    int  lines;          /* header plus rows, so the wipe knows its extent */
} Pick;

void     pick_begin(Pick *p, int lines);
void     pick_home(const Pick *p);
void     pick_end(Pick *p);
void     pick_size(int *cols, int *rows);
void     pick_row(int idx, int sel, const char *text, int linew, bool dim);
void     pick_header(const char *title, const char *subject, const char *hint, int cols);
unsigned pick_key(void);
int      pick_cols(const char *s);
bool     pick_fuzzy(const char *hay, const char *needle, int *score);
void     pick_fit(char *out, size_t cap, const char *s, int max);
int      pick_natcmp(const char *a, const char *b);

/* True to list this entry. Directories are offered unless this returns false.
   NULL lists every non-hidden name. */
typedef bool (*PickAcceptFn)(const char *path, const char *name, bool is_dir, void *ctx);

/* True if selecting this directory should return it rather than enter it.
   NULL always descends. Files are always returned. */
typedef bool (*PickLeafFn)(const char *path, void *ctx);

typedef struct {
    const char  *title;     /* header label; NULL → "Open" */
    PickAcceptFn accept;
    PickLeafFn   leaf;
    void        *ctx;
} PickDir;

/* Writes the chosen path to `out`. Returns 0 on a pick, 1 if cancelled. */
int pick_dir(const char *start, char *out, size_t cap, const PickDir *opts);

#endif /* PICK_H */

#ifdef PICK_IMPLEMENTATION

#include <ctype.h>
#include <dirent.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

/* Display columns, not bytes: the arrows and separators in the hints are two
   and three bytes each, and measuring them as bytes is what makes a header
   three times wider than it looks. */
int pick_cols(const char *s) {
    int n = 0;
    for (const char *p = s; *p; p++) if (((unsigned char)*p & 0xC0) != 0x80) n++;
    return n;
}

/* Copies `s` into `out`, cut to `max` columns with a trailing ellipsis. The cut
   falls on a character boundary, so a multi-byte name is never split. */
void pick_fit(char *out, size_t cap, const char *s, int max) {
    if (!cap) return;
    if (max < 1) { out[0] = '\0'; return; }
    if (pick_cols(s) <= max) { snprintf(out, cap, "%s", s); return; }

    size_t i = 0;
    for (int n = 0; s[i] && n < max - 1; n++) {
        i++;
        while (((unsigned char)s[i] & 0xC0) == 0x80) i++;
    }
    snprintf(out, cap, "%.*s\xe2\x80\xa6", (int)i, s);
}

/* Case-insensitive subsequence match, the way a fuzzy finder means it: every
   character of `needle` appears in `hay`, in order, not necessarily adjacent.
   The score favours runs of adjacent matches and matches that start a word, so
   "bld" ranks "Bloodstone" above "Bumbling Idlers". */
bool pick_fuzzy(const char *hay, const char *needle, int *score) {
    if (!*needle) { if (score) *score = 0; return true; }

    int total = 0, run = 0;
    const char *h = hay;
    for (const char *n = needle; *n; n++) {
        int want = tolower((unsigned char)*n);
        if (want == ' ') continue;
        const char *found = NULL;
        for (const char *p = h; *p; p++) {
            if (tolower((unsigned char)*p) != want) continue;
            found = p;
            break;
        }
        if (!found) return false;

        bool adjacent = (found == h && h != hay);
        bool word_start = (found == hay);
        if (!word_start && found > hay) {
            char prev = found[-1];
            word_start = prev == ' ' || prev == '_' || prev == '-' || prev == '.' ||
                         prev == '/' || prev == '(' || prev == '[';
        }
        run = adjacent ? run + 1 : 0;
        total += 1 + run * 8 + (word_start ? 6 : 0);
        total -= (int)(found - h) / 4;          /* the further the reach, the weaker */
        h = found + 1;
    }
    if (score) *score = total;
    return true;
}

int pick_natcmp(const char *a, const char *b) {
    while (*a && *b) {
        if (*a >= '0' && *a <= '9' && *b >= '0' && *b <= '9') {
            while (*a == '0') a++;
            while (*b == '0') b++;
            const char *sa = a, *sb = b;
            while (*a >= '0' && *a <= '9') a++;
            while (*b >= '0' && *b <= '9') b++;
            long la = a - sa, lb = b - sb;
            if (la != lb) return la < lb ? -1 : 1;
            int c = strncmp(sa, sb, (size_t)la);
            if (c) return c;
        } else {
            int ca = tolower((unsigned char)*a), cb = tolower((unsigned char)*b);
            if (ca != cb) return ca < cb ? -1 : 1;
            a++; b++;
        }
    }
    return (*a ? 1 : 0) - (*b ? 1 : 0);
}

void pick_begin(Pick *p, int lines) {
    p->lines = lines;
    p->restore = tcgetattr(STDIN_FILENO, &p->saved) == 0;
    if (p->restore) {
        struct termios raw = p->saved;
        /* ISIG off too, so ctrl-c arrives as a byte to cancel on rather than
           killing us with the cursor hidden and echo off. */
        raw.c_lflag &= (tcflag_t)~(ICANON | ECHO | ISIG);
        raw.c_cc[VMIN] = 1;
        raw.c_cc[VTIME] = 0;
        tcsetattr(STDIN_FILENO, TCSANOW, &raw);
    }
    fputs("\x1b[?25l", stdout);
}

/* Back to the top of the menu, to draw the next frame over the last one. */
void pick_home(const Pick *p) {
    if (p->lines > 1) printf("\r\x1b[%dA", p->lines - 1);
}

void pick_end(Pick *p) {
    pick_home(p);
    for (int i = 0; i < p->lines; i++)
        printf("\r\x1b[2K%s", i + 1 < p->lines ? "\r\n" : "");
    pick_home(p);
    fputs("\x1b[?25h", stdout);
    fflush(stdout);
    if (p->restore) tcsetattr(STDIN_FILENO, TCSANOW, &p->saved);
}

void pick_size(int *cols, int *rows) {
    struct winsize ws;
    *cols = 80;
    *rows = 24;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0) {
        *cols = ws.ws_col;
        *rows = ws.ws_row;
    }
}

/* `dim` marks a row as spent - a book already read to the end - so the list
   reads as what is left to do without hiding what is not. */
void pick_row(int idx, int sel, const char *text, int linew, bool dim) {
    char shown[700];
    pick_fit(shown, sizeof shown, text, linew);
    int pad = linew - pick_cols(shown);
    if (pad < 0) pad = 0;

    printf("\r\n\x1b[2K");
    if (idx == sel) printf("\x1b[7%sm \xe2\x96\xb8 %s%*s\x1b[0m", dim ? ";2" : "", shown, pad, "");
    else if (dim)   printf("\x1b[2m   %s%*s\x1b[0m", shown, pad, "");
    else            printf("   %s%*s", shown, pad, "");
}

/* The header, kept inside the window. A line wider than the terminal wraps, and
   a wrapped header pushes every row down a line while the redraw goes on
   addressing the old geometry - which is what tears the menu. */
void pick_header(const char *title, const char *subject, const char *hint, int cols) {
    int used = pick_cols(title), hintw = pick_cols(hint);
    bool with_hint = subject ? (cols - used - 1 - 2 - hintw >= 8)
                             : (used + 2 + hintw <= cols);

    printf("\r\x1b[2K\x1b[1m%s\x1b[0m", title);
    if (subject) {
        char cut[1200];
        int room = with_hint ? cols - used - 3 - hintw : cols - used - 1;
        pick_fit(cut, sizeof cut, subject, room < 1 ? 1 : room);
        printf(" %s", cut);
    }
    if (with_hint) printf("  \x1b[2m%s\x1b[0m", hint);
}

/* A key code, a raw byte, or 0 at end of input. */
unsigned pick_key(void) {
    unsigned char b;
    if (read(STDIN_FILENO, &b, 1) != 1) return 0;
    if (b != 0x1b) return b;

    struct timeval tv = { 0, 30000 };
    fd_set r;
    FD_ZERO(&r);
    FD_SET(STDIN_FILENO, &r);
    if (select(STDIN_FILENO + 1, &r, NULL, NULL, &tv) <= 0) return PICK_ESC;
    char seq[16];
    ssize_t n = read(STDIN_FILENO, seq, sizeof seq);
    if (n < 2 || (seq[0] != '[' && seq[0] != 'O')) return PICK_ESC;
    switch (seq[n - 1]) {
        case 'A': return PICK_UP;
        case 'B': return PICK_DOWN;
        case 'C': return PICK_RIGHT;
        case 'D': return PICK_LEFT;
        default:  return PICK_ESC;
    }
}

/* ---------------------------------------------------------------- dir -- */

typedef struct {
    char name[256];
    char path[PATH_MAX];
    bool is_dir;
} PickDirEnt;

typedef struct { int idx, score, order; } PickRank;

static int pick_dir_rank_cmp(const void *x, const void *y) {
    const PickRank *a = x, *b = y;
    if (a->score != b->score) return b->score - a->score;
    return a->order - b->order;
}

static int pick_dir_ent_cmp(const void *x, const void *y) {
    const PickDirEnt *a = x, *b = y;
    if (a->is_dir != b->is_dir) return a->is_dir ? -1 : 1;
    return pick_natcmp(a->name, b->name);
}

static int pick_dir_load(const char *dir, PickDirEnt *out, int cap, const PickDir *o) {
    DIR *d = opendir(dir);
    if (!d) return 0;
    struct dirent *de;
    int n = 0;
    while (n < cap && (de = readdir(d))) {
        if (de->d_name[0] == '.') continue;
        char p[PATH_MAX];
        if (snprintf(p, sizeof p, "%s/%s", dir, de->d_name) >= (int)sizeof p) continue;
        struct stat st;
        if (stat(p, &st) != 0) continue;
        bool is_dir = S_ISDIR(st.st_mode);
        if (o->accept && !o->accept(p, de->d_name, is_dir, o->ctx)) continue;
        PickDirEnt *e = &out[n++];
        e->is_dir = is_dir;
        snprintf(e->path, sizeof e->path, "%s", p);
        snprintf(e->name, sizeof e->name, "%s", de->d_name);
    }
    closedir(d);
    qsort(out, (size_t)n, sizeof *out, pick_dir_ent_cmp);
    return n;
}

static void pick_dir_parent(char *dir) {
    char *slash = strrchr(dir, '/');
    if (!slash) return;
    if (slash == dir) dir[1] = '\0';
    else *slash = '\0';
}

static void pick_dir_reset_view(int *view, int *nview, int n) {
    *nview = 0;
    for (int i = 0; i < n; i++) view[(*nview)++] = i;
}

int pick_dir(const char *start, char *out, size_t cap, const PickDir *opts) {
    PickDir o = {0};
    if (opts) o = *opts;
    const char *title = o.title ? o.title : "Open";

    char dir[PATH_MAX];
    if (!realpath(start, dir)) snprintf(dir, sizeof dir, "%s", start);

    PickDirEnt *e = malloc(sizeof *e * PICK_DIR_MAX);
    int *view = malloc(sizeof *view * PICK_DIR_MAX);
    PickRank *rows = malloc(sizeof *rows * PICK_DIR_MAX);
    if (!e || !view || !rows) { free(e); free(view); free(rows); return 1; }
    int n = pick_dir_load(dir, e, PICK_DIR_MAX, &o);

    if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO)) {
        free(e); free(view); free(rows);
        return 1;
    }

    int cols, rows_avail;
    pick_size(&cols, &rows_avail);
    int visible = rows_avail - 2;
    if (visible < 1) visible = 1;
    int linew = cols - 4;
    if (linew < 20) linew = 20;

    Pick p;
    pick_begin(&p, visible + 1);

    char query[64] = "";
    int  nq = 0;
    bool filtering = false;

    int nview = 0;
    pick_dir_reset_view(view, &nview, n);

    int sel = 0, first = 0;
    bool chosen = false, quit = false;
    for (bool initial = true; !chosen && !quit; initial = false) {
        if (!initial) pick_home(&p);

        char hint[160];
        if (filtering)
            snprintf(hint, sizeof hint, "/%s\xe2\x96\x8f  \xc2\xb7  %d of %d",
                     query, nview ? sel + 1 : 0, nview);
        else
            snprintf(hint, sizeof hint,
                     "\xe2\x86\x91/\xe2\x86\x93 move \xc2\xb7 "
                     "\xe2\x86\x92 enter \xc2\xb7 \xe2\x86\x90 up \xc2\xb7 "
                     "/ find \xc2\xb7 q cancel");
        pick_header(title, dir, hint, cols);

        for (int i = 0; i < visible; i++) {
            int vi = first + i;
            if (vi >= nview) { printf("\r\n\x1b[2K"); continue; }
            int idx = view[vi];
            char name[400], line[600];
            pick_fit(name, sizeof name, e[idx].name, linew - 4);
            snprintf(line, sizeof line, "%s%s", name, e[idx].is_dir ? "/" : "");
            pick_row(vi, sel, line, linew, false);
        }
        if (nview == 0) {
            pick_home(&p);
            printf("\r\n\x1b[2K   \x1b[2m(%s)\x1b[0m",
                   *query ? "nothing matches" : "nothing to open here");
            for (int i = 1; i < visible; i++) printf("\r\n\x1b[2K");
        }
        fflush(stdout);

        unsigned k = pick_key();
        bool refilter = false;

        /* While a filter is being typed the letters belong to it, so only the
           keys that cannot be part of a query still navigate. */
        if (filtering && k < 0x100 && k >= ' ' && k != 0x7f) {
            if (nq < (int)sizeof query - 1) { query[nq++] = (char)k; query[nq] = 0; }
            refilter = true;
        } else if (filtering && (k == 0x7f || k == 8)) {
            if (nq > 0) query[--nq] = 0;
            else filtering = false;
            refilter = true;
        } else if (k == '/' && !filtering) {
            filtering = true;
            nq = 0;
            query[0] = 0;
            refilter = true;
        } else if (k == PICK_ESC) {
            if (filtering || nq) { filtering = false; nq = 0; query[0] = 0; refilter = true; }
            else quit = true;
        } else {
            bool descend = (k == '\r' || k == '\n' || k == PICK_RIGHT ||
                            (!filtering && k == ' '));

            if (k == PICK_UP || (!filtering && k == 'k')) sel--;
            else if (k == PICK_DOWN || (!filtering && k == 'j')) sel++;
            else if (!filtering && k == 'g') sel = 0;
            else if (!filtering && k == 'G') sel = nview - 1;
            else if (k == PICK_LEFT || (!filtering && (k == 0x7f || k == 8))) {
                char was[PATH_MAX];
                snprintf(was, sizeof was, "%s", dir);
                pick_dir_parent(dir);
                n = pick_dir_load(dir, e, PICK_DIR_MAX, &o);
                filtering = false; nq = 0; query[0] = 0;
                pick_dir_reset_view(view, &nview, n);
                /* Come back out onto the directory just left, not onto the top. */
                sel = 0;
                for (int i = 0; i < nview; i++)
                    if (!strcmp(e[view[i]].path, was)) { sel = i; break; }
                first = 0;
            } else if (descend && nview > 0) {
                int idx = view[sel];
                if (!e[idx].is_dir || (o.leaf && o.leaf(e[idx].path, o.ctx))) {
                    snprintf(out, cap, "%s", e[idx].path);
                    chosen = true;
                } else {
                    snprintf(dir, sizeof dir, "%s", e[idx].path);
                    n = pick_dir_load(dir, e, PICK_DIR_MAX, &o);
                    filtering = false; nq = 0; query[0] = 0;
                    pick_dir_reset_view(view, &nview, n);
                    sel = first = 0;
                }
            } else if (k == 0 || (!filtering && k == 'q') || k == PICK_INTR || k == 4) {
                quit = true;
            }
        }

        if (refilter) {
            int m = 0;
            for (int i = 0; i < n; i++) {
                int sc;
                if (!pick_fuzzy(e[i].name, query, &sc)) continue;
                rows[m].idx = i;
                rows[m].score = sc;
                rows[m].order = i;
                m++;
            }
            if (*query) qsort(rows, (size_t)m, sizeof *rows, pick_dir_rank_cmp);
            nview = 0;
            for (int i = 0; i < m; i++) view[nview++] = rows[i].idx;
            sel = first = 0;
        }

        if (sel < 0) sel = 0;
        if (sel > nview - 1) sel = nview - 1;
        if (sel < 0) sel = 0;
        if (sel < first) first = sel;
        if (sel >= first + visible) first = sel - visible + 1;
    }

    pick_end(&p);
    free(e);
    free(view);
    free(rows);
    return chosen ? 0 : 1;
}

#endif /* PICK_IMPLEMENTATION */
