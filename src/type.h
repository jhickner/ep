#ifndef TYPE_H
#define TYPE_H

/**
 * type.h - typeset a chapter into RGBA pages with a real text engine.
 *
 * Where layout.h wraps blocks onto a character grid, this hands them to
 * CoreText: proportional faces, kerning, ligatures, justification and
 * hyphenation, drawn into a pixel buffer the caller shows with kitty
 * graphics. Only macOS has a backend; elsewhere type_available() is false.
 *
 * Pagination falls out of the typesetter rather than being estimated. Asking
 * for a page returns the position just past the last line that fitted, which
 * is where the next page begins, so pages tile the chapter exactly.
 *
 * Position is the same (block, byte offset) pair the rest of the reader uses,
 * so a typeset position and a wrapped-text position mean the same place.
 */

#include <stdbool.h>
#include <stdint.h>

#ifndef DOC_H
#include "doc.h"
#endif

typedef struct {
    const char *family;      /* serif text face; NULL for the default */
    double      size;        /* body size in pixels */
    double      leading;     /* line height as a multiple of the body size */
    double      margin;      /* page margin in pixels */
    double      indent;      /* paragraph indent in ems, 0 for none */
    double      hyphenation; /* 0 off, 1 hyphenate freely */
    bool        justify;
    bool        smooth;      /* subpixel antialiasing */
    bool        transparent; /* leave the paper clear, ignoring paper[] */
    uint8_t     paper[3], ink[3];
} TypeStyle;

/* A chapter built for one style: the text, where every piece of it came from,
   and - once paginated - where each page starts. Building is the expensive
   part and happens once; paging around inside it is then free. */
typedef struct TypeChapter TypeChapter;

bool type_available(void);
void type_style_default(TypeStyle *st);

/* True if the family named is the family that would be used. CoreText answers
   a name it does not know with Helvetica rather than with nothing, so asking
   is the only way a missing font is told from a present one. */
bool type_font_exists(const char *family);

TypeChapter *type_open(const Doc *d, const TypeStyle *st);
void         type_close(TypeChapter *tc);

/* Split into pages of w x h pixels, returning how many there are. */
int  type_paginate(TypeChapter *tc, int w, int h);
int  type_pages(const TypeChapter *tc);

/* Draw a page into a w x h RGBA buffer of the size last paginated. */
bool type_draw(TypeChapter *tc, int page, uint8_t *rgba);

/* Positions, in the (block, byte offset) the rest of the reader uses. */
int  type_page_of(const TypeChapter *tc, int block, int off);
void type_page_start(const TypeChapter *tc, int page, int *block, int *off);

#endif /* TYPE_H */

#ifdef TYPE_IMPLEMENTATION

#include <stdlib.h>
#include <string.h>

void type_style_default(TypeStyle *st) {
    memset(st, 0, sizeof *st);
    st->family      = "Iowan Old Style";
    st->size        = 17;
    st->leading     = 1.38;
    st->margin      = 28;
    st->indent      = 1.4;
    st->hyphenation = 1;
    st->justify     = true;
    st->smooth      = false;
    st->paper[0] = 0xfa; st->paper[1] = 0xf7; st->paper[2] = 0xf0;
    st->ink[0]   = 0x1c; st->ink[1]   = 0x1a; st->ink[2]   = 0x18;
}

#ifdef __APPLE__

#include <CoreFoundation/CoreFoundation.h>
#include <CoreGraphics/CoreGraphics.h>
#include <CoreText/CoreText.h>

bool type_available(void) { return true; }

/* ------------------------------------------------------------ positions -- */

/* Each run of text put into the string remembers where it came from, so a
   UTF-16 index in the laid-out string can be turned back into a (block, byte)
   position. Runs are appended in order, so a lookup is a walk from the end. */
typedef struct {
    int      block;
    int      byte;      /* byte offset in the block this run starts at */
    CFIndex  u16;       /* where the run starts in the string */
    CFIndex  u16_len;
    int      bytes;     /* length of the run's text */
} Mark;

