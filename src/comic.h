#ifndef COMIC_H
#define COMIC_H

/**
 * comic.h - the comic reader, implemented in comic.c
 *
 * Kept out of main.c so that the reader's own statics - a view, a fit, a page
 * cache - stay to themselves; the three book readers otherwise share nothing
 * but the terminal.
 *
 * A page is shown fitted to the window, or width-fitted and scrolled, or cut
 * into panels and walked one at a time; tab opens a thumbnail grid.
 */

#include <stdbool.h>
#include "term.h"
#include "screen.h"

typedef struct {
    bool panel_mode;    /* start in panel mode */
    bool fit_width;     /* start width-fitted */
    bool forced;        /* one of the above was asked for, so ignore the saved mode */
} ComicOpts;

/* Named as a comic: .cbr/.cbz/.cb7/.cbt. */
bool comic_is_file(const char *path);

/* A directory holding page images, and so a book rather than a shelf. */
bool comic_dir_is_book(const char *dir);

/* A directory to look inside rather than read. */
bool comic_dir_is_shelf(const char *dir);

/* For the directory picker: what to list, and which directories are openable
   as books in their own right. */
bool comic_accept(const char *path, const char *name, bool is_dir, void *ctx);
bool comic_leaf(const char *path, void *ctx);

/* Reads `paths` (] and [ move between them) with the terminal already in the
   state ui_start left it in. */
int comic_read(char **paths, int npaths, const ComicOpts *o, Term *tm, Screen *scr);

/* One-shot import of ~/.config/cbr/state, from when the comic reader was its
   own program. Does nothing once it has run, or if there is nothing to take. */
void comic_import_state(void);

#endif /* COMIC_H */
