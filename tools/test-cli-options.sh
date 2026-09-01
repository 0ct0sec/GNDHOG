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
expect_invalid '--frames needs a positive decimal integer' --frames 0
expect_invalid '--frames needs a positive decimal integer' --frames -1
expect_invalid '--frames needs a positive decimal integer' --frames forever

"$bin" --headless --no-autoconnect --baud 115200 --frames 1 \
    >"$tmp/out" 2>"$tmp/err"
checks=$((checks + 1))

"$bin" --headless --no-autoconnect --mute --frames 1 \
    >"$tmp/out" 2>"$tmp/err"
checks=$((checks + 1))

echo "cli-options: $checks scenarios passed"
