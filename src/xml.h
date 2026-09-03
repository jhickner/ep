#ifndef XML_H
#define XML_H

#include <stdbool.h>
#include <stddef.h>

/* Just enough XML to walk an OPF/NCX/XHTML file: find tags by local name,
   pull attributes off them, and decode entities. No tree, no validation. */

const char *xml_find(const char *p, const char *local);
bool        xml_attr(const char *tag, const char *attr, char *out, size_t n);
bool        xml_text(const char *tag, char *out, size_t n);
size_t      xml_decode(const char *src, size_t len, char *out, size_t n);
void        url_decode(char *s);

#endif /* XML_H */

#ifdef XML_IMPLEMENTATION

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Namespace prefixes are ignored: <opf:item> matches "item". */
static const char *xml_local(const char *name, int *len) {
    const char *e = name;
    while (*e && !isspace((unsigned char)*e) && *e != '>' && *e != '/') e++;
    const char *colon = memchr(name, ':', (size_t)(e - name));
    if (colon) name = colon + 1;
    *len = (int)(e - name);
    return name;
}

const char *xml_find(const char *p, const char *local) {
    size_t want = strlen(local);
    while ((p = strchr(p, '<')) != NULL) {
        if (p[1] == '/' || p[1] == '!' || p[1] == '?') { p++; continue; }
        int len;
        const char *n = xml_local(p + 1, &len);
        if ((size_t)len == want && strncasecmp(n, local, want) == 0) return p;
        p++;
    }
    return NULL;
}

bool xml_attr(const char *tag, const char *attr, char *out, size_t n) {
    size_t alen = strlen(attr);
    const char *p = tag + 1;
    while (*p && *p != '>') {
        if (*p == '"' || *p == '\'') {          // skip a value
            char q = *p++;
            while (*p && *p != q) p++;
            if (*p) p++;
            continue;
        }
        if (isspace((unsigned char)*p)) {
            p++;
            const char *name = p;
            while (*p && !isspace((unsigned char)*p) && *p != '=' && *p != '>' && *p != '/') p++;
            size_t nlen = (size_t)(p - name);
            const char *colon = memchr(name, ':', nlen);
            if (colon) { nlen -= (size_t)(colon + 1 - name); name = colon + 1; }
            while (isspace((unsigned char)*p)) p++;
            if (*p != '=') continue;
            p++;
            while (isspace((unsigned char)*p)) p++;
            char q = 0;
            if (*p == '"' || *p == '\'') q = *p++;
            const char *val = p;
            while (*p && (q ? *p != q : (!isspace((unsigned char)*p) && *p != '>'))) p++;
            if (nlen == alen && strncasecmp(name, attr, alen) == 0) {
                xml_decode(val, (size_t)(p - val), out, n);
                return true;
            }
            if (*p == q && q) p++;
            continue;
        }
        p++;
    }
    return false;
}

/* Text content of a tag, up to its closing tag; nested markup is stripped. */
bool xml_text(const char *tag, char *out, size_t n) {
    const char *p = strchr(tag, '>');
    if (!p) return false;
    p++;
    const char *end = strchr(p, '<');
    if (!end) end = p + strlen(p);
    xml_decode(p, (size_t)(end - p), out, n);
    return true;
}

static const struct { const char *name; unsigned cp; } xml_ents[] = {
    { "amp", '&' },    { "lt", '<' },      { "gt", '>' },     { "quot", '"' },
    { "apos", '\'' },  { "nbsp", ' ' },    { "mdash", 0x2014 },{ "ndash", 0x2013 },
    { "hellip", 0x2026 },{ "lsquo", 0x2018 },{ "rsquo", 0x2019 },
    { "ldquo", 0x201C },{ "rdquo", 0x201D },{ "bull", 0x2022 },
    { "copy", 0x00A9 },{ "deg", 0x00B0 },  { "eacute", 0x00E9 },{ "egrave", 0x00E8 },
    { "agrave", 0x00E0 },{ "acirc", 0x00E2 },{ "ecirc", 0x00EA },{ "uuml", 0x00FC },
    { "ouml", 0x00F6 },{ "auml", 0x00E4 }, { "ccedil", 0x00E7 },{ "ntilde", 0x00F1 },
    { "iexcl", 0x00A1 },{ "middot", 0x00B7 },{ "shy", 0 },     { "thinsp", ' ' },
    { "ensp", ' ' },   { "emsp", ' ' },    { "times", 0x00D7 },{ "prime", 0x2032 },
    { "dagger", 0x2020 },{ "sect", 0x00A7 },{ "para", 0x00B6 },{ "trade", 0x2122 },
    { "reg", 0x00AE },  { "pound", 0x00A3 },{ "euro", 0x20AC },{ "frac12", 0x00BD },
};

static int utf8_put(unsigned cp, char *out) {
    if (cp == 0) return 0;
    if (cp < 0x80) { out[0] = (char)cp; return 1; }
    if (cp < 0x800) {
        out[0] = (char)(0xC0 | cp >> 6); out[1] = (char)(0x80 | (cp & 0x3F)); return 2;
    }
    if (cp < 0x10000) {
        out[0] = (char)(0xE0 | cp >> 12);
        out[1] = (char)(0x80 | (cp >> 6 & 0x3F));
        out[2] = (char)(0x80 | (cp & 0x3F));
        return 3;
    }
    out[0] = (char)(0xF0 | cp >> 18);
    out[1] = (char)(0x80 | (cp >> 12 & 0x3F));
    out[2] = (char)(0x80 | (cp >> 6 & 0x3F));
    out[3] = (char)(0x80 | (cp & 0x3F));
    return 4;
}

size_t xml_decode(const char *src, size_t len, char *out, size_t n) {
    size_t o = 0;
    for (size_t i = 0; i < len && o + 5 < n; i++) {
        if (src[i] != '&') { out[o++] = src[i]; continue; }
        const char *semi = memchr(src + i, ';', len - i > 12 ? 12 : len - i);
        if (!semi) { out[o++] = '&'; continue; }
        size_t elen = (size_t)(semi - (src + i)) - 1;
        const char *e = src + i + 1;
        unsigned cp = 0;
        if (elen > 1 && e[0] == '#') {
            cp = (unsigned)strtoul(e[1] == 'x' || e[1] == 'X' ? e + 2 : e + 1, NULL,
                                   e[1] == 'x' || e[1] == 'X' ? 16 : 10);
        } else {
            for (size_t k = 0; k < sizeof xml_ents / sizeof *xml_ents; k++) {
                if (strlen(xml_ents[k].name) == elen &&
                    strncmp(e, xml_ents[k].name, elen) == 0) { cp = xml_ents[k].cp; break; }
            }
            if (!cp && strncmp(e, "shy", elen) != 0) { out[o++] = '&'; continue; }
        }
        o += (size_t)utf8_put(cp, out + o);
        i += elen + 1;
    }
    out[o < n ? o : n - 1] = 0;
    return o;
}

void url_decode(char *s) {
    char *w = s;
    for (char *r = s; *r; r++) {
        if (*r == '%' && isxdigit((unsigned char)r[1]) && isxdigit((unsigned char)r[2])) {
            char hex[3] = { r[1], r[2], 0 };
            *w++ = (char)strtol(hex, NULL, 16);
            r += 2;
        } else {
            *w++ = *r;
        }
    }
    *w = 0;
}

#endif /* XML_IMPLEMENTATION */
