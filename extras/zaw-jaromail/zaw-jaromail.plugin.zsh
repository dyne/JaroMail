#!/usr/bin/env zsh

if [[ -n $(declare -f -F zaw-register-src) ]]; then
    here=${${0:A}:h}
    for fn in $(ls $here/sources/); do
        source $here/sources/$fn
    done
else
    echo "zaw-jaromail is not loaded since zaw is not loaded."
    echo "Please load zaw first."
fi
