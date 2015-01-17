GnuPG mass-recv script, version 0.1
by Jaromil

This is a simple Zsh script that uses gnupg to download all missing ID
found in all signed keys from one's keyring.

Beware that by running this script your keyring will be populated by a
lot more keys. The script shows its operation and needs the option -f
to force its execution and really modify the GnuPG keyring in
$HOME/.gnupg.


