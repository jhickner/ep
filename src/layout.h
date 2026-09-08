#ifndef LAYOUT_H
#define LAYOUT_H

#include <stdbool.h>

#ifndef DOC_H
#include "doc.h"
#endif

typedef struct {
    const char          *text;
    const unsigned char *style;
    int  len;
    int  indent;
    bool center;
    int  block;
    int  type;
    int  img_row;
    int  img_rows;
} Line;

typedef struct { Line *lines; int n, cap; } Layout;

typedef int (*LayoutImgRows)(const Block *b, int width, void *ctx);

Layout layout_doc(const Doc *d, int width, LayoutImgRows img_rows, void *ctx);
void   layout_free(Layout *l);
int    u8_cols(const char *s, int len);

#endif

#ifdef LAYOUT_IMPLEMENTATION

#include <stdlib.h>
#include <string.h>

int u8_cols(const char *s, int len) {
    int n = 0;
    for (int i = 0; i < len; i++)
        if (((unsigned char)s[i] & 0xC0) != 0x80) n++;
    return n;
}

static void push(Layout *l, Line ln) {
    if (l->n == l->cap) {
        l->cap = l->cap ? l->cap * 2 : 256;
        l->lines = realloc(l->lines, (size_t)l->cap * sizeof(Line));
    }
    l->lines[l->n++] = ln;
}

static void push_blank(Layout *l) {
    push(l, (Line){ .text = "", .style = NULL, .len = 0, .block = -1,
                    .type = BLK_TEXT, .img_row = -1 });
}

Layout layout_doc(const Doc *d, int width, LayoutImgRows img_rows, void *ctx) {
    Layout l = {0};
    if (width < 4) width = 4;

    for (int bi = 0; bi < d->nblocks; bi++) {
        const Block *b = &d->blocks[bi];

        for (int i = 0; i < b->before && l.n > 0; i++) push_blank(&l);

        if (b->type == BLK_RULE) {
            push(&l, (Line){ .text = NULL, .len = 0, .block = bi,
                             .type = BLK_RULE, .img_row = -1, .center = true });
            continue;
        }
        if (b->type == BLK_IMG) {
            int rows = img_rows ? img_rows(b, width, ctx) : 1;
            if (rows < 1) rows = 1;
            for (int r = 0; r < rows; r++)
                push(&l, (Line){ .text = b->src, .len = b->src ? (int)strlen(b->src) : 0,
                                 .block = bi, .type = BLK_IMG, .img_row = r,
                                 .img_rows = rows, .center = true });
            continue;
        }

        int indent = b->indent;
        int avail  = width - indent;
        if (avail < 8) { indent = 0; avail = width; }

        int start = 0;
        while (start < b->len) {
            while (start < b->len && b->text[start] == ' ') start++;
            if (start >= b->len) break;

            int i = start, cols = 0, last_space = -1;
            while (i < b->len && cols < avail) {
                if (b->text[i] == ' ') last_space = i;
                int adv = 1;
                unsigned char c = (unsigned char)b->text[i];
                if      ((c & 0xF8) == 0xF0) adv = 4;
                else if ((c & 0xF0) == 0xE0) adv = 3;
                else if ((c & 0xE0) == 0xC0) adv = 2;
                i += adv;
                cols++;
            }

            int end;
            if (i >= b->len)                 end = b->len;
            else if (b->text[i] == ' ')      end = i;
            else if (last_space > start)     end = last_space;
            else                             end = i;

            int trim = end;
            while (trim > start && b->text[trim - 1] == ' ') trim--;

            push(&l, (Line){ .text = b->text + start, .style = b->style + start,
                             .len = trim - start, .indent = indent,
                             .center = b->center, .block = bi, .type = BLK_TEXT,
                             .img_row = -1 });
            start = end;
        }
    }
    return l;
}

void layout_free(Layout *l) {
    free(l->lines);
    l->lines = NULL;
    l->n = l->cap = 0;
}

#endif
