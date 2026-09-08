CC      ?= cc
CFLAGS  ?= -std=c11 -O2 -Wall -Wextra
# zlib is already needed to unpack an epub; kitty.h uses it too, to compress
# the image payload when the terminal is across an ssh connection.
CFLAGS  += -DPIX_HAVE_ZLIB
LDLIBS   = -lpthread -lz -lm
ifeq ($(shell uname),Darwin)
  LDLIBS += -framework CoreGraphics -framework CoreFoundation -framework CoreText
endif
PREFIX  ?= $(HOME)/.local

BIN  = ep
SRC  = src/main.c src/pdf.c src/type.c src/comic.c
DEPS = src/term.h src/screen.h src/kitty.h src/image.h src/stb_image.h \
       src/zip.h src/xml.h src/epub.h src/doc.h src/layout.h src/state.h src/pick.h src/pdf.h \
       src/type.h src/comic.h src/help.h src/book.h src/page.h src/panel.h src/cache.h \
       src/diskcache.h

# libjpeg-turbo decodes JPEGs straight out of the DCT at 1/2, 1/4 or 1/8 scale,
# which is most of the cost of showing a cover.
JPEG_PREFIX ?= $(shell pkg-config --variable=prefix libjpeg 2>/dev/null || \
                       brew --prefix jpeg-turbo 2>/dev/null)
ifneq ($(wildcard $(JPEG_PREFIX)/include/jpeglib.h),)
  CFLAGS  += -DPIX_HAVE_JPEG -I$(JPEG_PREFIX)/include
  LDLIBS  += -L$(JPEG_PREFIX)/lib -ljpeg
else ifneq ($(wildcard /usr/include/jpeglib.h),)
  CFLAGS  += -DPIX_HAVE_JPEG
  LDLIBS  += -ljpeg
endif

all: $(BIN)

$(BIN): $(SRC) $(DEPS)
	$(CC) $(CFLAGS) $(SRC) -o $@ $(LDLIBS)

# cbr was a program of its own until comics were folded in here; the name is
# kept as a way in, and the reader is chosen by the file either way.
install: $(BIN)
	install -d $(PREFIX)/bin
	install -m 755 $(BIN) $(PREFIX)/bin/$(BIN)
	ln -sf $(BIN) $(PREFIX)/bin/cbr

clean:
	rm -f $(BIN) tools/paneltest

# Renders a page's detected panels as numbered boxes over the page, for
# checking the segmentation without a terminal in the way.
tools/paneltest: tools/paneltest.c $(DEPS)
	$(CC) $(CFLAGS) tools/paneltest.c -Isrc -o $@ $(LDLIBS)

.PHONY: all install clean
