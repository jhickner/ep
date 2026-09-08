
#ifndef TERM_H
#define TERM_H

#include <stdbool.h>
#include <stdint.h>
#include <termios.h>

typedef struct {
    int fd;
    struct termios orig_termios;
    int width;
    int height;
    bool in_raw_mode;
    bool in_alt_screen;
} Term;

typedef enum {
    KEY_NONE = 0,
    KEY_CHAR,
    KEY_UP,
    KEY_DOWN,
    KEY_LEFT,
    KEY_RIGHT,
    KEY_ENTER,
    KEY_ESCAPE,
    KEY_BACKSPACE,
    KEY_TAB,
    KEY_SPACE,
    KEY_RESIZE,
    KEY_SHIFT_UP,
    KEY_SHIFT_DOWN,
    KEY_SHIFT_LEFT,
    KEY_SHIFT_RIGHT,
    KEY_MOUSE_PRESS,
    KEY_MOUSE_RELEASE,
    KEY_MOUSE_DRAG,
    KEY_CTRL_UP,
    KEY_CTRL_DOWN,
    KEY_CTRL_LEFT,
    KEY_CTRL_RIGHT,
    KEY_ALT_UP,
    KEY_ALT_DOWN,
    KEY_ALT_LEFT,
    KEY_ALT_RIGHT,

    KEY_BTN_A_PRESS,
    KEY_BTN_A_RELEASE,
    KEY_BTN_B_PRESS,
    KEY_BTN_B_RELEASE,
    KEY_BTN_X_PRESS,
    KEY_BTN_X_RELEASE,
    KEY_BTN_Y_PRESS,
    KEY_BTN_Y_RELEASE,

    KEY_SHIFT_ENTER,
    KEY_PASTE,
    KEY_MOUSE_WHEEL_UP,
    KEY_MOUSE_WHEEL_DOWN,
    KEY_PAGE_UP,
    KEY_PAGE_DOWN,
    KEY_SHIFT_SPACE,
    KEY_FOCUS_IN,
    KEY_FOCUS_OUT,
    KEY_HOME,
    KEY_END,
} KeyCode;

typedef struct {
    KeyCode code;
    char ch;
    int mouse_x;
    int mouse_y;
    int mouse_button;
    char *paste;
    int  paste_len;
    bool pressed;
    bool repeat;
} InputEvent;

void term_request_key_events(bool on, bool allow_tmux);

bool term_key_events(void);

bool term_init(Term *t);

void term_cleanup(Term *t);

void term_get_size(Term *t);

bool term_cell_size(Term *t, int *cw, int *ch);

void term_install_signal_restore(void);

void term_enter_alt_screen(Term *t);
void term_leave_alt_screen(Term *t);

void term_hide_cursor(void);
void term_show_cursor(void);
void term_move_cursor(int x, int y);

void term_clear(void);
void term_flush(void);

bool term_poll_event(Term *t, InputEvent *ev, int timeout_ms);

bool term_wait_event(Term *t, InputEvent *ev, int timeout_ms);

void term_write(const char *s);
void term_write_n(const char *s, int n);

void term_enable_mouse(void);
void term_disable_mouse(void);

void term_set_background_color(uint8_t r, uint8_t g, uint8_t b);

bool term_query_background_color(uint8_t *r, uint8_t *g, uint8_t *b,
                                 int timeout_ms);

bool term_query_foreground_color(uint8_t *r, uint8_t *g, uint8_t *b,
                                 int timeout_ms);

bool term_color_is_light(uint8_t r, uint8_t g, uint8_t b);

#endif

#if defined(TERM_IMPLEMENTATION) && !defined(TERM_IMPLEMENTED)
#define TERM_IMPLEMENTED

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/time.h>
#include <signal.h>
#include <pthread.h>
#include <poll.h>
#include <fcntl.h>

#define QUEUE_DEFINE(T, CAP)                                                   \
typedef struct {                                                               \
    T items[CAP];                                                              \
    int head;                                                                  \
    int tail;                                                                  \
    pthread_mutex_t mutex;                                                     \
} Queue_##T;                                                                   \
                                                                               \
__attribute__((unused))                                                        \
static inline void queue_##T##_init(Queue_##T *q) {                            \
    memset(q->items, 0, sizeof(q->items));                                     \
    q->head = 0;                                                               \
    q->tail = 0;                                                               \
    pthread_mutex_init(&q->mutex, NULL);                                       \
}                                                                              \
                                                                               \
