#!/bin/sh
# ZoneZap installer for WSL 2.
#
#   curl -fsSL https://boazcohenj.github.io/ZoneZap/install.sh | sudo sh
#
# Downloads the latest static zonezap binary for this CPU from GitHub Releases,
# checks its SHA-256, then runs `zonezap install`, which copies it to
# /usr/local/bin and starts it with this distro (systemd service, or the [boot]
# command in /etc/wsl.conf when systemd is off). Undo with: sudo zonezap uninstall
# Extra arguments are passed on, e.g. `| sudo sh -s -- --stream com.dropbox.attrs`.
set -eu

REPO="BoazCohenJ/ZoneZap"

fail() { echo "zonezap installer: $*" >&2; exit 1; }

[ "$(id -u)" = 0 ] || fail "run it with sudo: curl -fsSL https://boazcohenj.github.io/ZoneZap/install.sh | sudo sh"

case "$(uname -m)" in
    x86_64 | amd64) arch=x86_64 ;;
    aarch64 | arm64) arch=aarch64 ;;
    *) fail "unsupported CPU $(uname -m); build from source: https://github.com/$REPO" ;;
esac

if ! grep -qi microsoft /proc/version 2>/dev/null; then
    echo "Note: this doesn't look like WSL. ZoneZap is meant for WSL 2 distros." >&2
fi

# ZONEZAP_URL lets you install from a mirror or a local build folder (file:///...).
url="${ZONEZAP_URL:-https://github.com/$REPO/releases/latest/download}/zonezap-linux-$arch"
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT INT TERM

fetch() {
    if command -v curl >/dev/null 2>&1; then
        curl -fsSL "$1" -o "$2"
    elif command -v wget >/dev/null 2>&1; then
        wget -q "$1" -O "$2"
    else
        fail "needs curl or wget"
    fi
}

echo "Downloading zonezap-linux-$arch ..."
fetch "$url" "$tmp/zonezap" || fail "download failed: $url"
fetch "$url.sha256" "$tmp/zonezap.sha256" || fail "download failed: $url.sha256"

expected=$(cut -d' ' -f1 "$tmp/zonezap.sha256")
if command -v sha256sum >/dev/null 2>&1; then
    actual=$(sha256sum "$tmp/zonezap" | cut -d' ' -f1)
else
    actual=$(openssl dgst -sha256 "$tmp/zonezap" | sed 's/.*= //')
fi
[ -n "$expected" ] && [ "$expected" = "$actual" ] || fail "checksum mismatch (expected $expected, got $actual)"
echo "Checksum OK ($actual)"

chmod 755 "$tmp/zonezap"
"$tmp/zonezap" install "$@"
