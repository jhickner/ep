/**
 * page.h - background page decoder with a small resident window
 *
 * In exactly ONE .c file:
 *
 *     #define PAGE_IMPLEMENTATION
 *     #include "page.h"
 *
 * Depends on image.h and panel.h. Unlike a thumbnail cache, a slot here keeps
 * its decoded pixels: the same buffer is scaled to the window for page view and
 * cropped for panel view, so moving between the two costs nothing and neither
 * one re-reads the file. Panels are found on the worker thread as part of the
 * decode, for the same reason.
 *
 * Only a handful of pages are held at once - one is some tens of megabytes - so
 * `pages_want` is also the eviction point: ask for the pages that matter, in
 * the order they matter, and the rest are dropped.
 */

#ifndef PAGE_H
#define PAGE_H

#include <stdint.h>
#include <stdbool.h>
#include <pthread.h>

#ifndef IMAGE_H
#include "image.h"
#endif
#ifndef PANEL_H
#include "panel.h"
#endif

#define PAGE_MAX_PANELS 48

typedef enum {
    PAGE_EMPTY = 0,
    PAGE_QUEUED,
    PAGE_READY,
    PAGE_FAILED,
} PageState;

typedef struct {
    int       idx;            // page held, -1 when free
    int       state;
    int       gen;
    uint8_t  *rgb;            // decoded page, w*h*3
    int       w, h;           // decoded size (<= max_dim on the long edge)
    int       src_w, src_h;   // size on disk
    Panel     panels[PAGE_MAX_PANELS];
    int       npanels;
    int       pins;
    bool      busy;
    uint64_t  lru;
} PageSlot;

typedef struct {
    PageSlot       *slots;
    int             nslots;
    char          **paths;    // borrowed
    int             npaths;
    pthread_mutex_t mu;
    pthread_cond_t  cv;
    pthread_t      *threads;
    int             nthreads;
    bool            stop;
    int             completions;
    int             focus;
    int             max_dim;  // long edge a page is decoded to, 0 for native
    uint8_t         bg[3];
} PageStore;

bool pages_init(PageStore *p, const uint8_t bg[3], int nslots, int nthreads, int max_dim);
void pages_destroy(PageStore *p);

// Point the store at a book's page list. Everything resident is dropped.
void pages_set_list(PageStore *p, char **paths, int npaths);

// Keep page `idx` resident, decoding it if it is not. Call for the page being
// shown first and its neighbours after: a page nobody has asked for since the
// last frame is what gets evicted to make room.
void pages_want(PageStore *p, int idx);

// The page work should radiate outwards from. Only affects queue order.
void pages_set_focus(PageStore *p, int idx);

// Jobs finished since the last call. Non-zero means repaint.
int pages_take_completions(PageStore *p);

int pages_state(PageStore *p, int idx);

// Borrow page `idx`'s pixels, or NULL if it is not decoded. The buffer stays
// valid until pages_release; `panels` points into the slot and lives as long.
const uint8_t *pages_borrow(PageStore *p, int idx, int *w, int *h,
                            const Panel **panels, int *npanels);
void pages_release(PageStore *p, int idx);

#endif // PAGE_H

/* ======================================================================== */
/* Implementation                                                           */
/* ======================================================================== */
#ifdef PAGE_IMPLEMENTATION

#include <stdlib.h>
#include <string.h>

static uint64_t g_page_clock = 1;

// Caller holds the mutex.
static PageSlot *page_find(PageStore *p, int idx) {
    for (int i = 0; i < p->nslots; i++)
        if (p->slots[i].idx == idx && p->slots[i].state != PAGE_EMPTY)
            return &p->slots[i];
    return NULL;
}

// The queued page nearest the focus, or -1. Caller holds the mutex.
static int page_pick(PageStore *p) {
    int best = -1, best_d = 0;
    for (int i = 0; i < p->nslots; i++) {
        PageSlot *s = &p->slots[i];
        if (s->state != PAGE_QUEUED || s->busy) continue;
        int d = s->idx - p->focus;
        if (d < 0) d = -d;
        if (best < 0 || d < best_d) { best = i; best_d = d; }
    }
    return best;
}

static void *page_worker(void *arg) {
    PageStore *p = (PageStore *)arg;
    pthread_mutex_lock(&p->mu);
    for (;;) {
        int i;
        while (!p->stop && (i = page_pick(p)) < 0)
            pthread_cond_wait(&p->cv, &p->mu);
        if (p->stop) break;

        PageSlot *s = &p->slots[i];
        s->busy = true;
        int idx = s->idx, gen = s->gen;
        const char *path = (idx >= 0 && idx < p->npaths) ? p->paths[idx] : NULL;
        int max_dim = p->max_dim;
        uint8_t bg[3] = { p->bg[0], p->bg[1], p->bg[2] };
        pthread_mutex_unlock(&p->mu);

        Image im;
        memset(&im, 0, sizeof im);
        bool ok = false;
        if (path) {
            // Only cap a page that exceeds the cap: passing a box larger than
            // the file would have image.h scale the page up to meet it.
            int box = 0, pw = 0, ph = 0;
            if (max_dim > 0 && image_probe(path, &pw, &ph) &&
                (pw > max_dim || ph > max_dim))
                box = max_dim;
            ok = image_load_fit(path, bg, box, box, &im);
        }

        Panel panels[PAGE_MAX_PANELS];
        int npanels = 0;
        if (ok) npanels = panel_detect(im.rgb, im.w, im.h, panels, PAGE_MAX_PANELS);

        pthread_mutex_lock(&p->mu);
        s->busy = false;
        if (p->stop || s->gen != gen || s->idx != idx) {
            image_free(&im);          // the slot was reassigned under us
        } else if (ok) {
            free(s->rgb);
            s->rgb = im.rgb;
            s->w = im.w; s->h = im.h;
            s->src_w = im.src_w; s->src_h = im.src_h;
            memcpy(s->panels, panels, sizeof(Panel) * (size_t)npanels);
            s->npanels = npanels;
            s->state = PAGE_READY;
            p->completions++;
        } else {
            s->state = PAGE_FAILED;
            p->completions++;
        }
        pthread_cond_broadcast(&p->cv);
    }
    pthread_mutex_unlock(&p->mu);
    return NULL;
}

