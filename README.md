<p align="center">
  <img src="logo.png" alt="ysnp logo" width="200">
</p>

# ysnp — You Shall Not Pass

[![ci](https://github.com/uRadical/ysnp/actions/workflows/ci.yml/badge.svg)](https://github.com/uRadical/ysnp/actions/workflows/ci.yml)
[![release](https://github.com/uRadical/ysnp/actions/workflows/release.yml/badge.svg)](https://github.com/uRadical/ysnp/actions/workflows/release.yml)
[![latest release](https://img.shields.io/github/v/release/uRadical/ysnp?cacheSeconds=3600)](https://github.com/uRadical/ysnp/releases/latest)
[![license: MIT](https://img.shields.io/github/license/uRadical/ysnp?cacheSeconds=3600)](LICENSE)
![platforms](https://img.shields.io/badge/platform-Linux%20%7C%20macOS-blue)

A cross-platform image overlay for Linux and macOS, written in pure C
(with one Objective-C file for macOS Cocoa). Designed to be fired from a git
`pre-push` hook when checks fail: it slaps an image above every other window,
centered on screen, until you click or press <kbd>Escape</kbd>.

## What it does

When invoked, `ysnp` displays an image, at its own native size and centered on
the screen, on top of all other windows. No arguments, no message — just the
image. Animated GIFs loop until dismissed. Click the image or press
<kbd>Escape</kbd> to close it.

With `--giphy` (`-g`) it instead pulls a random rejection-themed GIF from
[Giphy](https://giphy.com) — it picks one of a handful of search terms
(`nope`, `denied`, `you shall not pass`, …) at random, fetches a random
matching GIF, and overlays that. If the network call fails it falls back to the
embedded default, so a push is never blocked by a flaky connection.

`--giphy` needs a Giphy API key. Official release binaries have one baked in at
build time (from a GitHub secret), so they work out of the box. For builds from
source, supply your own — either set `GIPHY_API_KEY` in the environment at
runtime, or bake one in at compile time with `make GIPHY_API_KEY=yourkey`. The
runtime environment variable always takes precedence over a baked-in key, so
you can override or rotate it without rebuilding. With no key from either
source, `--giphy` silently falls back to the embedded default image.

## Install

### Install script

The quickest way — detects your platform, downloads the matching release
binary, verifies its checksum, and installs it to `~/.local/bin` (no root):

```sh
curl -fsSL https://raw.githubusercontent.com/uRadical/ysnp/main/install.sh | sh
```

Install somewhere else with `YSNP_BIN_DIR`:

```sh
curl -fsSL https://raw.githubusercontent.com/uRadical/ysnp/main/install.sh | YSNP_BIN_DIR=/usr/local/bin sh
```

The script also creates `~/.config/ysnp/images/`, clears the macOS quarantine
flag, and warns if the install directory isn't on your `PATH`.

### Prebuilt binary

Or download the binary for your platform by hand from the
[latest release](https://github.com/uRadical/ysnp/releases/latest):
`ysnp-linux-x86_64` or `ysnp-macos-arm64` (Apple Silicon).

```sh
chmod +x ysnp-*                  # make it executable
mv ysnp-* ~/.local/bin/ysnp      # put it on your PATH
mkdir -p ~/.config/ysnp/images   # where ysnp looks for images
```

Each release also ships a `SHA256SUMS` file you can verify against.

On macOS the binary is unsigned, so Gatekeeper quarantines it on first run.
Clear the flag (or right-click → Open once):

```sh
xattr -d com.apple.quarantine ~/.local/bin/ysnp
```

The Linux release binary statically links libjpeg and giflib (their sonames
differ across distros) and dynamically links cairo, wayland-client, X11,
XRandR and libcurl, whose sonames are stable everywhere. It needs glibc ≥ the Ubuntu LTS
it was built on; on older systems build from source instead (below).

### From source

See [Dependencies](#dependencies) and [Build](#build).

## Dependencies

**Linux**

- `wayland-client`
- `cairo`
- `libjpeg`
- `giflib`
- `libX11` + `libXrandr`
- `libcurl` (for `--giphy`)
- `wayland-scanner` (build-time, from `wayland-protocols`)

On a wlroots-based compositor (Sway, Hyprland, labwc, river, …) the overlay is
a native `wlr-layer-shell` surface. On compositors without layer-shell (GNOME,
KDE) it falls back to an X11 override-redirect window via XWayland, which also
covers plain X11 desktops.

**macOS**

- Xcode command line tools only (`clang` + the Cocoa framework)

## Build

```sh
make           # build ./ysnp
make install   # install to ~/.local/bin and create the images dir
make install-hook  # copy hooks/pre-push into this repo's .git/hooks/
make test      # run the unit tests (AddressSanitizer + UBSan)
make fuzz      # build the libFuzzer image-decoder harness (Linux + clang)
make debug     # rebuild with -g -O0 -fsanitize=address,undefined
make clean     # remove build/ and the binary
```

`ysnp --version` reports the build's `git describe` (or the release tag).

To fuzz the image decoders (the code that parses untrusted bytes):

```sh
make fuzz
ASAN_OPTIONS=allocator_may_return_null=1 build/fuzz_decode -detect_leaks=0 tests/corpus
```

`make install` copies the binary to `~/.local/bin/ysnp` and creates
`~/.config/ysnp/images/`. Make sure `~/.local/bin` is on your `PATH`.

## Project layout

```
src/      hand-written C / Objective-C sources
assets/   default.gif (embedded fallback) + vendored wlr-layer-shell protocol XML
build/    generated headers, objects, the wayland protocol glue (git-ignored)
hooks/    sample pre-push hook
tests/    unit tests
```

Hand-written code lives in `src/`; everything under `build/` is generated by
the Makefile (the embedded-image header via `xxd`, and on Linux the
`wlr-layer-shell` protocol code via `wayland-scanner`).

## Adding images

Drop `.png`, `.jpg`, `.jpeg`, or `.gif` files into `~/.config/ysnp/images/`
(extension match is case-insensitive). On each invocation `ysnp` picks one at
random. If the directory is empty or missing, it falls back to the compiled-in
default — the animated Gandalf "You shall not pass!" GIF.

The overlay is shown at the image's own native size, centered on the screen
(not stretched to fill).

## Git hook

Copy the sample hook into your repo and make it executable:

```sh
cp hooks/pre-push .git/hooks/pre-push
chmod +x .git/hooks/pre-push
```

Then edit `.git/hooks/pre-push` to run your actual tests or linters. When they
fail, the hook launches `ysnp` and blocks the push. (`make install-hook` will
copy the sample hook into the current repo for you.)

## The embedded default

The compiled-in fallback is the animated Gandalf "You shall not pass!" GIF
(`assets/default.gif`), so `ysnp` works out of the box with no external image.
Override it for everyday use by adding your own images to
`~/.config/ysnp/images/`.

## Diagnostics

`ysnp` is normally launched from a hook in the background (`ysnp &`), so a
failure that only printed to stderr would be invisible. Instead, every error is:

- **logged** to `$XDG_STATE_HOME/ysnp/ysnp.log` (falls back to
  `~/.local/state/ysnp/ysnp.log`), with a timestamp; and
- surfaced as a **desktop notification** (`notify-send` on Linux, `osascript` on
  macOS) so you see *why* nothing appeared — most usefully when the overlay
  can't be shown (e.g. neither a `wlr-layer-shell` compositor nor an X11
  display is available).

If you ran a push and no overlay appeared, check that log first.

## Notes

- `main.c` contains zero `#ifdef`; all platform differences live behind
  `overlay.h` — `overlay_macos.m` (Objective-C) on macOS, and on Linux
  `overlay_linux.c`, which tries the Wayland backend (`overlay_wayland.c`)
  and falls back to X11 (`overlay_x11.c`). Image decoding shared by the
  Linux backends lives in `decode.c`.
- The Linux event loop is driven by `poll()` with a computed timeout — animated
  GIFs advance frames on schedule with no busy-looping; static images simply
  block until an input event arrives.
- GIF frame delays below 20 ms are clamped to 20 ms to avoid runaway loops on
  pathological files.

## License

MIT — see [LICENSE](LICENSE). The vendored Wayland protocol descriptions
(`assets/wlr-layer-shell-unstable-v1.xml` from the wlroots project, and
`assets/xdg-shell.xml` from wayland-protocols) each carry their own MIT
copyright notice.
