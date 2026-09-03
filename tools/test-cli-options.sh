#!/bin/sh
set -eu

bin=${1:-build/bfcli}
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT HUP INT TERM
export BFCLI_DATA_DIR="$tmp/data"

checks=0

expect_invalid() {
    pattern=$1
    shift
    set +e
    "$bin" "$@" >"$tmp/out" 2>"$tmp/err"
    rc=$?
    set -e
    if [ "$rc" -ne 2 ] || ! grep -F -- "$pattern" "$tmp/err" >/dev/null; then
        echo "cli-options: expected exit 2 and '$pattern' for: $*" >&2
        sed -n '1,4p' "$tmp/err" >&2
        exit 1
    fi
    checks=$((checks + 1))
}

expect_invalid '--baud needs a positive decimal integer' --baud nope
expect_invalid '--baud needs a positive decimal integer' --baud 99999999999999999999
expect_invalid 'unsupported baud rate 1200' --baud 1200
expect_invalid 'unsupported baud rate 12345' --baud 12345
expect_invalid '--fc-baud needs a positive decimal integer' --fc-baud nope
expect_invalid 'unsupported baud rate 1200' --mesh-baud 1200
expect_invalid 'unsupported baud rate 300' --gnss-baud 300
expect_invalid '--gnss-baud needs a value' --gnss-baud
expect_invalid '--frames needs a positive decimal integer' --frames 0
expect_invalid '--frames needs a positive decimal integer' --frames -1
expect_invalid '--frames needs a positive decimal integer' --frames forever
expect_invalid '--gnss needs a value' --gnss
expect_invalid 'unknown option --meshtastic' --meshtastic
expect_invalid 'cannot both be used' --sim --sim-mesh

"$bin" --headless --no-autoconnect --baud 115200 --frames 1 \
    >"$tmp/out" 2>"$tmp/err"
checks=$((checks + 1))

# One rate per peer, all three named on the same launch, with nothing plugged in.
"$bin" --headless --no-autoconnect --mute --no-gnss \
    --fc-baud 57600 --mesh-baud 921600 --gnss-baud 9600 --frames 1 \
    >"$tmp/out" 2>"$tmp/err"
checks=$((checks + 1))

# ...and none of those may have reached the config file. These switches are
# launch-wide overrides, exactly like --mute and --no-gnss, and the app has to
# be checked against the file it actually left behind.
if grep -qE '^(fc|mesh|gnss)\.baud = (57600|921600|9600)$' "$BFCLI_DATA_DIR/config.ini"; then
    echo "cli-options: a --*-baud override was persisted to config.ini" >&2
    grep -E 'baud' "$BFCLI_DATA_DIR/config.ini" >&2
    exit 1
fi
checks=$((checks + 1))

"$bin" --headless --no-autoconnect --mute --frames 1 \
    >"$tmp/out" 2>"$tmp/err"
checks=$((checks + 1))

# The mesh switches have to be accepted with nothing plugged in: a launcher
# entry carrying them must still start on an empty bench.
"$bin" --headless --no-autoconnect --mute --no-gnss --mesh --frames 1 \
    >"$tmp/out" 2>"$tmp/err"
checks=$((checks + 1))

"$bin" --headless --no-autoconnect --mute --gnss /dev/null --frames 1 \
    >"$tmp/out" 2>"$tmp/err"
checks=$((checks + 1))

echo "cli-options: $checks scenarios passed"
