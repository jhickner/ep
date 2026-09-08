/**
 * cache.h - threaded thumbnail cache (single-header)
 *
 * In exactly ONE .c file:
 *
 *     #define CACHE_IMPLEMENTATION
 *     #include "cache.h"
 *
 * Depends on image.h and diskcache.h. Worker threads decode and resample in the
 * background; the main thread only ever requests, polls, and draws. Requesting a
 * slot at a different box size supersedes any in-flight work for it.
 *
 * Work is served nearest-the-focus first rather than in arrival order. A
 * viewport scrolled several screens in a second would otherwise leave workers
 * grinding through rows nobody is looking at any more, because every visible
 * cell re-requests itself each frame.
 *
 * Results arrive in two passes. The first may be a JPEG's embedded thumbnail,
 * which costs almost nothing and fills the grid immediately; because such a
 * thumbnail can be stale, the slot is then re-queued behind all visible work
 * for a real decode that replaces it. Only that second result is written to the
 * disk store, so a directory is instant and correct on every later visit.
 */

#ifndef CACHE_H
#define CACHE_H

#include <stdint.h>
#include <stdbool.h>
#include <pthread.h>

#ifndef IMAGE_H
#include "image.h"
#endif
#ifndef DISKCACHE_H
#include "diskcache.h"
#endif

typedef enum {
    SLOT_EMPTY = 0,
    SLOT_QUEUED,
    SLOT_READY,
    SLOT_FAILED,
} SlotState;

typedef struct {
    int state;
    int gen;              // bumped on every request; stale results are dropped
    const char *path;     // borrowed from the caller's file list
    int box_w, box_h;     // box currently requested
    uint8_t *rgb;         // scaled pixels, freed once handed to the terminal
    int w, h;             // scaled size
    int src_w, src_h;     // size on disk
    bool sent;            // pixel data currently resident in the terminal
    bool busy;            // a worker holds this slot
    bool provisional;     // showing an embedded thumbnail, pending a real decode
    int pass;             // 0 = nothing yet, 1 = provisional shown, 2 = final
    uint64_t lru;
} Slot;

typedef struct {
    Slot *slots;
    int n;
    pthread_mutex_t mu;
    pthread_cond_t cv;
    int focus;            // index the viewport is centred on
    int win_lo, win_hi;   // inclusive range still worth working on
    pthread_t *threads;
    int nthreads;
    bool stop;
    int completions;      // consumed by cache_take_completions()
    bool use_disk;        // consult diskcache.h for results this size
    uint8_t bg[3];
} Cache;

bool cache_init(Cache *c, int n, int nthreads, const uint8_t bg[3], bool use_disk);
void cache_destroy(Cache *c);

// Set the index work should radiate outwards from - normally the selection.
// Only affects the order pending jobs are picked up in.
void cache_set_focus(Cache *c, int i);

// Confine work to [lo, hi]: queued jobs outside it are cancelled, in-flight
// ones are abandoned at the next checkpoint, and decoded pixels nobody
// collected are released. Call once per frame with the visible range plus
// whatever margin should stay warm.
//
// This is what keeps a fast scroll from committing the decoder to every image
// it flew past. Nothing is lost that costs much to recover: a cancelled slot
// re-requests itself the moment it is on screen again, and by then its pixels
// are usually in the disk store.
void cache_set_window(Cache *c, int lo, int hi);

// Ask for slot `i` scaled to fit box_w x box_h pixels. Cheap and idempotent:
// a repeat request for a size already ready or in flight does nothing.
void cache_request(Cache *c, int i, const char *path, int box_w, int box_h);

// Number of jobs that finished since the last call. Non-zero means repaint.
int cache_take_completions(Cache *c);

// Snapshot slot `i` for drawing. `out->rgb` is always NULL - pixels can only
// be obtained through cache_take_pixels, which is race-free.
void cache_peek(Cache *c, int i, Slot *out);

// Detach slot `i`'s pixels for transmission and mark it resident in the
// terminal. Returns NULL if there are none pending; otherwise the caller owns
// the buffer and must free it.
uint8_t *cache_take_pixels(Cache *c, int i, int *w, int *h);

