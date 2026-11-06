#!/usr/bin/env zsh
# Use the same non-destructive staged installation as make install.
set -eu
exec make -C "${0:A:h:h}" install "PREFIX=${PREFIX:-/usr/local}" "DESTDIR=${DESTDIR:-}"
