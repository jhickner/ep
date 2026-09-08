/**
 * diskcache.h - persistent thumbnail store (single-header)
 *
 * In exactly ONE .c file:
 *
 *     #define DISKCACHE_IMPLEMENTATION
 *     #include "diskcache.h"
 *
 * Decoded thumbnails are kept as raw RGB under ~/.config/pix/cache, keyed by
 * the source file's identity and the box it was scaled for. Re-entering a
 * directory then costs a read() per image instead of a decode.
 *
 * Entries are a few kilobytes each, so the store is bounded by total bytes and
 * pruned oldest-first. Everything here is safe to call from worker threads:
 * writes go to a private temp name and are renamed into place.
 */

#ifndef DISKCACHE_H
#define DISKCACHE_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

// Point the store at `dir`, or at ~/.config/pix/cache when NULL, creating it if
// needed. Until this succeeds every other call is a no-op, so a read-only or
// missing home directory just means no caching.
void dc_init(const char *dir);

// Look up the thumbnail for `path` at the given box. Returns a malloc'd w*h*3
// buffer the caller owns, or NULL on a miss. `size` and `mtime` come from the
// caller's stat, and are part of the key: an edited file misses.
uint8_t *dc_get(const char *path, long long size, long long mtime,
                int box_w, int box_h, int *w, int *h, int *src_w, int *src_h);

// Store a thumbnail. Silently does nothing when the store is unavailable or the
// image is too large to be worth keeping.
void dc_put(const char *path, long long size, long long mtime,
            int box_w, int box_h, const uint8_t *rgb, int w, int h,
            int src_w, int src_h);

// Delete oldest entries until the store is comfortably under `budget` bytes.
// Walks the whole directory, so callers run it once, off the hot path.
void dc_prune(size_t budget);

#endif // DISKCACHE_H

/* ======================================================================== */
/* Implementation                                                           */
/* ======================================================================== */
#ifdef DISKCACHE_IMPLEMENTATION

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>
#include <errno.h>
#include <sys/stat.h>

// Above this the entry stops paying for itself: previews are re-decoded at
// whatever size the window happens to be, so they would churn the store
// without ever being hit twice.
#define DC_MAX_PIXELS (512 * 512)

#define DC_MAGIC 0x54584950u   // "PIXT"
#define DC_VERSION 1u

typedef struct {
    uint32_t magic, version;
    int32_t w, h, src_w, src_h;
} DcHeader;

static char dc_dir[1024];
static bool dc_ready;

static bool dc_mkdir_p(char *path) {
    for (char *p = path + 1; *p; p++) {
        if (*p != '/') continue;
        *p = '\0';
        mkdir(path, 0700);
        *p = '/';
    }
    return mkdir(path, 0700) == 0 || errno == EEXIST;
}

void dc_init(const char *dir) {
    dc_ready = false;
    if (dir && *dir) {
        snprintf(dc_dir, sizeof dc_dir, "%s", dir);
    } else {
        const char *home = getenv("HOME");
        if (!home || !*home) return;
        snprintf(dc_dir, sizeof dc_dir, "%s/.config/pix/cache", home);
    }
    char tmp[sizeof dc_dir];
    memcpy(tmp, dc_dir, sizeof tmp);
    dc_ready = dc_mkdir_p(tmp);
}

// FNV-1a over everything that decides what the pixels look like. Collisions
// would show the wrong image, so the box dimensions go in alongside the file's
// identity - a thumbnail is only valid for the box it was fitted to.
static uint64_t dc_hash(const char *path, long long size, long long mtime,
                        int box_w, int box_h) {
    uint64_t h = 1469598103934665603ULL;
    #define DC_MIX(bytes, n) do { \
        const uint8_t *b_ = (const uint8_t *)(bytes); \
        for (size_t i_ = 0; i_ < (size_t)(n); i_++) { \
            h ^= b_[i_]; h *= 1099511628211ULL; \
        } \
    } while (0)
    DC_MIX(path, strlen(path));
    DC_MIX(&size, sizeof size);
    DC_MIX(&mtime, sizeof mtime);
    DC_MIX(&box_w, sizeof box_w);
    DC_MIX(&box_h, sizeof box_h);
    #undef DC_MIX
    return h;
}

static void dc_path_for(uint64_t key, char *buf, size_t n) {
    snprintf(buf, n, "%s/%016llx", dc_dir, (unsigned long long)key);
}

