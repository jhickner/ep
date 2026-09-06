CC      ?= cc
CFLAGS  ?= -std=c11 -O2 -Wall -Wextra
LDLIBS   = -lpthread -lz -lm
ifeq ($(shell uname),Darwin)
  LDLIBS += -framework CoreGraphics -framework CoreFoundation -framework CoreText
endif
PREFIX  ?= $(HOME)/.local

BIN  = ep
SRC  = src/main.c src/pdf.c src/type.c
DEPS = src/term.h src/screen.h src/kitty.h src/image.h src/stb_image.h \
       src/zip.h src/xml.h src/epub.h src/doc.h src/layout.h src/state.h src/pick.h src/pdf.h \
       src/type.h

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

install: $(BIN)
	install -d $(PREFIX)/bin
	install -m 755 $(BIN) $(PREFIX)/bin/$(BIN)

clean:
	rm -f $(BIN)

.PHONY: all install clean
