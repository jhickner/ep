/**
 * image.h - decode, orient, and resample images to RGB (single-header)
 *
 * In exactly ONE .c file:
 *
 *     #define IMAGE_IMPLEMENTATION
 *     #include "image.h"
 *
 * stb_image.h must sit next to this header. Build with -DPIX_HAVE_JPEG and
 * -ljpeg to route JPEGs through libjpeg-turbo, which can decode them at a
 * fraction of their stored size - far and away the cheapest way to make
 * thumbnails. Formats no decoder here reads (HEIC, WebP, ...) fall back to
 * converting via `sips` on macOS.
 *
 * Everything here is pure and reentrant, so worker threads can call it.
 */

#ifndef IMAGE_H
#define IMAGE_H

#include <stdint.h>
#include <stdbool.h>

typedef struct {
    uint8_t *rgb;        // w*h*3, malloc'd, caller frees
    int w, h;            // size of the buffer above
    int src_w, src_h;    // size on disk, after EXIF orientation
    bool provisional;    // came from the embedded thumbnail; see IMAGE_NO_EXIF_THUMB
} Image;

// Refuse the embedded EXIF thumbnail and decode the real image.
//
// An embedded thumbnail is orders of magnitude cheaper than any decode, but it
// is only a claim about the file's contents, and an editor that rewrites the
// pixels without regenerating it leaves that claim false - the browser would
// then show the image as it used to look, with nothing to indicate it. Callers
// that can afford to be slow and must be right pass this; callers that want it
// fast check `provisional` and come back for the real thing.
#define IMAGE_NO_EXIF_THUMB 1

// Decode `path` scaled to fit inside box_w x box_h, compositing any alpha onto
// `bg` and applying EXIF orientation. Pass box <= 0 for full resolution.
//
// The scaling is not an afterthought: JPEGs are decoded straight out of the DCT
// at the smallest 1/8th step that still covers the box, and an embedded EXIF
// thumbnail is used outright when it is large enough. A grid thumbnail
// therefore never materializes the full-resolution image.
//
// Returns false and leaves *out zeroed on failure.
bool image_load_fit(const char *path, const uint8_t bg[3], int box_w, int box_h,
                    Image *out);

// As above, with the flags documented against IMAGE_NO_EXIF_THUMB.
bool image_load_fit_ex(const char *path, const uint8_t bg[3], int box_w, int box_h,
                       int flags, Image *out);

// Consulted at checkpoints during a decode; returning true abandons it, and
// image_load_fit_cancel() then returns false with *out zeroed. Called often
// enough to matter on a slow file and rarely enough to be free on a fast one,
// so it should be cheap.
typedef bool (*ImageAbortFn)(void *ctx);

// As image_load_fit_ex, but abandons the work as soon as `abort_fn` says so.
// Worth using wherever the reason for decoding can disappear - a thumbnail
// whose row has scrolled off is not worth the megapixels still to come.
bool image_load_fit_cancel(const char *path, const uint8_t bg[3], int box_w, int box_h,
                           int flags, ImageAbortFn abort_fn, void *ctx, Image *out);

// Read just the pixel dimensions, without decoding. Cheap.
bool image_probe(const char *path, int *w, int *h);

// Largest w*h fitting inside box_w x box_h with the aspect of src_w x src_h.
void image_fit(int src_w, int src_h, int box_w, int box_h, int *out_w, int *out_h);

// Resample `src` to exactly dw x dh. Box-averages when shrinking, bilinear
// when enlarging. Returns a malloc'd w*h*3 buffer, or NULL on allocation
// failure.
uint8_t *image_scale(const uint8_t *src, int sw, int sh, int dw, int dh);

void image_free(Image *im);

#endif // IMAGE_H

/* ======================================================================== */
/* Implementation                                                           */
/* ======================================================================== */
#ifdef IMAGE_IMPLEMENTATION

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/wait.h>
#include <signal.h>
#include <time.h>

#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_STDIO_WRITE
#include "stb_image.h"

void image_free(Image *im) {
    if (!im) return;
    free(im->rgb);
    im->rgb = NULL;
    im->w = im->h = 0;
    im->src_w = im->src_h = 0;
}

