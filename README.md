# ep

A terminal reader for epubs, PDFs and comics. The file decides the reader.

## Build

```sh
make
make install          # $HOME/.local/bin, plus a cbr symlink
```

Needs a C compiler and zlib. `libjpeg-turbo` is picked up if present and is
most of the speed on comics and PDFs. Comics are unpacked with `bsdtar`.

On macOS, epub typesetting and PDF rendering use CoreText and CoreGraphics.
Elsewhere both are optional: epubs fall back to the character grid, and PDFs
need `pdftoppm` and `pdfinfo` from poppler.

Pages are drawn with the kitty graphics protocol, so PDFs and comics need
kitty or Ghostty. Inside tmux, also `tmux set -g allow-passthrough all`.

## Use

```sh
ep book.epub
ep guide.pdf
ep comic.cbz
ep ~/books              # browse
ep --resume             # pick up where you left off
```

`?` shows the keys.

## epub

Typeset into pages with CoreText: drop caps, small caps, hyphenation and
justification. `--text` wraps onto the character grid instead.

![a chapter opening](doc/epub.png)

## PDF

![a page of a PDF](doc/pdf.jpg)

## comics

`.cbr`, `.cbz`, `.cb7`, `.cbt`, or a directory of page images. `f` walks the
page a panel at a time; `tab` opens a thumbnail grid.

![a comic page](doc/comic.jpg)
