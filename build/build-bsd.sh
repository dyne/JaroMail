#!/usr/bin/env zsh
# CMake selects the platform/compiler; keep the historical entry point.
exec "${0:A:h}/build-gnu.sh" "$@"