__attribute__((unused))                                                        \
static inline void queue_##T##_destroy(Queue_##T *q) {                         \
    pthread_mutex_destroy(&q->mutex);                                          \
}                                                                              \
                                                                               \
__attribute__((unused))                                                        \
static inline bool queue_##T##_push(Queue_##T *q, const T *item) {             \
    pthread_mutex_lock(&q->mutex);                                             \
    int next = (q->tail + 1) % CAP;                                            \
    if (next == q->head) {                                                     \
        pthread_mutex_unlock(&q->mutex);                                       \
        return false;                                                          \
    }                                                                          \
    q->items[q->tail] = *item;                                                 \
    q->tail = next;                                                            \
    pthread_mutex_unlock(&q->mutex);                                           \
    return true;                                                               \
}                                                                              \
                                                                               \
__attribute__((unused))                                                        \
static inline bool queue_##T##_pop(Queue_##T *q, T *item) {                    \
    pthread_mutex_lock(&q->mutex);                                             \
    if (q->head == q->tail) {                                                  \
        pthread_mutex_unlock(&q->mutex);                                       \
        return false;                                                          \
    }                                                                          \
    *item = q->items[q->head];                                                 \
    q->head = (q->head + 1) % CAP;                                             \
    pthread_mutex_unlock(&q->mutex);                                           \
    return true;                                                               \
}                                                                              \
                                                                               \
__attribute__((unused))                                                        \
static inline bool queue_##T##_is_empty(Queue_##T *q) {                        \
    pthread_mutex_lock(&q->mutex);                                             \
    bool empty = (q->head == q->tail);                                         \
    pthread_mutex_unlock(&q->mutex);                                           \
    return empty;                                                              \
}

QUEUE_DEFINE(InputEvent, 64)

static Term *g_term = NULL;

static bool g_want_key_events = false;
static bool g_kbd_allow_tmux  = false;
static bool g_key_events      = false;
static bool g_tmux            = false;

static Queue_InputEvent g_queue;
static pthread_t g_input_thread;
static volatile bool g_running = false;

static volatile sig_atomic_t g_resize_pending = 0;

static int g_wake_pipe[2] = {-1, -1};
static void term_wake_notify(void) {
    if (g_wake_pipe[1] >= 0) { char c = 'x'; ssize_t w = write(g_wake_pipe[1], &c, 1); (void)w; }
}

static char  *g_acc = NULL;
static size_t g_acc_cap = 0;

#define PASTE_START    "\x1b[200~"
#define PASTE_END      "\x1b[201~"
#define PASTE_MARK_LEN 6

static bool parse_sgr_mouse(const char *buf, ssize_t n, InputEvent *ev) {
    if (n < 6 || buf[0] != '\x1b' || buf[1] != '[' || buf[2] != '<') return false;

    int button = 0, x = 0, y = 0;
    int i = 3;

    while (i < n && buf[i] >= '0' && buf[i] <= '9') {
        button = button * 10 + (buf[i] - '0');
        i++;
    }
    if (i >= n || buf[i] != ';') return false;
    i++;

    while (i < n && buf[i] >= '0' && buf[i] <= '9') {
        x = x * 10 + (buf[i] - '0');
        i++;
    }
    if (i >= n || buf[i] != ';') return false;
    i++;

    while (i < n && buf[i] >= '0' && buf[i] <= '9') {
        y = y * 10 + (buf[i] - '0');
        i++;
    }
    if (i >= n) return false;

    ev->mouse_x = x - 1;
    ev->mouse_y = y - 1;
    ev->mouse_button = button & 3;

    if (button & 64) {

        if ((button & 3) == 0)      ev->code = KEY_MOUSE_WHEEL_UP;
        else if ((button & 3) == 1) ev->code = KEY_MOUSE_WHEEL_DOWN;

    } else if (buf[i] == 'm') {
        ev->code = KEY_MOUSE_RELEASE;
    } else if (buf[i] == 'M') {
        if (button & 32) {
            ev->code = KEY_MOUSE_DRAG;
        } else {
            ev->code = KEY_MOUSE_PRESS;
        }
    } else {
        return false;
    }

    return true;
}

