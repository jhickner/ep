/**
 * kitty.h - kitty graphics protocol emitter (single-header, stb-style)
 *
 * In exactly ONE .c file:
 *
 *     #define KITTY_IMPLEMENTATION
 *     #include "kitty.h"
 *
 * Images are transmitted once (kg_transmit) and then placed as many times as
 * needed. Placements are cheap; transmission is not. All commands carry q=2 so
 * the terminal stays silent - responses would otherwise land in term.h's input
 * queue and be decoded as keystrokes.
 *
 * Output goes through term.h's buffered writer, so call term_flush() to push a
 * frame out.
 *
 * TWO PLACEMENT STYLES
 *
 *   Unicode placeholders (kg_virtual_place + placeholder cells) are the default
 *   and the only style that works under tmux. The image is anchored to real
 *   text cells - U+10EEEE carrying the image id in its foreground color and
 *   row/column in combining diacritics - so a multiplexer or editor that knows
 *   nothing about graphics still moves, scrolls and redraws it correctly.
 *   Emitting the cells is the caller's job; screen.h does it via
 *   glyph_placeholder().
 *
 *   Direct placement (kg_place) puts the image at the cursor with optional
 *   sub-cell pixel offsets. Nothing tracks it, so tmux loses it on the first
 *   redraw. It buys pixel-exact positioning without cell-aligned padding.
 *
 *   Placeholders can match direct placement's precision: scale the image to
 *   exactly cols*cell_w by rows*cell_h and letterbox inside that buffer, which
 *   is what kg_fit_cells() computes.
 *
 * TMUX
 *
 *   Every graphics escape is an APC sequence tmux does not understand, so under
 *   tmux each one must be wrapped in a DCS passthrough with its ESCs doubled.
 *   kg_init() detects $TMUX and does this for you from then on. tmux also has
 *   to be told to allow it at all - see kg_tmux_allow_passthrough().
 *
 * IMAGE IDS
 *
 *   In placeholder mode the id travels in the cell's foreground color, so it
 *   must fit in 24 bits. Ids above that need a third diacritic carrying the
 *   high byte; kg_placeholder_cell() emits it, but staying under 2^24 keeps the
 *   encoding to two diacritics. Id 0 is not valid.
 */

#ifndef KITTY_H
#define KITTY_H

#include <stdint.h>
#include <stdbool.h>

#ifndef TERM_H
#include "term.h"
#endif

// The placeholder base character. Every cell of a placeholder placement holds
// this codepoint; row/column arrive as combining marks.
#define KG_PLACEHOLDER_CP 0x10EEEEu

// Largest image id representable without a third diacritic.
#define KG_MAX_ID_24BIT 0xFFFFFFu

// Highest row or column a placeholder can address (the diacritic table runs
// out at 297 entries).
int kg_max_rowcolumn(void);

// Detect the environment once, before any drawing. Enables tmux passthrough
// wrapping if $TMUX is set. Safe to skip if you never run under tmux.
void kg_init(void);

// Force passthrough wrapping on or off, overriding what kg_init() detected.
void kg_set_passthrough(bool on);

// True if escapes are currently being wrapped for tmux.
bool kg_passthrough(void);

// Force the temp-file transfer path on or off, overriding what kg_init()
// detected. Large images are otherwise handed over as a file the terminal reads
// and deletes, which is far cheaper than base64 through the pty but requires
// the terminal to share our filesystem.
void kg_set_tempfile(bool on);
bool kg_tempfile_enabled(void);

// Unlink any transfer files the terminal never got around to reading. Call
// once on the way out; harmless at any other time.
void kg_cleanup_tempfiles(void);

// Sweep up transfer files left behind by a process that was killed before it
// could clean up after itself. Only touches files older than `age_secs`, so a
// pix running alongside this one keeps its own. Walks a directory: call it off
// the hot path.
void kg_sweep_stale_tempfiles(int age_secs);

// Bracket a frame in a synchronized update, so nothing is painted until the
// whole frame has arrived. Without it a frame large enough to be flushed in
// pieces - anything carrying image data - is drawn as it trickles in, and the
// images build up in strips.
//
// It is also what puts placeholder cells on screen under tmux 3.7 and later.
//
// Those releases drop a cell's combining diacritics whenever the pane is not at
// column 0: screen_write_combine() tests visibility with a pane-relative x
// against window coordinates, decides the cell is covered by whatever pane
// really sits there, and skips the write. A placeholder cell is nothing but
// combining marks, so the image ends up smeared into bands.
//
// The cells still reach tmux's grid intact - only the write to the terminal is
// skipped - so ending a synchronized update, which tmux consumes itself and
// answers with a full pane redraw out of that grid, puts them up correctly.
//
// Caveat: tmux 3.7a alone does not redraw when the update ends, so there the
// cells wait for its one-second sync timer.
void kg_placeholder_redraw_begin(void);
void kg_placeholder_redraw_end(void);