void image_fit(int src_w, int src_h, int box_w, int box_h, int *out_w, int *out_h) {
    if (src_w <= 0 || src_h <= 0 || box_w <= 0 || box_h <= 0) {
        *out_w = *out_h = 0;
        return;
    }
    // Compare src_w/src_h against box_w/box_h without floating point.
    if ((int64_t)src_w * box_h > (int64_t)box_w * src_h) {
        *out_w = box_w;
        *out_h = (int)(((int64_t)src_h * box_w + src_w / 2) / src_w);
    } else {
        *out_h = box_h;
        *out_w = (int)(((int64_t)src_w * box_h + src_h / 2) / src_h);
    }
    if (*out_w < 1) *out_w = 1;
    if (*out_h < 1) *out_h = 1;
}

/* ---------------------------------------------------------------- EXIF -- */

static uint16_t rd16(const uint8_t *p, bool le) {
    return le ? (uint16_t)(p[0] | p[1] << 8) : (uint16_t)(p[1] | p[0] << 8);
}
static uint32_t rd32(const uint8_t *p, bool le) {
    return le ? ((uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24)
              : ((uint32_t)p[3] | (uint32_t)p[2] << 8 | (uint32_t)p[1] << 16 | (uint32_t)p[0] << 24);
}

typedef struct {
    bool is_jpeg;       // starts with SOI, so libjpeg is worth a try
    int orient;         // 1-8; 1 when absent or unparseable
    uint8_t *thumb;     // malloc'd JPEG bytes of the embedded thumbnail, or NULL
    size_t thumb_len;
} Exif;

static void exif_free(Exif *e) {
    free(e->thumb);
    e->thumb = NULL;
    e->thumb_len = 0;
}

// Walk one IFD looking for `tag`, returning its first SHORT/LONG value.
// `base` points at the TIFF header, `avail` bounds it.
static bool ifd_find(const uint8_t *base, uint32_t avail, uint32_t ifd, bool le,
                     uint16_t tag, uint32_t *value) {
    if (ifd + 2 > avail) return false;
    uint16_t count = rd16(base + ifd, le);
    for (uint16_t i = 0; i < count; i++) {
        uint32_t e = ifd + 2 + (uint32_t)i * 12;
        if (e + 12 > avail) break;
        if (rd16(base + e, le) != tag) continue;
        uint16_t type = rd16(base + e + 2, le);
        *value = (type == 3) ? rd16(base + e + 8, le) : rd32(base + e + 8, le);
        return true;
    }
    return false;
}

// Offset of the next IFD, or 0. Follows the 4 bytes after the entry array.
static uint32_t ifd_next(const uint8_t *base, uint32_t avail, uint32_t ifd, bool le) {
    if (ifd + 2 > avail) return 0;
    uint32_t off = ifd + 2 + (uint32_t)rd16(base + ifd, le) * 12;
    if (off + 4 > avail) return 0;
    return rd32(base + off, le);
}

// Read a JPEG's APP1/EXIF block: the orientation tag from IFD0, and the
// embedded thumbnail (IFD1's tags 0x0201/0x0202) copied out for later decoding.
// Leaves *out zeroed-but-valid for anything that isn't a JPEG with EXIF.
static void exif_read(const char *path, Exif *out) {
    memset(out, 0, sizeof *out);
    out->orient = 1;

    FILE *f = fopen(path, "rb");
    if (!f) return;

    uint8_t *seg = NULL;
    uint8_t sig[2];
    if (fread(sig, 1, 2, f) != 2 || sig[0] != 0xFF || sig[1] != 0xD8) goto done;
    out->is_jpeg = true;

    for (;;) {
        int c = fgetc(f);
        if (c == EOF) goto done;
        if (c != 0xFF) continue;
        int marker;
        do { marker = fgetc(f); } while (marker == 0xFF);
        if (marker == EOF || marker == 0xD9 || marker == 0xDA) goto done;  // EOI / start of scan
        if (marker == 0xD8 || (marker >= 0xD0 && marker <= 0xD7)) continue; // no payload

        uint8_t lenb[2];
        if (fread(lenb, 1, 2, f) != 2) goto done;
        long seglen = (lenb[0] << 8 | lenb[1]) - 2;
        if (seglen < 0) goto done;

        if (marker != 0xE1 || seglen < 14) { fseek(f, seglen, SEEK_CUR); continue; }

        long n = seglen > 65536 ? 65536 : seglen;
        seg = (uint8_t *)malloc((size_t)n);
        if (!seg) goto done;
        if ((long)fread(seg, 1, (size_t)n, f) != n) goto done;
        if (memcmp(seg, "Exif\0\0", 6) != 0) goto done;

        const uint8_t *tiff = seg + 6;
        uint32_t avail = (uint32_t)(n - 6);
        if (avail <= 8) goto done;
        bool le = tiff[0] == 'I';
        if ((tiff[0] != 'I' && tiff[0] != 'M') || rd16(tiff + 2, le) != 42) goto done;

        uint32_t ifd0 = rd32(tiff + 4, le);
        uint32_t v;
        if (ifd_find(tiff, avail, ifd0, le, 0x0112, &v) && v >= 1 && v <= 8)
            out->orient = (int)v;

        // IFD1 describes the thumbnail. Its offset/length are relative to the
        // TIFF header, so both have to land inside the segment we read.
        uint32_t ifd1 = ifd_next(tiff, avail, ifd0, le);
        uint32_t toff, tlen;
        if (ifd1 && ifd_find(tiff, avail, ifd1, le, 0x0201, &toff) &&
            ifd_find(tiff, avail, ifd1, le, 0x0202, &tlen) &&
            tlen > 4 && toff <= avail && tlen <= avail - toff &&
            tiff[toff] == 0xFF && tiff[toff + 1] == 0xD8) {
            out->thumb = (uint8_t *)malloc(tlen);
            if (out->thumb) {
                memcpy(out->thumb, tiff + toff, tlen);
                out->thumb_len = tlen;
            }
        }
        goto done;
    }
done:
    free(seg);
    fclose(f);
}

// Apply an EXIF orientation (1-8) in place, swapping *w/*h for the transposed
// cases. Returns the new buffer (caller's pointer is freed on success).
//
// Always called on the already-scaled image, never the decoded source: the
// transform is an index permutation, so doing it last costs output pixels
// rather than the megapixels a camera JPEG arrives with.
static uint8_t *apply_orientation(uint8_t *src, int *w, int *h, int orient) {
    if (orient <= 1 || orient > 8) return src;

    int sw = *w, sh = *h;
    bool swap = (orient >= 5);
    int dw = swap ? sh : sw, dh = swap ? sw : sh;

    uint8_t *dst = (uint8_t *)malloc((size_t)dw * dh * 3);
    if (!dst) return src;

    for (int y = 0; y < sh; y++) {
        for (int x = 0; x < sw; x++) {
            int nx, ny;
            switch (orient) {
                case 2: nx = sw - 1 - x; ny = y;            break;  // mirror
                case 3: nx = sw - 1 - x; ny = sh - 1 - y;   break;  // 180
                case 4: nx = x;          ny = sh - 1 - y;   break;  // flip
                case 5: nx = y;          ny = x;            break;  // transpose
                case 6: nx = sh - 1 - y; ny = x;            break;  // 90 cw
                case 7: nx = sh - 1 - y; ny = sw - 1 - x;   break;  // transverse
                case 8: nx = y;          ny = sw - 1 - x;   break;  // 270 cw
                default: nx = x;         ny = y;            break;
            }
            memcpy(dst + ((size_t)ny * dw + nx) * 3, src + ((size_t)y * sw + x) * 3, 3);
        }
    }
    free(src);
    *w = dw;
    *h = dh;
    return dst;
}

/* ---------------------------------------------------------------- JPEG -- */
#ifdef PIX_HAVE_JPEG

#include <setjmp.h>
#include <jpeglib.h>

// libjpeg's default error handler calls exit(). Replace it with a longjmp back
// into the decode function, which owns the cleanup.
typedef struct {
    struct jpeg_error_mgr pub;
    jmp_buf jb;
} JpegErr;

static void jpeg_err_exit(j_common_ptr ci) {
    longjmp(((JpegErr *)ci->err)->jb, 1);
}
static void jpeg_err_silent(j_common_ptr ci) { (void)ci; }

// libjpeg calls this every MCU row or so, including during the coefficient
// passes of a progressive image. Those happen inside jpeg_start_decompress,
// before a single scanline exists, so for a progressive JPEG this is the only
// checkpoint that arrives while there is still work worth abandoning.
typedef struct {
    struct jpeg_progress_mgr pub;
    ImageAbortFn abort_fn;
    void *ctx;
    jmp_buf *jb;
} JpegProgress;

static void jpeg_progress_check(j_common_ptr ci) {
    JpegProgress *p = (JpegProgress *)ci->progress;
    if (p->abort_fn && p->abort_fn(p->ctx)) longjmp(*p->jb, 1);
}

// Decode a JPEG - from `path`, or from `mem` when that is non-NULL - at the
// coarsest 1/8th DCT step whose output still covers need_w x need_h. Passing
// need <= 0 decodes at full size.
//
// This is the whole point of linking libjpeg: the IDCT can emit an N/8-scale
// image directly, so a 4032x3024 photo destined for a 150px thumbnail is
// decoded as 504x378 - a 64x reduction in both work and peak memory - and never
// exists at full resolution.
//
// Returns malloc'd RGB (3 bytes per pixel) at *w x *h, with the file's own
// dimensions in *full_w x *full_h.
static uint8_t *jpeg_decode(const char *path, const uint8_t *mem, size_t memlen,
                            int need_w, int need_h,
                            int *w, int *h, int *full_w, int *full_h,
                            ImageAbortFn abort_fn, void *ctx) {
    struct jpeg_decompress_struct ci;
    JpegErr je;
    // Touched on both sides of setjmp, so they must survive the longjmp.
    volatile uint8_t *out = NULL;
    FILE *volatile f = NULL;

    ci.err = jpeg_std_error(&je.pub);
    je.pub.error_exit = jpeg_err_exit;
    je.pub.output_message = jpeg_err_silent;

    if (setjmp(je.jb)) {
        ci.progress = NULL;
        jpeg_destroy_decompress(&ci);
        if (f) fclose(f);
        free((uint8_t *)out);
        return NULL;
    }

    jpeg_create_decompress(&ci);

    JpegProgress prog;
    if (abort_fn) {
        memset(&prog, 0, sizeof prog);
        prog.pub.progress_monitor = jpeg_progress_check;
        prog.abort_fn = abort_fn;
        prog.ctx = ctx;
        prog.jb = &je.jb;
        ci.progress = &prog.pub;
    }

    if (mem) {
        jpeg_mem_src(&ci, mem, (unsigned long)memlen);
    } else {
        f = fopen(path, "rb");
        if (!f) { jpeg_destroy_decompress(&ci); return NULL; }
        jpeg_stdio_src(&ci, f);
    }

    jpeg_read_header(&ci, TRUE);
    *full_w = (int)ci.image_width;
    *full_h = (int)ci.image_height;

    int num = 8;
    if (need_w > 0 && need_h > 0) {
        for (int n = 1; n <= 8; n++) {
            int ow = (int)((ci.image_width * (unsigned)n + 7) / 8);
            int oh = (int)((ci.image_height * (unsigned)n + 7) / 8);
            if (ow >= need_w && oh >= need_h) { num = n; break; }
        }
    }
    ci.scale_num = (unsigned)num;
    ci.scale_denom = 8;
    ci.out_color_space = JCS_RGB;   // libjpeg widens grayscale for us
    // Below full scale the extra smoothing is invisible once we box-filter.
    if (num < 8) ci.do_fancy_upsampling = FALSE;

    jpeg_start_decompress(&ci);
    if (ci.output_components != 3) longjmp(je.jb, 1);

    *w = (int)ci.output_width;
    *h = (int)ci.output_height;
    out = (uint8_t *)malloc((size_t)*w * (size_t)*h * 3);
    if (!out) longjmp(je.jb, 1);

    size_t stride = (size_t)*w * 3;
    while (ci.output_scanline < ci.output_height) {
        // Hand libjpeg a run of rows so it can empty its upsampler in one call.
        JSAMPROW rows[16];
        unsigned n = ci.output_height - ci.output_scanline;
        if (n > 16) n = 16;
        for (unsigned i = 0; i < n; i++)
            rows[i] = (JSAMPROW)((uint8_t *)out + (ci.output_scanline + i) * stride);
        if (jpeg_read_scanlines(&ci, rows, n) == 0) break;
        // Every sixteen rows is often enough to bail out of a large image
        // promptly, and far too rare for the check to cost anything.
        if (abort_fn && abort_fn(ctx)) longjmp(je.jb, 1);
    }

    jpeg_finish_decompress(&ci);
    ci.progress = NULL;
    jpeg_destroy_decompress(&ci);
    if (f) fclose(f);
    return (uint8_t *)out;
}
#endif // PIX_HAVE_JPEG

/* -------------------------------------------------------------- decode -- */

// Convert an stb-unsupported file to PNG in a temp path using macOS `sips`.
// Returns a malloc'd path the caller must unlink+free, or NULL.
static char *convert_fallback(const char *path, ImageAbortFn abort_fn, void *ctx) {
#ifndef __APPLE__
    (void)path; (void)abort_fn; (void)ctx;
    return NULL;
#else
    char tmpl[] = "/tmp/pix-conv-XXXXXX";
    int fd = mkstemp(tmpl);
    if (fd < 0) return NULL;
    close(fd);

    char *out = (char *)malloc(strlen(tmpl) + 5);
    if (!out) { unlink(tmpl); return NULL; }
    sprintf(out, "%s.png", tmpl);
    unlink(tmpl);

    pid_t pid = fork();
    if (pid < 0) { free(out); return NULL; }
    if (pid == 0) {
        int null = open("/dev/null", O_WRONLY);
        if (null >= 0) { dup2(null, 1); dup2(null, 2); close(null); }
        execlp("sips", "sips", "-s", "format", "png", path, "--out", out, (char *)NULL);
        _exit(127);
    }
    // Poll rather than block: this is by far the slowest path here, and it is
    // the one most worth abandoning when nothing wants the result any more.
    int status = 0;
    for (;;) {
        pid_t r = waitpid(pid, &status, abort_fn ? WNOHANG : 0);
        if (r == pid) break;
        if (r < 0) { free(out); return NULL; }
        if (abort_fn(ctx)) {
            kill(pid, SIGKILL);
            waitpid(pid, NULL, 0);
            unlink(out);
            free(out);
            return NULL;
        }
        struct timespec ts = { 0, 5L * 1000 * 1000 };
        nanosleep(&ts, NULL);
    }
    if (WIFEXITED(status) && WEXITSTATUS(status) == 0) return out;

    unlink(out);
    free(out);
    return NULL;
#endif
}

// Decode via stb at full resolution, returning RGB. Alpha, when the file has
// any, is composited onto `bg` in place - the RGB triples are strictly behind
// the RGBA quads they come from, so the buffer can be narrowed and shrunk
// rather than duplicated.
static uint8_t *stb_decode(const char *path, const uint8_t bg[3], int *w, int *h) {
    int comp = 0, got = 0;
    // Ask only for what the file carries: the common no-alpha case then skips
    // both the composite pass and a full-size second buffer.
    int req = (stbi_info(path, w, h, &comp) && comp != 2 && comp != 4) ? 3 : 4;

    uint8_t *px = stbi_load(path, w, h, &got, req);
    if (!px || req == 3) return px;

    size_t n = (size_t)*w * (size_t)*h;
    for (size_t i = 0; i < n; i++) {
        unsigned a = px[i * 4 + 3];
        if (a == 255) {
            px[i * 3 + 0] = px[i * 4 + 0];
            px[i * 3 + 1] = px[i * 4 + 1];
            px[i * 3 + 2] = px[i * 4 + 2];
        } else {
            unsigned ia = 255 - a;
            for (int c = 0; c < 3; c++)
                px[i * 3 + c] = (uint8_t)((px[i * 4 + c] * a + bg[c] * ia + 127) / 255);
        }
    }
    uint8_t *shrunk = (uint8_t *)realloc(px, n * 3);
    return shrunk ? shrunk : px;
}

bool image_probe(const char *path, int *w, int *h) {
    int comp;
    return stbi_info(path, w, h, &comp) != 0;
}

#ifdef PIX_HAVE_JPEG
// True if a x b and c x d describe the same shape to within ~4%. EXIF
// thumbnails are sometimes cropped to 4:3 regardless of the real frame, and
// those must not be mistaken for a cheap copy of it.
static bool aspect_matches(int a, int b, int c, int d) {
    if (a <= 0 || b <= 0 || c <= 0 || d <= 0) return false;
    int64_t x = (int64_t)a * d, y = (int64_t)b * c;
    int64_t lo = x < y ? x : y;
    return (x > y ? x - y : y - x) * 25 <= lo;
}
#endif

bool image_load_fit(const char *path, const uint8_t bg[3], int box_w, int box_h,
                    Image *out) {
    return image_load_fit_cancel(path, bg, box_w, box_h, 0, NULL, NULL, out);
}

bool image_load_fit_ex(const char *path, const uint8_t bg[3], int box_w, int box_h,
                       int flags, Image *out) {
    return image_load_fit_cancel(path, bg, box_w, box_h, flags, NULL, NULL, out);
}

#define IMAGE_GIVE_UP() do { \
    if (abort_fn && abort_fn(ctx)) { free(raw); exif_free(&ex); return false; } \
} while (0)

