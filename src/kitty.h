
#ifndef KITTY_H
#define KITTY_H

#include <stdint.h>
#include <stdbool.h>

#ifndef TERM_H
#include "term.h"
#endif

#define KG_PLACEHOLDER_CP 0x10EEEEu

#define KG_MAX_ID_24BIT 0xFFFFFFu

int kg_max_rowcolumn(void);

void kg_init(void);

void kg_set_passthrough(bool on);

bool kg_passthrough(void);

void kg_set_tempfile(bool on);
bool kg_tempfile_enabled(void);

void kg_cleanup_tempfiles(void);

void kg_sweep_stale_tempfiles(int age_secs);

void kg_placeholder_redraw_begin(void);
void kg_placeholder_redraw_end(void);

bool kg_tmux_allow_passthrough(void);

void kg_transmit(uint32_t id, const uint8_t *rgb, int w, int h);

void kg_transmit_ex(uint32_t id, const uint8_t *px, int w, int h, int channels);

void kg_virtual_place(uint32_t id, int cols, int rows);

void kg_placeholder_cell(uint32_t id, int row, int col);

void kg_fit_cells(int img_w, int img_h, int cell_w, int cell_h,
                  int max_cols, int max_rows,
                  int *out_cols, int *out_rows, int *px_w, int *px_h);

void kg_place(uint32_t id, uint32_t pid, int col, int row, int x_off, int y_off);

void kg_clear_placements(void);

void kg_clear_placement(uint32_t id, uint32_t pid);

void kg_delete(uint32_t id);

void kg_delete_all(void);

bool kg_supported(void);

int kg_probe(int timeout_ms);

#endif

#ifdef KITTY_IMPLEMENTATION

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <dirent.h>
#include <sys/stat.h>
#ifdef PIX_HAVE_ZLIB
#include <zlib.h>
#endif
#include <sys/select.h>
#include <sys/time.h>