static void parse_input(const char *buf, ssize_t n, InputEvent *ev) {
    memset(ev, 0, sizeof(InputEvent));
    ev->pressed = true;

    if (parse_sgr_mouse(buf, n, ev)) return;

    if (n == 2 && buf[0] == '\x1b') {
        switch (buf[1]) {
            case 'b': ev->code = KEY_ALT_LEFT; return;
            case 'f': ev->code = KEY_ALT_RIGHT; return;
            default: break;
        }
    }

    if (n == 3 && buf[0] == '\x1b' && buf[1] == 'O') {
        switch (buf[2]) {
            case 'A': ev->code = KEY_UP; return;
            case 'B': ev->code = KEY_DOWN; return;
            case 'C': ev->code = KEY_RIGHT; return;
            case 'D': ev->code = KEY_LEFT; return;
            case 'H': ev->code = KEY_HOME; return;
            case 'F': ev->code = KEY_END; return;
            default: return;
        }
    }

    if (n == 1) {
        switch (buf[0]) {
            case '\x1b':
                ev->code = KEY_ESCAPE;
                break;
            case '\r':
                ev->code = KEY_ENTER;
                break;
            case '\n':

                ev->code = KEY_SHIFT_ENTER;
                break;
            case '\t':
                ev->code = KEY_TAB;
                break;
            case 127:
            case '\b':
                ev->code = KEY_BACKSPACE;
                break;
            case ' ':
                ev->code = KEY_SPACE;
                break;
            default:
                if (buf[0] >= 32 && buf[0] < 127) {
                    ev->code = KEY_CHAR;
                    ev->ch = buf[0];
                } else if (buf[0] >= 1 && buf[0] <= 26) {
                    ev->code = KEY_CHAR;
                    ev->ch = buf[0];
                }
                break;
        }
    } else if (n >= 3 && buf[0] == '\x1b' && buf[1] == '[') {

        if (buf[n - 1] == 'u') {
            int code = 0, shifted = -1, mods = 1, event = 1;
            int field = 0, sub = 0, val = 0;
            bool have = false;
            for (int i = 2; i < n; i++) {
                char ch = buf[i];
                if (ch >= '0' && ch <= '9') { val = val * 10 + (ch - '0'); have = true; continue; }
                if (have) {
                    if      (field == 0 && sub == 0) code = val;
                    else if (field == 0 && sub == 1) shifted = val;
                    else if (field == 1 && sub == 0) mods = val;
                    else if (field == 1 && sub == 1) event = val;

                }
                val = 0; have = false;
                if (ch == ':') sub++;
                else if (ch == ';') { field++; sub = 0; }
                else break;
            }

            if (event == 3 && !g_key_events) return;
            ev->pressed = (event != 3);
            ev->repeat  = (event == 2);

            int m = mods - 1;
            bool shift = (m & 1) != 0;
            bool ctrl  = (m & 4) != 0;

            if (code >= 57441 && code <= 57454) return;

            switch (code) {
                case 13: ev->code = shift ? KEY_SHIFT_ENTER : KEY_ENTER; return;
                case 27: ev->code = KEY_ESCAPE; return;
                case 9:  ev->code = KEY_TAB; return;
                case 127: ev->code = KEY_BACKSPACE; return;
                case 32: ev->code = shift ? KEY_SHIFT_SPACE : KEY_SPACE; return;
                default: break;
            }
            if (code >= 32 && code < 127) {
                int c = code;
                if (shift && shifted >= 32 && shifted < 127) c = shifted;
                else if (shift && code >= 'a' && code <= 'z') c = code - 32;
                ev->code = KEY_CHAR;
                if (ctrl) {
                    char low = (c >= 'A' && c <= 'Z') ? (char)(c + 32) : (char)c;
                    ev->ch = (char)(low & 0x1f);
                } else {
                    ev->ch = (char)c;
                }
            }
            return;
        }

        int num = 0, mods = 1, event = 1;
        {
            int field = 0, sub = 0, val = 0;
            bool have = false;
            for (int i = 2; i < n; i++) {
                char ch = buf[i];
                if (ch >= '0' && ch <= '9') { val = val * 10 + (ch - '0'); have = true; continue; }
                if (have) {
                    if      (field == 0 && sub == 0) num = val;
                    else if (field == 1 && sub == 0) mods = val;
                    else if (field == 1 && sub == 1) event = val;
                }
                val = 0; have = false;
                if (ch == ':') sub++;
                else if (ch == ';') { field++; sub = 0; }
                else break;
            }
        }

        if (event == 3 && !g_key_events) return;
        ev->pressed = (event != 3);
        ev->repeat  = (event == 2);

        int m = mods - 1;
        bool shift = (m & 1) != 0, alt = (m & 2) != 0, ctrl = (m & 4) != 0;

        switch (buf[n - 1]) {
            case 'A': ev->code = shift ? KEY_SHIFT_UP    : alt ? KEY_ALT_UP
                               : ctrl  ? KEY_CTRL_UP     : KEY_UP;    break;
            case 'B': ev->code = shift ? KEY_SHIFT_DOWN  : alt ? KEY_ALT_DOWN
                               : ctrl  ? KEY_CTRL_DOWN   : KEY_DOWN;  break;
            case 'C': ev->code = shift ? KEY_SHIFT_RIGHT : alt ? KEY_ALT_RIGHT
                               : ctrl  ? KEY_CTRL_RIGHT  : KEY_RIGHT; break;
            case 'D': ev->code = shift ? KEY_SHIFT_LEFT  : alt ? KEY_ALT_LEFT
                               : ctrl  ? KEY_CTRL_LEFT   : KEY_LEFT;  break;

            case 'I': ev->code = KEY_FOCUS_IN; break;
            case 'O': ev->code = KEY_FOCUS_OUT; break;
            case 'H': ev->code = KEY_HOME; break;
            case 'F': ev->code = KEY_END; break;
            case '~':
                if (num == 5) ev->code = KEY_PAGE_UP;
                else if (num == 6) ev->code = KEY_PAGE_DOWN;
                else if (num == 1 || num == 7) ev->code = KEY_HOME;
                else if (num == 4 || num == 8) ev->code = KEY_END;
                break;
            default: break;
        }
    }
}

