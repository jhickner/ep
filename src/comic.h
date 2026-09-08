#ifndef COMIC_H
#define COMIC_H

#include <stdbool.h>
#include "term.h"
#include "screen.h"

typedef struct {
    bool panel_mode;
    bool fit_width;
    bool forced;
} ComicOpts;

bool comic_is_file(const char *path);

bool comic_dir_is_book(const char *dir);

bool comic_dir_is_shelf(const char *dir);

bool comic_accept(const char *path, const char *name, bool is_dir, void *ctx);
bool comic_leaf(const char *path, void *ctx);

int comic_read(char **paths, int npaths, const ComicOpts *o, Term *tm, Screen *scr);

void comic_import_state(void);

#endif