static const uint32_t kg_diacritics[] = {
    0x0305,0x030D,0x030E,0x0310,0x0312,0x033D,0x033E,0x033F,
    0x0346,0x034A,0x034B,0x034C,0x0350,0x0351,0x0352,0x0357,
    0x035B,0x0363,0x0364,0x0365,0x0366,0x0367,0x0368,0x0369,
    0x036A,0x036B,0x036C,0x036D,0x036E,0x036F,0x0483,0x0484,
    0x0485,0x0486,0x0487,0x0592,0x0593,0x0594,0x0595,0x0597,
    0x0598,0x0599,0x059C,0x059D,0x059E,0x059F,0x05A0,0x05A1,
    0x05A8,0x05A9,0x05AB,0x05AC,0x05AF,0x05C4,0x0610,0x0611,
    0x0612,0x0613,0x0614,0x0615,0x0616,0x0617,0x0657,0x0658,
    0x0659,0x065A,0x065B,0x065D,0x065E,0x06D6,0x06D7,0x06D8,
    0x06D9,0x06DA,0x06DB,0x06DC,0x06DF,0x06E0,0x06E1,0x06E2,
    0x06E4,0x06E7,0x06E8,0x06EB,0x06EC,0x0730,0x0732,0x0733,
    0x0735,0x0736,0x073A,0x073D,0x073F,0x0740,0x0741,0x0743,
    0x0745,0x0747,0x0749,0x074A,0x07EB,0x07EC,0x07ED,0x07EE,
    0x07EF,0x07F0,0x07F1,0x07F3,0x0816,0x0817,0x0818,0x0819,
    0x081B,0x081C,0x081D,0x081E,0x081F,0x0820,0x0821,0x0822,
    0x0823,0x0825,0x0826,0x0827,0x0829,0x082A,0x082B,0x082C,
    0x082D,0x0951,0x0953,0x0954,0x0F82,0x0F83,0x0F86,0x0F87,
    0x135D,0x135E,0x135F,0x17DD,0x193A,0x1A17,0x1A75,0x1A76,
    0x1A77,0x1A78,0x1A79,0x1A7A,0x1A7B,0x1A7C,0x1B6B,0x1B6D,
    0x1B6E,0x1B6F,0x1B70,0x1B71,0x1B72,0x1B73,0x1CD0,0x1CD1,
    0x1CD2,0x1CDA,0x1CDB,0x1CE0,0x1DC0,0x1DC1,0x1DC3,0x1DC4,
    0x1DC5,0x1DC6,0x1DC7,0x1DC8,0x1DC9,0x1DCB,0x1DCC,0x1DD1,
    0x1DD2,0x1DD3,0x1DD4,0x1DD5,0x1DD6,0x1DD7,0x1DD8,0x1DD9,
    0x1DDA,0x1DDB,0x1DDC,0x1DDD,0x1DDE,0x1DDF,0x1DE0,0x1DE1,
    0x1DE2,0x1DE3,0x1DE4,0x1DE5,0x1DE6,0x1DFE,0x20D0,0x20D1,
    0x20D4,0x20D5,0x20D6,0x20D7,0x20DB,0x20DC,0x20E1,0x20E7,
    0x20E9,0x20F0,0x2CEF,0x2CF0,0x2CF1,0x2DE0,0x2DE1,0x2DE2,
    0x2DE3,0x2DE4,0x2DE5,0x2DE6,0x2DE7,0x2DE8,0x2DE9,0x2DEA,
    0x2DEB,0x2DEC,0x2DED,0x2DEE,0x2DEF,0x2DF0,0x2DF1,0x2DF2,
    0x2DF3,0x2DF4,0x2DF5,0x2DF6,0x2DF7,0x2DF8,0x2DF9,0x2DFA,
    0x2DFB,0x2DFC,0x2DFD,0x2DFE,0x2DFF,0xA66F,0xA67C,0xA67D,
    0xA6F0,0xA6F1,0xA8E0,0xA8E1,0xA8E2,0xA8E3,0xA8E4,0xA8E5,
    0xA8E6,0xA8E7,0xA8E8,0xA8E9,0xA8EA,0xA8EB,0xA8EC,0xA8ED,
    0xA8EE,0xA8EF,0xA8F0,0xA8F1,0xAAB0,0xAAB2,0xAAB3,0xAAB7,
    0xAAB8,0xAABE,0xAABF,0xAAC1,0xFE20,0xFE21,0xFE22,0xFE23,
    0xFE24,0xFE25,0xFE26,0x10A0F,0x10A38,0x1D185,0x1D186,0x1D187,
    0x1D188,0x1D189,0x1D1AA,0x1D1AB,0x1D1AC,0x1D1AD,0x1D242,0x1D243,
    0x1D244,
};

#define KG_DIACRITIC_COUNT ((int)(sizeof kg_diacritics / sizeof kg_diacritics[0]))

int kg_max_rowcolumn(void) { return KG_DIACRITIC_COUNT - 1; }

static bool kg_wrap = false;
static bool kg_tempfile = false;

void kg_set_passthrough(bool on) { kg_wrap = on; }
bool kg_passthrough(void) { return kg_wrap; }

void kg_set_tempfile(bool on) { kg_tempfile = on; }
bool kg_tempfile_enabled(void) { return kg_tempfile; }

void kg_init(void) {
    kg_wrap = getenv("TMUX") != NULL;

    kg_tempfile = getenv("SSH_CONNECTION") == NULL && getenv("SSH_TTY") == NULL &&
                  getenv("SSH_CLIENT") == NULL;
}

void kg_placeholder_redraw_begin(void) {
    term_write_n("\x1b[?2026h", 8);
}

void kg_placeholder_redraw_end(void) {
    term_write_n("\x1b[?2026l", 8);
}

bool kg_tmux_allow_passthrough(void) {
    if (!getenv("TMUX")) return true;

    return system("tmux set -p allow-passthrough all >/dev/null 2>&1") == 0;
}

#define KG_WRAP_MAX 262144

static int kg_batch_depth = 0;
static bool kg_batch_open = false;
static size_t kg_batch_len = 0;

