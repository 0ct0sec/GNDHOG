#!/bin/sh
# Build a policy-compliant Cardputer Zero AppStore Debian package.

set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
PYTHON=${PYTHON:-python3}
READELF=${READELF:-aarch64-linux-gnu-readelf}
DPKG_DEB=${DPKG_DEB:-dpkg-deb}
MANIFEST="$ROOT/app-builder.json"
BINARY=${GNDHOG_ARM64_BINARY:-"$ROOT/build-arm64/bfcli"}
OUTPUT_DIR=${1:-"$ROOT/dist"}

command -v "$PYTHON" >/dev/null 2>&1 || {
    echo "python3 is required to read and validate app-builder.json" >&2
    exit 1
}
command -v "$READELF" >/dev/null 2>&1 || {
    echo "aarch64-linux-gnu-readelf is required; install binutils-aarch64-linux-gnu" >&2
    exit 1
}
command -v "$DPKG_DEB" >/dev/null 2>&1 || {
    echo "dpkg-deb is required; install dpkg-dev" >&2
    exit 1
}

[ -f "$MANIFEST" ] || { echo "missing $MANIFEST" >&2; exit 1; }
[ -f "$BINARY" ] || { echo "missing ARM64 binary: $BINARY (run make arm64)" >&2; exit 1; }

PACKAGE=$(
    "$PYTHON" -c 'import json,sys; print(json.load(open(sys.argv[1], encoding="utf-8"))["package_name"])' "$MANIFEST"
)
VERSION=$(
    "$PYTHON" -c 'import json,sys; print(json.load(open(sys.argv[1], encoding="utf-8"))["version"])' "$MANIFEST"
)

[ "$PACKAGE" = "bfcli" ] || { echo "unexpected package_name: $PACKAGE" >&2; exit 1; }
case "$VERSION" in
    ''|*[!0-9A-Za-z.+:~-]*) echo "invalid Debian version: $VERSION" >&2; exit 1 ;;
esac

if ! "$READELF" -h "$BINARY" | grep -q 'Machine:.*AArch64'; then
    echo "$BINARY is not an AArch64 executable" >&2
    exit 1
fi

STAGE_PARENT=$(mktemp -d "${TMPDIR:-/tmp}/gndhog-package.XXXXXX")
trap 'rm -rf "$STAGE_PARENT"' EXIT HUP INT TERM
STAGE="$STAGE_PARENT/$PACKAGE"

install -d -m 0755 \
    "$STAGE/DEBIAN" \
    "$STAGE/opt/bfcli/bin" \
    "$STAGE/usr/share/APPLaunch/applications" \
    "$STAGE/usr/share/APPLaunch/share/images" \
    "$STAGE/usr/share/doc/bfcli"

install -m 0755 "$BINARY" "$STAGE/opt/bfcli/bin/bfcli"
install -m 0755 "$ROOT/packaging/run-bfcli" "$STAGE/opt/bfcli/run-bfcli"
install -m 0644 "$ROOT/packaging/bfcli.desktop" \
    "$STAGE/usr/share/APPLaunch/applications/bfcli.desktop"
install -m 0644 "$ROOT/assets/gndhog-zero_100.png" \
    "$STAGE/usr/share/APPLaunch/share/images/gndhog-zero_100.png"
install -m 0644 "$ROOT/README.md" "$STAGE/usr/share/doc/bfcli/README.md"
install -m 0644 "$ROOT/packaging/copyright" "$STAGE/usr/share/doc/bfcli/copyright"

sed "s/@VERSION@/$VERSION/g" "$ROOT/packaging/control.in" > "$STAGE/DEBIAN/control"
chmod 0644 "$STAGE/DEBIAN/control"

mkdir -p "$OUTPUT_DIR"
OUTPUT="$OUTPUT_DIR/${PACKAGE}_${VERSION}_arm64.deb"
"$DPKG_DEB" --root-owner-group --build "$STAGE" "$OUTPUT"
"$PYTHON" "$ROOT/tools/validate-app-store.py" "$OUTPUT"

echo "Cardputer Zero package ready: $OUTPUT"
