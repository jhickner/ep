#ifndef DOC_H
#define DOC_H

#include <stdbool.h>
#include <stddef.h>

/* An XHTML chapter reduced to a list of blocks: a paragraph of styled text,
   an image, or a rule. Blocks are width-independent; layout.h wraps them. */

enum { BLK_TEXT, BLK_IMG, BLK_RULE };

enum { DS_BOLD = 1, DS_ITALIC = 2, DS_DIM = 4 };

typedef struct {
    int            type;
    char          *text;      /* whitespace-collapsed UTF-8 */
    unsigned char *style;     /* one DS_* byte per byte of text */
    int            len;
    int            indent;
    bool           center;
    bool           heading;
    int            before;    /* blank lines before the block */
    char          *src;       /* BLK_IMG: zip path of the image */
} Block;

typedef struct { char *id; int block; } Anchor;

typedef struct {
    Block  *blocks;  int nblocks;
    Anchor *anchors; int nanchors;
} Doc;

Doc  doc_parse(const char *xhtml, const char *base_dir);
void doc_free(Doc *d);
int  doc_anchor_block(const Doc *d, const char *id);

#endif /* DOC_H */

#ifdef DOC_IMPLEMENTATION

#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdio.h>

#ifndef XML_H
#include "xml.h"
#endif
#ifndef EPUB_H
#include "epub.h"
#endif

typedef struct {
    Doc  *doc;
    char *buf; unsigned char *sty;
    int   len, cap;

    unsigned char style;
    int   bold, italic;
    int   indent, before;
    bool  center, pre, heading;
    bool  pending_space;
    const char *base;
    int   list_depth;
    int   ol_counter[8];
    bool  ol[8];
    unsigned char span[32];
    int   span_depth;
} Parser;

static void blk_reserve(Parser *p, int extra) {
    if (p->len + extra + 1 <= p->cap) return;
    p->cap = (p->cap ? p->cap * 2 : 256);
    while (p->cap < p->len + extra + 1) p->cap *= 2;
    p->buf = realloc(p->buf, (size_t)p->cap);
    p->sty = realloc(p->sty, (size_t)p->cap);
}

static void emit(Parser *p, const char *s, int n) {
    blk_reserve(p, n);
    for (int i = 0; i < n; i++) {
        p->buf[p->len]   = s[i];
        p->sty[p->len++] = p->style;
    }
}

static Block *block_new(Parser *p, int type) {
    p->doc->blocks = realloc(p->doc->blocks,
                             (size_t)(p->doc->nblocks + 1) * sizeof(Block));
    Block *b = &p->doc->blocks[p->doc->nblocks++];
    memset(b, 0, sizeof *b);
    b->type = type;
    return b;
}

/* Close the paragraph under construction, dropping it if it is blank. */
static void flush(Parser *p) {
    while (p->len > 0 && p->buf[p->len - 1] == ' ') p->len--;
    p->pending_space = false;
    if (p->len == 0) return;
    Block *b = block_new(p, BLK_TEXT);
    b->len    = p->len;
    b->text   = malloc((size_t)p->len + 1);
    b->style  = malloc((size_t)p->len + 1);
    memcpy(b->text, p->buf, (size_t)p->len);
    memcpy(b->style, p->sty, (size_t)p->len);
    b->text[p->len] = 0;
    b->indent  = p->indent;
    b->center  = p->center;
    b->before  = p->before;
    b->heading = p->heading;
    p->len = 0;
    p->before = 1;
}

static void anchor_add(Parser *p, const char *id) {
    p->doc->anchors = realloc(p->doc->anchors,
                              (size_t)(p->doc->nanchors + 1) * sizeof(Anchor));
    Anchor *a = &p->doc->anchors[p->doc->nanchors++];
    a->id    = strdup(id);
    a->block = p->doc->nblocks;   /* the next block to be produced */
}