bool image_load_fit_cancel(const char *path, const uint8_t bg[3], int box_w, int box_h,
                           int flags, ImageAbortFn abort_fn, void *ctx, Image *out) {
    memset(out, 0, sizeof *out);

    uint8_t *raw = NULL;
    Exif ex;
    memset(&ex, 0, sizeof ex);
    IMAGE_GIVE_UP();
    exif_read(path, &ex);

    // Source size before orientation. A probe failure is not fatal - it only
    // means we cannot tell the decoder how little it may get away with.
    int fw = 0, fh = 0;
    image_probe(path, &fw, &fh);

    bool swap = (ex.orient >= 5);
    int need_w = 0, need_h = 0;
    if (box_w > 0 && box_h > 0 && fw > 0 && fh > 0) {
        int dw, dh;
        image_fit(swap ? fh : fw, swap ? fw : fh, box_w, box_h, &dw, &dh);
        need_w = swap ? dh : dw;
        need_h = swap ? dw : dh;
    }

    int rw = 0, rh = 0;
    int orient = ex.orient;
    bool provisional = false;
#ifndef PIX_HAVE_JPEG
    (void)need_w; (void)need_h; (void)flags;
#endif

#ifdef PIX_HAVE_JPEG
    // The embedded thumbnail is free next to any real decode, so take it
    // whenever it is genuinely large enough to fill the box.
    if (ex.is_jpeg && ex.thumb && need_w > 0 && !(flags & IMAGE_NO_EXIF_THUMB)) {
        int tw, th, tfw, tfh;
        uint8_t *t = jpeg_decode(NULL, ex.thumb, ex.thumb_len, 0, 0,
                                 &tw, &th, &tfw, &tfh, abort_fn, ctx);
        if (t) {
            if (tw >= need_w && th >= need_h && aspect_matches(tw, th, fw, fh)) {
                raw = t; rw = tw; rh = th;
                provisional = true;
            } else {
                free(t);
            }
        }
    }
    if (!raw && ex.is_jpeg) {
        int jfw = 0, jfh = 0;
        raw = jpeg_decode(path, NULL, 0, need_w, need_h, &rw, &rh, &jfw, &jfh,
                          abort_fn, ctx);
        if (raw && jfw > 0) { fw = jfw; fh = jfh; }
    }
#endif

    // An abandoned jpeg_decode also returns NULL, and without this the fallback
    // below would go on to decode the whole file at full size anyway.
    IMAGE_GIVE_UP();

    char *tmp = NULL;
    if (!raw) {
        raw = stb_decode(path, bg, &rw, &rh);
        IMAGE_GIVE_UP();
        if (!raw) {
            // Formats no decoder here handles (HEIC, ...). sips writes a PNG,
            // which carries no orientation of its own to apply afterwards.
            tmp = convert_fallback(path, abort_fn, ctx);
            if (!tmp) { exif_free(&ex); return false; }
            raw = stb_decode(tmp, bg, &rw, &rh);
            unlink(tmp);
            free(tmp);
            if (!raw) { exif_free(&ex); return false; }
            orient = 1;
        }
        fw = rw; fh = rh;
    }
    exif_free(&ex);
    memset(&ex, 0, sizeof ex);

    // The resample below is proportional to the source, so it is worth one last
    // look before committing to it.
    if (abort_fn && abort_fn(ctx)) { free(raw); return false; }

    swap = (orient >= 5);
    int src_w = swap ? fh : fw, src_h = swap ? fw : fh;

    // Target in display space, then in the decoded buffer's own space.
    int dw, dh;
    if (box_w > 0 && box_h > 0) image_fit(src_w, src_h, box_w, box_h, &dw, &dh);
    else { dw = src_w; dh = src_h; }
    int tw = swap ? dh : dw, th = swap ? dw : dh;

    uint8_t *scaled;
    if (tw == rw && th == rh) {
        scaled = raw;              // already the right size; no copy needed
    } else {
        scaled = image_scale(raw, rw, rh, tw, th);
        free(raw);
        if (!scaled) return false;
    }

    out->rgb = apply_orientation(scaled, &tw, &th, orient);
    out->w = tw;
    out->h = th;
    out->src_w = src_w;
    out->src_h = src_h;
    out->provisional = provisional;
    return true;
}

