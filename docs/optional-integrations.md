# Optional Integrations

This file lists integrations that are *not* part of the core CLI workflow (`init`, `update`, `filter`, `search`, `send/fetch`) and clarifies install/test expectations.

## Default Install Scope

- `make install` copies `docs/*`, `src/mutt/*`, `src/stats/*`, `src/zlibs/*`, `src/zuper/zuper*`, `src/jaro`, and `build/gnu/*`.
- Most content under `extras/` and `src/MailToMutt` is **not** installed by default.

## Retained Optional Integrations

### Interactive IMAP shell

`jaro -a ACCOUNT imap shell` opens one persistent connection using the normal
account configuration and credential lookup. It requires Python 3.9 or newer with its
standard library; `JARO_IMAP_PYTHON` selects the interpreter (default `python3`).
The installed helper is private data, invoked by that interpreter. Other IMAP
commands do not require Python. `help`, `quit`, Ctrl-D and Ctrl-C control the
session. Commands accept shell-style quoting without evaluating shell code.
`JARO_IMAP_TIMEOUT` bounds each network operation to 1–300 seconds (default 30).
The account's transport, certificate policy and CA file apply to this connection;
explicit plaintext or disabled verification produces a warning.

`folders`, `ls` and `ls --folders` list the current folder's immediate children.
`pwd` prints its logical path; `cd PATH` navigates and `cd` returns to `/`.
`refresh` replaces the current child listing without walking descendants.
Nonselectable folders are marked `[container]` and remain navigable. Header
browsing through `ls --messages` is currently unavailable.

Display paths use `/` between logical components independently of server
hierarchy delimiters. Percent escapes represent UTF-8 bytes for literal slash,
percent, wildcard, quote, backslash, whitespace and control characters; literal
`.` and `..` components are `%2E` and `%2E%2E`. Thus a flat mailbox named
`Work/projects` appears as `/Work%2Fprojects`. Unescaped `.` and `..` navigate
the logical tree. Unicode appears directly; the session retains the server's
original mailbox bytes, delimiter and flags and negotiates UTF-8 when supported.

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