// Ask tmux to permit DCS passthrough for the current pane, which it refuses by
// default. Returns true if the option was set (or no tmux is involved, so
// nothing was needed). Scoped to the pane, so it neither disturbs other panes
// nor outlives them; a global equivalent belongs in the user's tmux.conf:
//
//     set -gq allow-passthrough all
//
// Runs `tmux set -p allow-passthrough all` via the tmux binary, so it does
// nothing useful if tmux is not on PATH.
bool kg_tmux_allow_passthrough(void);

// Send RGB pixel data (w*h*3 bytes) to the terminal under `id`, without
// displaying it. Safe to call again with the same id to replace the data.
void kg_transmit(uint32_t id, const uint8_t *rgb, int w, int h);

// As above for 3 (RGB) or 4 (RGBA) channels. RGBA is what letterbox padding
// wants: transparent pixels let the terminal's own background show through, so
// fitting an image to a cell rectangle doesn't paint a box around it.
void kg_transmit_ex(uint32_t id, const uint8_t *px, int w, int h, int channels);

// Declare that image `id` will be drawn into a cols x rows cell rectangle by
// placeholder cells appearing later. Draws nothing by itself. The image is
// fitted to the rectangle preserving aspect ratio, so pass a buffer already
// sized to the rectangle if you want no scaling of your own to be undone.
//
// Replaces the image's previous placement, so calling it again with a new
// rectangle is how an image is resized.
void kg_virtual_place(uint32_t id, int cols, int rows);

// Emit one placeholder cell for image `id` at image-relative (row, col),
// including the SGR foreground that carries the id. Ordinary text - no
// passthrough involved. Callers driving a cell buffer usually want screen.h's
// glyph_placeholder() instead, which batches these properly.
void kg_placeholder_cell(uint32_t id, int row, int col);

// Cell rectangle and pixel size an image should be scaled to in placeholder
// mode: the largest cell-aligned box within max_cols x max_rows that preserves
// the image's aspect ratio. Letterbox the scaled image inside *px_w x *px_h to
// center it, since placeholders have no sub-cell offset.
void kg_fit_cells(int img_w, int img_h, int cell_w, int cell_h,
                  int max_cols, int max_rows,
                  int *out_cols, int *out_rows, int *px_w, int *px_h);

// Display image `id` at cell (col, row), 0-based, using placement id `pid`.
// x_off/y_off nudge it by up to one cell in pixels, which is what makes
// pixel-exact centering inside a cell-aligned box possible. Direct placement:
// not tracked by tmux, so prefer placeholders there.
void kg_place(uint32_t id, uint32_t pid, int col, int row, int x_off, int y_off);

// Remove every placement on screen. Transmitted image data is kept, so the
// placements can be recreated without resending pixels. Placeholder placements
// disappear by overwriting their cells instead, which needs none of this.
void kg_clear_placements(void);

// Remove one placement. Not every terminal honours the blanket clear above, so
// callers that must be certain an image is gone name it explicitly. The image
// data is kept.
void kg_clear_placement(uint32_t id, uint32_t pid);

// Free image `id` and any of its placements.
void kg_delete(uint32_t id);

// Free every image this process transmitted.
void kg_delete_all(void);

// True if the terminal is likely to understand the protocol, judging only by
// the environment. Unreliable under tmux, where TERM describes tmux and the
// outer terminal is visible only through variables it happened to export - a
// session reattached from a different terminal will be misjudged. Prefer
// kg_probe() when the answer matters.
bool kg_supported(void);