typedef struct { Mark *v; int n, cap; } Marks;

static void mark_add(Marks *m, int block, int byte, CFIndex u16, CFIndex len, int bytes) {
    if (m->n == m->cap) {
        m->cap = m->cap ? m->cap * 2 : 64;
        m->v = realloc(m->v, (size_t)m->cap * sizeof *m->v);
    }
    m->v[m->n++] = (Mark){ block, byte, u16, len, bytes };
}

/* Bytes into `s` that `want` UTF-16 units cover. */
static int u16_to_bytes(const char *s, int len, CFIndex want) {
    int i = 0;
    CFIndex u = 0;
    while (i < len && u < want) {
        unsigned char c = (unsigned char)s[i];
        int n = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : 4;
        if (i + n > len) break;
        u += n == 4 ? 2 : 1;      /* astral characters are a surrogate pair */
        i += n;
    }
    return i;
}

/* UTF-16 units that `len` bytes of `s` come to. */
static CFIndex bytes_to_u16(const char *s, int len) {
    CFIndex u = 0;
    for (int i = 0; i < len; ) {
        unsigned char c = (unsigned char)s[i];
        int n = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : 4;
        if (i + n > len) break;
        u += n == 4 ? 2 : 1;
        i += n;
    }
    return u;
}

/* ---------------------------------------------------------------- fonts -- */

typedef struct { CTFontRef reg, bold, ital, bi, head; } Fonts;

static CTFontRef variant(CTFontRef base, CTFontSymbolicTraits want, double size) {
    CTFontRef f = CTFontCreateCopyWithSymbolicTraits(base, size, NULL, want,
                                                     kCTFontTraitBold | kCTFontTraitItalic);
    return f ? f : (CTFontRef)CFRetain(base);
}

bool type_font_exists(const char *family) {
    if (!family || !*family) return false;
    CFStringRef want = CFStringCreateWithCString(NULL, family, kCFStringEncodingUTF8);
    if (!want) return false;
    CTFontRef f = CTFontCreateWithName(want, 12, NULL);
    bool ok = false;
    if (f) {
        CFStringRef got = CTFontCopyFamilyName(f);
        ok = got && CFStringCompare(got, want, kCFCompareCaseInsensitive) == kCFCompareEqualTo;
        if (got) CFRelease(got);
        CFRelease(f);
    }
    CFRelease(want);
    return ok;
}

static void fonts_make(Fonts *f, const TypeStyle *st) {
    CFStringRef name = CFStringCreateWithCString(NULL, st->family ? st->family : "Georgia",
                                                 kCFStringEncodingUTF8);
    CTFontRef base = name ? CTFontCreateWithName(name, st->size, NULL) : NULL;
    if (name) CFRelease(name);
    if (!base) base = CTFontCreateUIFontForLanguage(kCTFontUIFontSystem, st->size, NULL);

    f->reg  = base;
    f->bold = variant(base, kCTFontTraitBold, st->size);
    f->ital = variant(base, kCTFontTraitItalic, st->size);
    f->bi   = variant(base, kCTFontTraitBold | kCTFontTraitItalic, st->size);
    f->head = variant(base, kCTFontTraitBold, st->size * 1.22);
}

static void fonts_free(Fonts *f) {
    if (f->reg)  CFRelease(f->reg);
    if (f->bold) CFRelease(f->bold);
    if (f->ital) CFRelease(f->ital);
    if (f->bi)   CFRelease(f->bi);
    if (f->head) CFRelease(f->head);
}

/* ----------------------------------------------------------- paragraphs -- */

/* Line breaking is done by hand below, so each paragraph's shape is carried
   here rather than left to a CoreText paragraph style. */
typedef struct {
    CFIndex         start, end;
    CGFloat         first, head, before;   /* indents and space above, pixels */
    CGFloat         mult;                  /* line height multiple */
    CTTextAlignment align;
} Para;

typedef struct { Para *v; int n, cap; } Paras;