static void emit_event(InputEvent *ev) {
    if (!queue_InputEvent_push(&g_queue, ev) && ev->code == KEY_PASTE)
        free(ev->paste);
    term_wake_notify();
}

static ssize_t find_bytes(const char *hay, size_t hlen, const char *needle, size_t nlen) {
    if (nlen == 0 || hlen < nlen) return -1;
    for (size_t i = 0; i + nlen <= hlen; i++)
        if (memcmp(hay + i, needle, nlen) == 0) return (ssize_t)i;
    return -1;
}

static size_t next_token_len(const char *buf, size_t len) {
    if (len == 0) return 0;
    if (buf[0] != '\x1b') return 1;
    if (len == 1) return 1;

    if (buf[1] == 'O') return len >= 3 ? 3 : 2;
    if (buf[1] != '[') return 2;
    for (size_t i = 2; i < len; i++) {
        unsigned char c = (unsigned char)buf[i];
        if (c >= 0x40 && c <= 0x7E) return i + 1;
    }
    return 0;
}

static void *input_thread_func(void *arg) {
    Term *t = (Term *)arg;
    size_t len = 0;

    while (g_running) {

        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(t->fd, &fds);
        struct timeval tv = { .tv_sec = 0, .tv_usec = 10000 };

        int ret = select(t->fd + 1, &fds, NULL, NULL, &tv);
        if (ret <= 0 || !FD_ISSET(t->fd, &fds)) continue;

        if (g_acc_cap - len < 4096) {
            size_t ncap = g_acc_cap ? g_acc_cap * 2 : 8192;
            char *nb = realloc(g_acc, ncap);
            if (!nb) { len = 0; continue; }
            g_acc = nb;
            g_acc_cap = ncap;
        }

        ssize_t n = read(t->fd, g_acc + len, g_acc_cap - len);
        if (n <= 0) continue;
        len += (size_t)n;

        size_t off = 0;
        for (;;) {
            size_t avail = len - off;
            if (avail == 0) break;

            if (avail >= PASTE_MARK_LEN &&
                memcmp(g_acc + off, PASTE_START, PASTE_MARK_LEN) == 0) {
                const char *body = g_acc + off + PASTE_MARK_LEN;
                size_t body_max = len - off - PASTE_MARK_LEN;
                ssize_t e = find_bytes(body, body_max, PASTE_END, PASTE_MARK_LEN);
                if (e < 0) break;
                size_t plen = (size_t)e;
                char *p = malloc(plen + 1);
                if (p) {
                    memcpy(p, body, plen);
                    p[plen] = '\0';
                    InputEvent ev = { .code = KEY_PASTE, .paste = p, .paste_len = (int)plen };
                    emit_event(&ev);
                }
                off += PASTE_MARK_LEN + plen + PASTE_MARK_LEN;
                continue;
            }

            size_t tl = next_token_len(g_acc + off, avail);
            if (tl == 0) break;
            InputEvent ev;
            parse_input(g_acc + off, (ssize_t)tl, &ev);
            if (ev.code != KEY_NONE) emit_event(&ev);
            off += tl;
        }
        if (off > 0) { memmove(g_acc, g_acc + off, len - off); len -= off; }

        if (len > 16u * 1024 * 1024) len = 0;
    }
    return NULL;
}

