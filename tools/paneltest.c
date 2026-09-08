/**
 * paneltest - draw a page's detected panels over the page, as a PPM
 *
 *     paneltest <image> [out.ppm]
 *
 * Each panel is outlined and tagged with that many ticks along its top edge, so
 * both the boxes and the reading order can be checked at a glance. The panel
 * rectangles are also printed to stdout.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define IMAGE_IMPLEMENTATION
#include "image.h"
#define PANEL_IMPLEMENTATION
#include "panel.h"

static void hline(uint8_t *p, int w, int h, int x0, int x1, int y, const uint8_t c[3]) {
    if (y < 0 || y >= h) return;
    for (int x = x0; x <= x1; x++) {
        if (x < 0 || x >= w) continue;
        memcpy(p + ((size_t)y * w + x) * 3, c, 3);
    }
}

static void vline(uint8_t *p, int w, int h, int y0, int y1, int x, const uint8_t c[3]) {
    if (x < 0 || x >= w) return;
    for (int y = y0; y <= y1; y++) {
        if (y < 0 || y >= h) continue;
        memcpy(p + ((size_t)y * w + x) * 3, c, 3);
    }
}

static void box(uint8_t *p, int w, int h, Panel r, const uint8_t c[3], int thick) {
    for (int t = 0; t < thick; t++) {
        hline(p, w, h, r.x, r.x + r.w - 1, r.y + t, c);
        hline(p, w, h, r.x, r.x + r.w - 1, r.y + r.h - 1 - t, c);
        vline(p, w, h, r.y, r.y + r.h - 1, r.x + t, c);
        vline(p, w, h, r.y, r.y + r.h - 1, r.x + r.w - 1 - t, c);
    }
}

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: paneltest <image> [out.ppm]\n"); return 2; }
    const uint8_t bg[3] = { 0x11, 0x12, 0x16 };

    // Decode the way the reader does, or the panels measured here are not the
    // panels it will find: page.h caps a page at PAGE_MAX_DIM and otherwise
    // leaves it at native size.
    int cap = 0, pw = 0, ph = 0;
    if (image_probe(argv[1], &pw, &ph) && (pw > 3000 || ph > 3000)) cap = 3000;

    Image im;
    if (!image_load_fit(argv[1], bg, cap, cap, &im)) {
        fprintf(stderr, "paneltest: cannot decode %s\n", argv[1]);
        return 1;
    }

    int aw = 0, ah = 0;
    uint8_t *g = panel_gray(im.rgb, im.w, im.h, &aw, &ah);
    int bg_border = g ? panel_bg_border(g, aw, ah) : -1;
    int bg_flat = g ? panel_bg_flat(g, aw, ah, bg_border) : -1;
    free(g);

    Panel panels[64];
    int n = panel_detect(im.rgb, im.w, im.h, panels, 64);
    printf("%s  %dx%d  %d panels  bg=%d (flat=%d border=%d)\n",
           argv[1], im.w, im.h, n, bg_flat >= 0 ? bg_flat : bg_border,
           bg_flat, bg_border);

    for (int i = 0; i < n; i++) {
        printf("  %2d  x=%-5d y=%-5d w=%-5d h=%-5d\n",
               i + 1, panels[i].x, panels[i].y, panels[i].w, panels[i].h);

        // Cycle through a few saturated colours so adjacent panels never share
        // one, and mark the ordinal with ticks inside the top edge.
        static const uint8_t pal[6][3] = {
            {0xff,0x30,0x30}, {0x30,0xd0,0xff}, {0x40,0xff,0x60},
            {0xff,0xd0,0x20}, {0xff,0x40,0xff}, {0xff,0xff,0xff},
        };
        const uint8_t *c = pal[i % 6];
        box(im.rgb, im.w, im.h, panels[i], c, 4);

        int tx = panels[i].x + 10, ty = panels[i].y + 12;
        for (int t = 0; t <= i && t < 40; t++) {
            Panel tick = { tx + t * 10, ty, 6, 16 };
            for (int y = 0; y < tick.h; y++)
                hline(im.rgb, im.w, im.h, tick.x, tick.x + tick.w - 1, tick.y + y, c);
        }
    }

    const char *out = argc > 2 ? argv[2] : "panels.ppm";
    FILE *f = fopen(out, "wb");
    if (!f) { perror(out); return 1; }
    fprintf(f, "P6\n%d %d\n255\n", im.w, im.h);
    fwrite(im.rgb, 1, (size_t)im.w * im.h * 3, f);
    fclose(f);
    fprintf(stderr, "wrote %s\n", out);

    image_free(&im);
    return 0;
}
