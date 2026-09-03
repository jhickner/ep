#ifndef ZIP_H
#define ZIP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

typedef struct {
    char    *name;
    uint16_t method;
    uint32_t comp_size, uncomp_size, local_off;
} ZipEntry;

typedef struct {
    FILE     *f;
    ZipEntry *entries;
    int       count;
} Zip;

bool  zip_open(Zip *z, const char *path);
void  zip_close(Zip *z);
int   zip_find(const Zip *z, const char *name);
void *zip_read(Zip *z, const char *name, size_t *out_len);

#endif /* ZIP_H */

#ifdef ZIP_IMPLEMENTATION

#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <zlib.h>

static uint16_t zip_r16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t zip_r32(const uint8_t *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

bool zip_open(Zip *z, const char *path) {
    memset(z, 0, sizeof *z);
    z->f = fopen(path, "rb");
    if (!z->f) return false;

    if (fseek(z->f, 0, SEEK_END) != 0) goto fail;
    long fsize = ftell(z->f);
    if (fsize < 22) goto fail;

    // The end-of-central-directory record sits within the last 64K + 22 bytes.
    long tail = fsize < 66000 ? fsize : 66000;
    uint8_t *buf = malloc((size_t)tail);
    if (!buf) goto fail;
    fseek(z->f, fsize - tail, SEEK_SET);
    if (fread(buf, 1, (size_t)tail, z->f) != (size_t)tail) { free(buf); goto fail; }

    long eocd = -1;
    for (long i = tail - 22; i >= 0; i--) {
        if (buf[i] == 'P' && buf[i+1] == 'K' && buf[i+2] == 5 && buf[i+3] == 6) { eocd = i; break; }
    }
    if (eocd < 0) { free(buf); goto fail; }

    int      count = zip_r16(buf + eocd + 10);
    uint32_t cd_size = zip_r32(buf + eocd + 12);
    uint32_t cd_off  = zip_r32(buf + eocd + 16);
    free(buf);
    if (count <= 0 || cd_size == 0) goto fail;

    uint8_t *cd = malloc(cd_size);
    if (!cd) goto fail;
    fseek(z->f, (long)cd_off, SEEK_SET);
    if (fread(cd, 1, cd_size, z->f) != cd_size) { free(cd); goto fail; }

    z->entries = calloc((size_t)count, sizeof(ZipEntry));
    if (!z->entries) { free(cd); goto fail; }

    uint32_t p = 0;
    for (int i = 0; i < count && p + 46 <= cd_size; i++) {
        if (memcmp(cd + p, "PK\x01\x02", 4) != 0) break;
        uint16_t nlen = zip_r16(cd + p + 28);
        uint16_t xlen = zip_r16(cd + p + 30);
        uint16_t clen = zip_r16(cd + p + 32);
        if (p + 46 + nlen > cd_size) break;

        ZipEntry *e = &z->entries[z->count];
        e->method      = zip_r16(cd + p + 10);
        e->comp_size   = zip_r32(cd + p + 20);
        e->uncomp_size = zip_r32(cd + p + 24);
        e->local_off   = zip_r32(cd + p + 42);
        e->name        = malloc(nlen + 1u);
        if (!e->name) break;
        memcpy(e->name, cd + p + 46, nlen);
        e->name[nlen] = 0;
        z->count++;
        p += 46u + nlen + xlen + clen;
    }
    free(cd);
    if (z->count == 0) goto fail;
    return true;

fail:
    zip_close(z);
    return false;
}

void zip_close(Zip *z) {
    for (int i = 0; i < z->count; i++) free(z->entries[i].name);
    free(z->entries);
    if (z->f) fclose(z->f);
    memset(z, 0, sizeof *z);
}

int zip_find(const Zip *z, const char *name) {
    for (int i = 0; i < z->count; i++)
        if (strcmp(z->entries[i].name, name) == 0) return i;
    // Some producers differ only in case, or leave a leading "./".
    for (int i = 0; i < z->count; i++)
        if (strcasecmp(z->entries[i].name, name) == 0) return i;
    return -1;
}

void *zip_read(Zip *z, const char *name, size_t *out_len) {
    int idx = zip_find(z, name);
    if (idx < 0) return NULL;
    ZipEntry *e = &z->entries[idx];

    uint8_t hdr[30];
    fseek(z->f, (long)e->local_off, SEEK_SET);
    if (fread(hdr, 1, 30, z->f) != 30 || memcmp(hdr, "PK\x03\x04", 4) != 0) return NULL;
    long data = (long)e->local_off + 30 + zip_r16(hdr + 26) + zip_r16(hdr + 28);

    uint8_t *comp = malloc(e->comp_size ? e->comp_size : 1);
    if (!comp) return NULL;
    fseek(z->f, data, SEEK_SET);
    if (fread(comp, 1, e->comp_size, z->f) != e->comp_size) { free(comp); return NULL; }

    uint8_t *out = malloc((size_t)e->uncomp_size + 1);
    if (!out) { free(comp); return NULL; }

    if (e->method == 0) {
        memcpy(out, comp, e->uncomp_size);
    } else if (e->method == 8) {
        z_stream s = {0};
        s.next_in = comp; s.avail_in = e->comp_size;
        s.next_out = out; s.avail_out = e->uncomp_size;
        if (inflateInit2(&s, -MAX_WBITS) != Z_OK) { free(comp); free(out); return NULL; }
        int rc = inflate(&s, Z_FINISH);
        inflateEnd(&s);
        if (rc != Z_STREAM_END && s.total_out != e->uncomp_size) {
            free(comp); free(out); return NULL;
        }
    } else {
        free(comp); free(out); return NULL;
    }

    free(comp);
    out[e->uncomp_size] = 0;
    if (out_len) *out_len = e->uncomp_size;
    return out;
}

#endif /* ZIP_IMPLEMENTATION */