static void handle_sigwinch(int sig) {
    (void)sig;

    g_resize_pending = 1;
    term_wake_notify();
}

static bool term_take_resize(Term *t, InputEvent *ev) {
    if (!g_resize_pending) return false;
    g_resize_pending = 0;
    if (t) term_get_size(t);
    *ev = (InputEvent){ .code = KEY_RESIZE, .pressed = true };
    return true;
}

#define TERM_KITTY_KBD_BASE 13
#define TERM_KITTY_KBD_POP  "\x1b[<1u"

void term_request_key_events(bool on, bool allow_tmux) {
    g_want_key_events = on;
    g_kbd_allow_tmux  = allow_tmux;
}

bool term_key_events(void) { return g_key_events; }

static void term_write_kbd_raw(const char *seq, bool passthrough) {
    if (!passthrough) { term_write(seq); return; }
    char buf[64];
    size_t o = 0;
    memcpy(buf, "\x1bPtmux;", 7);
    o = 7;
    for (const char *p = seq; *p && o + 3 < sizeof buf; p++) {
        if (*p == 0x1b) buf[o++] = 0x1b;
        buf[o++] = *p;
    }
    buf[o++] = 0x1b;
    buf[o++] = '\\';
    term_write_n(buf, (int)o);
}

static bool term_kbd_passthrough(void) {
    return g_tmux && g_kbd_allow_tmux && g_key_events;
}

static void term_write_kbd(const char *seq) {
    term_write_kbd_raw(seq, term_kbd_passthrough());
}

static void term_kbd_push(void) {
    char seq[16];
    snprintf(seq, sizeof seq, "\x1b[>%du",
             TERM_KITTY_KBD_BASE | (g_key_events ? 2 : 0));
    term_write_kbd(seq);
}

static void term_kbd_pop(void) { term_write_kbd(TERM_KITTY_KBD_POP); }

static char   g_restore[256];
static size_t g_restore_len = 0;
static bool   g_restore_installed = false;

static void term_restore_append(const char *seq) {
    size_t n = strlen(seq);
    if (g_restore_len + n <= sizeof g_restore) {
        memcpy(g_restore + g_restore_len, seq, n);
        g_restore_len += n;
    }
}

static void term_restore_append_kbd(const char *seq) {
    if (!term_kbd_passthrough()) { term_restore_append(seq); return; }
    char buf[64];
    size_t o = 0;
    memcpy(buf, "\x1bPtmux;", 7);
    o = 7;
    for (const char *p = seq; *p && o + 3 < sizeof buf; p++) {
        if (*p == 0x1b) buf[o++] = 0x1b;
        buf[o++] = *p;
    }
    buf[o++] = 0x1b;
    buf[o++] = '\\';
    buf[o] = '\0';
    term_restore_append(buf);
}

static void term_restore_rebuild(void) {
    g_restore_len = 0;

    if (g_term && g_term->in_alt_screen) {
        term_restore_append_kbd(TERM_KITTY_KBD_POP);
        term_restore_append("\x1b[?1049l");
    }
    term_restore_append_kbd(TERM_KITTY_KBD_POP);
    if (g_key_events) term_restore_append("\x1b[?1004l");
    term_restore_append("\x1b[?1006l\x1b[?1002l\x1b[?1000l");
    term_restore_append("\x1b[?2004l");
    term_restore_append("\x1b[?25h");
}

static void term_restore_handler(int sig) {
    if (g_restore_len) {
        ssize_t w = write(STDOUT_FILENO, g_restore, g_restore_len);
        (void)w;
    }
    if (g_term && g_term->in_raw_mode)
        tcsetattr(g_term->fd, TCSANOW, &g_term->orig_termios);
    signal(sig, SIG_DFL);
    raise(sig);
}

void term_install_signal_restore(void) {
    term_restore_rebuild();
    if (g_restore_installed) return;
    g_restore_installed = true;

    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = term_restore_handler;
    sigemptyset(&sa.sa_mask);

    static const int sigs[] = { SIGINT, SIGTERM, SIGHUP, SIGQUIT,
                                SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT };
    for (unsigned i = 0; i < sizeof sigs / sizeof *sigs; i++)
        sigaction(sigs[i], &sa, NULL);
}