/* ------------------------------------------------------------ resample -- */

static uint8_t *scale_box(const uint8_t *src, int sw, int sh, int dw, int dh) {
    uint8_t *dst = (uint8_t *)malloc((size_t)dw * dh * 3);
    if (!dst) return NULL;

    for (int y = 0; y < dh; y++) {
        int y0 = (int)((int64_t)y * sh / dh);
        int y1 = (int)((int64_t)(y + 1) * sh / dh);
        if (y1 <= y0) y1 = y0 + 1;
        for (int x = 0; x < dw; x++) {
            int x0 = (int)((int64_t)x * sw / dw);
            int x1 = (int)((int64_t)(x + 1) * sw / dw);
            if (x1 <= x0) x1 = x0 + 1;

            uint32_t r = 0, g = 0, b = 0, n = 0;
            for (int sy = y0; sy < y1; sy++) {
                const uint8_t *row = src + ((size_t)sy * sw + x0) * 3;
                for (int sx = x0; sx < x1; sx++, row += 3) {
                    r += row[0]; g += row[1]; b += row[2]; n++;
                }
            }
            uint8_t *o = dst + ((size_t)y * dw + x) * 3;
            o[0] = (uint8_t)(r / n);
            o[1] = (uint8_t)(g / n);
            o[2] = (uint8_t)(b / n);
        }
    }
    return dst;
}

