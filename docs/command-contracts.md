# Command Contracts

This file documents current behavior from `src/jaro` and `src/zlibs/*` without redefining semantics.

## Core runtime contract

- Required env: `JAROWORKDIR` (set by wrapper/install), `JAROMAILDIR` (default `$HOME/Mail` when unset).
- Common side effects: creates/updates `Accounts/`, `Filters.txt`, `notmuch-config`, `.mutt/*`, `cache/*`.
- Exit behavior: non-zero on missing deps, invalid maildir/account config, or external tool failures.
- The dispatcher preserves a handler's non-zero return status; handlers that
  set `exitcode` must capture it immediately after the operation they report.
  Pipelines that forward tool output must preserve producer failures.

## Setup and maintenance

- `init [MAILDIR]`: creates canonical maildir tree and default config files.
- `update` checks and installs a newer stable Linux release in the current
  installation prefix. `-n update` previews without network or filesystem changes.
- `fresh [MAIL_ROOT]` refreshes filters and the existing notmuch index.
- `fresh-filters [MAIL_ROOT]` publishes the shared local generation and portable
  Sieve script. `sieve` retains the filter-only workflow for migrated profiles.
- `fresh-search [MAIL_ROOT]` refreshes the existing index. `index` initializes it.

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
  `test-serve-dovecot.sh` starts real Dovecot and checks folder listing/nesting,
  exact message routing, persistent flag changes, deletion/expunge, delivery
  into an already selected Maildir, same-user credential-file login, runtime
  ownership and signal shutdown. It can use `SIEVE_DEPS_PREFIX` for
  the existing 2.4 installation and `JARO_SERVE_TEST_TMPDIR` for a short private
  test parent. No external mail server is contacted.
- IMAP clients can change flags, move or delete messages as usual. Selecting a
  folder can move `new/` files into `cur/`; the bridge does not promise a
  read-only filesystem. INBOX is a separate root Maildir; existing mail remains
  accessible in named folders such as `known` and `priv`.

### Same-user credential file

`serve` prints a `Credentials:` path pointing to the existing
`$MAILDIRS/.imap/passwd` file (`$JAROMAILDIR/.imap/passwd` when that override is
set). This is the filesystem credential cookie for local clients running as
the mail owner. The runtime directory is created with mode 0700 and the file
with mode 0600. The file contains one line in Dovecot passwd-file format:

```text
username:{PLAIN}password
```

The default username is `jaro`; `JARO_SERVE_USER` overrides it.
`JARO_SERVE_PASSWORD` overrides the password; otherwise a fresh random password
is generated when configuration is written. Config-only mode also writes these
credentials. A rejected competing launch leaves the running server's credentials
unchanged. The file remains after shutdown and is replaced by the next generated
configuration, so its presence does not mean that a server is running. Clients
should reread it for a new server session. These credentials are separate from
account passwords and the OS keyring.

For example, after starting `jaro serve` with an explicit `JAROMAILDIR`, a
same-user Python client can read the cookie without scraping terminal output:

```python
import imaplib
import os
from pathlib import Path

cookie = Path(os.environ["JAROMAILDIR"]) / ".imap/passwd"
username, stored = cookie.read_text().removesuffix("\n").split(":", 1)
if not stored.startswith("{PLAIN}"):
    raise ValueError("Unsupported serve credential format")
password = stored[len("{PLAIN}"):]
with imaplib.IMAP4("127.0.0.1", int(os.environ.get("JARO_SERVE_PORT", "61443"))) as client:
    client.login(username, password)
    client.select("known", readonly=True)
```

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

- `addr`: list addressbook entries (allow by default, or the exact list selected by `-l`).
- `import`: consumes stdin and appends unique entries to selected list.
- `isknown`: checks sender against active list; exit `0` if known, non-zero otherwise.
- `learn`: parses messages from stdin and inserts sender/all entries.
- IO: reads and writes the profile-wide `contacts.sqlite`. Legacy text books are first-use import inputs.
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

## Noninteractive profile operations

These commands use the selected JaroMail installation and profile. JSON is UTF-8.
A consumer must check the exit status before consuming a result. Diagnostics go
to stderr. Preserve revisions and IDs verbatim between reading and editing.

### Accounts and recipient groups

`jaro account list` and `jaro group list` return `{"version":1,"ids":[...]}`.
`get ID` returns `version`, `id`, `revision` and `fields`. Account IDs name
files in the configured Accounts directory, including the legacy .txt lookup.
The revision is SHA-256 over the complete native file. Password values are
excluded from returned fields. Group IDs name files in Groups.

`put ID --expect REV` accepts a JSON object of changed fields on stdin.
Use `absent` to create a record. Account scalar values are strings
(`name`, `email`, `login`, `proto`, `imap`, `smtp`, `imap_port`, `smtp_port`,
`transport`, `auth`, `cert`). `folders` and `exclude` are arrays of strings.
The installed parser applies its usual defaults and validates the new record
before atomic replacement. Comments, passwords and unrelated directives are
preserved. Groups accept `members` (mailbox strings) and `mode` (individual,
cc, carboncopy or bcc). `jaro group delete ID --expect REV` removes one group.
Requests and records are limited to 1 MiB. A stale revision returns 6, an
invalid request or unreadable record returns 4.

