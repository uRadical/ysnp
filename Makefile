UNAME    := $(shell uname)
BIN       = ysnp
SRCDIR    = src
BUILDDIR  = build

CFLAGS   += -std=gnu11 -Wall -Wextra -O2 -I$(BUILDDIR)
DEPFLAGS  = -MMD -MP

ifdef DEBUG
    CFLAGS += -g -O0 -fsanitize=address,undefined
endif

DEFAULT_IMG = $(BUILDDIR)/default_img.h

ifeq ($(UNAME), Darwin)
    CC      = clang
    CFLAGS += -fobjc-arc
    OBJS    = $(BUILDDIR)/main.o $(BUILDDIR)/image.o $(BUILDDIR)/log.o \
              $(BUILDDIR)/overlay_macos.o
    LIBS    = -framework Cocoa
    GENERATED = $(DEFAULT_IMG)
else
    CC      = cc
    WAYLAND_SCANNER ?= wayland-scanner
    WLR_XML ?= $(firstword \
        $(wildcard /usr/share/wayland-protocols/unstable/wlr-layer-shell-unstable-v1.xml) \
        $(wildcard /usr/local/share/wayland-protocols/unstable/wlr-layer-shell-unstable-v1.xml) \
        assets/wlr-layer-shell-unstable-v1.xml)
    WLR_PROTO_C = $(BUILDDIR)/wlr-layer-shell-unstable-v1-protocol.c
    WLR_PROTO_H = $(BUILDDIR)/wlr-layer-shell-unstable-v1-client-protocol.h
    OBJS    = $(BUILDDIR)/main.o $(BUILDDIR)/image.o $(BUILDDIR)/log.o \
              $(BUILDDIR)/overlay_wayland.o \
              $(BUILDDIR)/wlr-layer-shell-unstable-v1-protocol.o
    CFLAGS += $(shell pkg-config --cflags wayland-client cairo libjpeg)
    LIBS    = $(shell pkg-config --libs wayland-client cairo libjpeg) -lgif
    GENERATED = $(DEFAULT_IMG) $(WLR_PROTO_H) $(WLR_PROTO_C)
endif

all: $(BIN)

$(BUILDDIR):
	mkdir -p $(BUILDDIR)

# --- generated sources ---------------------------------------------------

$(DEFAULT_IMG): assets/default.gif | $(BUILDDIR)
	xxd -i $< | sed \
	    's/assets_default_gif/default_img_data/g; \
	     s/unsigned int default_img_data_len/unsigned int default_img_len/' > $@

$(WLR_PROTO_H): $(WLR_XML) | $(BUILDDIR)
	$(WAYLAND_SCANNER) client-header $< $@

$(WLR_PROTO_C): $(WLR_XML) | $(BUILDDIR)
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

.PHONY: all debug test install install-hook clean