// Forget that slot `i` is resident in the terminal (after a kitty delete).
void cache_mark_evicted(Cache *c, int i);

// Touch slot `i`'s recency and return the least-recently-used resident slot
// other than the ones in `keep`, or -1 when fewer than `limit` are resident.
void cache_touch(Cache *c, int i);
int cache_lru_victim(Cache *c, int limit);

#endif // CACHE_H

/* ======================================================================== */
/* Implementation                                                           */
/* ======================================================================== */
#ifdef CACHE_IMPLEMENTATION

#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <sys/stat.h>

static uint64_t g_cache_clock = 1;

// Added to a slot's priority once it holds a provisional result, which parks
// every refinement behind every slot still waiting for its first pixels.
#define CACHE_REFINE_PENALTY (1 << 20)

// The most urgent unclaimed job in the window, or -1. Caller holds the mutex.
//
// Priority is computed here against the focus as it stands now, never stored on
// the slot. A stored priority goes stale the instant the viewport moves: a slot
// queued while it was under the cursor would keep its head-of-queue standing
// long after scrolling left it far behind, and hundreds of those will bury the
// handful of images actually on screen.
//
// Scanning only the window also keeps this off the directory size.
static int cache_pick(Cache *c) {
    int best = -1, best_prio = INT_MAX;
    int lo = c->win_lo < 0 ? 0 : c->win_lo;
    int hi = c->win_hi >= c->n ? c->n - 1 : c->win_hi;

    for (int i = lo; i <= hi; i++) {
        const Slot *s = &c->slots[i];
        if (s->busy) continue;
        // pass 1 means the slot is displayable but still owes a real decode; it
        // stays eligible without ever leaving SLOT_READY.
        bool owed = (s->state == SLOT_QUEUED) ||
                    (s->state == SLOT_READY && s->pass == 1);
        if (!owed) continue;

        int d = i - c->focus;
        if (d < 0) d = -d;
        int prio = d + (s->pass == 1 ? CACHE_REFINE_PENALTY : 0);
        if (prio < best_prio) { best_prio = prio; best = i; }
    }
    return best;
}

// What a worker consults mid-decode to decide whether the result is still
// wanted. The generation catches a re-request or a resize; the window catches
// the row having scrolled away.
typedef struct {
    Cache *c;
    int idx, gen;
} CacheAbort;

static bool cache_abort(void *v) {
    CacheAbort *a = (CacheAbort *)v;
    Cache *c = a->c;
    pthread_mutex_lock(&c->mu);
    bool give_up = c->stop || c->slots[a->idx].gen != a->gen ||
                   a->idx < c->win_lo || a->idx > c->win_hi;
    pthread_mutex_unlock(&c->mu);
    return give_up;
}