static uint8_t *scale_bilinear(const uint8_t *src, int sw, int sh, int dw, int dh) {
    uint8_t *dst = (uint8_t *)malloc((size_t)dw * dh * 3);
    if (!dst) return NULL;

    // 16.16 fixed point; the -0.5 shift centers samples on source pixels.
    for (int y = 0; y < dh; y++) {
        int64_t fy = ((int64_t)y * sh * 65536) / dh - 32768;
        if (fy < 0) fy = 0;
        int y0 = (int)(fy >> 16);
        int y1 = y0 + 1 < sh ? y0 + 1 : sh - 1;
        uint32_t wy = (uint32_t)(fy & 0xFFFF);

        for (int x = 0; x < dw; x++) {
            int64_t fx = ((int64_t)x * sw * 65536) / dw - 32768;
            if (fx < 0) fx = 0;
            int x0 = (int)(fx >> 16);
            int x1 = x0 + 1 < sw ? x0 + 1 : sw - 1;
            uint32_t wx = (uint32_t)(fx & 0xFFFF);

            const uint8_t *p00 = src + ((size_t)y0 * sw + x0) * 3;
            const uint8_t *p01 = src + ((size_t)y0 * sw + x1) * 3;
            const uint8_t *p10 = src + ((size_t)y1 * sw + x0) * 3;
            const uint8_t *p11 = src + ((size_t)y1 * sw + x1) * 3;
            uint8_t *o = dst + ((size_t)y * dw + x) * 3;

            for (int c = 0; c < 3; c++) {
                uint32_t top = (p00[c] * (65536 - wx) + p01[c] * wx) >> 16;
                uint32_t bot = (p10[c] * (65536 - wx) + p11[c] * wx) >> 16;
                o[c] = (uint8_t)((top * (65536 - wy) + bot * wy) >> 16);
            }
        }
    }
    return dst;
}

uint8_t *image_scale(const uint8_t *src, int sw, int sh, int dw, int dh) {
    if (!src || sw <= 0 || sh <= 0 || dw <= 0 || dh <= 0) return NULL;
    if (dw == sw && dh == sh) {
        uint8_t *dst = (uint8_t *)malloc((size_t)dw * dh * 3);
        if (dst) memcpy(dst, src, (size_t)dw * dh * 3);
        return dst;
    }
    if (dw <= sw && dh <= sh) return scale_box(src, sw, sh, dw, dh);
    return scale_bilinear(src, sw, sh, dw, dh);
}

#endif // IMAGE_IMPLEMENTATION