static Para *para_push(Paras *p) {
    if (p->n == p->cap) {
        p->cap = p->cap ? p->cap * 2 : 32;
        p->v = realloc(p->v, (size_t)p->cap * sizeof *p->v);
    }
    return &p->v[p->n++];
}

static void para_shape(Para *pa, const TypeStyle *st, const Block *b, bool indent,
                       bool after_heading) {
    pa->align = b->center || b->heading ? kCTTextAlignmentCenter
              : st->justify             ? kCTTextAlignmentJustified
                                        : kCTTextAlignmentNatural;
    pa->head  = (CGFloat)(b->indent * st->size * 0.5);
    pa->first = pa->head + (CGFloat)(indent ? st->indent * st->size : 0);
    /* A paragraph in a run of prose is set tight against the one above and
       marked by its indent; only a real break - a heading, a section - opens
       space. doc.h gives every block one blank line, so one means none. */
    pa->before = (CGFloat)(b->before > 1 ? (b->before - 1) * st->size * 0.8 : 0);
    if (after_heading && pa->before < st->size * 0.8) pa->before = (CGFloat)(st->size * 0.8);
    pa->mult   = (CGFloat)st->leading;
}

/* ------------------------------------------------------------- building -- */

static CFDictionaryRef attrs_make(CTFontRef font, CGColorRef ink) {
    CFStringRef keys[] = { kCTFontAttributeName, kCTForegroundColorAttributeName };
    CFTypeRef  vals[] = { font, ink };
    return CFDictionaryCreate(NULL, (const void **)keys, (const void **)vals, 2,
                              &kCFTypeDictionaryKeyCallBacks,
                              &kCFTypeDictionaryValueCallBacks);
}

static void append_run(CFMutableAttributedStringRef m, Marks *marks,
                       const char *text, int len, int block, int byte,
                       CFDictionaryRef attrs) {
    if (len <= 0) return;
    CFStringRef s = CFStringCreateWithBytes(NULL, (const UInt8 *)text, len,
                                            kCFStringEncodingUTF8, false);
    if (!s) return;
    CFAttributedStringRef a = CFAttributedStringCreate(NULL, s, attrs);
    CFIndex at = CFAttributedStringGetLength(m);
    CFAttributedStringReplaceAttributedString(m, CFRangeMake(at, 0), a);
    if (block >= 0)
        mark_add(marks, block, byte, at, CFAttributedStringGetLength(m) - at, len);
    CFRelease(a);
    CFRelease(s);
}

/* ---------------------------------------------------------- hyphenation -- */

/* CoreText has no hyphenation of its own - a paragraph style cannot ask for
   it, and it will not draw the hyphen either. What it does honour is U+00AD as
   a place a line may break, so hyphenation here comes in two halves: ask
   CoreFoundation where a word may break and put a soft hyphen there, then draw
   a hyphen onto whichever line ends at one.
 *
 * The soft hyphens go in as runs of their own rather than into the text, which
 * leaves every text run an exact substring of its block and so keeps positions
 * translatable both ways. */

#define SOFT_HYPHEN "\xc2\xad"
#define SOFT_HYPHEN_CH 0x00AD

typedef struct { int *at; int n, cap; } Breaks;

static void breaks_add(Breaks *br, int byte) {
    if (br->n == br->cap) {
        br->cap = br->cap ? br->cap * 2 : 32;
        br->at = realloc(br->at, (size_t)br->cap * sizeof *br->at);
    }
    br->at[br->n++] = byte;
}

static int cmp_cfindex(const void *a, const void *b) {
    CFIndex x = *(const CFIndex *)a, y = *(const CFIndex *)b;
    return x < y ? -1 : x > y ? 1 : 0;
}