// Ask the terminal itself, before any TUI setup: transmit a 1x1 image with a
// response requested, followed by a primary device attributes request. A
// terminal that speaks the protocol answers the first; one that doesn't still
// answers the second, which is what lets a negative answer be distinguished
// from a slow one. Puts stdin in raw mode for the duration and restores it.
//
// Call kg_init() first so the query is wrapped for tmux when needed.
//
// Under tmux the device attributes trick is skipped, because tmux answers that
// one itself, locally and immediately, while the graphics query still has to
// reach the outer terminal and come back - a race the local reply always wins,
// which would report every tmux pane as unsupported. So under tmux a terminal
// that stays silent yields -1 rather than 0, and the caller should fall back to
// kg_supported(). The cost is waiting out timeout_ms when there is no support.
//
// Returns 1 (supported), 0 (answered, but not supported), or -1 (no answer
// within timeout_ms, or stdin/stdout is not a terminal).
int kg_probe(int timeout_ms);

#endif // KITTY_H

/* ======================================================================== */
/* Implementation                                                           */
/* ======================================================================== */
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

// Combining marks used to encode row/column numbers, from kitty's
// gen/rowcolumn-diacritics.txt: the Unicode 6.0.0 combining class 230 marks
// that have no decomposition mapping, minus those that could be fused into a
// precomposed character by normalization. Index N encodes row/column N.
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
    // A file we write is a file the terminal can read - unless it is at the
    // far end of an ssh session, where the path would name nothing.
    kg_tempfile = getenv("SSH_CONNECTION") == NULL && getenv("SSH_TTY") == NULL &&
                  getenv("SSH_CLIENT") == NULL;
}

// Unwrapped on purpose: under tmux these are addressed to tmux, not to the
// terminal behind it.
void kg_placeholder_redraw_begin(void) {
    term_write_n("\x1b[?2026h", 8);
}

void kg_placeholder_redraw_end(void) {
    term_write_n("\x1b[?2026l", 8);
}

bool kg_tmux_allow_passthrough(void) {
    if (!getenv("TMUX")) return true;
    // >/dev/null so tmux's own output never reaches the terminal we are drawing
    // on. system() is acceptable here: no untrusted data is interpolated.
    return system("tmux set -p allow-passthrough all >/dev/null 2>&1") == 0;
}

// Payload bytes packed into one tmux passthrough DCS while batching. tmux
// discards a longer DCS silently and whole, and the input-buffer-size option
// that decides how long is too long cannot be set below its 1MB default.
#define KG_WRAP_MAX 262144

static int kg_batch_depth = 0;      // nested kg_batch_begin() calls
static bool kg_batch_open = false;  // a passthrough DCS is open, unterminated
static size_t kg_batch_len = 0;     // payload bytes written into it

// The DCS payload with its ESCs doubled, which is what tmux forwards.
static void kg_emit_escaped(const char *seq, int n) {
    int run = 0;
    for (int i = 0; i < n; i++) {
        if (seq[i] == '\x1b') {
            if (run) term_write_n(seq + i - run, run);
            run = 0;
            term_write_n("\x1b\x1b", 2);   // tmux eats one ESC of each pair
        } else {
            run++;
        }
    }
    if (run) term_write_n(seq + n - run, run);
}

// One graphics escape, wrapped for tmux when needed. The payload always arrives
// complete (never split across calls) because the DCS wrapper has to enclose
// the whole APC sequence, ESC ... ST included. Inside a batch the wrapper is
// shared with the escapes either side of it.
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

// Share one passthrough DCS between everything emitted until the matching end.
//
// tmux ends every passthrough DCS with tty_invalidate(), which re-sends a
// cursor-style reset, a mouse-mode reset and a cursor move to home - to
// whichever pane is active, not the one drawing. The cost is per DCS, not per
// byte, so a frame that wraps each of its hundred 4KB chunks separately makes
// the cursor in another pane stutter a hundred times.
static void kg_batch_begin(void) { kg_batch_depth++; }

static void kg_batch_end(void) {
    if (kg_batch_depth > 0 && --kg_batch_depth > 0) return;
    kg_batch_depth = 0;
    // Closing here rather than on the next kg_emit() keeps the invariant that
    // no unterminated DCS outlives the call that opened it: tmux resets one
    // only after 5s, so anything written meanwhile would be swallowed.
    if (kg_batch_open) term_write("\x1b\\");
    kg_batch_open = false;
    kg_batch_len = 0;
}

// Emit a string literal, letting the compiler supply the length.
#define kg_emit_lit(s) kg_emit((s), (int)(sizeof(s) - 1))