`jaro account credential ID --service imap|smtp --expect REV` reads up to
4096 opaque bytes on stdin, excluding NUL, CR and LF, and uses the configured
GNU host keyring. Success returns `stored: true` and the account revision.
Exit 7 indicates an unavailable or failed keyring. Native interactive password
setup remains available. Keep secret input out of command arguments and logs.

### Contact store

`jaro contacts books` and `list BOOK` return NUL-terminated fields. Both start
with `JCT1` and the profile generation. Books are exact names. Each contact row
contains ID, revision, name, nickname, notes, insertion date, address count,
the addresses, extra-field count, and that many key/value pairs. Numeric fields
are decimal. This is the same SQLite store and migration path used by abook.

`create-book BOOK GEN`, `rename-book OLD NEW GEN` and `delete-book BOOK GEN`
use the observed profile generation. Rename and delete apply to custom books.
`put BOOK ID REV` reads NUL-terminated name, nickname, notes and zero or more
addresses. For insertion use ID 0 and the profile generation. For an existing
contact use its ID and row revision. Extra fields and the insertion date
survive an edit. `delete BOOK ID REV` removes one row. Mutations return
`JCT1`, new generation and ID (0 for a book operation). Edits in allow and
block refresh the published policy. If that refresh fails after commit, the
diagnostic reports the committed edit. Re-read the record before retrying.
Missing records return 1, invalid records 4, busy or stale revisions 6.

### Rule preview and publication

`jaro policy preview SOURCE SAMPLE` reads replacement bytes for Filters.txt
or Aliases.txt on stdin. SAMPLE is an existing root-relative new/cur Maildir
occurrence. The configured Pigeonhole runtime returns a JSON `decision` with
rule, action, folder, status, source and line, along with `version`, `token`
and `generation`. No rule text is published during preview.
`jaro policy publish SOURCE SAMPLE --expect TOKEN` takes the same input and
returns the published generation. The token binds the native compiler inputs,
candidate and selected occurrence. A stale comparison returns 6. Publication
uses the existing policy journal and rollback. Inputs are limited to 64 MiB.

### SMTP submission

`jaro -a ACCOUNT outbox submit` consumes a JSON object with integer `version`
1, application `key` and ordered `envelopes`. Each envelope contains `mime`
(base64 over exact MIME bytes) and `recipients` (the expanded SMTP mailbox
list). Keys are 1–160 ASCII letters, digits or characters in `._:-`. The body
limit is 64 MiB. Each submission permits 1–128 envelopes and each envelope
1–256 recipients. Repeating the same input returns the same ordered IDs.
Changed input under an existing key fails. Responses contain `version` and
`envelopes`, with `id` and `state` for each entry. Lost responses and interrupted
publication can be retried with the original key and input. Preserve the private
submissions directory, native recovery claims and journal for the life of the key.

`jaro outbox send ID --expect eligible|prepared` selects one item.
`outbox retry ID --expect STATE --confirm-duplicate` and
`outbox archive ID --expect STATE` check the state under the outbox lock.
An uncertain retry may duplicate mail. Archive records an external delivery
verification without SMTP. Explicit bulk `jaro send` still drains eligible
messages. Targeted and bulk workers both use the configured account bound at
submission and the stored SMTP recipients. Ordinary `jaro queue` retains its
stdin and header-recipient behavior.

### Maildirs, cleanup, replay and statistics

`jaro maildir folders` streams one JSON relative folder name per line.
`messages FOLDER` streams objects with folder, part (cur or new), name and
revision. Stop the process to cancel discovery. Filesystem bytes are preserved
through Python JSON surrogate escaping. Every folder component must be real.
Backend-private trees and snapshot-recovered outboxes are excluded from browsing
and filing.

`apply DEST [--flags FLAGS]` takes an array of selected occurrence objects on
stdin. Within the same folder, `--flags` replaces the standard DFPRST flags.
An empty string clears them. Existing keyword letters and any comma-delimited
extension are preserved. Changing flags and refiling preserve the exact message
bytes and the nanosecond modification time JaroMail uses for the received date.
A transfer publishes a no-clobber destination before removing the selected source.
`merge SRC DEST` previews a folder. Use `--apply` to execute and `--remove-empty`
to request removal of an empty source after successful transfers.
`backup-preview DEST QUERY` selects indexed occurrences, using the native
top-level raw-text fallback when the query has no matches. Apply the returned
`items` to DEST, then run `jaro fresh-search` to refresh the index.

`legacy-preview FOLDER...` or `legacy-preview lastlog` previews ordered
Message-ID duplicates with formail. For removal pass `{"folders":[...],"items":[...]}`
to `legacy-remove --confirm-message-id`; the current duplicate set is checked
again. Matching Message-IDs can have different message bytes.
Selections allow up to 10,000 occurrences and 8 MiB of request data. Mutations
return individual `results` with index and status. Exit 7 means a partial or
failed operation. A changed source, missing provider or invalid selection
requires a new preview or diagnostic. A merge reports `removed_source`.

`jaro replay-record search|notmuch|extract|parse|imap|last` returns
`version`, actual `producer` and base64 `data` (up to 1 MiB).
`jaro stats0 folder|name|email|domain` reads NUL-terminated selected rows,
up to 100,000 rows and 8 MiB. Output starts with NUL-terminated `JST1`
followed by category/count pairs, each field NUL-terminated, sorted by category.
