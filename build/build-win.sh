#!/usr/bin/env zsh
# Cygwin uses the same CMake targets and external runtime dependencies.
exec "${0:A:h}/build-gnu.sh" "$@"

