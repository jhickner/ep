// Renders a page window to a PPM, for checking pdf.h's transforms by eye.
//   pdfshot file.pdf page scale off_x off_y w h out.ppm
#include <stdio.h>
#include <stdlib.h>
#include "../src/pdf.h"

int main(int argc, char **argv) {
    if (argc < 9) { fprintf(stderr, "usage: pdfshot f.pdf page scale ox oy w h out.ppm\n"); return 2; }
    PdfDoc *d = pdf_open(argv[1]);
    if (!d) { fprintf(stderr, "open failed\n"); return 1; }
    int page = atoi(argv[2]);
    double scale = atof(argv[3]);
    int ox = atoi(argv[4]), oy = atoi(argv[5]), w = atoi(argv[6]), h = atoi(argv[7]);
    double pw, ph;
    pdf_page_size(d, page, &pw, &ph);
    fprintf(stderr, "pages=%d page size=%.1fx%.1f pts\n", pdf_pages(d), pw, ph);

    uint8_t *px = malloc((size_t)w * h * 4);
    if (!pdf_render(d, page, scale, ox, oy, w, h, px)) { fprintf(stderr, "render failed\n"); return 1; }
    FILE *f = fopen(argv[8], "wb");
    fprintf(f, "P6\n%d %d\n255\n", w, h);
    for (int i = 0; i < w * h; i++) fwrite(px + i * 4, 1, 3, f);
    fclose(f);
    pdf_close(d);
    return 0;
}
