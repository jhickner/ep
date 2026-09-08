
#ifndef BOOK_H
#define BOOK_H

#include <stdbool.h>

typedef struct {
    char  *path;
    char  *title;
    char  *dir;
    char **pages;
    int    npages;
    bool   extracted;
} Book;

bool book_open(const char *path, Book *b);
void book_close(Book *b);

const char *book_cache_root(void);

void book_cache_prune(unsigned long long budget, const char *keep);

bool book_is_archive(const char *path);

bool book_is_comic(const char *path);

#endif

#ifdef BOOK_IMPLEMENTATION

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <ctype.h>
#include <limits.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <sys/wait.h>

static const char *book_ext(const char *path) {
    const char *slash = strrchr(path, '/');
    const char *dot = strrchr(slash ? slash + 1 : path, '.');
    return dot ? dot + 1 : "";
}

bool book_is_archive(const char *path) {
    static const char *exts[] = { "cbr", "cbz", "cb7", "cbt",
                                  "rar", "zip", "7z", "tar", NULL };
    const char *e = book_ext(path);
    for (int i = 0; exts[i]; i++) if (!strcasecmp(e, exts[i])) return true;
    return false;
}

bool book_is_comic(const char *path) {
    static const char *exts[] = { "cbr", "cbz", "cb7", "cbt", NULL };
    const char *e = book_ext(path);
    for (int i = 0; exts[i]; i++) if (!strcasecmp(e, exts[i])) return true;
    return false;
}

static bool book_is_image(const char *name) {
    static const char *exts[] = { "jpg", "jpeg", "png", "gif", "bmp", "webp",
                                  "tif", "tiff", "avif", "heic", NULL };
    if (name[0] == '.') return false;
    const char *e = book_ext(name);
    for (int i = 0; exts[i]; i++) if (!strcasecmp(e, exts[i])) return true;
    return false;
}

static int book_natcmp(const char *a, const char *b) {
    while (*a && *b) {
        if (*a >= '0' && *a <= '9' && *b >= '0' && *b <= '9') {
            while (*a == '0') a++;
            while (*b == '0') b++;
            const char *sa = a, *sb = b;
            while (*a >= '0' && *a <= '9') a++;
            while (*b >= '0' && *b <= '9') b++;
            long la = a - sa, lb = b - sb;
            if (la != lb) return la < lb ? -1 : 1;
            int c = strncmp(sa, sb, (size_t)la);
            if (c) return c;
        } else {
            int ca = tolower((unsigned char)*a), cb = tolower((unsigned char)*b);
            if (ca != cb) return ca < cb ? -1 : 1;
            a++; b++;
        }
    }
    return (*a ? 1 : 0) - (*b ? 1 : 0);
}

static int book_cmp(const void *x, const void *y) {
    return book_natcmp(*(char *const *)x, *(char *const *)y);
}

typedef struct { char **v; int n, cap; } BookVec;

static void bookvec_push(BookVec *v, char *s) {
    if (v->n == v->cap) {
        v->cap = v->cap ? v->cap * 2 : 64;
        v->v = (char **)realloc(v->v, (size_t)v->cap * sizeof *v->v);
    }
    v->v[v->n++] = s;
}

