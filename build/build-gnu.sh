#!/usr/bin/env zsh
# Retained command spelling; CMake is the sole build implementation.
set -eu
cd "${0:A:h:h}"
target="${1:-all}"
case "$target" in
    all|fetchaddr|parsedate|dotlock|gpgewrap|jaro-filter-compile) ;;
    *) print -u2 -- "Unknown build target: $target"; exit 2 ;;
esac
exec python3 build/cmake-build.py --target "$target"
