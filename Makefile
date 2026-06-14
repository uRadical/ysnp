UNAME    := $(shell uname)
BIN       = ysnp
SRCDIR    = src
BUILDDIR  = build

# Overridable for reproducible release builds: make VERSION=v1.0.0
VERSION  ?= $(shell git describe --tags --always --dirty 2>/dev/null || echo unknown)

CFLAGS   += -std=gnu11 -Wall -Wextra -O2 -I$(BUILDDIR) -DYSNP_VERSION='"$(VERSION)"'
DEPFLAGS  = -MMD -MP

# Optionally bake a default Giphy API key into the build (for --giphy):
#   make GIPHY_API_KEY=xxxx
# The release workflow passes this from a GitHub secret. When unset, --giphy
# still works if GIPHY_API_KEY is in the environment at runtime; otherwise it
# falls back to the embedded default image.
GIPHY_API_KEY ?=
ifneq ($(GIPHY_API_KEY),)
    CFLAGS += -DGIPHY_DEFAULT_API_KEY='"$(GIPHY_API_KEY)"'
endif

ifdef DEBUG
    CFLAGS += -g -O0 -fsanitize=address,undefined
endif

DEFAULT_IMG = $(BUILDDIR)/default_img.h

ifeq ($(UNAME), Darwin)
    CC      = clang
    CFLAGS += -fobjc-arc
    OBJS    = $(BUILDDIR)/main.o $(BUILDDIR)/image.o $(BUILDDIR)/log.o \
              $(BUILDDIR)/giphy.o $(BUILDDIR)/overlay_macos.o
    LIBS    = -framework Cocoa -lcurl
    GENERATED = $(DEFAULT_IMG)
else
    CC      = cc
    WAYLAND_SCANNER ?= wayland-scanner
    WLR_XML ?= $(firstword \
        $(wildcard /usr/share/wayland-protocols/unstable/wlr-layer-shell-unstable-v1.xml) \
        $(wildcard /usr/local/share/wayland-protocols/unstable/wlr-layer-shell-unstable-v1.xml) \
        assets/wlr-layer-shell-unstable-v1.xml)
    # layer-shell's get_popup request references xdg_popup, so the generated
    # layer-shell code needs xdg_popup_interface, which lives in xdg-shell.
    XDG_XML ?= $(firstword \
        $(wildcard /usr/share/wayland-protocols/stable/xdg-shell/xdg-shell.xml) \
        $(wildcard /usr/local/share/wayland-protocols/stable/xdg-shell/xdg-shell.xml) \
        assets/xdg-shell.xml)
    WLR_PROTO_C = $(BUILDDIR)/wlr-layer-shell-unstable-v1-protocol.c
    WLR_PROTO_H = $(BUILDDIR)/wlr-layer-shell-unstable-v1-client-protocol.h
    XDG_PROTO_C = $(BUILDDIR)/xdg-shell-protocol.c
    OBJS    = $(BUILDDIR)/main.o $(BUILDDIR)/image.o $(BUILDDIR)/log.o \
              $(BUILDDIR)/giphy.o \
              $(BUILDDIR)/decode.o $(BUILDDIR)/overlay_linux.o \
              $(BUILDDIR)/overlay_wayland.o $(BUILDDIR)/overlay_x11.o \
              $(BUILDDIR)/wlr-layer-shell-unstable-v1-protocol.o \
              $(BUILDDIR)/xdg-shell-protocol.o
    CFLAGS += $(shell pkg-config --cflags wayland-client cairo libjpeg x11 xrandr libcurl)
    # libjpeg's soname differs across distros (Ubuntu: .so.8, Fedora: .so.62),
    # so release binaries link the small decoders statically; the remaining
    # dynamic deps (cairo, X11, wayland, glibc) share sonames everywhere.
    ifdef STATIC_DECODERS
        LIBS = $(shell pkg-config --libs wayland-client cairo x11 xrandr libcurl) \
               -l:libjpeg.a -l:libgif.a
    else
        LIBS = $(shell pkg-config --libs wayland-client cairo libjpeg x11 xrandr libcurl) -lgif
    endif
    GENERATED = $(DEFAULT_IMG) $(WLR_PROTO_H) $(WLR_PROTO_C) $(XDG_PROTO_C)
