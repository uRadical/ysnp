#!/bin/sh
# ysnp installer — detect the platform, download the matching release binary
# from GitHub, verify its checksum, and install it without needing root.
#
#   curl -fsSL https://raw.githubusercontent.com/uRadical/ysnp/main/install.sh | sh
#
# Override where it installs with YSNP_BIN_DIR, e.g.
#   curl -fsSL .../install.sh | YSNP_BIN_DIR=/usr/local/bin sh

set -eu

REPO="uRadical/ysnp"
# Default to ~/.local/bin: writable without root and the XDG convention.
BIN_DIR="${YSNP_BIN_DIR:-$HOME/.local/bin}"
IMAGES_DIR="$HOME/.config/ysnp/images"

die() { printf 'error: %s\n' "$*" >&2; exit 1; }
info() { printf '%s\n' "$*" >&2; }

# --- pick the release asset for this platform ----------------------------
os="$(uname -s)"
arch="$(uname -m)"
case "$os $arch" in
    "Darwin arm64")          asset="ysnp-macos-arm64" ;;
    "Linux x86_64")          asset="ysnp-linux-x86_64" ;;
    "Linux aarch64"|"Linux arm64"|"Darwin x86_64")
        die "no prebuilt binary for $os $arch — build from source: https://github.com/$REPO#from-source" ;;
    *)
        die "unsupported platform: $os $arch — see https://github.com/$REPO#from-source" ;;
esac
info "Detected $os $arch -> $asset"

# --- locate a downloader and a sha256 tool -------------------------------
if command -v curl >/dev/null 2>&1; then
    fetch() { curl -fsSL "$1" -o "$2"; }
elif command -v wget >/dev/null 2>&1; then
    fetch() { wget -qO "$2" "$1"; }
else
    die "need curl or wget to download"
fi

if command -v sha256sum >/dev/null 2>&1; then
    sha256() { sha256sum "$1" | awk '{print $1}'; }
elif command -v shasum >/dev/null 2>&1; then
    sha256() { shasum -a 256 "$1" | awk '{print $1}'; }
else
    sha256() { echo ""; }  # no tool available; we'll skip verification
fi

base_url="https://github.com/$REPO/releases/latest/download"

# --- download into a temp dir, verify, then install ----------------------
tmp="$(mktemp -d "${TMPDIR:-/tmp}/ysnp-install.XXXXXX")"
trap 'rm -rf "$tmp"' EXIT INT TERM

info "Downloading latest release..."
fetch "$base_url/$asset" "$tmp/$asset" || die "download failed for $asset"

if fetch "$base_url/SHA256SUMS" "$tmp/SHA256SUMS" 2>/dev/null; then
    want="$(awk -v a="$asset" '$2 == a {print $1}' "$tmp/SHA256SUMS")"
    got="$(sha256 "$tmp/$asset")"
    if [ -z "$got" ]; then
        info "warning: no sha256 tool found; skipping checksum verification"
    elif [ -z "$want" ]; then
        info "warning: $asset not listed in SHA256SUMS; skipping verification"
    elif [ "$want" != "$got" ]; then
        die "checksum mismatch for $asset (expected $want, got $got)"
    else
        info "Checksum verified."
    fi
else
    info "warning: could not fetch SHA256SUMS; skipping checksum verification"
fi

mkdir -p "$BIN_DIR"
install -m 755 "$tmp/$asset" "$BIN_DIR/ysnp"
mkdir -p "$IMAGES_DIR"

# macOS quarantines unsigned downloads; clear it so it runs without a prompt.
if [ "$os" = "Darwin" ] && command -v xattr >/dev/null 2>&1; then
    xattr -d com.apple.quarantine "$BIN_DIR/ysnp" 2>/dev/null || true
fi

info "Installed ysnp to $BIN_DIR/ysnp"
info "Add your own images to $IMAGES_DIR"

# --- warn if the install dir isn't on PATH -------------------------------
case ":$PATH:" in
    *":$BIN_DIR:"*) ;;
    *)
        info ""
        info "note: $BIN_DIR is not on your PATH. Add it, e.g.:"
        info "  echo 'export PATH=\"$BIN_DIR:\$PATH\"' >> ~/.profile"
        info "  (or ~/.zshrc on macOS), then restart your shell."
        ;;
esac
