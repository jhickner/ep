#ifndef STATE_H
#define STATE_H

/**
 * state.h - where each book was left, in ~/.config/ep/state.
 *
 * One line per book, most recently read first:
 *
 *     spine <tab> block <tab> when <tab> page <tab> total <tab> mode <tab>
 *     off <tab> path
 *
 * `spine`/`block`/`off` are the reading position and are exact; `off` is a
 * byte offset into the block, so the place survives a change of column width.
 * `page`/`total` are the printed-page estimate shown in the status line and
 * by --resume. Lines written before `off` existed simply parse without it.
 */

#include <limits.h>
#include <stdbool.h>
#include <time.h>

#define STATE_LINES 400

typedef struct {
    int         spine, block, off, page, total, mode;
    time_t      when;
    const char *path;    /* into the line parsed, so it does not outlive it */
} StateLine;

typedef struct {
    char   path[PATH_MAX];
    char   title[400];
    int    spine, block, off, page, total, mode;
    time_t when;
} RecentBook;

bool state_lookup(const char *path, StateLine *out);
void state_save(const char *path, int spine, int block, int off, int page, int total, int mode);
void state_forget(const char *path);
int  state_recent(RecentBook *out, int cap);
void state_dir(char *out, size_t n);

#endif /* STATE_H */

#ifdef STATE_IMPLEMENTATION

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

/* EP_STATE_DIR moves the store somewhere else entirely, so a test run cannot
   touch the history of the books actually being read. */
void state_dir(char *out, size_t n) {
    const char *override = getenv("EP_STATE_DIR");
    if (override && *override) { snprintf(out, n, "%s", override); return; }
    const char *home = getenv("HOME");
    snprintf(out, n, "%s/.config/ep", home ? home : ".");
}

static void state_path(char *out, size_t n) {
    char dir[PATH_MAX];
    state_dir(dir, sizeof dir);
    snprintf(out, n, "%s/state", dir);
}

/* Splits a state line in place. Absolute paths never look like a number, so
   the leading numeric fields are self-delimiting. */
static bool state_parse(char *line, StateLine *out) {
    char *nl = strchr(line, '\n');
    if (nl) *nl = '\0';

    long long num[7] = { 0, 0, 0, 0, 0, 0, 0 };
    int nnum = 0;
    char *p = line;
    while (nnum < 7) {
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
    if (nnum < 2 || !*p) return false;

    out->spine = (int)num[0];
    out->block = (int)num[1];
    out->when  = nnum > 2 ? (time_t)num[2] : 0;
    out->page  = nnum > 3 ? (int)num[3] : 0;
    out->total = nnum > 4 ? (int)num[4] : 0;
    out->mode  = nnum > 5 ? (int)num[5] : 0;
    out->off   = nnum > 6 ? (int)num[6] : 0;
    out->path  = p;
    return true;
}

bool state_lookup(const char *path, StateLine *out) {
    memset(out, 0, sizeof *out);

    char sp[PATH_MAX];
    state_path(sp, sizeof sp);
    FILE *f = fopen(sp, "r");
    if (!f) return false;

    char line[PATH_MAX + 64];
    bool found = false;
    StateLine sl;
    while (!found && fgets(line, sizeof line, f)) {
        if (state_parse(line, &sl) && !strcmp(sl.path, path)) {
            *out = sl;
            out->path = NULL;
            found = true;
        }
    }
    fclose(f);
    return found;
}

/* Rewrites the store with `path` at the head, or without it when `spine` is
   negative. Every other line is carried over as it was. */
static void state_put(const char *path, int spine, int block, int off, int page, int total, int mode) {
    char sp[PATH_MAX], dir[PATH_MAX];
    state_path(sp, sizeof sp);
    state_dir(dir, sizeof dir);
    mkdir(dir, 0755);

    char (*keep)[PATH_MAX + 64] = malloc(sizeof(*keep) * STATE_LINES);
    if (!keep) return;
    int nkeep = 0;

    FILE *f = fopen(sp, "r");
    if (f) {
        char line[PATH_MAX + 64];
        while (nkeep < STATE_LINES - 1 && fgets(line, sizeof line, f)) {
            char copy[PATH_MAX + 64];
            snprintf(copy, sizeof copy, "%s", line);
            StateLine sl;
            if (!state_parse(copy, &sl)) continue;
            if (!strcmp(sl.path, path)) continue;      /* superseded, or dropped */
            snprintf(keep[nkeep++], sizeof keep[0], "%d\t%d\t%lld\t%d\t%d\t%d\t%d\t%s",
                     sl.spine, sl.block, (long long)sl.when, sl.page, sl.total,
                     sl.mode, sl.off, sl.path);
        }
        fclose(f);
    }

    /* Keep the previous generation. The store is small and losing it costs
       every book's place, so a copy is worth the two syscalls. */
    char bak[PATH_MAX + 8];
    snprintf(bak, sizeof bak, "%s.bak", sp);
    FILE *in = fopen(sp, "r");
    if (in) {
        FILE *out = fopen(bak, "w");
        if (out) {
            char buf[4096];
            size_t got;
            while ((got = fread(buf, 1, sizeof buf, in)) > 0) fwrite(buf, 1, got, out);
            fclose(out);
        }
        fclose(in);
    }

    char tmp[PATH_MAX + 8];
    snprintf(tmp, sizeof tmp, "%s.tmp", sp);
    FILE *o = fopen(tmp, "w");
    if (o) {
        if (spine >= 0)
            fprintf(o, "%d\t%d\t%lld\t%d\t%d\t%d\t%d\t%s\n",
                    spine, block, (long long)time(NULL), page, total, mode, off, path);
        for (int i = 0; i < nkeep; i++) fprintf(o, "%s\n", keep[i]);
        fclose(o);
        rename(tmp, sp);
    }
    free(keep);
}

void state_save(const char *path, int spine, int block, int off, int page, int total, int mode) {
    state_put(path, spine, block, off, page, total, mode);
}

void state_forget(const char *path) {
    state_put(path, -1, 0, 0, 0, 0, 0);
}

/* Newest first. Books whose file has since gone are left out. */
int state_recent(RecentBook *out, int cap) {
    char sp[PATH_MAX];
    state_path(sp, sizeof sp);
    FILE *f = fopen(sp, "r");
    if (!f) return 0;

    char line[PATH_MAX + 64];
    int n = 0;
    while (n < cap && fgets(line, sizeof line, f)) {
        StateLine sl;
        if (!state_parse(line, &sl)) continue;
        struct stat st;
        if (stat(sl.path, &st) != 0) continue;

        RecentBook *b = &out[n++];
        snprintf(b->path, sizeof b->path, "%s", sl.path);
        b->spine = sl.spine;
        b->block = sl.block;
        b->off   = sl.off;
        b->page  = sl.page;
        b->total = sl.total;
        b->mode  = sl.mode;
        b->when  = sl.when;

        const char *base = strrchr(b->path, '/');
        base = base ? base + 1 : b->path;
        snprintf(b->title, sizeof b->title, "%s", base);
        char *dot = strrchr(b->title, '.');
        if (dot && dot != b->title) *dot = '\0';
    }
    fclose(f);
    return n;
}

#endif /* STATE_IMPLEMENTATION */