static void kg_emit_escaped(const char *seq, int n) {
    int run = 0;
    for (int i = 0; i < n; i++) {
        if (seq[i] == '\x1b') {
            if (run) term_write_n(seq + i - run, run);
            run = 0;
            term_write_n("\x1b\x1b", 2);
        } else {
            run++;
        }
    }
    if (run) term_write_n(seq + n - run, run);
}

static void kg_emit(const char *seq, int n) {
    if (!kg_wrap) {
        term_write_n(seq, n);
        return;
    }
    if (kg_batch_depth) {
        if (kg_batch_open && kg_batch_len + (size_t)n > KG_WRAP_MAX) {
            term_write("\x1b\\");
            kg_batch_open = false;
        }
        if (!kg_batch_open) {
            term_write("\x1bPtmux;");
            kg_batch_open = true;
            kg_batch_len = 0;
        }
        kg_emit_escaped(seq, n);
        kg_batch_len += (size_t)n;
        return;
    }
    term_write("\x1bPtmux;");
    kg_emit_escaped(seq, n);
    term_write("\x1b\\");
}

static void kg_batch_begin(void) { kg_batch_depth++; }

static void kg_batch_end(void) {
    if (kg_batch_depth > 0 && --kg_batch_depth > 0) return;
    kg_batch_depth = 0;

    if (kg_batch_open) term_write("\x1b\\");
    kg_batch_open = false;
    kg_batch_len = 0;
}

#define kg_emit_lit(s) kg_emit((s), (int)(sizeof(s) - 1))

