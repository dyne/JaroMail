# Optional Integrations

This file lists integrations that are *not* part of the core CLI workflow (`init`, `update`, `filter`, `search`, `send/fetch`) and clarifies install/test expectations.

## Default Install Scope

- `make install` copies `doc/*`, `src/mutt/*`, `src/stats/*`, `src/zlibs/*`, `src/zuper/zuper*`, `src/jaro`, and `build/gnu/*`.
- Most content under `extras/` and `src/MailToMutt` is **not** installed by default.

## Retained Optional Integrations

### `src/stats` and `stats` command

- Installed by default: **yes**
- Test coverage: **not directly covered by current smoke tests**
- Status: retained for compatibility, but legacy-oriented surface

### `extras/roundcube-majordomo`

- Installed by default: **no**
- Test coverage: **none in core test suite**
- Status: optional integration for Roundcube users only

### `extras/osx-abquery` and `src/MailToMutt`

- Installed by default: **no**
- Test coverage: **none in core test suite**
- Status: macOS-specific optional tools, outside default maintenance path

### `extras/emlx2maildir`, `extras/gnupg-mass-recv`, `extras/shell_completion`, `extras/zaw-jaromail`

- Installed by default: **no**
- Test coverage: **none in core test suite**
- Status: optional helper/tooling scripts

## Core Validation Path

Use this path for changes that target the default CLI experience:

```sh
make
extras/test/run-source.sh
PREFIX="$PWD/.tmp-install" make install
find "$PWD/.tmp-install" -maxdepth 4 -type f | sort
rm -rf "$PWD/.tmp-install"
```

If a change only affects optional integrations, document the exact manual verification steps next to the integration.