static void *cache_worker(void *arg) {
    Cache *c = (Cache *)arg;

    for (;;) {
        pthread_mutex_lock(&c->mu);
        int i = -1;
        while (!c->stop && (i = cache_pick(c)) < 0)
            pthread_cond_wait(&c->cv, &c->mu);
        if (c->stop) { pthread_mutex_unlock(&c->mu); return NULL; }

        Slot *s = &c->slots[i];
        s->busy = true;
        int gen = s->gen;
        int pass = s->pass;
        int bw = s->box_w, bh = s->box_h;
        const char *path = s->path;
        bool use_disk = c->use_disk;
        uint8_t bg[3] = { c->bg[0], c->bg[1], c->bg[2] };
        pthread_mutex_unlock(&c->mu);

        uint8_t *scaled = NULL;
        int dw = 0, dh = 0, sw = 0, sh = 0;
        bool provisional = false;

        // The disk store is keyed on the file's identity, so one stat decides
        // both whether to look and what to look for. Only final results are
        // ever stored, so a hit needs no refining.
        struct stat st;
        bool have_st = use_disk && stat(path, &st) == 0;
        if (pass == 0 && have_st)
            scaled = dc_get(path, (long long)st.st_size, (long long)st.st_mtime,
                            bw, bh, &dw, &dh, &sw, &sh);

        CacheAbort ab = { c, i, gen };
        if (!scaled) {
            // The refining pass is the one that must be right, so it is the one
            // that refuses the embedded thumbnail.
            Image im;
            int flags = (pass == 0) ? 0 : IMAGE_NO_EXIF_THUMB;
            if (image_load_fit_cancel(path, bg, bw, bh, flags,
                                      cache_abort, &ab, &im)) {
                scaled = im.rgb;
                dw = im.w; dh = im.h;
                sw = im.src_w; sh = im.src_h;
                provisional = im.provisional;
                if (!provisional && have_st)
                    dc_put(path, (long long)st.st_size, (long long)st.st_mtime,
                           bw, bh, scaled, dw, dh, sw, sh);
            }
        }

        // Distinguish work we gave up on from work that genuinely failed: an
        // abandoned slot has to look untouched, or it would show an error mark
        // and never be retried when it scrolls back on screen.
        bool abandoned = !scaled && cache_abort(&ab);

        pthread_mutex_lock(&c->mu);
        s->busy = false;
        // A resize or a re-request while we were decoding invalidates this
        // result. The slot is still SLOT_QUEUED in that case, so waking a
        // worker hands it straight back out with the new box.
        if (s->gen != gen) {
            free(scaled);
            pthread_cond_signal(&c->cv);
        } else if (abandoned) {
            free(scaled);
            s->state = SLOT_EMPTY;
            s->pass = 0;
            s->provisional = false;
            s->box_w = s->box_h = 0;
        } else if (pass == 1 && !scaled) {
            // The refinement failed on a file whose thumbnail decoded fine.
            // Keep what is on screen rather than replacing a usable image with
            // an error mark; it is the best we are going to get.
            s->pass = 2;
            s->provisional = false;
        } else {
            free(s->rgb);
            s->rgb = scaled;
            s->w = dw; s->h = dh;
            s->src_w = sw; s->src_h = sh;
            s->state = scaled ? SLOT_READY : SLOT_FAILED;
            s->sent = false;
            s->provisional = provisional;
            s->pass = provisional ? 1 : 2;
            c->completions++;
            // pass 1 leaves the slot eligible for a second look, but behind
            // every slot still waiting for its first pixels. The state stays
            // SLOT_READY throughout, so what is on screen is never taken away.
            if (provisional) pthread_cond_signal(&c->cv);
        }
        pthread_mutex_unlock(&c->mu);
    }
}

bool cache_init(Cache *c, int n, int nthreads, const uint8_t bg[3], bool use_disk) {
    memset(c, 0, sizeof *c);
    if (n <= 0) n = 1;
    if (nthreads < 1) nthreads = 1;

    c->n = n;
    c->use_disk = use_disk;
    c->win_lo = 0;
    c->win_hi = n - 1;
    c->slots = (Slot *)calloc((size_t)n, sizeof(Slot));
    c->threads = (pthread_t *)calloc((size_t)nthreads, sizeof(pthread_t));
    if (!c->slots || !c->threads) { cache_destroy(c); return false; }

    memcpy(c->bg, bg, 3);
    pthread_mutex_init(&c->mu, NULL);
    pthread_cond_init(&c->cv, NULL);

    for (int i = 0; i < nthreads; i++) {
        if (pthread_create(&c->threads[i], NULL, cache_worker, c) != 0) break;
        c->nthreads++;
    }
    return c->nthreads > 0;
}

void cache_destroy(Cache *c) {
    if (c->nthreads) {
        pthread_mutex_lock(&c->mu);
        c->stop = true;
        pthread_cond_broadcast(&c->cv);
        pthread_mutex_unlock(&c->mu);
        for (int i = 0; i < c->nthreads; i++) pthread_join(c->threads[i], NULL);
        pthread_mutex_destroy(&c->mu);
        pthread_cond_destroy(&c->cv);
    }
    if (c->slots)
        for (int i = 0; i < c->n; i++) free(c->slots[i].rgb);
    free(c->slots);
    free(c->threads);
    memset(c, 0, sizeof *c);
}

void cache_set_focus(Cache *c, int i) {
    pthread_mutex_lock(&c->mu);
    c->focus = i;
    pthread_mutex_unlock(&c->mu);
}

