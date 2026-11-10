#!/bin/sh
# One-shot installer for macvdmtool (Apple Silicon / macOS).
#
#   curl -fsSL https://raw.githubusercontent.com/nguyendinhbaotu/macvdmtool/m4-support/install.sh | sudo sh
#
# Downloads the prebuilt arm64 binary from the latest GitHub release, verifies
# its SHA-256, clears the quarantine flag, ad-hoc signs it and installs it into
# $PREFIX (default /usr/local/bin). If no release is available it falls back to
# building from source with the Xcode command line tools.
set -eu

REPO="${REPO:-nguyendinhbaotu/macvdmtool}"
PREFIX="${PREFIX:-/usr/local/bin}"
DEST="$PREFIX/macvdmtool"

if [ "$(uname -s)" != "Darwin" ]; then
    echo "macvdmtool requires macOS." >&2
    exit 1
fi
if [ "$(uname -m)" != "arm64" ]; then
    echo "macvdmtool requires an Apple Silicon (arm64) Mac." >&2
    exit 1
fi

echo "Installing macvdmtool into $DEST ..."

tmp="$(mktemp -t macvdmtool)"
trap 'rm -f "$tmp"' EXIT

url="https://github.com/$REPO/releases/latest/download/macvdmtool"
if ! curl -fsSL "$url" -o "$tmp"; then
    echo "No prebuilt binary available; building from source instead."
    dir="$(mktemp -d)"
    git clone --depth 1 "https://github.com/$REPO.git" "$dir"
    (cd "$dir" && make)
    cp "$dir/macvdmtool" "$tmp"
    rm -rf "$dir"
fi

sums="https://github.com/$REPO/releases/latest/download/SHA256SUMS"
if curl -fsSL "$sums" -o /tmp/macvdmtool-sha256 2>/dev/null; then
    expected="$(grep ' macvdmtool$' /tmp/macvdmtool-sha256 | awk '{print $1}')"
    actual="$(shasum -a 256 "$tmp" | awk '{print $1}')"
    if [ -n "$expected" ] && [ "$expected" != "$actual" ]; then
        echo "Checksum mismatch: expected $expected, got $actual" >&2
        exit 1
    fi
    echo "SHA-256 verified: $actual"
    rm -f /tmp/macvdmtool-sha256
fi

mkdir -p "$PREFIX"
mv "$tmp" "$DEST"
chmod 755 "$DEST"
# Remove the browser quarantine flag and ad-hoc sign, so Gatekeeper does not
# block the unsigned binary.
xattr -dr com.apple.quarantine "$DEST" 2>/dev/null || true
codesign -s - "$DEST" 2>/dev/null || true

echo "Installed: $DEST"
echo "Test it with:  sudo $DEST status"
