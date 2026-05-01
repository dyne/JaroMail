# Command Contracts

This file documents current behavior from `src/jaro` and `src/zlibs/*` without redefining semantics.

## Core runtime contract

- Required env: `JAROWORKDIR` (set by wrapper/install), `JAROMAILDIR` (default `$HOME/Mail` when unset).
- Common side effects: creates/updates `Accounts/`, `Filters.txt`, `notmuch-config`, `.mutt/*`, `cache/*`.
- Exit behavior: non-zero on missing deps, invalid maildir/account config, or external tool failures.

## Setup family (`init`, `update`)

- `init [MAILDIR]`: creates canonical maildir tree and default config files.
- `update`: refreshes filters, mutt configs, and notmuch indexes.
- IO: reads `Accounts/*`, `Aliases.txt`, `Filters.txt`; writes `cache/filters*`, `.mutt/*`, `notmuch-config`.
- Test-safe: yes (`extras/test/run-source.sh`, `extras/test/test-filtering.sh`).

## Transport family (`fetch`, `send`, `smtp`, `imap`)

- `fetch`: pulls mail into `incoming/` via fetchmail.
- `send`/`smtp`: sends queued messages from `outbox/` via msmtp.
- `imap`: account-bound IMAP actions.
- IO: reads account credentials and queue; writes to maildirs and replay logs.
- Test-safe: partially (`run-source.sh` covers queue/index/filter path; network calls are environment dependent).

## Index/search family (`index`, `search`, `extract`, `headers`)

- `index`: runs notmuch indexing + canonical tag normalization.
- `search <expr>`: emits matching file paths (replay-aware).
- `extract|parse`: extracts addresses from stdin/maildirs/files.
- `headers`: prints compact date/folder/from/subject lines from file lists.
- IO: reads maildirs and notmuch DB; writes `cache/replay/*`.
- Test-safe: yes (`run-source.sh`, `test-addressbook-parse.sh`).

## Addressbook family (`addr`, `import`, `isknown`, `learn`)

- `addr`: list addressbook entries (`whitelist` by default or `-l` selected list).
- `import`: consumes stdin and appends unique entries to selected list.
- `isknown`: checks sender against active list; exit `0` if known, non-zero otherwise.
- `learn`: parses messages from stdin and inserts sender/all entries.
- IO: reads/writes `*.abook`.
- Test-safe: yes (`test-addressbook-parse.sh`).

## Filtering/maildir family (`filter`, `deliver`, `ismd`)

- `filter [MAILDIR]`: applies routing rules from cache/filters + addressbooks.
- `deliver [MAILDIR]`: injects stdin message into target Maildir.
- `ismd <path>`: validates Maildir structure and reports success/failure.
- IO: reads `Filters.txt`, addressbooks, mail headers; rewrites message locations.
- Test-safe: yes (`test-filtering.sh`, `run-source.sh`).

## Fallback behavior (no command / unknown command)

- No subcommand: opens mutt (`-Z`) or requested folder.
- Unknown token:
  - email-like token -> compose to recipient,
  - maildir path/name -> open that folder in mutt,
  - existing file path -> attach/open via mutt,
  - else -> default mutt launch.

## Missing test pointers

- `fetch`, `send`, `smtp`, `imap`, and optional integrations are not fully covered by focused local tests yet.
