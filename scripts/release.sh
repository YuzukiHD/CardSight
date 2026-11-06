#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# Build a source release tarball: no build output, no vendor binaries, licenses included.
set -eu

root=$(cd "$(dirname "$0")/.." && pwd)
version=${1:-$(date +%Y%m%d)}
name="cardsight-$version"
out="$root/dist"

for file in LICENSE THIRD_PARTY.md README.md apps/cardsight/FONT_LICENSE.txt; do
	[ -f "$root/$file" ] || {
		echo "missing $file" >&2
		exit 1
	}
done

mkdir -p "$out"
tmp=$(mktemp -d)
mkdir -p "$tmp/$name"
tar -C "$root" \
	--exclude=./build --exclude=./dist --exclude=./.git \
	--exclude='*.a' --exclude='*.o' --exclude='*.d' \
	-cf - . | tar -C "$tmp/$name" -xf -
# the submodule's git link is not useful inside a tarball; name where it came from
mkdir -p "$tmp/$name/third_party/SyterKit"
printf 'SyterKit submodule; see .gitmodules for the repository and the pinned commit.\n' \
	> "$tmp/$name/third_party/SyterKit/SOURCE.txt"
tar -C "$tmp" -czf "$out/$name.tar.gz" "$name"
rm -rf "$tmp"
tar -tzf "$out/$name.tar.gz" > "$out/$name.tar.gz.contents"
echo "$out/$name.tar.gz"