static bool term_probe_kitty_kbd(int fd) {

    term_write_kbd_raw("\x1b[?u", g_tmux && g_kbd_allow_tmux);
    term_flush();

    char buf[64];
    size_t len = 0;
    struct timeval start;
    gettimeofday(&start, NULL);

    while (len < sizeof buf - 1) {
        struct timeval now;
        gettimeofday(&now, NULL);
        int spent = (int)((now.tv_sec - start.tv_sec) * 1000 +
                          (now.tv_usec - start.tv_usec) / 1000);
        int left = 300 - spent;
        if (left <= 0) break;

        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(fd, &fds);
        struct timeval tv = { left / 1000, (left % 1000) * 1000 };
        if (select(fd + 1, &fds, NULL, NULL, &tv) <= 0) break;

        ssize_t n = read(fd, buf + len, sizeof buf - 1 - len);
        if (n <= 0) break;
        len += (size_t)n;
        if (memchr(buf, 'u', len)) break;
    }
    return len >= 3 && buf[0] == '\x1b' && buf[1] == '[' && buf[2] == '?';
}

bool term_init(Term *t) {
    memset(t, 0, sizeof(Term));
    t->fd = STDIN_FILENO;

    if (tcgetattr(t->fd, &t->orig_termios) == -1) {
        return false;
    }

    struct termios raw = t->orig_termios;
    raw.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
    raw.c_oflag &= ~(OPOST);
    raw.c_cflag |= (CS8);
    raw.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG);
    raw.c_cc[VMIN] = 1;
    raw.c_cc[VTIME] = 0;

    if (tcsetattr(t->fd, TCSAFLUSH, &raw) == -1) {
        return false;
    }
    t->in_raw_mode = true;

    term_get_size(t);

    g_tmux = getenv("TMUX") != NULL;
    g_key_events = g_want_key_events && term_probe_kitty_kbd(t->fd);

    g_term = t;
    struct sigaction sa;
    sa.sa_handler = handle_sigwinch;
    sa.sa_flags = SA_RESTART;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGWINCH, &sa, NULL);

    queue_InputEvent_init(&g_queue);
    if (pipe(g_wake_pipe) == 0) {
        fcntl(g_wake_pipe[0], F_SETFL, O_NONBLOCK);
        fcntl(g_wake_pipe[1], F_SETFL, O_NONBLOCK);
    }
    g_running = true;
    if (pthread_create(&g_input_thread, NULL, input_thread_func, t) != 0) {

        g_running = false;
        if (g_wake_pipe[0] >= 0) { close(g_wake_pipe[0]); close(g_wake_pipe[1]); g_wake_pipe[0]=g_wake_pipe[1]=-1; }
        queue_InputEvent_destroy(&g_queue);
        signal(SIGWINCH, SIG_DFL);
        g_term = NULL;
        tcsetattr(t->fd, TCSAFLUSH, &t->orig_termios);
        t->in_raw_mode = false;
        return false;
    }

    term_enable_mouse();

    term_kbd_push();

    if (g_key_events) term_write("\x1b[?1004h");

    term_write("\x1b[?2004h");
    term_flush();

    return true;
}

void term_enable_mouse(void) {
    term_write("\x1b[?1000h");
    term_write("\x1b[?1002h");
    term_write("\x1b[?1006h");
}

void term_disable_mouse(void) {
    term_write("\x1b[?1006l");
    term_write("\x1b[?1002l");
    term_write("\x1b[?1000l");
}

void term_cleanup(Term *t) {
    term_write("\x1b[?2004l");

    if (t->in_alt_screen) {
        term_leave_alt_screen(t);
    }
    term_kbd_pop();
    if (g_key_events) term_write("\x1b[?1004l");
    term_disable_mouse();

    term_write("\x1b]111\x1b\\");
    term_flush();

    g_running = false;
    pthread_cancel(g_input_thread);
    pthread_join(g_input_thread, NULL);

    InputEvent drain_ev;
    while (queue_InputEvent_pop(&g_queue, &drain_ev)) {
        if (drain_ev.code == KEY_PASTE) free(drain_ev.paste);
    }
    queue_InputEvent_destroy(&g_queue);
    if (g_wake_pipe[0] >= 0) { close(g_wake_pipe[0]); close(g_wake_pipe[1]); g_wake_pipe[0]=g_wake_pipe[1]=-1; }
    free(g_acc);
    g_acc = NULL;
    g_acc_cap = 0;

    term_show_cursor();
    if (t->in_raw_mode) {

        tcflush(t->fd, TCIFLUSH);
        tcsetattr(t->fd, TCSAFLUSH, &t->orig_termios);
        t->in_raw_mode = false;
    }
    g_term = NULL;
}