static bool tag_is(const char *lt, const char *name) {
    size_t n = strlen(name);
    if (strncasecmp(lt + 1, name, n) != 0) return false;
    char c = lt[1 + n];
    return c == '>' || c == '/' || c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

static bool close_is(const char *lt, const char *name) {
    if (lt[1] != '/') return false;
    size_t n = strlen(name);
    if (strncasecmp(lt + 2, name, n) != 0) return false;
    char c = lt[2 + n];
    return c == '>' || c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

static const char *skip_element(const char *lt, const char *name) {
    char close[32];
    snprintf(close, sizeof close, "</%s", name);
    const char *end = strcasestr(lt + 1, close);
    if (!end) return lt + strlen(lt);
    const char *gt = strchr(end, '>');
    return gt ? gt + 1 : end;
}

static void doc_set_style(Parser *p) {
    p->style = (unsigned char)((p->bold > 0 ? DS_BOLD : 0) |
                               (p->italic > 0 ? DS_ITALIC : 0));
}

static bool cls_has(const char *lt, const char *needle) {
    char cls[256] = "";
    if (!xml_attr(lt, "class", cls, sizeof cls)) {
        char st[256] = "";
        if (!xml_attr(lt, "style", st, sizeof st)) return false;
        return strcasestr(st, needle) != NULL;
    }
    return strcasestr(cls, needle) != NULL;
}

static void text_run(Parser *p, const char *s, int n) {
    char decoded[4096];
    while (n > 0) {
        int chunk = n > 2000 ? 2000 : n;
        /* Do not split an entity across chunks. */
        if (chunk < n) {
            int back = 0;
            while (back < 12 && chunk - back > 0 && s[chunk - back - 1] != '&') back++;
            if (chunk - back > 0 && s[chunk - back - 1] == '&') chunk -= back + 1;
            if (chunk <= 0) chunk = n;
        }
        size_t dl = xml_decode(s, (size_t)chunk, decoded, sizeof decoded);
        for (size_t i = 0; i < dl; i++) {
            unsigned char c = (unsigned char)decoded[i];
            if (!p->pre && (c == ' ' || c == '\t' || c == '\n' || c == '\r' ||
                            (c == 0xC2 && i + 1 < dl && (unsigned char)decoded[i+1] == 0xA0))) {
                if (c == 0xC2) i++;
                if (p->len > 0) p->pending_space = true;
                continue;
            }
            if (p->pending_space) { emit(p, " ", 1); p->pending_space = false; }
            emit(p, (const char *)&decoded[i], 1);
        }
        s += chunk;
        n -= chunk;
    }
}

Doc doc_parse(const char *xhtml, const char *base_dir) {
    Doc doc = {0};
    Parser p = {0};
    p.doc  = &doc;
    p.base = base_dir;

    const char *cur = strcasestr(xhtml, "<body");
    if (cur) cur = strchr(cur, '>') ? strchr(cur, '>') + 1 : cur;
    else     cur = xhtml;

    while (*cur) {
        const char *lt = strchr(cur, '<');
        if (!lt) { text_run(&p, cur, (int)strlen(cur)); break; }
        if (lt > cur) text_run(&p, cur, (int)(lt - cur));

        if (lt[1] == '!' || lt[1] == '?') {          /* comment / doctype / CDATA */
            const char *gt = strncmp(lt, "<!--", 4) == 0 ? strstr(lt, "-->") : strchr(lt, '>');
            cur = gt ? gt + (strncmp(lt, "<!--", 4) == 0 ? 3 : 1) : lt + 1;
            continue;
        }

        const char *gt = strchr(lt, '>');
        if (!gt) break;
        const char *next = gt + 1;

        if (tag_is(lt, "script") || tag_is(lt, "style") || tag_is(lt, "head")) {
            char name[16];
            int i = 0;
            for (const char *q = lt + 1; i < 15 && isalpha((unsigned char)*q); q++) name[i++] = *q;
            name[i] = 0;
            cur = skip_element(lt, name);
            continue;
        }
        if (close_is(lt, "body")) break;

        if (lt[1] != '/') {
            char id[128] = "";
            /* An id on inline markup still anchors to its paragraph. */
            if (xml_attr(lt, "id", id, sizeof id) && *id) anchor_add(&p, id);
        }

        if (tag_is(lt, "br")) {
            int before = p.before;
            flush(&p);
            p.before = p.len == 0 ? 0 : before;
            p.before = 0;
            cur = next;
            continue;
        }
        if (tag_is(lt, "hr")) {
            flush(&p);
            Block *b = block_new(&p, BLK_RULE);
            b->before = 1;
            p.before = 1;
            cur = next;
            continue;
        }
        if (tag_is(lt, "img") || tag_is(lt, "image")) {
            char src[512] = "";
            if (!xml_attr(lt, "src", src, sizeof src))
                xml_attr(lt, "href", src, sizeof src);
            if (*src) {
                flush(&p);
                Block *b = block_new(&p, BLK_IMG);
                char full[1024];
                epub_resolve(p.base, src, full, sizeof full);
                b->src = strdup(full);
                b->before = 1;
                b->center = true;
                p.before = 1;
            }
            cur = next;
            continue;
        }

        for (int h = 1; h <= 6; h++) {
            char tag[3] = { 'h', (char)('0' + h), 0 };
            if (tag_is(lt, tag)) {
                flush(&p);
                p.before = 2;
                p.center = true;
                p.heading = true;
                p.bold++;
                doc_set_style(&p);
            } else if (close_is(lt, tag)) {
                flush(&p);
                if (p.bold > 0) p.bold--;
                doc_set_style(&p);
                p.center = false;
                p.heading = false;
                p.before = 1;
            }
        }

        if (tag_is(lt, "p") || tag_is(lt, "div") || tag_is(lt, "tr") ||
            tag_is(lt, "table") || tag_is(lt, "section")) {
            flush(&p);
            if (cls_has(lt, "cent")) p.center = true;
        } else if (close_is(lt, "p") || close_is(lt, "div") || close_is(lt, "tr") ||
                   close_is(lt, "table") || close_is(lt, "section")) {
            flush(&p);
            p.center = false;
        } else if (tag_is(lt, "blockquote")) {
            flush(&p);
            p.indent += 4;
        } else if (close_is(lt, "blockquote")) {
            flush(&p);
            p.indent -= 4;
            if (p.indent < 0) p.indent = 0;
        } else if (tag_is(lt, "ul") || tag_is(lt, "ol")) {
            flush(&p);
            if (p.list_depth < 7) {
                p.ol[p.list_depth] = tag_is(lt, "ol");
                p.ol_counter[p.list_depth] = 0;
                p.list_depth++;
                p.indent += 2;
            }
        } else if (close_is(lt, "ul") || close_is(lt, "ol")) {
            flush(&p);
            if (p.list_depth > 0) { p.list_depth--; p.indent -= 2; }
            if (p.indent < 0) p.indent = 0;
        } else if (tag_is(lt, "li")) {
            flush(&p);
            p.before = 0;
            int d = p.list_depth > 0 ? p.list_depth - 1 : 0;
            char bullet[16];
            if (p.ol[d]) snprintf(bullet, sizeof bullet, "%d. ", ++p.ol_counter[d]);
            else         snprintf(bullet, sizeof bullet, "• ");
            emit(&p, bullet, (int)strlen(bullet));
        } else if (close_is(lt, "li")) {
            flush(&p);
        } else if (tag_is(lt, "pre")) {
            flush(&p);
            p.pre = true;
        } else if (close_is(lt, "pre")) {
            flush(&p);
            p.pre = false;
        } else if (tag_is(lt, "b") || tag_is(lt, "strong")) {
            p.bold++; doc_set_style(&p);
        } else if (close_is(lt, "b") || close_is(lt, "strong")) {
            if (p.bold > 0) p.bold--; doc_set_style(&p);
        } else if (tag_is(lt, "i") || tag_is(lt, "em") || tag_is(lt, "cite") ||
                   tag_is(lt, "dfn") || tag_is(lt, "var")) {
            p.italic++; doc_set_style(&p);
        } else if (close_is(lt, "i") || close_is(lt, "em") || close_is(lt, "cite") ||
                   close_is(lt, "dfn") || close_is(lt, "var")) {
            if (p.italic > 0) p.italic--; doc_set_style(&p);
        } else if (tag_is(lt, "span")) {
            unsigned char applied = 0;
            if (cls_has(lt, "ital")) { p.italic++; applied |= DS_ITALIC; }
            if (cls_has(lt, "bold")) { p.bold++;   applied |= DS_BOLD; }
            if (p.span_depth < 32) p.span[p.span_depth++] = applied;
            doc_set_style(&p);
        } else if (close_is(lt, "span")) {
            if (p.span_depth > 0) {
                unsigned char applied = p.span[--p.span_depth];
                if ((applied & DS_ITALIC) && p.italic > 0) p.italic--;
                if ((applied & DS_BOLD) && p.bold > 0) p.bold--;
                doc_set_style(&p);
            }
        }

        cur = next;
    }
    flush(&p);
    free(p.buf);
    free(p.sty);
    return doc;
}

int doc_anchor_block(const Doc *d, const char *id) {
    for (int i = 0; i < d->nanchors; i++)
        if (strcmp(d->anchors[i].id, id) == 0) return d->anchors[i].block;
    return -1;
}

void doc_free(Doc *d) {
    for (int i = 0; i < d->nblocks; i++) {
        free(d->blocks[i].text);
        free(d->blocks[i].style);
        free(d->blocks[i].src);
    }
    free(d->blocks);
    for (int i = 0; i < d->nanchors; i++) free(d->anchors[i].id);
    free(d->anchors);
    memset(d, 0, sizeof *d);
}

#endif /* DOC_IMPLEMENTATION */