/* Byte offsets in `b` at which a hyphen may be set, ascending. */
static void hyphen_points(const Block *b, CFLocaleRef loc, Breaks *br) {
    br->n = 0;
    if (!loc || b->len <= 0) return;
    CFStringRef cf = CFStringCreateWithBytes(NULL, (const UInt8 *)b->text, b->len,
                                             kCFStringEncodingUTF8, false);
    if (!cf) return;
    CFIndex n = CFStringGetLength(cf);

    /* The lookup answers with the last opportunity at or before the index it
       is given, and has none to offer at a word's edge - so it is asked word
       by word, each answer becoming the next question until the word runs out.
       Words are found in the UTF-8 text, whose spaces are the string's too,
       counting into UTF-16 along the way. */
    CFIndex *pts = NULL;
    int npts = 0, cap = 0;
    CFIndex u = 0, wstart = 0;
    int wlen = 0;

    for (int i = 0; i <= b->len; ) {
        bool space = i == b->len || b->text[i] == ' ' || b->text[i] == '\t' ||
                     b->text[i] == '\n';
        if (!space) {
            if (wlen == 0) wstart = u;
            unsigned char c = (unsigned char)b->text[i];
            int w = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : 4;
            if (i + w > b->len) break;
            u += w == 4 ? 2 : 1;
            i += w;
            wlen++;
            continue;
        }
        if (wlen >= 5) {          /* shorter words are not worth the lookup */
            for (CFIndex at = u; at > wstart; ) {
                CFIndex hy = CFStringGetHyphenationLocationBeforeIndex(cf, at,
                                 CFRangeMake(0, n), 0, loc, NULL);
                if (hy == kCFNotFound || hy <= wstart) break;
                if (npts == cap) {
                    cap = cap ? cap * 2 : 64;
                    pts = realloc(pts, (size_t)cap * sizeof *pts);
                }
                pts[npts++] = hy;
                at = hy;
            }
        }
        wlen = 0;
        if (i == b->len) break;
        u++;                      /* the space itself */
        i++;
    }
    CFRelease(cf);
    if (!npts) { free(pts); return; }
    qsort(pts, (size_t)npts, sizeof *pts, cmp_cfindex);

    /* One walk of the text turns the UTF-16 answers into byte offsets. */
    int i = 0;
    CFIndex at = 0;
    for (int k = 0; k < npts; k++) {
        while (i < b->len && at < pts[k]) {
            unsigned char c = (unsigned char)b->text[i];
            int w = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : 4;
            if (i + w > b->len) break;
            at += w == 4 ? 2 : 1;
            i += w;
        }
        if (at == pts[k] && (br->n == 0 || br->at[br->n - 1] != i)) breaks_add(br, i);
    }
    free(pts);
}

/* Bytes of `b` from `from` that share one style. */
static int style_run(const Block *b, int from) {
    if (!b->style) return b->len - from;
    int i = from + 1;
    while (i < b->len && b->style[i] == b->style[from]) i++;
    return i - from;
}

static CTFontRef face_for(const Fonts *f, const Block *b, unsigned char sty) {
    if (b->heading) return f->head;
    bool bold = (sty & DS_BOLD) != 0, ital = (sty & DS_ITALIC) != 0;
    return bold && ital ? f->bi : bold ? f->bold : ital ? f->ital : f->reg;
}