static void book_scan(const char *dir, BookVec *out, int depth) {
    if (depth > 8) return;
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *de;
    while ((de = readdir(d))) {
        if (de->d_name[0] == '.') continue;
        char p[PATH_MAX];
        if (snprintf(p, sizeof p, "%s/%s", dir, de->d_name) >= (int)sizeof p) continue;
        struct stat st;
        if (stat(p, &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) book_scan(p, out, depth + 1);
        else if (S_ISREG(st.st_mode) && st.st_size > 0 && book_is_image(de->d_name))
            bookvec_push(out, strdup(p));
    }
    closedir(d);
}

const char *book_cache_root(void) {
    static char root[PATH_MAX];
    if (!root[0]) {
        const char *xdg = getenv("XDG_CACHE_HOME"), *home = getenv("HOME");
        if (xdg && *xdg) snprintf(root, sizeof root, "%s/cbr", xdg);
        else             snprintf(root, sizeof root, "%s/.cache/cbr", home ? home : "/tmp");
    }
    return root;
}

static void book_mkdirs(const char *path) {
    char tmp[PATH_MAX];
    snprintf(tmp, sizeof tmp, "%s", path);
    for (char *p = tmp + 1; *p; p++) {
        if (*p != '/') continue;
        *p = '\0';
        mkdir(tmp, 0755);
        *p = '/';
    }
    mkdir(tmp, 0755);
}

static unsigned long long book_key(const char *path, const struct stat *st) {
    unsigned long long h = 1469598103934665603ULL;
    for (const char *p = path; *p; p++) { h ^= (unsigned char)*p; h *= 1099511628211ULL; }
    h ^= (unsigned long long)st->st_size;  h *= 1099511628211ULL;
    h ^= (unsigned long long)st->st_mtime; h *= 1099511628211ULL;
    return h;
}

static bool book_run(char *const argv[]) {
    pid_t pid = fork();
    if (pid < 0) return false;
    if (pid == 0) {
        int null = open("/dev/null", O_WRONLY);
        if (null >= 0) { dup2(null, 1); dup2(null, 2); close(null); }
        execvp(argv[0], argv);
        _exit(127);
    }
    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

static bool book_extract(const char *archive, const char *dest) {
    char tmp[PATH_MAX];
    snprintf(tmp, sizeof tmp, "%s.part%d", dest, (int)getpid());
    book_mkdirs(tmp);

    char *tar[] = { (char *)"bsdtar", (char *)"-xf", (char *)archive,
                    (char *)"-C", tmp, NULL };
    bool ok = book_run(tar);
    if (!ok) {
        char *un[] = { (char *)"unar", (char *)"-q", (char *)"-D",
                       (char *)"-o", tmp, (char *)archive, NULL };
        ok = book_run(un);
    }
    if (!ok) {
        char odir[PATH_MAX];
        snprintf(odir, sizeof odir, "-o%s", tmp);
        char *sz[] = { (char *)"7z", (char *)"x", (char *)"-y",
                       odir, (char *)archive, NULL };
        ok = book_run(sz);
    }
    if (!ok) { char *rm[] = { (char *)"rm", (char *)"-rf", tmp, NULL }; book_run(rm); return false; }

    rename(tmp, dest);
    return true;
}

bool book_open(const char *path, Book *b) {
    memset(b, 0, sizeof *b);

    struct stat st;
    if (stat(path, &st) != 0) return false;

    char dir[PATH_MAX];
    bool extracted = false;

    if (S_ISDIR(st.st_mode)) {
        snprintf(dir, sizeof dir, "%s", path);
    } else {
        snprintf(dir, sizeof dir, "%s/%016llx", book_cache_root(), book_key(path, &st));
        struct stat ds;
        if (stat(dir, &ds) != 0 || !S_ISDIR(ds.st_mode)) {
            book_mkdirs(book_cache_root());
            if (!book_extract(path, dir)) return false;
        } else {
            utimes(dir, NULL);
        }
        extracted = true;
    }

    BookVec v = {0};
    book_scan(dir, &v, 0);
    if (!v.n) { free(v.v); return false; }
    qsort(v.v, (size_t)v.n, sizeof *v.v, book_cmp);

    b->path = strdup(path);
    b->dir = strdup(dir);
    b->pages = v.v;
    b->npages = v.n;
    b->extracted = extracted;

    const char *base = strrchr(path, '/');
    base = base ? base + 1 : path;
    b->title = strdup(base);
    char *dot = strrchr(b->title, '.');
    if (dot && dot != b->title && book_is_archive(path)) *dot = '\0';
    return true;
}

static bool book_is_cache_name(const char *name, bool *part) {
    int n = 0;
    while (isxdigit((unsigned char)name[n])) n++;
    if (n != 16) return false;
    *part = !strncmp(name + 16, ".part", 5);
    return name[16] == '\0' || *part;
}

static unsigned long long book_dir_size(const char *dir, int depth) {
    if (depth > 8) return 0;
    DIR *d = opendir(dir);
    if (!d) return 0;
    unsigned long long total = 0;
    struct dirent *de;
    while ((de = readdir(d))) {
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
        char p[PATH_MAX];
        if (snprintf(p, sizeof p, "%s/%s", dir, de->d_name) >= (int)sizeof p) continue;
        struct stat st;
        if (lstat(p, &st) != 0) continue;
        if (S_ISDIR(st.st_mode))       total += book_dir_size(p, depth + 1);
        else if (S_ISREG(st.st_mode))  total += (unsigned long long)st.st_size;
    }
    closedir(d);
    return total;
}

typedef struct {
    char               path[PATH_MAX];
    unsigned long long size;
    time_t             used;
} BookCached;

static int book_cached_cmp(const void *x, const void *y) {
    time_t a = ((const BookCached *)x)->used, b = ((const BookCached *)y)->used;
    return a < b ? -1 : a > b ? 1 : 0;
}

#define BOOK_PART_STALE 3600

void book_cache_prune(unsigned long long budget, const char *keep) {
    const char *root = book_cache_root();
    DIR *d = opendir(root);
    if (!d) return;

    int cap = 512, n = 0;
    BookCached *e = (BookCached *)malloc(sizeof *e * (size_t)cap);
    if (!e) { closedir(d); return; }

    time_t now = time(NULL);
    unsigned long long total = 0;
    struct dirent *de;
    while (n < cap && (de = readdir(d))) {
        bool part = false;
        if (!book_is_cache_name(de->d_name, &part)) continue;
        char p[PATH_MAX];
        if (snprintf(p, sizeof p, "%s/%s", root, de->d_name) >= (int)sizeof p) continue;
        if (keep && !strcmp(p, keep)) continue;
        struct stat st;
        if (stat(p, &st) != 0 || !S_ISDIR(st.st_mode)) continue;

        if (part) {
            if (now - st.st_mtime > BOOK_PART_STALE) {
                char *rm[] = { (char *)"rm", (char *)"-rf", p, NULL };
                book_run(rm);
            }
            continue;
        }

        snprintf(e[n].path, sizeof e[n].path, "%s", p);
        e[n].size = book_dir_size(p, 0);
        e[n].used = st.st_mtime;
        total += e[n].size;
        n++;
    }
    closedir(d);

    qsort(e, (size_t)n, sizeof *e, book_cached_cmp);
    for (int i = 0; i < n && total > budget; i++) {
        char *rm[] = { (char *)"rm", (char *)"-rf", e[i].path, NULL };
        if (book_run(rm)) total -= e[i].size;
    }
    free(e);
}

void book_close(Book *b) {
    if (!b) return;
    for (int i = 0; i < b->npages; i++) free(b->pages[i]);
    free(b->pages);
    free(b->path);
    free(b->dir);
    free(b->title);
    memset(b, 0, sizeof *b);
}

#endif
