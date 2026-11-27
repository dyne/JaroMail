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

`folders` and `ls --folders` list the current folder's immediate children.
`ls` also shows a read-only header page in a selectable folder; `ls --messages`
shows only headers. Pages default to the 50 newest messages by UID.
`ls --limit N` chooses 1–500 messages per page; combine it with `--messages`
to omit children. `more` continues with older messages using the same limit.
Paging excludes arrivals after its initial upper UID. If the mailbox generation
changes or repeated mailbox changes invalidate the cursor, run `ls` to restart.
Navigation, `refresh` and connection recovery also reset the cursor.
`pwd` prints its logical path; `cd PATH` navigates and `cd` returns to `/`.
`refresh` replaces the current child listing without walking descendants.
Nonselectable folders are marked `[container]` and remain navigable.

`du [PATH]` reports a labeled server mailbox size when supported. `du --exact`
measures only RFC822.SIZE metadata over a captured UID inventory; missing UIDs
produce partial coverage. These logical mailbox bytes differ from compressed
archive size and account quota. Server totals may exceed the measured sum.
`du --recursive` walks descendants, including those beneath containers.
`--depth N` includes at most N child levels (0 includes only the starting folder),
and `--top N` ranks selectable folders by direct bytes; either option implies
recursive mode. For example, `du / --depth 2 --top 10` shows the largest direct
mailboxes within two levels. The table separates direct and subtree bytes;
partial subtree totals are lower bounds. Shared/labeled copies may count twice.
Folder completeness and measured-message coverage have separate denominators.
Progress goes to stderr. Ctrl-C retains incomplete results and renews the
connection before accepting another command. Ordinary `du` preserves header
paging and the current path; recovery resets the pager.

Display paths use `/` between logical components independently of server
hierarchy delimiters. Percent escapes represent UTF-8 bytes for literal slash,
percent, wildcard, quote, backslash, whitespace and control characters; literal
`.` and `..` components are `%2E` and `%2E%2E`. Thus a flat mailbox named
`Work/projects` appears as `/Work%2Fprojects`. Unescaped `.` and `..` navigate
the logical tree. Unicode appears directly; the session retains the server's
original mailbox bytes, delimiter and flags and negotiates UTF-8 when supported.

`mkdir PATH` creates an exact mailbox. `mv OLD NEW` (also `rename`) renames a
whole subtree with one server RENAME; `NEW` is the complete destination path,
and its parent must exist. Root, INBOX moves, descendant cycles and existing
destinations are rejected. Successful writes refresh folder views and remap
the current path; a removed current folder falls back to a surviving ancestor.
Subscription changes are observed and counted; the shell never subscribes or
unsubscribes folders automatically.

`rename-glob SOURCE TEMPLATE` previews wildcard renames without changing the
server. SOURCE has a literal parent path and exactly one unescaped `*` in its
final component; it captures zero or more Unicode characters, case-sensitively.
For example, `rename-glob 'List-*' 'Lists/{1}'` maps `/List-Dev` to `/Lists/Dev`
when `/Lists` already exists. `{1}` substitutes the capture as literal text
within each template component, so captured slashes, percent signs, dots and
template characters cannot introduce another path or substitution. Ordinary
display escapes still apply. Use `\*` or `%2A` for a literal star and `\\` or
`%5C` for a literal backslash. Quote both operands to keep spaces together;
for this command backslash escapes survive quote removal, including inside
single quotes. In double quotes or outside quotes, a backslash also keeps the
next character in the same argument. There is no shell expansion.

Every matched old/new path is printed in sorted order, with its entire subtree
scope and match count. Output streams all mappings in piped and terminal use;
redirect it to a file for a large review. Existing destinations (including
excluded folders and descendant collisions), protected paths, duplicate or
overlapping subtrees, cycles and rename dependency chains block the preview.
Moves between different server hierarchy delimiters are also blocked because
their descendant rename behavior has not been established.
A preview uses fresh LIST observations, which may be limited by server access.
No preview changes folders or messages.

Each preview prints a session-local ID. `apply ID` displays its count and asks
for `yes` on the controlling terminal; command stdin cannot confirm, including
when commands arrive through a pipe. The saved mapping contains absolute raw
mailbox identities, so `cd` cannot retarget it. A new preview replaces the ID;
every apply attempt consumes it, including refusal or cancellation.

After confirmation, apply refreshes the matching set, source subtrees and
destinations, then rechecks each original pair immediately before its serial
RENAME. It stops on drift, a refusal or an unknown result and reports every pair
as completed, failed, unknown or pending. No write is replayed. Inspect the report
and create a fresh preview before continuing a partial run.

A tagged success is reported as confirmed and a tagged refusal as rejected.
If a connection is lost or a write is interrupted before completion, its outcome
is unknown. The shell closes that connection, reconnects, and observes the exact
old/new names where the mailbox encoding still matches. These observations do
not establish which client made a change. A name that is not listed may be
hidden by server permissions; it is not proof of absence. The write is never replayed
automatically; inspect the refreshed state before deciding whether to retry.

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
