#ifndef PDF_H
#define PDF_H

/**
 * pdf.h - rasterize PDF pages into RGBA windows.
 *
 * On macOS this is CoreGraphics, so there is no subprocess and no temp file.
 * Elsewhere it shells out to poppler's pdftoppm and crops what came back.
 *
 * Rendering is windowed on purpose: the caller asks for the pixels it is about
 * to show, at the scale it will show them, so a page zoomed past the window
 * costs the window rather than the page.
 */

#include <stdbool.h>
#include <stdint.h>

typedef struct PdfDoc PdfDoc;

PdfDoc *pdf_open(const char *path);
void    pdf_close(PdfDoc *d);
int     pdf_pages(const PdfDoc *d);

/* Size of a page in points, with its rotation applied. */
bool pdf_page_size(PdfDoc *d, int page, double *w, double *h);

/* Draw page `page` (0-based) at `scale` pixels per point into an out_w x out_h
   RGBA buffer, taking the window whose top-left corner is (off_x, off_y) pixels
   from the page's top-left. Anything outside the page comes back white. */
bool pdf_render(PdfDoc *d, int page, double scale, int off_x, int off_y,
                int out_w, int out_h, uint8_t *rgba);

#endif /* PDF_H */

#ifdef PDF_IMPLEMENTATION

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef __APPLE__

#include <CoreFoundation/CoreFoundation.h>
#include <CoreGraphics/CoreGraphics.h>

struct PdfDoc {
    CGPDFDocumentRef doc;
    int pages;
};

PdfDoc *pdf_open(const char *path) {
    CFStringRef s = CFStringCreateWithCString(NULL, path, kCFStringEncodingUTF8);
    if (!s) return NULL;
    CFURLRef url = CFURLCreateWithFileSystemPath(NULL, s, kCFURLPOSIXPathStyle, false);
    CFRelease(s);
    if (!url) return NULL;
    CGPDFDocumentRef doc = CGPDFDocumentCreateWithURL(url);
    CFRelease(url);
    if (!doc) return NULL;
    if (CGPDFDocumentIsEncrypted(doc) && !CGPDFDocumentUnlockWithPassword(doc, "")) {
        CGPDFDocumentRelease(doc);
        return NULL;
    }
    PdfDoc *d = calloc(1, sizeof *d);
    if (!d) { CGPDFDocumentRelease(doc); return NULL; }
    d->doc = doc;
    d->pages = (int)CGPDFDocumentGetNumberOfPages(doc);
    return d->pages > 0 ? d : (free(d), CGPDFDocumentRelease(doc), NULL);
}

void pdf_close(PdfDoc *d) {
    if (!d) return;
    CGPDFDocumentRelease(d->doc);
    free(d);
}

int pdf_pages(const PdfDoc *d) { return d ? d->pages : 0; }

/* The crop box is what a viewer shows; the media box usually matches it but
   includes printer furniture when it does not. */
static CGPDFPageRef pdf_page(PdfDoc *d, int page) {
    if (!d || page < 0 || page >= d->pages) return NULL;
    return CGPDFDocumentGetPage(d->doc, (size_t)page + 1);
}

bool pdf_page_size(PdfDoc *d, int page, double *w, double *h) {
    CGPDFPageRef pg = pdf_page(d, page);
    if (!pg) return false;
    CGRect box = CGPDFPageGetBoxRect(pg, kCGPDFCropBox);
    if (CGRectIsEmpty(box)) box = CGPDFPageGetBoxRect(pg, kCGPDFMediaBox);
    double pw = box.size.width, ph = box.size.height;
    int rot = CGPDFPageGetRotationAngle(pg);
    if (((rot % 360) + 360) % 360 % 180 == 90) { double t = pw; pw = ph; ph = t; }
    *w = pw; *h = ph;
    return pw > 0 && ph > 0;
}