/* The whole chapter, as one attributed string. */
static CFAttributedStringRef build(const Doc *d, const TypeStyle *st, const Fonts *f,
                                   CGColorRef ink, Marks *marks, Paras *paras) {
    CFMutableAttributedStringRef m = CFAttributedStringCreateMutable(NULL, 0);
    if (!m) return NULL;
    CFAttributedStringBeginEditing(m);

    CFLocaleRef loc = NULL;
    if (st->hyphenation > 0) {
        loc = CFLocaleCopyCurrent();
        if (loc && !CFStringIsHyphenationAvailableForLocale(loc)) {
            CFRelease(loc);
            loc = CFLocaleCreate(NULL, CFSTR("en_US"));
            if (loc && !CFStringIsHyphenationAvailableForLocale(loc)) {
                CFRelease(loc);
                loc = NULL;
            }
        }
    }
    Breaks br = {0};
    CFDictionaryRef plain = attrs_make(f->reg, ink);
    bool prev_break = true;      /* the chapter's first paragraph is flush */
    bool after_heading = false;

    for (int i = 0; i < d->nblocks; i++) {
        const Block *b = &d->blocks[i];
        int start = 0;

        if (b->type == BLK_IMG) continue;

        if (b->type == BLK_RULE) {
            Para *pa = para_push(paras);
            pa->start = CFAttributedStringGetLength(m);
            Block rule = { .type = BLK_TEXT, .center = true, .before = 2 };
            para_shape(pa, st, &rule, false, false);
            append_run(m, marks, "* * *", 5, i, 0, plain);
            append_run(m, marks, "\n", 1, -1, 0, plain);
            pa->end = CFAttributedStringGetLength(m);
            prev_break = true;
            after_heading = false;
            continue;
        }
        if (b->len == 0) continue;

        Para *pa = para_push(paras);
        pa->start = CFAttributedStringGetLength(m);
        /* Indent marks a new paragraph in a run of prose; the one that opens a
           section, or follows a heading or a break, is set flush instead. */
        para_shape(pa, st, b, st->indent > 0 && !b->heading && !b->center &&
                              b->indent == 0 && b->before <= 1 && !prev_break,
                   after_heading);

        CFDictionaryRef cache[8] = {0};      /* one per DS_* combination */
        if (loc && !b->heading) hyphen_points(b, loc, &br); else br.n = 0;
        int nextb = 0;
        while (nextb < br.n && br.at[nextb] <= start) nextb++;

        for (int p = start; p < b->len; ) {
            int n = style_run(b, p);
            unsigned char sty = (unsigned char)((b->style ? b->style[p] : 0) & 7);
            if (!cache[sty]) cache[sty] = attrs_make(face_for(f, b, sty), ink);

            /* Break the run where a hyphen may fall, dropping in the soft
               hyphen between the pieces. */
            int q = p;
            while (nextb < br.n && br.at[nextb] < p + n) {
                int cut = br.at[nextb++];
                if (cut <= q) continue;
                append_run(m, marks, b->text + q, cut - q, i, q, cache[sty]);
                append_run(m, marks, SOFT_HYPHEN, 2, -1, 0, cache[sty]);
                q = cut;
            }
            append_run(m, marks, b->text + q, p + n - q, i, q, cache[sty]);
            p += n;
        }
        append_run(m, marks, "\n", 1, -1, 0, cache[0] ? cache[0] : plain);
        for (int k = 0; k < 8; k++) if (cache[k]) CFRelease(cache[k]);

        pa->end = CFAttributedStringGetLength(m);
        prev_break = b->heading || b->before > 1;
        after_heading = b->heading;
    }

    free(br.at);
    CFRelease(plain);
    if (loc) CFRelease(loc);
    CFAttributedStringEndEditing(m);
    return m;
}

/* --------------------------------------------------------------- render -- */

/* Turn the UTF-16 index a page stopped at into a reading position. */
static void resolve(const Marks *m, const Doc *d, CFIndex end, int *block, int *off) {
    for (int i = m->n - 1; i >= 0; i--) {
        const Mark *k = &m->v[i];
        if (end < k->u16) continue;
        if (end >= k->u16 + k->u16_len) {       /* past this run entirely */
            *block = k->block;
            *off   = k->byte + k->bytes;
            if (*off >= d->blocks[k->block].len) { *block = k->block + 1; *off = 0; }
            return;
        }
        *block = k->block;
        *off   = k->byte + u16_to_bytes(d->blocks[k->block].text + k->byte,
                                        k->bytes, end - k->u16);
        return;
    }
    *block = m->n ? m->v[0].block : 0;
    *off   = 0;
}

static double line_width(CTLineRef line) {
    return CTLineGetTypographicBounds(line, NULL, NULL, NULL) -
           CTLineGetTrailingWhitespaceWidth(line);
}

/* The line covering [pos, pos+n), with the soft hyphen it ends on - if it ends
   on one - swapped for a hyphen that will actually be drawn. */