void term_get_size(Term *t) {
    struct winsize ws;

    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0 && ws.ws_row > 0) {
        t->width = ws.ws_col;
        t->height = ws.ws_row;
    } else {
        t->width = 80;
        t->height = 24;
    }
}

bool term_cell_size(Term *t, int *cw, int *ch) {
    *cw = 8;
    *ch = 17;

    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) != 0) return false;
    if (ws.ws_col <= 0 || ws.ws_row <= 0) return false;
    if (ws.ws_xpixel <= 0 || ws.ws_ypixel <= 0) return false;

    int w = ws.ws_xpixel / ws.ws_col;
    int h = ws.ws_ypixel / ws.ws_row;
    if (w <= 0 || h <= 0 || w > 64 || h > 64) return false;

    if (t) { t->width = ws.ws_col; t->height = ws.ws_row; }
    *cw = w;
    *ch = h;
    return true;
}

void term_enter_alt_screen(Term *t) {
    term_write("\x1b[?1049h");

    term_kbd_push();
    term_flush();
    t->in_alt_screen = true;
    term_restore_rebuild();
}

void term_leave_alt_screen(Term *t) {
    term_kbd_pop();
    term_write("\x1b[?1049l");
    term_flush();
    t->in_alt_screen = false;
    term_restore_rebuild();
}

void term_hide_cursor(void) {
    term_write("\x1b[?25l");
}

void term_show_cursor(void) {
    term_write("\x1b[?25h");
}

void term_move_cursor(int x, int y) {
    char buf[32];
    snprintf(buf, sizeof(buf), "\x1b[%d;%dH", y + 1, x + 1);
    term_write(buf);
}

void term_clear(void) {
    term_write("\x1b[2J");
    term_write("\x1b[H");
}

void term_flush(void) {
    fflush(stdout);
}

void term_write(const char *s) {
    fputs(s, stdout);
}

void term_write_n(const char *s, int n) {
    fwrite(s, 1, n, stdout);
}

void term_set_background_color(uint8_t r, uint8_t g, uint8_t b) {
    char buf[64];
    snprintf(buf, sizeof(buf), "\x1b]11;rgb:%02x%02x/%02x%02x/%02x%02x\x1b\\",
             r, r, g, g, b, b);
    term_write(buf);
}

bool term_color_is_light(uint8_t r, uint8_t g, uint8_t b) {
    return (299u * r + 587u * g + 114u * b) / 1000u > 127u;
}

static bool term_parse_hex_channel(const char **p, uint8_t *out) {
    unsigned v = 0;
    int digits = 0;
    while (digits < 4) {
        char c = **p;
        unsigned d;
        if      (c >= '0' && c <= '9') d = (unsigned)(c - '0');
        else if (c >= 'a' && c <= 'f') d = (unsigned)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') d = (unsigned)(c - 'A' + 10);
        else break;
        v = v * 16 + d;
        (*p)++;
        digits++;
    }
    if (digits == 0) return false;

    unsigned max = (1u << (4 * digits)) - 1u;
    *out = (uint8_t)((v * 255u + max / 2) / max);
    return true;
}

static bool term_colorfgbg(bool background, uint8_t *r, uint8_t *g, uint8_t *b) {
    const char *v = getenv("COLORFGBG");
    if (!v) return false;

    const char *at = background ? strrchr(v, ';') : NULL;
    if (background) {
        if (!at || !at[1]) return false;
        at++;
    } else {
        at = v;
    }

    char *end;
    long idx = strtol(at, &end, 10);
    if (end == at || idx < 0 || idx > 15) return false;

    static const uint8_t pal[16][3] = {
        {0,0,0},       {170,0,0},   {0,170,0},   {170,85,0},
        {0,0,170},     {170,0,170}, {0,170,170}, {170,170,170},
        {85,85,85},    {255,85,85}, {85,255,85}, {255,255,85},
        {85,85,255},   {255,85,255},{85,255,255},{255,255,255},
    };
    *r = pal[idx][0]; *g = pal[idx][1]; *b = pal[idx][2];
    return true;
}

/* One question and its answer. The caller supplies the escape because the
   same question has to be asked two ways under tmux. */
