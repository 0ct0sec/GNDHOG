#!/bin/sh
# Exercise source identity in a disposable repo, never the working checkout.
set -eu
script="$(cd "$(dirname "$0")" && pwd)/build-info.sh"
test_dir=$(mktemp -d /tmp/gndhog-build-info.XXXXXX)
trap 'rm -rf "$test_dir"' EXIT HUP INT TERM
mkdir "$test_dir/repo" "$test_dir/plain"
cd "$test_dir/repo"
git init -q
git config user.name 'build-info test'
git config user.email 'build-info@test.invalid'
touch source
git add source
git commit -qm fixture
out="$test_dir/build_info.h"
sh "$script" "$out"
first=$(git rev-parse --short=7 HEAD)
grep -q "\"$first\"" "$out"
touch -t 200001010000 "$out"
sh "$script" "$out"
[ "$(stat -c %Y "$out")" = 946684800 ]
printf changed > source
sh "$script" "$out"
grep -q "\"$first-dirty\"" "$out"
git add source
sh "$script" "$out"
grep -q "\"$first-dirty\"" "$out"
git commit -qm next
sh "$script" "$out"
second=$(git rev-parse --short=7 HEAD)
[ "$first" != "$second" ]
grep -q "\"$second\"" "$out"
touch new-source
sh "$script" "$out"
grep -q "\"$second-dirty\"" "$out"
cd "$test_dir/plain"
sh "$script" "$out"
grep -q '"unknown"' "$out"
echo 'build-info: 7 scenarios passed'