uint8_t *dc_get(const char *path, long long size, long long mtime,
                int box_w, int box_h, int *w, int *h, int *src_w, int *src_h) {
    if (!dc_ready) return NULL;

    char file[1152];
    dc_path_for(dc_hash(path, size, mtime, box_w, box_h), file, sizeof file);

    FILE *f = fopen(file, "rb");
    if (!f) return NULL;

    DcHeader hd;
    uint8_t *px = NULL;
    if (fread(&hd, sizeof hd, 1, f) != 1) goto done;
    if (hd.magic != DC_MAGIC || hd.version != DC_VERSION) goto done;
    if (hd.w <= 0 || hd.h <= 0 || (long long)hd.w * hd.h > DC_MAX_PIXELS) goto done;

    size_t n = (size_t)hd.w * (size_t)hd.h * 3;
    px = (uint8_t *)malloc(n);
    if (!px) goto done;
    if (fread(px, 1, n, f) != n) { free(px); px = NULL; goto done; }

    *w = hd.w; *h = hd.h;
    *src_w = hd.src_w; *src_h = hd.src_h;
done:
    fclose(f);
    return px;
}

void dc_put(const char *path, long long size, long long mtime,
            int box_w, int box_h, const uint8_t *rgb, int w, int h,
            int src_w, int src_h) {
    if (!dc_ready || !rgb || w <= 0 || h <= 0) return;
    if ((long long)w * h > DC_MAX_PIXELS) return;

    // A private temp name, then rename: a reader never sees a half-written
    // entry, and two workers racing on the same key both end up correct.
    char tmp[1152];
    snprintf(tmp, sizeof tmp, "%s/.tmp-%d-%p", dc_dir, (int)getpid(), (void *)rgb);
    FILE *f = fopen(tmp, "wb");
    if (!f) return;

    DcHeader hd = { DC_MAGIC, DC_VERSION, w, h, src_w, src_h };
    size_t n = (size_t)w * (size_t)h * 3;
    bool ok = fwrite(&hd, sizeof hd, 1, f) == 1 && fwrite(rgb, 1, n, f) == n;
    ok = (fclose(f) == 0) && ok;

    char file[1152];
    dc_path_for(dc_hash(path, size, mtime, box_w, box_h), file, sizeof file);
    if (!ok || rename(tmp, file) != 0) unlink(tmp);
}

typedef struct { char name[32]; long long mtime; long long size; } DcEntry;

static int dc_by_age(const void *a, const void *b) {
    long long d = ((const DcEntry *)a)->mtime - ((const DcEntry *)b)->mtime;
    return d < 0 ? -1 : d > 0 ? 1 : 0;
}

void dc_prune(size_t budget) {
    if (!dc_ready) return;

    DIR *d = opendir(dc_dir);
    if (!d) return;

    DcEntry *ents = NULL;
    int n = 0, cap = 0;
    size_t total = 0;

    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        if (de->d_name[0] == '.') continue;
        if (strlen(de->d_name) >= sizeof ents->name) continue;

        char file[1152];
        snprintf(file, sizeof file, "%s/%s", dc_dir, de->d_name);
        struct stat st;
        if (stat(file, &st) != 0 || !S_ISREG(st.st_mode)) continue;

        if (n == cap) {
            cap = cap ? cap * 2 : 256;
            DcEntry *grown = (DcEntry *)realloc(ents, (size_t)cap * sizeof *ents);
            if (!grown) break;
            ents = grown;
        }
        snprintf(ents[n].name, sizeof ents[n].name, "%s", de->d_name);
        ents[n].mtime = (long long)st.st_mtime;
        ents[n].size = (long long)st.st_size;
        total += (size_t)st.st_size;
        n++;
    }
    closedir(d);

    if (total > budget) {
        // Drop to well under the budget so this doesn't run every session.
        size_t target = budget - budget / 4;
        qsort(ents, (size_t)n, sizeof *ents, dc_by_age);
        for (int i = 0; i < n && total > target; i++) {
            char file[1152];
            snprintf(file, sizeof file, "%s/%s", dc_dir, ents[i].name);
            if (unlink(file) == 0) total -= (size_t)ents[i].size;
        }
    }
    free(ents);
}

#endif // DISKCACHE_IMPLEMENTATION
