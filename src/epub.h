#ifndef EPUB_H
#define EPUB_H

#include <stdbool.h>
#include <stddef.h>

#ifndef ZIP_H
#include "zip.h"
#endif

typedef struct {
    char *id, *href, *type, *props;
} EpubItem;

typedef struct {
    char *title;
    char *href;
    char *anchor;
    int   spine;
    int   depth;
} EpubTocEntry;

typedef struct {
    Zip   zip;
    char  root[512];
    char  title[256], author[256];
    char *cover;

    EpubItem *items;   int nitems;
    char    **spine;   int nspine;

    EpubTocEntry *toc; int ntoc;
} Epub;

bool  epub_open(Epub *e, const char *path);
void  epub_close(Epub *e);
void *epub_read(Epub *e, const char *zip_path, size_t *len);
void  epub_resolve(const char *base_dir, const char *href, char *out, size_t n);

#endif

#ifdef EPUB_IMPLEMENTATION

#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdio.h>

#ifndef XML_H
#include "xml.h"
#endif

void *epub_read(Epub *e, const char *zip_path, size_t *len) {
    return zip_read(&e->zip, zip_path, len);
}

void epub_resolve(const char *base_dir, const char *href, char *out, size_t n) {
    char tmp[1024];
    if (href[0] == '/') snprintf(tmp, sizeof tmp, "%s", href + 1);
    else                snprintf(tmp, sizeof tmp, "%s%s", base_dir, href);
    url_decode(tmp);

    char *parts[64];
    int   np = 0;
    for (char *tok = strtok(tmp, "/"); tok && np < 64; tok = strtok(NULL, "/")) {
        if (strcmp(tok, ".") == 0) continue;
        if (strcmp(tok, "..") == 0) { if (np) np--; continue; }
        parts[np++] = tok;
    }
    size_t o = 0;
    out[0] = 0;
    for (int i = 0; i < np; i++)
        o += (size_t)snprintf(out + o, o < n ? n - o : 0, "%s%s", i ? "/" : "", parts[i]);
}

static char *xdup(const char *s) { return s ? strdup(s) : NULL; }

static const char *item_href(Epub *e, const char *id) {
    for (int i = 0; i < e->nitems; i++)
        if (e->items[i].id && strcmp(e->items[i].id, id) == 0) return e->items[i].href;
    return NULL;
}

static int spine_index(Epub *e, const char *zip_path) {
    for (int i = 0; i < e->nspine; i++)
        if (strcmp(e->spine[i], zip_path) == 0) return i;
    return -1;
}

static void toc_push(Epub *e, const char *title, const char *href, int depth) {
    char full[1024];
    char rel[1024];
    snprintf(rel, sizeof rel, "%s", href);
    char *hash = strchr(rel, '#');
    char *anchor = NULL;
    if (hash) { *hash = 0; anchor = hash + 1; }
    epub_resolve(e->root, rel, full, sizeof full);

    e->toc = realloc(e->toc, (size_t)(e->ntoc + 1) * sizeof *e->toc);
    EpubTocEntry *t = &e->toc[e->ntoc++];
    t->title  = xdup(title);
    t->href   = xdup(full);
    t->anchor = anchor && *anchor ? xdup(anchor) : NULL;
    t->depth  = depth;
    t->spine  = spine_index(e, full);
}

static void parse_ncx(Epub *e, const char *xml) {
    int depth = 0;
    const char *p = xml;
    while (*p) {
        const char *lt = strchr(p, '<');
        if (!lt) break;
        if (strncasecmp(lt, "</navPoint", 10) == 0) { if (depth) depth--; p = lt + 1; continue; }
        if (strncasecmp(lt, "<navPoint", 9) == 0) {
            const char *lbl = xml_find(lt, "navLabel");
            const char *txt = lbl ? xml_find(lbl, "text") : NULL;
            const char *con = xml_find(lt, "content");
            char title[256] = "", src[512] = "";
            if (txt) xml_text(txt, title, sizeof title);
            if (con) xml_attr(con, "src", src, sizeof src);
            if (*src) toc_push(e, title, src, depth);
            depth++;
            p = lt + 1;
            continue;
        }
        p = lt + 1;
    }
}

static void parse_nav(Epub *e, const char *xml) {
    const char *nav = xml;
    for (;;) {
        nav = xml_find(nav, "nav");
        if (!nav) return;
        char type[64] = "";
        xml_attr(nav, "type", type, sizeof type);
        if (!*type || strstr(type, "toc")) break;
        nav++;
    }
    int depth = -1;
    const char *p = nav;
    while (*p) {
        const char *lt = strchr(p, '<');
        if (!lt) break;
        if (strncasecmp(lt, "</nav", 5) == 0) break;
        if (strncasecmp(lt, "<ol", 3) == 0)  depth++;
        if (strncasecmp(lt, "</ol", 4) == 0) depth--;
        if (strncasecmp(lt, "<a", 2) == 0 && (lt[2] == ' ' || lt[2] == '>')) {
            char href[512] = "", title[256] = "";
            xml_attr(lt, "href", href, sizeof href);

            const char *gt = strchr(lt, '>');
            const char *end = gt ? strcasestr(gt, "</a") : NULL;
            if (gt && end) {
                char raw[1024];
                size_t o = 0;
                for (const char *q = gt + 1; q < end && o + 1 < sizeof raw; q++) {
                    if (*q == '<') { while (q < end && *q != '>') q++; continue; }
                    raw[o++] = *q;
                }
                raw[o] = 0;
                xml_decode(raw, o, title, sizeof title);
            }
            char *t = title;
            while (*t == ' ' || *t == '\n' || *t == '\t' || *t == '\r') t++;
            for (char *z = t + strlen(t); z > t && (z[-1] == ' ' || z[-1] == '\n' ||
                                                    z[-1] == '\t' || z[-1] == '\r'); z--) z[-1] = 0;
            if (*href) toc_push(e, t, href, depth < 0 ? 0 : depth);
        }
        p = lt + 1;
    }
}