static CTLineRef make_line(CFAttributedStringRef str, CFIndex pos, CFIndex n, bool hyphen) {
    CFAttributedStringRef sub = CFAttributedStringCreateWithSubstring(NULL, str,
                                                                     CFRangeMake(pos, n));
    if (!sub) return NULL;
    CTLineRef line;
    if (hyphen) {
        CFMutableAttributedStringRef ms = CFAttributedStringCreateMutableCopy(NULL, 0, sub);
        CFAttributedStringReplaceString(ms, CFRangeMake(n - 1, 1), CFSTR("-"));
        line = CTLineCreateWithAttributedString(ms);
        CFRelease(ms);
    } else {
        line = CTLineCreateWithAttributedString(sub);
    }
    CFRelease(sub);
    return line;
}

struct TypeChapter {
    const Doc      *d;
    TypeStyle       st;
    Fonts           f;
    CGColorSpaceRef cs;
    CGColorRef      ink;
    CFAttributedStringRef str;
    CFStringRef     text;          /* borrowed from str */
    CTTypesetterRef ts;
    Marks           marks;
    Paras           paras;
    double          hyphen_w;
    int             w, h;
    CFIndex        *start;         /* where each page begins in the string */
    int             npages, cap;
};

TypeChapter *type_open(const Doc *d, const TypeStyle *st) {
    if (!d || !st) return NULL;
    TypeChapter *tc = calloc(1, sizeof *tc);
    if (!tc) return NULL;
    tc->d  = d;
    tc->st = *st;
    tc->cs = CGColorSpaceCreateDeviceRGB();
    CGFloat comps[4] = { st->ink[0] / 255.0, st->ink[1] / 255.0, st->ink[2] / 255.0, 1 };
    tc->ink = CGColorCreate(tc->cs, comps);
    fonts_make(&tc->f, st);
    tc->str = build(d, st, &tc->f, tc->ink, &tc->marks, &tc->paras);
    if (!tc->str || tc->paras.n == 0) return tc;      /* nothing to set */
    tc->text = CFAttributedStringGetString(tc->str);
    tc->ts   = CTTypesetterCreateWithAttributedString(tc->str);

    /* Room for a hyphen that may be added after the fact has to be left in
       the measure each break is suggested against. */
    if (st->hyphenation > 0) {
        CFDictionaryRef at = attrs_make(tc->f.reg, tc->ink);
        CFAttributedStringRef hs = CFAttributedStringCreate(NULL, CFSTR("-"), at);
        CTLineRef hl = CTLineCreateWithAttributedString(hs);
        tc->hyphen_w = CTLineGetTypographicBounds(hl, NULL, NULL, NULL);
        CFRelease(hl); CFRelease(hs); CFRelease(at);
    }
    return tc;
}

void type_close(TypeChapter *tc) {
    if (!tc) return;
    if (tc->ts)  CFRelease(tc->ts);
    if (tc->str) CFRelease(tc->str);
    fonts_free(&tc->f);
    CGColorRelease(tc->ink);
    CGColorSpaceRelease(tc->cs);
    free(tc->marks.v);
    free(tc->paras.v);
    free(tc->start);
    free(tc);
}

int type_pages(const TypeChapter *tc) { return tc ? tc->npages : 0; }

/* The paragraph a string index falls in. */
static int para_at(const TypeChapter *tc, CFIndex pos) {
    for (int i = 0; i < tc->paras.n; i++)
        if (pos < tc->paras.v[i].end) return i;
    return tc->paras.n;
}

/* Set one page from `pos`, drawing it if there is a context, and answer with
   the index it ran out of room at. Measuring and drawing walk the same path,
   so the pages a chapter is split into are the pages that get drawn. */