static const char KG_B64[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static void kg_b64_group(const uint8_t *src, int n, char *dst) {
    uint32_t v = (uint32_t)src[0] << 16;
    if (n > 1) v |= (uint32_t)src[1] << 8;
    if (n > 2) v |= (uint32_t)src[2];
    dst[0] = KG_B64[(v >> 18) & 0x3F];
    dst[1] = KG_B64[(v >> 12) & 0x3F];
    dst[2] = n > 1 ? KG_B64[(v >> 6) & 0x3F] : '=';
    dst[3] = n > 2 ? KG_B64[v & 0x3F] : '=';
}

#define KG_CHUNK 4096

#define KG_TEMPFILE_MIN 32768

#define KG_TEMPFILE_TRACK 512

static char *kg_tempfiles[KG_TEMPFILE_TRACK];
static int kg_tempfile_next = 0;

void kg_cleanup_tempfiles(void) {
    for (int i = 0; i < KG_TEMPFILE_TRACK; i++) {
        if (!kg_tempfiles[i]) continue;
        unlink(kg_tempfiles[i]);
        free(kg_tempfiles[i]);
        kg_tempfiles[i] = NULL;
    }
}

void kg_sweep_stale_tempfiles(int age_secs) {
    const char *dir = getenv("TMPDIR");
    if (!dir || !*dir) dir = "/tmp";

    DIR *d = opendir(dir);
    if (!d) return;

    time_t cutoff = time(NULL) - age_secs;
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {

        if (strncmp(de->d_name, "pix-", 4) != 0 || strlen(de->d_name) != 10) continue;

        char path[1024];
        if (snprintf(path, sizeof path, "%s/%s", dir, de->d_name) >= (int)sizeof path)
            continue;
        struct stat st;
        if (stat(path, &st) == 0 && S_ISREG(st.st_mode) && st.st_mtime < cutoff)
            unlink(path);
    }
    closedir(d);
}

static void kg_track_tempfile(char *path) {
    char **slot = &kg_tempfiles[kg_tempfile_next];
    kg_tempfile_next = (kg_tempfile_next + 1) % KG_TEMPFILE_TRACK;
    if (*slot) { unlink(*slot); free(*slot); }
    *slot = path;
}

static int kg_b64(const uint8_t *src, size_t n, char *dst) {
    int len = 0;
    for (size_t i = 0; i < n; i += 3) {
        int g = (int)(n - i);
        if (g > 3) g = 3;
        kg_b64_group(src + i, g, dst + len);
        len += 4;
    }
    return len;
}

static void kg_transmit_direct(uint32_t id, const uint8_t *data, size_t total,
                               int w, int h, int channels, const char *extra) {
    size_t bytes_per_chunk = (KG_CHUNK / 4) * 3;

    char buf[128 + KG_CHUNK + 2];
    bool first = true;

    kg_batch_begin();
    for (size_t off = 0; off < total; off += bytes_per_chunk) {
        size_t n = total - off;
        if (n > bytes_per_chunk) n = bytes_per_chunk;
        bool last = (off + n >= total);

        int len;
        if (first) {
            len = snprintf(buf, sizeof buf,
                           "\x1b_Ga=t,f=%d,s=%d,v=%d,i=%u,q=2%s,m=%d;",
                           channels == 4 ? 32 : 24, w, h, id, extra, last ? 0 : 1);
            first = false;
        } else {
            len = snprintf(buf, sizeof buf, "\x1b_Gm=%d,q=2;", last ? 0 : 1);
        }

        len += kg_b64(data + off, n, buf + len);
        buf[len++] = '\x1b';
        buf[len++] = '\\';
        kg_emit(buf, len);
    }
    kg_batch_end();
}

static bool kg_transmit_tempfile(uint32_t id, const uint8_t *px, size_t total,
                                 int w, int h, int channels) {
    const char *dir = getenv("TMPDIR");
    if (!dir || !*dir) dir = "/tmp";

    size_t dlen = strlen(dir);
    while (dlen > 1 && dir[dlen - 1] == '/') dlen--;

    char *path = (char *)malloc(dlen + 24);
    if (!path) return false;
    memcpy(path, dir, dlen);
    memcpy(path + dlen, "/pix-XXXXXX", 12);

    int fd = mkstemp(path);
    if (fd < 0) { free(path); return false; }

    bool ok = true;
    for (size_t off = 0; off < total && ok; ) {
        ssize_t n = write(fd, px + off, total - off);
        if (n > 0) off += (size_t)n;
        else if (n < 0 && errno == EINTR) continue;
        else ok = false;
    }
    close(fd);
    if (!ok) { unlink(path); free(path); return false; }

    char buf[512];
    int len = snprintf(buf, sizeof buf,
                       "\x1b_Ga=t,f=%d,s=%d,v=%d,i=%u,q=2,t=f;",
                       channels == 4 ? 32 : 24, w, h, id);
    size_t plen = strlen(path);
    if (((plen + 2) / 3) * 4 + (size_t)len + 2 > sizeof buf) {
        unlink(path);
        free(path);
        return false;
    }
    len += kg_b64((const uint8_t *)path, plen, buf + len);
    buf[len++] = '\x1b';
    buf[len++] = '\\';
    kg_emit(buf, len);

    kg_track_tempfile(path);
    return true;
}

void kg_transmit(uint32_t id, const uint8_t *rgb, int w, int h) {
    kg_transmit_ex(id, rgb, w, h, 3);
}

void kg_transmit_ex(uint32_t id, const uint8_t *px, int w, int h, int channels) {
    if (!px || w <= 0 || h <= 0 || (channels != 3 && channels != 4)) return;

    size_t total = (size_t)w * (size_t)h * (size_t)channels;

    if (kg_tempfile && total >= KG_TEMPFILE_MIN &&
        kg_transmit_tempfile(id, px, total, w, h, channels))
        return;

#ifdef PIX_HAVE_ZLIB

    if (total >= KG_TEMPFILE_MIN) {
        uLongf zlen = compressBound((uLong)total);
        uint8_t *z = (uint8_t *)malloc(zlen);
        if (z) {
            if (compress2(z, &zlen, px, (uLong)total, 1) == Z_OK &&
                (size_t)zlen < total - total / 20) {
                kg_transmit_direct(id, z, (size_t)zlen, w, h, channels, ",o=z");
                free(z);
                return;
            }
            free(z);
        }
    }
#endif

    kg_transmit_direct(id, px, total, w, h, channels, "");
}

void kg_virtual_place(uint32_t id, int cols, int rows) {
    if (cols <= 0 || rows <= 0) return;
    char cmd[128];

    int n = snprintf(cmd, sizeof cmd, "\x1b_Ga=d,d=i,i=%u,q=2\x1b\\", id);
    kg_emit(cmd, n);

    n = snprintf(cmd, sizeof cmd, "\x1b_Ga=p,U=1,i=%u,c=%d,r=%d,q=2\x1b\\",
                 id, cols, rows);
    kg_emit(cmd, n);
}

static int kg_utf8(uint32_t cp, char *out) {
    if (cp < 0x80) {
        out[0] = (char)cp;
        return 1;
    }
    if (cp < 0x800) {
        out[0] = (char)(0xC0 | (cp >> 6));
        out[1] = (char)(0x80 | (cp & 0x3F));
        return 2;
    }
    if (cp < 0x10000) {
        out[0] = (char)(0xE0 | (cp >> 12));
        out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[2] = (char)(0x80 | (cp & 0x3F));
        return 3;
    }
    out[0] = (char)(0xF0 | (cp >> 18));
    out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
    out[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
    out[3] = (char)(0x80 | (cp & 0x3F));
    return 4;
}

void kg_placeholder_cell(uint32_t id, int row, int col) {
    if (row < 0 || col < 0 ||
        row >= KG_DIACRITIC_COUNT || col >= KG_DIACRITIC_COUNT) return;

    char buf[64];
    int n = snprintf(buf, sizeof buf, "\x1b[38;2;%u;%u;%um",
                     (id >> 16) & 0xFF, (id >> 8) & 0xFF, id & 0xFF);
    n += kg_utf8(KG_PLACEHOLDER_CP, buf + n);
    n += kg_utf8(kg_diacritics[row], buf + n);
    n += kg_utf8(kg_diacritics[col], buf + n);

    uint32_t high = id >> 24;
    if (high) n += kg_utf8(kg_diacritics[high], buf + n);

    term_write_n(buf, n);
}

void kg_fit_cells(int img_w, int img_h, int cell_w, int cell_h,
                  int max_cols, int max_rows,
                  int *out_cols, int *out_rows, int *px_w, int *px_h) {
    int cols = max_cols, rows = max_rows;

    if (img_w > 0 && img_h > 0 && cell_w > 0 && cell_h > 0 &&
        max_cols > 0 && max_rows > 0) {

        long box_w = (long)max_cols * cell_w, box_h = (long)max_rows * cell_h;
        if ((long)img_w * box_h > (long)img_h * box_w) {
            long want_h = (long)img_h * box_w / img_w;
            rows = (int)((want_h + cell_h - 1) / cell_h);
        } else {
            long want_w = (long)img_w * box_h / img_h;
            cols = (int)((want_w + cell_w - 1) / cell_w);
        }
        if (cols < 1) cols = 1;
        if (rows < 1) rows = 1;
        if (cols > max_cols) cols = max_cols;
        if (rows > max_rows) rows = max_rows;
    }

    if (cols > KG_DIACRITIC_COUNT) cols = KG_DIACRITIC_COUNT;
    if (rows > KG_DIACRITIC_COUNT) rows = KG_DIACRITIC_COUNT;

    if (out_cols) *out_cols = cols;
    if (out_rows) *out_rows = rows;
    if (px_w) *px_w = cols * cell_w;
    if (px_h) *px_h = rows * cell_h;
}

void kg_place(uint32_t id, uint32_t pid, int col, int row, int x_off, int y_off) {
    char cmd[128];
    term_move_cursor(col, row);

    int n = snprintf(cmd, sizeof cmd,
                     "\x1b_Ga=p,i=%u,p=%u,X=%d,Y=%d,C=1,z=0,q=2\x1b\\",
                     id, pid, x_off, y_off);
    kg_emit(cmd, n);
}

void kg_clear_placements(void) {

    kg_emit_lit("\x1b_Ga=d,d=a,q=2\x1b\\");
}

void kg_clear_placement(uint32_t id, uint32_t pid) {
    char cmd[64];
    int n = snprintf(cmd, sizeof cmd, "\x1b_Ga=d,d=i,i=%u,p=%u,q=2\x1b\\", id, pid);
    kg_emit(cmd, n);
}

void kg_delete(uint32_t id) {
    char cmd[64];
    int n = snprintf(cmd, sizeof cmd, "\x1b_Ga=d,d=I,i=%u,q=2\x1b\\", id);
    kg_emit(cmd, n);
}

void kg_delete_all(void) {
    kg_emit_lit("\x1b_Ga=d,d=A,q=2\x1b\\");
}

bool kg_supported(void) {
    const char *v;
    if ((v = getenv("TERM")) && (strstr(v, "kitty") || strstr(v, "ghostty")))
        return true;
    if ((v = getenv("TERM_PROGRAM")) &&
        (strstr(v, "ghostty") || strstr(v, "Ghostty") || strstr(v, "WezTerm")))
        return true;
    if (getenv("KITTY_WINDOW_ID") || getenv("GHOSTTY_RESOURCES_DIR"))
        return true;
    return false;
}

#define KG_PROBE_GRAPHICS "\x1b_Gi=31,s=1,v=1,a=q,t=d,f=24;AAAA\x1b\\"

static bool kg_write_all(int fd, const char *s, size_t n) {
    while (n) {
        ssize_t w = write(fd, s, n);
        if (w <= 0) return false;
        s += w; n -= (size_t)w;
    }
    return true;
}

static int kg_elapsed_ms(const struct timeval *start) {
    struct timeval now;
    gettimeofday(&now, NULL);
    return (int)((now.tv_sec - start->tv_sec) * 1000 +
                 (now.tv_usec - start->tv_usec) / 1000);
}

int kg_probe(int timeout_ms) {
    if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO)) return -1;

    struct termios orig;
    if (tcgetattr(STDIN_FILENO, &orig) == -1) return -1;
    struct termios raw = orig;
    raw.c_lflag &= ~(ECHO | ICANON);
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 0;
    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == -1) return -1;

    char query[256];
    int qn = 0;
    if (kg_wrap) {
        qn += snprintf(query + qn, sizeof query - qn, "\x1bPtmux;");
        for (const char *p = KG_PROBE_GRAPHICS; *p; p++) {
            if (*p == '\x1b') query[qn++] = '\x1b';
            query[qn++] = *p;
        }
        qn += snprintf(query + qn, sizeof query - qn, "\x1b\\");
    } else {
        qn += snprintf(query + qn, sizeof query - qn, "%s%s",
                       KG_PROBE_GRAPHICS, "\x1b[c");
    }

    int result = -1;
    if (kg_write_all(STDOUT_FILENO, query, (size_t)qn)) {
        char buf[512];
        size_t len = 0;
        struct timeval start;
        gettimeofday(&start, NULL);

        while (result == -1) {
            int left = timeout_ms - kg_elapsed_ms(&start);
            if (left <= 0) break;

            fd_set fds;
            FD_ZERO(&fds);
            FD_SET(STDIN_FILENO, &fds);
            struct timeval tv = { left / 1000, (left % 1000) * 1000 };
            int r = select(STDIN_FILENO + 1, &fds, NULL, NULL, &tv);
            if (r <= 0) break;

            ssize_t n = read(STDIN_FILENO, buf + len, sizeof buf - 1 - len);
            if (n <= 0) break;
            len += (size_t)n;
            buf[len] = '\0';

            char *da;
            if (strstr(buf, "_Gi=31;OK")) result = 1;

            else if (!kg_wrap && (da = strstr(buf, "\x1b[?")) != NULL &&
                     memchr(da, 'c', len - (size_t)(da - buf))) result = 0;
            else if (len == sizeof buf - 1) break;
        }
    }

    tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig);
    return result;
}

#endif