bool pages_init(PageStore *p, const uint8_t bg[3], int nslots, int nthreads, int max_dim) {
    memset(p, 0, sizeof *p);
    if (nslots < 2) nslots = 2;
    if (nthreads < 1) nthreads = 1;
    p->slots = (PageSlot *)calloc((size_t)nslots, sizeof *p->slots);
    p->threads = (pthread_t *)calloc((size_t)nthreads, sizeof *p->threads);
    if (!p->slots || !p->threads) { free(p->slots); free(p->threads); return false; }
    p->nslots = nslots;
    p->max_dim = max_dim;
    memcpy(p->bg, bg, 3);
    for (int i = 0; i < nslots; i++) p->slots[i].idx = -1;
    pthread_mutex_init(&p->mu, NULL);
    pthread_cond_init(&p->cv, NULL);
    for (int i = 0; i < nthreads; i++)
        if (pthread_create(&p->threads[i], NULL, page_worker, p) == 0) p->nthreads++;
    return p->nthreads > 0;
}

void pages_destroy(PageStore *p) {
    pthread_mutex_lock(&p->mu);
    p->stop = true;
    pthread_cond_broadcast(&p->cv);
    pthread_mutex_unlock(&p->mu);
    for (int i = 0; i < p->nthreads; i++) pthread_join(p->threads[i], NULL);
    for (int i = 0; i < p->nslots; i++) free(p->slots[i].rgb);
    free(p->slots);
    free(p->threads);
    pthread_mutex_destroy(&p->mu);
    pthread_cond_destroy(&p->cv);
    memset(p, 0, sizeof *p);
}

void pages_set_list(PageStore *p, char **paths, int npaths) {
    pthread_mutex_lock(&p->mu);
    p->paths = paths;
    p->npaths = npaths;
    for (int i = 0; i < p->nslots; i++) {
        PageSlot *s = &p->slots[i];
        s->gen++;                     // orphan anything in flight
        if (!s->busy) { free(s->rgb); s->rgb = NULL; }
        s->idx = -1;
        s->state = PAGE_EMPTY;
        s->npanels = 0;
        s->pins = 0;
    }
    p->focus = 0;
    p->completions = 0;
    pthread_mutex_unlock(&p->mu);
}

void pages_set_focus(PageStore *p, int idx) {
    pthread_mutex_lock(&p->mu);
    p->focus = idx;
    pthread_mutex_unlock(&p->mu);
}

void pages_want(PageStore *p, int idx) {
    if (idx < 0) return;
    pthread_mutex_lock(&p->mu);
    if (idx >= p->npaths) { pthread_mutex_unlock(&p->mu); return; }

    PageSlot *s = page_find(p, idx);
    if (s) { s->lru = ++g_page_clock; pthread_mutex_unlock(&p->mu); return; }

    // Free slot, else the stalest one nobody is drawing from or working on.
    PageSlot *victim = NULL;
    for (int i = 0; i < p->nslots; i++) {
        PageSlot *c = &p->slots[i];
        if (c->state == PAGE_EMPTY && !c->busy) { victim = c; break; }
        if (c->pins || c->busy) continue;
        if (!victim || victim->state != PAGE_EMPTY) {
            if (!victim || c->lru < victim->lru) victim = c;
        }
    }
    if (!victim) { pthread_mutex_unlock(&p->mu); return; }

    victim->gen++;
    free(victim->rgb);
    victim->rgb = NULL;
    victim->w = victim->h = 0;
    victim->npanels = 0;
    victim->idx = idx;
    victim->state = PAGE_QUEUED;
    victim->lru = ++g_page_clock;
    pthread_cond_broadcast(&p->cv);
    pthread_mutex_unlock(&p->mu);
}

int pages_take_completions(PageStore *p) {
    pthread_mutex_lock(&p->mu);
    int n = p->completions;
    p->completions = 0;
    pthread_mutex_unlock(&p->mu);
    return n;
}

int pages_state(PageStore *p, int idx) {
    pthread_mutex_lock(&p->mu);
    PageSlot *s = page_find(p, idx);
    int st = s ? s->state : PAGE_EMPTY;
    pthread_mutex_unlock(&p->mu);
    return st;
}

const uint8_t *pages_borrow(PageStore *p, int idx, int *w, int *h,
                            const Panel **panels, int *npanels) {
    pthread_mutex_lock(&p->mu);
    PageSlot *s = page_find(p, idx);
    const uint8_t *rgb = NULL;
    if (s && s->state == PAGE_READY && s->rgb) {
        s->pins++;
        s->lru = ++g_page_clock;
        rgb = s->rgb;
        if (w) *w = s->w;
        if (h) *h = s->h;
        if (panels) *panels = s->panels;
        if (npanels) *npanels = s->npanels;
    }
    pthread_mutex_unlock(&p->mu);
    return rgb;
}

void pages_release(PageStore *p, int idx) {
    pthread_mutex_lock(&p->mu);
    PageSlot *s = page_find(p, idx);
    if (s && s->pins > 0) s->pins--;
    pthread_mutex_unlock(&p->mu);
}

#endif // PAGE_IMPLEMENTATION