bool pdf_render(PdfDoc *d, int page, double scale, int off_x, int off_y,
                int out_w, int out_h, uint8_t *rgba) {
    CGPDFPageRef pg = pdf_page(d, page);
    if (!pg || out_w <= 0 || out_h <= 0) return false;
    double pw, ph;
    if (!pdf_page_size(d, page, &pw, &ph)) return false;

    CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
    CGContextRef ctx = CGBitmapContextCreate(rgba, (size_t)out_w, (size_t)out_h, 8,
                                             (size_t)out_w * 4, cs,
                                             kCGImageAlphaPremultipliedLast |
                                             kCGBitmapByteOrder32Big);
    CGColorSpaceRelease(cs);
    if (!ctx) return false;

    CGContextSetRGBFillColor(ctx, 1, 1, 1, 1);
    CGContextFillRect(ctx, CGRectMake(0, 0, out_w, out_h));
    CGContextSetInterpolationQuality(ctx, kCGInterpolationHigh);
    CGContextSetShouldAntialias(ctx, true);

    /* Walk from the bitmap's bottom-left origin into PDF page space: flip to a
       top-left origin, slide the window, scale points to pixels, then flip once
       more because the page itself measures up from its bottom-left. */
    CGContextTranslateCTM(ctx, 0, out_h);
    CGContextScaleCTM(ctx, 1, -1);
    CGContextTranslateCTM(ctx, -off_x, -off_y);
    CGContextScaleCTM(ctx, scale, scale);
    CGContextTranslateCTM(ctx, 0, ph);
    CGContextScaleCTM(ctx, 1, -1);
    CGContextConcatCTM(ctx, CGPDFPageGetDrawingTransform(pg, kCGPDFCropBox,
                                                         CGRectMake(0, 0, pw, ph),
                                                         0, true));
    CGContextDrawPDFPage(ctx, pg);
    CGContextRelease(ctx);
    return true;
}

#else  /* poppler */

#include <unistd.h>

#ifndef IMAGE_H
#include "image.h"
#endif

struct PdfDoc {
    char path[4096];
    int  pages;
    double w, h;
};

static int pdf_run_int(const char *cmd, const char *key, double *num) {
    FILE *f = popen(cmd, "r");
    if (!f) return 0;
    char line[512];
    int got = 0;
    while (fgets(line, sizeof line, f)) {
        if (strncmp(line, key, strlen(key)) != 0) continue;
        const char *p = line + strlen(key);
        while (*p == ' ' || *p == ':') p++;
        *num = atof(p);
        got = 1;
        break;
    }
    pclose(f);
    return got;
}

PdfDoc *pdf_open(const char *path) {
    PdfDoc *d = calloc(1, sizeof *d);
    if (!d) return NULL;
    snprintf(d->path, sizeof d->path, "%s", path);

    char cmd[8192];
    snprintf(cmd, sizeof cmd, "pdfinfo '%s' 2>/dev/null", path);
    double n = 0, w = 0;
    if (!pdf_run_int(cmd, "Pages", &n)) { free(d); return NULL; }
    d->pages = (int)n;
    if (pdf_run_int(cmd, "Page size", &w)) d->w = w;
    /* "Page size: 612 x 792 pts" - the height needs a second look. */
    FILE *f = popen(cmd, "r");
    if (f) {
        char line[512];
        while (fgets(line, sizeof line, f))
            if (!strncmp(line, "Page size:", 10)) {
                double a, b;
                if (sscanf(line + 10, "%lf x %lf", &a, &b) == 2) { d->w = a; d->h = b; }
                break;
            }
        pclose(f);
    }
    if (d->pages <= 0 || d->w <= 0 || d->h <= 0) { free(d); return NULL; }
    return d;
}

void pdf_close(PdfDoc *d) { free(d); }
int  pdf_pages(const PdfDoc *d) { return d ? d->pages : 0; }

bool pdf_page_size(PdfDoc *d, int page, double *w, double *h) {
    (void)page;
    if (!d) return false;
    *w = d->w; *h = d->h;
    return true;
}

bool pdf_render(PdfDoc *d, int page, double scale, int off_x, int off_y,
                int out_w, int out_h, uint8_t *rgba) {
    if (!d || page < 0 || page >= d->pages) return false;

    char base[4096], png[4096], cmd[9000];
    snprintf(base, sizeof base, "/tmp/ep-pdf-%d", (int)getpid());
    snprintf(cmd, sizeof cmd,
             "pdftoppm -png -r %d -f %d -l %d -singlefile '%s' '%s' 2>/dev/null",
             (int)(scale * 72.0 + 0.5), page + 1, page + 1, d->path, base);
    if (system(cmd) != 0) return false;
    snprintf(png, sizeof png, "%s.png", base);

    static const uint8_t white[3] = { 255, 255, 255 };
    Image im = {0};
    bool ok = image_load_fit(png, white, 0, 0, &im);
    unlink(png);
    if (!ok) return false;

    for (int y = 0; y < out_h; y++) {
        for (int x = 0; x < out_w; x++) {
            int sx = x + off_x, sy = y + off_y;
            uint8_t *o = rgba + ((size_t)y * out_w + x) * 4;
            if (sx < 0 || sy < 0 || sx >= im.w || sy >= im.h) {
                o[0] = o[1] = o[2] = o[3] = 255;
            } else {
                const uint8_t *s = im.rgb + ((size_t)sy * im.w + sx) * 3;
                o[0] = s[0]; o[1] = s[1]; o[2] = s[2]; o[3] = 255;
            }
        }
    }
    image_free(&im);
    return true;
}

#endif /* __APPLE__ */

#endif /* PDF_IMPLEMENTATION */