endif

all: $(BIN)

$(BUILDDIR):
	mkdir -p $(BUILDDIR)

# --- generated sources ---------------------------------------------------

$(DEFAULT_IMG): assets/default.gif | $(BUILDDIR)
	xxd -i $< | sed -e 's/assets_default_gif/default_img_data/g' -e 's/unsigned int default_img_data_len/unsigned int default_img_len/' > $@

$(WLR_PROTO_H): $(WLR_XML) | $(BUILDDIR)
	$(WAYLAND_SCANNER) client-header $< $@

$(WLR_PROTO_C): $(WLR_XML) | $(BUILDDIR)
	$(WAYLAND_SCANNER) private-code $< $@

$(XDG_PROTO_C): $(XDG_XML) | $(BUILDDIR)
	$(WAYLAND_SCANNER) private-code $< $@

# --- compilation ---------------------------------------------------------

$(BUILDDIR)/%.o: $(SRCDIR)/%.c | $(BUILDDIR) $(GENERATED)
	$(CC) $(DEPFLAGS) $(CFLAGS) -c $< -o $@

$(BUILDDIR)/%.o: $(SRCDIR)/%.m | $(BUILDDIR) $(GENERATED)
	$(CC) $(DEPFLAGS) $(CFLAGS) -c $< -o $@

$(BUILDDIR)/%.o: $(BUILDDIR)/%.c | $(BUILDDIR) $(GENERATED)
	$(CC) $(DEPFLAGS) $(CFLAGS) -c $< -o $@

$(BIN): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $(OBJS) $(LIBS)

# --- developer targets ---------------------------------------------------

debug:
	$(MAKE) clean
	$(MAKE) DEBUG=1

test: | $(BUILDDIR)
	$(CC) -std=gnu11 -Wall -Wextra -g -fsanitize=address,undefined \
	    tests/test_image.c -o $(BUILDDIR)/test_image
	$(BUILDDIR)/test_image

# libFuzzer harness over the image decoders (Linux + clang only). Run:
#   ASAN_OPTIONS=allocator_may_return_null=1 \
#       build/fuzz_decode -detect_leaks=0 tests/corpus
# (see tests/fuzz_decode.c for why both flags)
fuzz: $(DEFAULT_IMG)
	clang -std=gnu11 -Wall -Wextra -g -O1 \
	    -fsanitize=fuzzer,address,undefined -I$(BUILDDIR) \
	    $(shell pkg-config --cflags cairo libjpeg) \
	    tests/fuzz_decode.c -o $(BUILDDIR)/fuzz_decode \
	    $(shell pkg-config --libs cairo libjpeg) -lgif

install: all
	mkdir -p ~/.local/bin
	install -m 755 $(BIN) ~/.local/bin/$(BIN)
	mkdir -p ~/.config/ysnp/images
	@echo "ysnp installed."
	@echo "Add .png .jpg .jpeg .gif images to ~/.config/ysnp/images/"

install-hook:
	@GIT_DIR=$$(git rev-parse --git-dir 2>/dev/null) || { \
	    echo "Not inside a git repo — run from your repo, or copy hooks/pre-push to .git/hooks/ manually"; exit 1; }; \
	cp hooks/pre-push "$$GIT_DIR/hooks/pre-push"; \
	chmod +x "$$GIT_DIR/hooks/pre-push"; \
	echo "pre-push hook installed to $$GIT_DIR/hooks/"

clean:
	rm -rf $(BUILDDIR) $(BIN)

-include $(OBJS:.o=.d)

.PHONY: all debug test fuzz install install-hook clean
