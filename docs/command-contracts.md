# Command Contracts

This file documents current behavior from `src/jaro` and `src/zlibs/*` without redefining semantics.

## Core runtime contract

- Required env: `JAROWORKDIR` (set by wrapper/install), `JAROMAILDIR` (default `$HOME/Mail` when unset).
- Common side effects: creates/updates `Accounts/`, `Filters.txt`, `notmuch-config`, `.mutt/*`, `cache/*`.
- Exit behavior: non-zero on missing deps, invalid maildir/account config, or external tool failures.
- The dispatcher preserves a handler's non-zero return status; handlers that
  set `exitcode` must capture it immediately after the operation they report.
  Pipelines that forward tool output must preserve producer failures.

## Setup family (`init`, `update`)

- `init [MAILDIR]`: creates canonical maildir tree and default config files.
- `update`: refreshes filters, mutt configs, and notmuch indexes.
- IO: reads `Accounts/*`, `Aliases.txt`, `Filters.txt`; writes `cache/filters*`, `.mutt/*`, `notmuch-config`.
- Test-safe: yes (`extras/test/run-source.sh`, `extras/test/test-filtering.sh`).

## Transport family (`fetch`, `send`, `smtp`, `imap`)

- `fetch`: pulls mail into `incoming/` via fetchmail.
- A successful poll with no messages returns zero; transport failures return
  non-zero and stop the subsequent filter/index workflow.
- `send`: sends eligible queued messages from `outbox/` via msmtp;
  `smtp` sends stdin directly and does not use the outbox recovery journal.
- `send` returns non-zero if any message fails, even if a later message succeeds.
- Confirmed accepted items finish local archive/removal without another SMTP
  invocation. Interrupted/uncertain and invalid state is held; held counts are
  reported. Every nonzero msmtp result remains uncertain rather than proving
  non-delivery. Legacy anonymous sending keeps its original behavior.
- `imap`: account-bound IMAP actions.
- IO: reads account credentials and queue; writes to maildirs and replay logs.
- Test-safe: partially (`run-source.sh` covers queue/index/filter path; network calls are environment dependent).

## Local IMAP bridge (`serve`)

- `serve`: generates local Dovecot config in `$MAILDIRS/.imap` and serves existing Maildirs over localhost IMAP.
- IO: reads local Maildir tree; writes `$MAILDIRS/.imap/dovecot.conf`, `$MAILDIRS/.imap/passwd`, and runtime logs/state.
- Network scope: loopback-only (`127.0.0.1`) with generated local credentials; no POP3/LMTP/submission surfaces.
- Requires Dovecot **2.4.x** and its matching `doveconf`/IMAP service for live
  use. Unsupported versions fail before writing runtime files. A rejected
  configuration is an error, never a successful skip.
- Run as the mail owner, not root. Internal/login/auth-worker processes use
  that user's identity and do not require chroot capabilities or system config.
- `jaro -n serve` previews without bootstrap initialization, credential
  generation, configuration writes, version probes or daemon startup.
- A per-mail-root advisory lock is held through daemon shutdown. A second
  launch, including config-only or a different port, cannot replace a live
  instance's credentials. Busy ports are checked before configuration writes.
  Do not delete `.imap/lock`; the OS releases ownership when the process exits.
- `JARO_SERVE_HOST` accepts `localhost` or `127.0.0.1`; `JARO_SERVE_PORT`
  defaults to 61443 (1024–65535); `JARO_SERVE_MAX_CONNECTIONS` defaults to 4
  (1–16). `JARO_SERVE_USER` defaults to `jaro`; `JARO_SERVE_PASSWORD` can
  override the generated password. These credentials are separate from accounts.
- Paths are quoted, including spaces and `#`. Colons, percent signs and
  newlines in the mail root are unsupported. Dovecot's UNIX socket path length
  limit also applies; use a short mail-root path if its listeners cannot start.
- `JARO_SERVE_CONFIG_ONLY=1` renders without launching; Dovecot is optional for
  this mode, but any available tools must accept the configuration. The live
  test skips with exit 77 only when its required version/tools are unavailable.
- Test coverage: `test-serve-config.sh` covers CLI, validation and dry-run;
  `test-serve-dovecot.sh` starts Dovecot, logs in, reads a fixture, rejects a
  second launch and checks signal shutdown. It can use `SIEVE_DEPS_PREFIX` for
  the existing 2.4 installation and `JARO_SERVE_TEST_TMPDIR` for a short private
  test parent. No external mail server is contacted.
- IMAP clients can change flags, move or delete messages as usual. Selecting a
  folder can move `new/` files into `cur/`; the bridge does not promise a
  read-only filesystem. INBOX is a separate root Maildir; existing mail remains
  accessible in named folders such as `known` and `priv`.

## Outbox recovery (`outbox list`, `outbox retry`, `outbox archive`)

- `outbox list`: read-only `ID STATE OUTCOME` rows. No message text, recipients,
  filenames, bootstrap initialization, account reads, keyring or transport calls.
- `outbox retry ID --confirm-duplicate`: retries one in-flight/uncertain held
  item under the outbox lock; the flag acknowledges possible duplication.
- `outbox archive ID`: records external verification before finishing the local
  sent copy; zero SMTP and credential calls. Accepted/verified/complete items can
  also complete local cleanup.
- `-n`: validates without lock/state writes or transport. Invalid, stale,
  ambiguous and content-mismatched selections do not change delivery state.
- IDs bind the stable queue basename to a content-checked immutable journal.
  Private records and independent claims must be preserved for recovery.
- Tests: `test-outbox-controls.sh`, `test-send-recovery.sh`,
  `test-send-locking.sh`, including source and staged installed invocations.

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

- `fetch`, `send`, `smtp`, `imap`, `serve` live launch path, and optional integrations are not fully covered by focused local tests yet.