static void parse_opf(Epub *e, const char *opf_path, char *xml) {
    snprintf(e->root, sizeof e->root, "%s", opf_path);
    char *slash = strrchr(e->root, '/');
    if (slash) slash[1] = 0; else e->root[0] = 0;

    const char *t = xml_find(xml, "title");
    if (t) xml_text(t, e->title, sizeof e->title);
    const char *a = xml_find(xml, "creator");
    if (a) xml_text(a, e->author, sizeof e->author);

    char cover_id[128] = "";
    for (const char *m = xml; (m = xml_find(m, "meta")) != NULL; m++) {
        char name[64] = "";
        xml_attr(m, "name", name, sizeof name);
        if (strcasecmp(name, "cover") == 0) {
            xml_attr(m, "content", cover_id, sizeof cover_id);
            break;
        }
    }

    const char *man = xml_find(xml, "manifest");
    const char *spn = xml_find(xml, "spine");
    if (man) {
        const char *stop = spn && spn > man ? spn : NULL;
        for (const char *it = man; (it = xml_find(it, "item")) != NULL; it++) {
            if (stop && it >= stop) break;
            char id[128] = "", href[512] = "", type[128] = "", props[128] = "";
            xml_attr(it, "id", id, sizeof id);
            xml_attr(it, "href", href, sizeof href);
            xml_attr(it, "media-type", type, sizeof type);
            xml_attr(it, "properties", props, sizeof props);
            if (!*href) continue;
            char full[1024];
            epub_resolve(e->root, href, full, sizeof full);
            e->items = realloc(e->items, (size_t)(e->nitems + 1) * sizeof *e->items);
            EpubItem *item = &e->items[e->nitems++];
            item->id = xdup(id); item->href = strdup(full);
            item->type = xdup(type); item->props = xdup(props);
        }
    }

    if (spn) {
        for (const char *ir = spn; (ir = xml_find(ir, "itemref")) != NULL; ir++) {
            char idref[128] = "", linear[16] = "";
            xml_attr(ir, "idref", idref, sizeof idref);
            xml_attr(ir, "linear", linear, sizeof linear);
            if (strcasecmp(linear, "no") == 0) continue;
            const char *href = item_href(e, idref);
            if (!href) continue;
            e->spine = realloc(e->spine, (size_t)(e->nspine + 1) * sizeof *e->spine);
            e->spine[e->nspine++] = strdup(href);
        }
    }

    if (*cover_id) {
        const char *href = item_href(e, cover_id);
        if (href) e->cover = strdup(href);
    }
    if (!e->cover) {
        for (int i = 0; i < e->nitems; i++) {
            if (e->items[i].props && strstr(e->items[i].props, "cover-image")) {
                e->cover = strdup(e->items[i].href);
                break;
            }
        }
    }

    char *doc = NULL;
    for (int i = 0; i < e->nitems && !e->ntoc; i++) {
        if (!e->items[i].props || !strstr(e->items[i].props, "nav")) continue;
        doc = epub_read(e, e->items[i].href, NULL);
        if (doc) { parse_nav(e, doc); free(doc); }
    }
    if (!e->ntoc) {
        char ncx_id[128] = "";
        if (spn) xml_attr(spn, "toc", ncx_id, sizeof ncx_id);
        const char *ncx = *ncx_id ? item_href(e, ncx_id) : NULL;
        if (!ncx) {
            for (int i = 0; i < e->nitems; i++)
                if (e->items[i].type && strstr(e->items[i].type, "dtbncx")) {
                    ncx = e->items[i].href; break;
                }
        }
        if (ncx && (doc = epub_read(e, ncx, NULL)) != NULL) {
            parse_ncx(e, doc);
            free(doc);
        }
    }
}

bool epub_open(Epub *e, const char *path) {
    memset(e, 0, sizeof *e);
    if (!zip_open(&e->zip, path)) return false;

    char *container = epub_read(e, "META-INF/container.xml", NULL);
    char opf_path[512] = "";
    if (container) {
        const char *rf = xml_find(container, "rootfile");
        if (rf) xml_attr(rf, "full-path", opf_path, sizeof opf_path);
        free(container);
    }
    if (!*opf_path) {
        for (int i = 0; i < e->zip.count; i++) {
            const char *n = e->zip.entries[i].name;
            size_t l = strlen(n);
            if (l > 4 && strcasecmp(n + l - 4, ".opf") == 0) {
                snprintf(opf_path, sizeof opf_path, "%s", n);
                break;
            }
        }
    }
    url_decode(opf_path);
    char *opf = *opf_path ? epub_read(e, opf_path, NULL) : NULL;
    if (!opf) { epub_close(e); return false; }
    parse_opf(e, opf_path, opf);
    free(opf);

    if (e->nspine == 0) { epub_close(e); return false; }
    return true;
}

void epub_close(Epub *e) {
    for (int i = 0; i < e->nitems; i++) {
        free(e->items[i].id); free(e->items[i].href);
        free(e->items[i].type); free(e->items[i].props);
    }
    free(e->items);
    for (int i = 0; i < e->nspine; i++) free(e->spine[i]);
    free(e->spine);
    for (int i = 0; i < e->ntoc; i++) {
        free(e->toc[i].title); free(e->toc[i].href); free(e->toc[i].anchor);
    }
    free(e->toc);
    free(e->cover);
    zip_close(&e->zip);
    memset(e, 0, sizeof *e);
}

#endif