static CFIndex layout_page(TypeChapter *tc, CFIndex pos, CGContextRef ctx) {
    const TypeStyle *st = &tc->st;
    double left = st->margin, right = tc->w - st->margin;
    double bottom = st->margin, y = tc->h - st->margin;
    if (right - left <= st->size) return pos;

    CFIndex total = CFAttributedStringGetLength(tc->str);
    bool page_top = true, full = false;

    for (int pi = para_at(tc, pos); pi < tc->paras.n && !full; pi++) {
        const Para *pa = &tc->paras.v[pi];
        if (pos < pa->start) pos = pa->start;
        /* Space above a paragraph is space between paragraphs; a page does
           not open with it. */
        double after = page_top ? y : y - pa->before;
        bool first = pos == pa->start;

        while (pos < pa->end && !full) {
            double indent = first ? pa->first : pa->head;
            double avail  = right - left - indent;
            if (avail <= st->size) { full = true; break; }

            CFIndex n = CTTypesetterSuggestLineBreak(tc->ts, pos, avail - tc->hyphen_w);
            if (n <= 0) { full = true; break; }
            if (pos + n > pa->end) n = pa->end - pos;

            bool hyphen = CFStringGetCharacterAtIndex(tc->text, pos + n - 1) == SOFT_HYPHEN_CH;
            CTLineRef line = make_line(tc->str, pos, n, hyphen && ctx != NULL);
            if (!line) { full = true; break; }

            /* A line that did not need the hyphen's room gets it back. */
            if (!hyphen && tc->hyphen_w > 0) {
                CFIndex more = CTTypesetterSuggestLineBreak(tc->ts, pos, avail);
                if (more > n && pos + more <= pa->end &&
                    CFStringGetCharacterAtIndex(tc->text, pos + more - 1) != SOFT_HYPHEN_CH) {
                    CTLineRef wider = make_line(tc->str, pos, more, false);
                    if (wider && line_width(wider) <= avail) {
                        CFRelease(line);
                        line = wider;
                        n = more;
                    } else if (wider) {
                        CFRelease(wider);
                    }
                }
            }

            double asc, desc, lead;
            CTLineGetTypographicBounds(line, &asc, &desc, &lead);
            double lh = (asc + desc + lead) * pa->mult;
            if (after - asc * pa->mult - desc < bottom) {     /* out of page */
                CFRelease(line);
                full = true;
                break;
            }
            y = after;

            if (ctx) {
                bool last = pos + n >= pa->end;
                CTLineRef draw = line;
                if (pa->align == kCTTextAlignmentJustified && !last) {
                    CTLineRef j = CTLineCreateJustifiedLine(line, 1.0, avail);
                    if (j) draw = j;
                }
                double x = left + indent;
                if (pa->align == kCTTextAlignmentCenter)
                    x = left + indent + (avail - line_width(draw)) / 2;
                CGContextSetTextPosition(ctx, x, y - asc * pa->mult);
                CTLineDraw(draw, ctx);
                if (draw != line) CFRelease(draw);
            }
            CFRelease(line);

            after = y - lh;
            pos += n;
            first = false;
            page_top = false;
        }
        if (!full) y = after;
    }
    return pos > total ? total : pos;
}

int type_paginate(TypeChapter *tc, int w, int h) {
    if (!tc || w <= 0 || h <= 0) return 0;
    tc->w = w;
    tc->h = h;
    tc->npages = 0;
    if (!tc->ts || tc->paras.n == 0) return 0;

    CFIndex total = CFAttributedStringGetLength(tc->str);
    CFIndex pos = tc->paras.v[0].start;
    while (pos < total) {
        if (tc->npages == tc->cap) {
            tc->cap = tc->cap ? tc->cap * 2 : 32;
            tc->start = realloc(tc->start, (size_t)tc->cap * sizeof *tc->start);
        }
        tc->start[tc->npages++] = pos;
        CFIndex end = layout_page(tc, pos, NULL);
        if (end <= pos) break;        /* a page that holds nothing would loop */
        pos = end;
    }
    return tc->npages;
}

