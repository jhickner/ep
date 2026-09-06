// Renders typeset pages of an epub chapter to PPMs, for checking type.h by eye.
//   typeshot book.epub spine npages w h size out-prefix
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ZIP_IMPLEMENTATION
#include "../src/zip.h"
#define XML_IMPLEMENTATION
#include "../src/xml.h"
#define EPUB_IMPLEMENTATION
#include "../src/epub.h"
#define DOC_IMPLEMENTATION
#include "../src/doc.h"
#include "../src/type.h"

int main(int argc, char **argv) {
    if (argc < 8) {
        fprintf(stderr, "usage: typeshot book.epub spine npages w h size out-prefix\n");
        return 2;
    }
    int spine = atoi(argv[2]), npages = atoi(argv[3]);
    int w = atoi(argv[4]), h = atoi(argv[5]);
    double size = atof(argv[6]);
    const char *prefix = argv[7];

    Epub bk;
    if (!epub_open(&bk, argv[1])) { fprintf(stderr, "open failed\n"); return 1; }
    if (spine >= bk.nspine) spine = bk.nspine - 1;

    char *xhtml = epub_read(&bk, bk.spine[spine], NULL);
    if (!xhtml) { fprintf(stderr, "chapter read failed\n"); return 1; }
    char base[512];
    snprintf(base, sizeof base, "%s", bk.spine[spine]);
    char *slash = strrchr(base, '/');
    if (slash) slash[1] = 0; else base[0] = 0;
    Doc d = doc_parse(xhtml, base);
    fprintf(stderr, "spine %d/%d (%s): %d blocks\n", spine, bk.nspine, bk.spine[spine], d.nblocks);

    TypeStyle st;
    type_style_default(&st);
    if (getenv("EP_FONT")) st.family = getenv("EP_FONT");
    st.size = size;
    st.margin = size * 2.6;
    if (getenv("EP_NOJUSTIFY")) st.justify = false;
    if (getenv("EP_NOHYPHEN")) st.hyphenation = 0;
    if (getenv("EP_LEADING")) st.leading = atof(getenv("EP_LEADING"));
    if (getenv("EP_COLUMNS")) st.columns = atoi(getenv("EP_COLUMNS"));
    if (getenv("EP_MEASURE")) st.measure = atof(getenv("EP_MEASURE"));
    unsigned rr, gg, bb;
    if (getenv("EP_PAPER") && sscanf(getenv("EP_PAPER"), "%2x%2x%2x", &rr, &gg, &bb) == 3) {
        st.paper[0] = rr; st.paper[1] = gg; st.paper[2] = bb;
    }
    if (getenv("EP_INK") && sscanf(getenv("EP_INK"), "%2x%2x%2x", &rr, &gg, &bb) == 3) {
        st.ink[0] = rr; st.ink[1] = gg; st.ink[2] = bb;
    }

    if (getenv("EP_TRANSPARENT")) st.transparent = true;
    static char initials[512];
    snprintf(initials, sizeof initials, "%s/.config/ep/initials", getenv("HOME"));
    st.initials = getenv("EP_NOINITIALS") ? NULL : initials;

    TypeChapter *tc = type_open(&d, &st);
    int total = type_paginate(tc, w, h);
    fprintf(stderr, "%d pages\n", total);

    uint8_t *px = malloc((size_t)w * h * 4);
    for (int i = 0; i < npages && i < total; i++) {
        if (!type_draw(tc, i, px)) { fprintf(stderr, "draw failed\n"); return 1; }
        char out[512];
        snprintf(out, sizeof out, "%s%d.ppm", prefix, i);
        // PPM has no alpha, so a transparent page is composited over the
        // colour a terminal would show behind it (EP_OVER, default dark).
        unsigned o0 = 0x1d, o1 = 0x1f, o2 = 0x21;
        if (getenv("EP_OVER")) sscanf(getenv("EP_OVER"), "%2x%2x%2x", &o0, &o1, &o2);
        long clear = 0;
        FILE *f = fopen(out, "wb");
        fprintf(f, "P6\n%d %d\n255\n", w, h);
        for (int j = 0; j < w * h; j++) {
            uint8_t rgb[3];
            int a = px[j * 4 + 3];
            unsigned over[3] = { o0, o1, o2 };
            if (a == 0) clear++;
            for (int c = 0; c < 3; c++)
                rgb[c] = st.transparent
                       ? (uint8_t)((px[j * 4 + c] * a + over[c] * (255 - a)) / 255)
                       : px[j * 4 + c];
            fwrite(rgb, 1, 3, f);
        }
        fclose(f);
        if (st.transparent)
            fprintf(stderr, "page %d: %.1f%% fully clear\n", i, 100.0 * clear / (w * h));
    }

    // every page start must name the page it came from
    for (int i = 0; i < total; i++) {
        int block, off;
        type_page_start(tc, i, &block, &off);
        int back = type_page_of(tc, block, off);
        if (back != i)
            fprintf(stderr, "page %d start (%d,%d) resolves to page %d\n", i, block, off, back);
    }
    fprintf(stderr, "round trip checked\n");
    type_close(tc);
    return 0;
}
