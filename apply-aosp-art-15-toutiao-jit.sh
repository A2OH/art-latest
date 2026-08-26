#!/bin/sh
set -eu

art_tree=${1:-/home/dspfac/aosp-art-15}
script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
patch_file="$script_dir/patches/aosp-art-15-toutiao-jit.patch"

git -C "$art_tree" apply --check "$patch_file"
git -C "$art_tree" apply "$patch_file"
