#!/bin/sh
# A build identifies its source, not the date somebody copied the executable.
set -eu
out=$1
commit=unknown
if git rev-parse --verify HEAD >/dev/null 2>&1; then
    commit=$(git rev-parse --short=7 HEAD)
    if [ -n "$(git status --porcelain --untracked-files=normal)" ]; then
        commit="$commit-dirty"
    fi
fi
mkdir -p "$(dirname "$out")"
tmp="$out.tmp"
trap 'rm -f "$tmp"' EXIT HUP INT TERM
printf '#pragma once\n#define GNDHOG_BUILD_COMMIT "%s"\n' "$commit" > "$tmp"
if ! cmp -s "$tmp" "$out"; then mv "$tmp" "$out"; fi