void cache_set_window(Cache *c, int lo, int hi) {
    pthread_mutex_lock(&c->mu);
    c->win_lo = lo;
    c->win_hi = hi;

    for (int i = 0; i < c->n; i++) {
        if (i >= lo && i <= hi) continue;
        Slot *s = &c->slots[i];
        if (s->busy) continue;   // a worker owns it and checks the window itself

        if (s->state == SLOT_QUEUED) {
            s->state = SLOT_EMPTY;   // never started; simply forget it
            s->gen++;
        } else if (s->rgb && !s->sent) {
            // Decoded, but scrolled past before anything collected it. The disk
            // store makes this cheap to produce again, so the memory is better
            // spent on what is actually on screen.
            free(s->rgb);
            s->rgb = NULL;
            s->state = SLOT_EMPTY;
            s->provisional = false;
            s->pass = 0;
            s->box_w = s->box_h = 0;
            s->gen++;
        }
    }
    pthread_mutex_unlock(&c->mu);
}

void cache_request(Cache *c, int i, const char *path, int box_w, int box_h) {
    if (i < 0 || i >= c->n || box_w <= 0 || box_h <= 0) return;

    pthread_mutex_lock(&c->mu);
    Slot *s = &c->slots[i];
    bool same = (s->box_w == box_w && s->box_h == box_h && s->path == path);
    if (same && s->state != SLOT_EMPTY) { pthread_mutex_unlock(&c->mu); return; }

    s->path = path;
    s->box_w = box_w;
    s->box_h = box_h;
    s->gen++;
    s->state = SLOT_QUEUED;
    s->pass = 0;
    s->provisional = false;
    pthread_cond_signal(&c->cv);
    pthread_mutex_unlock(&c->mu);
}

int cache_take_completions(Cache *c) {
    pthread_mutex_lock(&c->mu);
    int n = c->completions;
    c->completions = 0;
    pthread_mutex_unlock(&c->mu);
    return n;
}

void cache_peek(Cache *c, int i, Slot *out) {
    memset(out, 0, sizeof *out);
    if (i < 0 || i >= c->n) return;
    pthread_mutex_lock(&c->mu);
    *out = c->slots[i];
    out->rgb = NULL;
    pthread_mutex_unlock(&c->mu);
}

uint8_t *cache_take_pixels(Cache *c, int i, int *w, int *h) {
    if (i < 0 || i >= c->n) return NULL;
    pthread_mutex_lock(&c->mu);
    Slot *s = &c->slots[i];
    uint8_t *rgb = s->rgb;
    if (rgb) {
        s->rgb = NULL;
        s->sent = true;
        s->lru = g_cache_clock++;
        *w = s->w;
        *h = s->h;
    }
    pthread_mutex_unlock(&c->mu);
    return rgb;
}

void cache_mark_evicted(Cache *c, int i) {
    if (i < 0 || i >= c->n) return;
    pthread_mutex_lock(&c->mu);
    Slot *s = &c->slots[i];
    s->sent = false;
    s->state = SLOT_EMPTY;   // pixels are gone from both sides; decode again on demand
    s->box_w = s->box_h = 0;
    s->pass = 0;
    s->provisional = false;
    // Bump the generation so a job still decoding the old path (the file list
    // may have just been re-sorted under it) can't publish into this slot.
    s->gen++;
    free(s->rgb);
    s->rgb = NULL;
    pthread_mutex_unlock(&c->mu);
}

void cache_touch(Cache *c, int i) {
    if (i < 0 || i >= c->n) return;
    pthread_mutex_lock(&c->mu);
    c->slots[i].lru = g_cache_clock++;
    pthread_mutex_unlock(&c->mu);
}

int cache_lru_victim(Cache *c, int limit) {
    pthread_mutex_lock(&c->mu);
    int resident = 0, victim = -1;
    uint64_t oldest = UINT64_MAX;
    for (int i = 0; i < c->n; i++) {
        if (!c->slots[i].sent) continue;
        resident++;
        if (c->slots[i].lru < oldest) { oldest = c->slots[i].lru; victim = i; }
    }
    pthread_mutex_unlock(&c->mu);
    return resident > limit ? victim : -1;
}

#endif // CACHE_IMPLEMENTATION