static bool term_osc_exchange(const char *query, int qlen, uint8_t *r,
                              uint8_t *g, uint8_t *b, int timeout_ms) {
    struct termios orig;
    if (tcgetattr(STDIN_FILENO, &orig) == -1) return false;

    struct termios raw = orig;
    raw.c_lflag &= ~(ECHO | ICANON);
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 0;
    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == -1) return false;

    bool ok = false;

    if (write(STDOUT_FILENO, query, (size_t)qlen) == (ssize_t)qlen) {
        fflush(stdout);

        char buf[256];
        size_t len = 0;
        struct timeval start;
        gettimeofday(&start, NULL);

        for (;;) {
            struct timeval now;
            gettimeofday(&now, NULL);
            int spent = (int)((now.tv_sec - start.tv_sec) * 1000 +
                              (now.tv_usec - start.tv_usec) / 1000);
            int left = timeout_ms - spent;
            if (left <= 0) break;

            fd_set fds;
            FD_ZERO(&fds);
            FD_SET(STDIN_FILENO, &fds);
            struct timeval tv = { left / 1000, (left % 1000) * 1000 };
            if (select(STDIN_FILENO + 1, &fds, NULL, NULL, &tv) <= 0) break;

            ssize_t n = read(STDIN_FILENO, buf + len, sizeof buf - 1 - len);
            if (n <= 0) break;
            len += (size_t)n;
            buf[len] = '\0';

            /* Most terminals answer rgb:RRRR/GGGG/BBBB; a few use #RRGGBB. */
            const char *rgb = strstr(buf, "rgb:");
            const char *hash = strchr(buf, '#');
            bool done = memchr(buf, '\a', len) != NULL || strstr(buf, "\x1b\\");
            if (rgb && done) {
                const char *p = rgb + 4;
                ok = term_parse_hex_channel(&p, r) && *p++ == '/' &&
                     term_parse_hex_channel(&p, g) && *p++ == '/' &&
                     term_parse_hex_channel(&p, b);
                break;
            }
            if (hash && done) {
                unsigned rr, gg, bb;
                if (sscanf(hash + 1, "%2x%2x%2x", &rr, &gg, &bb) == 3) {
                    *r = (uint8_t)rr; *g = (uint8_t)gg; *b = (uint8_t)bb;
                    ok = true;
                }
                break;
            }
            if (len == sizeof buf - 1) break;
        }
    }

    tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig);
    return ok;
}

/* OSC 10 is the foreground, OSC 11 the background; the exchange is the same.
 *
 * Under tmux the question gets asked twice. tmux answers it itself when it
 * can, which is the quick way; when it cannot, the query has to travel to the
 * terminal outside as a passthrough with its ESCs doubled - the treatment
 * kitty.h gives its graphics escapes, and it needs allow-passthrough set. */
static bool term_query_osc_color(int osc, uint8_t *r, uint8_t *g, uint8_t *b,
                                 int timeout_ms) {
    bool background = osc == 11;
    if (!r || !g || !b) return false;
    if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO))
        return term_colorfgbg(background, r, g, b);

    char q[64];
    int n = snprintf(q, sizeof q, "\x1b]%d;?\x1b\\", osc);
    if (term_osc_exchange(q, n, r, g, b, timeout_ms)) return true;

    if (getenv("TMUX")) {
        n = snprintf(q, sizeof q, "\x1bPtmux;\x1b\x1b]%d;?\x1b\x1b\\\x1b\\", osc);
        if (term_osc_exchange(q, n, r, g, b, timeout_ms)) return true;
    }
    return term_colorfgbg(background, r, g, b);
}

bool term_query_background_color(uint8_t *r, uint8_t *g, uint8_t *b, int timeout_ms) {
    return term_query_osc_color(11, r, g, b, timeout_ms);
}

bool term_query_foreground_color(uint8_t *r, uint8_t *g, uint8_t *b, int timeout_ms) {
    return term_query_osc_color(10, r, g, b, timeout_ms);
}

bool term_poll_event(Term *t, InputEvent *ev, int timeout_ms) {

    (void)timeout_ms;
    if (term_take_resize(t, ev)) return true;
    return queue_InputEvent_pop(&g_queue, ev);
}

bool term_wait_event(Term *t, InputEvent *ev, int timeout_ms) {
    if (term_take_resize(t, ev)) return true;
    if (queue_InputEvent_pop(&g_queue, ev)) return true;
    if (g_wake_pipe[0] < 0) return false;
    struct pollfd pfd = { .fd = g_wake_pipe[0], .events = POLLIN, .revents = 0 };
    poll(&pfd, 1, timeout_ms);
    if (pfd.revents & POLLIN) {
        char drain[64]; while (read(g_wake_pipe[0], drain, sizeof drain) > 0) {}
    }
    if (term_take_resize(t, ev)) return true;
    return queue_InputEvent_pop(&g_queue, ev);
}

#endif