bool type_draw(TypeChapter *tc, int page, uint8_t *rgba) {
    if (!tc || !rgba || page < 0 || page >= tc->npages) return false;

    CGContextRef ctx = CGBitmapContextCreate(rgba, (size_t)tc->w, (size_t)tc->h, 8,
                                             (size_t)tc->w * 4, tc->cs,
                                             kCGImageAlphaPremultipliedLast |
                                             kCGBitmapByteOrder32Big);
    if (!ctx) return false;

    const TypeStyle *st = &tc->st;
    if (st->transparent) {
        memset(rgba, 0, (size_t)tc->w * (size_t)tc->h * 4);
    } else {
        CGContextSetRGBFillColor(ctx, st->paper[0] / 255.0, st->paper[1] / 255.0,
                                 st->paper[2] / 255.0, 1);
        CGContextFillRect(ctx, CGRectMake(0, 0, tc->w, tc->h));
    }
    CGContextSetShouldAntialias(ctx, true);
    CGContextSetShouldSmoothFonts(ctx, st->smooth);
    CGContextSetShouldSubpixelPositionFonts(ctx, true);
    CGContextSetShouldSubpixelQuantizeFonts(ctx, false);
    CGContextSetTextMatrix(ctx, CGAffineTransformIdentity);

    layout_page(tc, tc->start[page], ctx);
    CGContextRelease(ctx);

    /* A bitmap context can only hold premultiplied alpha, and the graphics
       protocol reads it straight, so the two are reconciled here. Only the
       antialiased edges of glyphs are part-transparent; the rest is either
       clear or solid ink and passes through untouched. */
    if (st->transparent) {
        uint8_t *p = rgba;
        for (long i = 0, n = (long)tc->w * tc->h; i < n; i++, p += 4) {
            int a = p[3];
            if (a == 0 || a == 255) continue;
            for (int c = 0; c < 3; c++) {
                int v = (p[c] * 255 + a / 2) / a;
                p[c] = (uint8_t)(v > 255 ? 255 : v);
            }
        }
    }
    return true;
}

void type_page_start(const TypeChapter *tc, int page, int *block, int *off) {
    *block = 0;
    *off   = 0;
    if (!tc || page < 0 || page >= tc->npages) return;
    resolve(&tc->marks, tc->d, tc->start[page], block, off);
}

int type_page_of(const TypeChapter *tc, int block, int off) {
    if (!tc || tc->npages == 0) return 0;

    /* The position as a string index: inside the run that holds it, or at the
       first run past it if it names text that was not set - an image, say. */
    CFIndex want = -1;
    for (int i = 0; i < tc->marks.n; i++) {
        const Mark *k = &tc->marks.v[i];
        if (k->block < block) continue;
        if (k->block > block) { want = k->u16; break; }
        if (off < k->byte) { want = k->u16; break; }
        if (off < k->byte + k->bytes) {
            want = k->u16 + bytes_to_u16(tc->d->blocks[block].text + k->byte, off - k->byte);
            break;
        }
    }
    if (want < 0) return tc->npages - 1;

    int page = 0;
    for (int i = 0; i < tc->npages; i++)
        if (tc->start[i] <= want) page = i; else break;
    return page;
}

#else  /* no typesetter */

struct TypeChapter { int unused; };

bool type_available(void) { return false; }

TypeChapter *type_open(const Doc *d, const TypeStyle *st) { (void)d; (void)st; return NULL; }
void type_close(TypeChapter *tc) { (void)tc; }
int  type_paginate(TypeChapter *tc, int w, int h) { (void)tc; (void)w; (void)h; return 0; }
int  type_pages(const TypeChapter *tc) { (void)tc; return 0; }
bool type_draw(TypeChapter *tc, int page, uint8_t *rgba) {
    (void)tc; (void)page; (void)rgba; return false;
}
int  type_page_of(const TypeChapter *tc, int block, int off) {
    (void)tc; (void)block; (void)off; return 0;
}
void type_page_start(const TypeChapter *tc, int page, int *block, int *off) {
    (void)tc; (void)page; *block = 0; *off = 0;
}

#endif /* __APPLE__ */

#endif /* TYPE_IMPLEMENTATION */
