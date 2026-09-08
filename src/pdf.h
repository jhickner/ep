#ifndef PDF_H
#define PDF_H

#include <stdbool.h>
#include <stdint.h>

typedef struct PdfDoc PdfDoc;

PdfDoc *pdf_open(const char *path);
void    pdf_close(PdfDoc *d);
int     pdf_pages(const PdfDoc *d);

bool pdf_page_size(PdfDoc *d, int page, double *w, double *h);

bool pdf_render(PdfDoc *d, int page, double scale, int off_x, int off_y,
                int out_w, int out_h, uint8_t *rgba);

#endif

#ifdef PDF_IMPLEMENTATION

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(__APPLE__) && !defined(PDF_FORCE_TOOLS)

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

#else

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <unistd.h>
#include <sys/wait.h>

#ifndef IMAGE_H
#include "image.h"
#endif

typedef enum { PDF_NONE = 0, PDF_POPPLER, PDF_MUPDF, PDF_GS } PdfTool;

struct PdfDoc {
    char    path[PATH_MAX];
    PdfTool tool;
    int     pages;
    double  w, h;
};

static bool pdf_run(char *const argv[], char *out, size_t cap) {
    int fd[2] = { -1, -1 };
    if (out && pipe(fd) != 0) return false;

    pid_t pid = fork();
    if (pid < 0) {
        if (out) { close(fd[0]); close(fd[1]); }
        return false;
    }
    if (pid == 0) {
        int null = open("/dev/null", O_RDWR);
        if (out) { dup2(fd[1], 1); close(fd[0]); close(fd[1]); }
        else if (null >= 0) dup2(null, 1);
        if (null >= 0) { dup2(null, 2); close(null); }
        execvp(argv[0], argv);
        _exit(127);
    }

    if (out) {
        close(fd[1]);
        size_t n = 0;
        ssize_t got;
        while (n + 1 < cap && (got = read(fd[0], out + n, cap - 1 - n)) > 0)
            n += (size_t)got;
        out[n] = '\0';
        close(fd[0]);
    }

    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

static bool pdf_field(const char *text, const char *key, double *num) {
    size_t klen = strlen(key);
    for (const char *p = text; p && *p; ) {
        if (!strncmp(p, key, klen)) {
            const char *v = p + klen;
            while (*v == ' ' || *v == '\t' || *v == ':') v++;
            *num = atof(v);
            return true;
        }
        p = strchr(p, '\n');
        if (p) p++;
    }
    return false;
}

static void pdf_tmp(const PdfDoc *d, char *base, size_t cap) {
    const char *tmp = getenv("TMPDIR");
    if (!tmp || !*tmp) tmp = "/tmp";
    snprintf(base, cap, "%s/ep-pdf-%d-%p", tmp, (int)getpid(), (const void *)d);
}

static bool pdf_to_png(const PdfDoc *d, int page, int dpi,
                       const char *base, char *png, size_t cap) {
    char r[32], first[32], last[32], out[PATH_MAX];
    snprintf(r, sizeof r, "%d", dpi);
    snprintf(first, sizeof first, "-dFirstPage=%d", page + 1);
    snprintf(last, sizeof last, "-dLastPage=%d", page + 1);

    if (d->tool == PDF_POPPLER) {
        char f[32], l[32];
        snprintf(f, sizeof f, "%d", page + 1);
        snprintf(l, sizeof l, "%d", page + 1);
        char *argv[] = { (char *)"pdftoppm", (char *)"-png", (char *)"-r", r,
                         (char *)"-f", f, (char *)"-l", l,
                         (char *)"-singlefile", (char *)d->path,
                         (char *)base, NULL };
        if (!pdf_run(argv, NULL, 0)) return false;
        snprintf(png, cap, "%s.png", base);
        return true;
    }

    snprintf(out, sizeof out, "%s.png", base);
    if (d->tool == PDF_MUPDF) {
        char n[32];
        snprintf(n, sizeof n, "%d", page + 1);
        char *argv[] = { (char *)"mutool", (char *)"draw",
                         (char *)"-o", out, (char *)"-r", r,
                         (char *)d->path, n, NULL };
        if (!pdf_run(argv, NULL, 0)) return false;
        snprintf(png, cap, "%s", out);
        return true;
    }

    if (d->tool == PDF_GS) {
        char sout[PATH_MAX + 16], sr[40];
        snprintf(sout, sizeof sout, "-sOutputFile=%s", out);
        snprintf(sr, sizeof sr, "-r%d", dpi);
        char *argv[] = { (char *)"gs", (char *)"-q", (char *)"-dBATCH",
                         (char *)"-dNOPAUSE", (char *)"-dSAFER",
                         (char *)"-sDEVICE=png16m", sr, first, last, sout,
                         (char *)"--", (char *)d->path, NULL };
        if (!pdf_run(argv, NULL, 0)) return false;
        snprintf(png, cap, "%s", out);
        return true;
    }
    return false;
}

static bool pdf_measure(PdfDoc *d) {
    char base[PATH_MAX], png[PATH_MAX];
    pdf_tmp(d, base, sizeof base);
    if (!pdf_to_png(d, 0, 72, base, png, sizeof png)) return false;
    int w = 0, h = 0;
    bool ok = image_probe(png, &w, &h) && w > 0 && h > 0;
    unlink(png);
    if (ok) { d->w = w; d->h = h; }
    return ok;
}

static bool pdf_gs_count_cmd(const char *path, char *out, size_t cap) {
    char esc[PATH_MAX * 2];
    size_t n = 0;
    for (const char *p = path; *p; p++) {
        if (n + 2 >= sizeof esc) return false;
        if (*p == '(' || *p == ')' || *p == '\\') esc[n++] = '\\';
        esc[n++] = *p;
    }
    esc[n] = '\0';
    return (size_t)snprintf(out, cap,
        "(%s) (r) file runpdfbegin pdfpagecount = quit", esc) < cap;
}

PdfDoc *pdf_open(const char *path) {
    PdfDoc *d = calloc(1, sizeof *d);
    if (!d) return NULL;
    if ((size_t)snprintf(d->path, sizeof d->path, "%s", path) >= sizeof d->path) {
        free(d);
        return NULL;
    }

    char buf[8192];
    double n = 0;

    char *info[] = { (char *)"pdfinfo", d->path, NULL };
    if (pdf_run(info, buf, sizeof buf) && pdf_field(buf, "Pages:", &n) && n > 0) {
        d->tool  = PDF_POPPLER;
        d->pages = (int)n;
        const char *sz = strstr(buf, "Page size:");
        if (sz) sscanf(sz + 10, "%lf x %lf", &d->w, &d->h);
    }

    if (!d->tool) {
        char *mi[] = { (char *)"mutool", (char *)"info", d->path, NULL };
        if (pdf_run(mi, buf, sizeof buf) && pdf_field(buf, "Pages:", &n) && n > 0) {
            d->tool  = PDF_MUPDF;
            d->pages = (int)n;
        }
    }

    if (!d->tool) {
        char ps[PATH_MAX * 2 + 64];
        if (pdf_gs_count_cmd(d->path, ps, sizeof ps)) {
            char *gs[] = { (char *)"gs", (char *)"-q", (char *)"-dNODISPLAY",
                           (char *)"-dBATCH", (char *)"-dNOSAFER",
                           (char *)"-c", ps, NULL };
            if (pdf_run(gs, buf, sizeof buf)) {
                int pages = atoi(buf);
                if (pages > 0) { d->tool = PDF_GS; d->pages = pages; }
            }
        }
    }

    if (!d->tool) { free(d); return NULL; }
    if ((d->w <= 0 || d->h <= 0) && !pdf_measure(d)) { free(d); return NULL; }
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

    char base[PATH_MAX], png[PATH_MAX];
    pdf_tmp(d, base, sizeof base);
    if (!pdf_to_png(d, page, (int)(scale * 72.0 + 0.5), base, png, sizeof png))
        return false;

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

#endif

#endif