static const char KG_B64[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

// Encode `n` bytes (n <= 3) into 4 base64 chars with padding.
static void kg_b64_group(const uint8_t *src, int n, char *dst) {
    uint32_t v = (uint32_t)src[0] << 16;
    if (n > 1) v |= (uint32_t)src[1] << 8;
    if (n > 2) v |= (uint32_t)src[2];
    dst[0] = KG_B64[(v >> 18) & 0x3F];
    dst[1] = KG_B64[(v >> 12) & 0x3F];
    dst[2] = n > 1 ? KG_B64[(v >> 6) & 0x3F] : '=';
    dst[3] = n > 2 ? KG_B64[v & 0x3F] : '=';
}

#define KG_CHUNK 4096   // base64 chars per escape sequence (protocol maximum)

// Payloads at or above this go through a temp file when that is allowed. Below
// it the file create/write/unlink round trip costs more than the escape it
// would replace.
#define KG_TEMPFILE_MIN 32768

// How many temp-file paths to remember for cleanup. Kitty unlinks each file
// once it has read it, so this only ever mops up after a terminal that ignored
// the transfer, and the oldest entry is always long since consumed.
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
        // mkstemp's six template characters, and nothing else.
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

// Base64 `n` bytes into `dst`, which must hold ((n+2)/3)*4 chars. Returns the
// number written.
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

// Stream `data` to the terminal as base64 inside the escape stream. `extra` is
// appended to the first chunk's key list, which is where o=z goes.
static void kg_transmit_direct(uint32_t id, const uint8_t *data, size_t total,
                               int w, int h, int channels, const char *extra) {
    size_t bytes_per_chunk = (KG_CHUNK / 4) * 3;
    // Header, payload and terminator go out as one buffer so kg_emit() can wrap
    // the sequence as a unit.
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

// Hand the pixels over as a file (t=f) instead of as escape-stream base64.
//
// The win is the whole payload: a 1200x800 preview is ~3.8MB of base64 through
// the pty - and through tmux's passthrough on top of that - where this is a
// hundred bytes plus a write the kernel absorbs into the page cache. Kitty
// unlinks the file itself once it has read it, which is why it has to live in a
// temp directory.
//
// Only correct when the terminal shares our filesystem, so kg_init() turns it
// off the moment it sees an SSH session.
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
    // Only worth it when the bytes are actually expensive - which is to say
    // when we are going over a network, since that is the only reason the temp
    // file route was unavailable. Letterbox padding deflates to nothing, so
    // this pays even on photographic data.
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
    // A placement is identified by (image id, placement id); one declared
    // without a placement id is added, not substituted, so re-declaring leaves
    // stale rectangles the terminal may resolve this image's cells against.
    // Lowercase d=i drops those placements and keeps the pixels.
    int n = snprintf(cmd, sizeof cmd, "\x1b_Ga=d,d=i,i=%u,q=2\x1b\\", id);
    kg_emit(cmd, n);
    // U=1 marks the placement virtual: it reserves nothing on screen and draws
    // nothing until placeholder cells referring to this id appear.
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
        // Compare aspect ratios in pixels, then round the limiting dimension up
        // so the image never overflows the box it was told to fit.
        long box_w = (long)max_cols * cell_w, box_h = (long)max_rows * cell_h;
        if ((long)img_w * box_h > (long)img_h * box_w) {
            long want_h = (long)img_h * box_w / img_w;          // width-limited
            rows = (int)((want_h + cell_h - 1) / cell_h);
        } else {
            long want_w = (long)img_w * box_h / img_h;          // height-limited
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
    // C=1 keeps the cursor where it is, so a placement never scrolls the screen.
    int n = snprintf(cmd, sizeof cmd,
                     "\x1b_Ga=p,i=%u,p=%u,X=%d,Y=%d,C=1,z=0,q=2\x1b\\",
                     id, pid, x_off, y_off);
    kg_emit(cmd, n);
}

void kg_clear_placements(void) {
    // Lowercase 'a' deletes placements but keeps the pixel data cached.
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

    // Built here rather than as a literal because the graphics half needs tmux
    // wrapping, and because the device attributes request is only useful when
    // tmux is not in the way to answer it locally.
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
            // The device attributes reply (CSI ? … c) is answered by every
            // terminal and comes after the graphics response would have, so
            // seeing it complete means there wasn't one. Only true without tmux
            // in between - see the note on kg_probe().
            else if (!kg_wrap && (da = strstr(buf, "\x1b[?")) != NULL &&
                     memchr(da, 'c', len - (size_t)(da - buf))) result = 0;
            else if (len == sizeof buf - 1) break;
        }
    }

    tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig);
    return result;
}

#endif // KITTY_IMPLEMENTATION
