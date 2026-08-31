#!/bin/sh
# Install bfcli on an M5Stack Cardputer Zero.
#
# Staged and reversible: the previous binary is kept as bfcli.prev, and
# --uninstall puts everything back. Nothing outside /opt/bfcli and the
# APPLaunch entry is touched -- no services, no boot partition, no device tree.
#
# Usage:  sudo ./install.sh [--from DIR] [--user NAME] [--add-groups] [--uninstall]

set -eu

PREFIX=/opt/bfcli
LAUNCH_DIR=/usr/share/APPLaunch
DESKTOP="$LAUNCH_DIR/applications/bfcli.desktop"
SRC_DIR=""
TARGET_USER="${SUDO_USER:-${USER:-root}}"
ADD_GROUPS=0
UNINSTALL=0

while [ $# -gt 0 ]; do
    case "$1" in
        --from) SRC_DIR="$2"; shift 2 ;;
        --user) TARGET_USER="$2"; shift 2 ;;
        --add-groups) ADD_GROUPS=1; shift ;;
        --uninstall) UNINSTALL=1; shift ;;
        -h|--help) sed -n '2,10p' "$0"; exit 0 ;;
        *) echo "unknown option: $1" >&2; exit 2 ;;
    esac
done

if [ "$(id -u)" -ne 0 ]; then
    echo "This needs root: sudo $0 $*" >&2
    exit 1
fi

# ---------------------------------------------------------------- uninstall

if [ "$UNINSTALL" -eq 1 ]; then
    rm -f "$DESKTOP"
    rm -rf "$PREFIX"
    echo "Removed $PREFIX and $DESKTOP."
    echo "Backups under ~/.local/share/bfcli were left alone."
    exit 0
fi

# ------------------------------------------------------------------- checks

if [ -z "$SRC_DIR" ]; then
    SRC_DIR="$(cd "$(dirname "$0")/.." && pwd)"
fi

BINARY=""
for candidate in "$SRC_DIR/build-arm64/bfcli" "$SRC_DIR/build/bfcli" "$SRC_DIR/bfcli"; do
    [ -f "$candidate" ] && { BINARY="$candidate"; break; }
done
if [ -z "$BINARY" ]; then
    echo "No bfcli binary found under $SRC_DIR." >&2
    echo "Build one first:  make arm64   (cross)   or   make -j1   (on the device)" >&2
    exit 1
fi

ARCH="$(uname -m)"
if [ "$ARCH" != "aarch64" ]; then
    echo "warning: this host is $ARCH, not aarch64 -- installing anyway." >&2
fi
if [ -r /proc/device-tree/model ]; then
    MODEL="$(tr -d '\000' < /proc/device-tree/model)"
    echo "Board: $MODEL"
fi

# The binary must actually be runnable here before anything is replaced.
if [ "$ARCH" = "aarch64" ]; then
    if ! "$BINARY" --selftest >/dev/null 2>&1; then
        echo "warning: $BINARY --selftest did not pass on this machine." >&2
        echo "         Installing anyway; run it by hand to see why." >&2
    fi
fi

# ------------------------------------------------------------------ install

mkdir -p "$PREFIX/bin"

if [ -f "$PREFIX/bin/bfcli" ]; then
    cp -f "$PREFIX/bin/bfcli" "$PREFIX/bin/bfcli.prev"
    echo "Kept the previous binary as $PREFIX/bin/bfcli.prev"
fi

# Install to a temporary name and rename, so a half-copied binary is never
# left in place if this is interrupted.
cp -f "$BINARY" "$PREFIX/bin/bfcli.new"
chmod 0755 "$PREFIX/bin/bfcli.new"
mv -f "$PREFIX/bin/bfcli.new" "$PREFIX/bin/bfcli"

cat > "$PREFIX/run-bfcli" <<'WRAPPER'
#!/bin/sh
# APPLaunch entry point. Escape unwinds bfcli's screens and then exits, which
# releases the framebuffer and the keyboard so the launcher can reclaim them.
exec /opt/bfcli/bin/bfcli "$@"
WRAPPER
chmod 0755 "$PREFIX/run-bfcli"

if [ -d "$LAUNCH_DIR/applications" ]; then
    cat > "$DESKTOP" <<'ENTRY'
[Desktop Entry]
Type=Application
Name=Betaflight CLI
Comment=Betaflight CLI over USB for tinywhoops
Exec=/opt/bfcli/run-bfcli
Terminal=false
Categories=Utility;
ENTRY
    chmod 0644 "$DESKTOP"
    echo "Launcher entry: $DESKTOP"
else
    echo "note: $LAUNCH_DIR/applications not found; skipped the launcher entry."
    echo "      Run $PREFIX/run-bfcli directly."
fi

# ------------------------------------------------------------------- groups

echo
echo "Access check for user '$TARGET_USER':"
NEEDED=""
for grp in video input dialout; do
    if getent group "$grp" >/dev/null 2>&1; then
        if id -nG "$TARGET_USER" 2>/dev/null | tr ' ' '\n' | grep -qx "$grp"; then
            echo "  $grp     ok"
        else
            echo "  $grp     MISSING"
            NEEDED="$NEEDED $grp"
        fi
    fi
done

if [ -n "$NEEDED" ]; then
    if [ "$ADD_GROUPS" -eq 1 ]; then
        for grp in $NEEDED; do usermod -aG "$grp" "$TARGET_USER"; done
        echo "  Added$NEEDED. Log out and back in for this to take effect."
    else
        echo
        echo "  bfcli needs: video (framebuffer), input (keyboard), dialout (serial)."
        echo "  Add them with:  sudo usermod -aG$(echo "$NEEDED" | tr ' ' ',') $TARGET_USER"
        echo "  or re-run this script with --add-groups."
    fi
fi

echo
echo "Installed. Launch it from APPLaunch, or run: $PREFIX/run-bfcli"
echo "Check what it can see first with:  $PREFIX/bin/bfcli --list-ports"
