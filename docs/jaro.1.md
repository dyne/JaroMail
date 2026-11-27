jaro(1)
=======

## Name

jaro - command line interface for Jaromail workflows

## Synopsis

`jaro [options] [command] [command-options]`

## Description

`jaro` is the operational CLI for Jaromail. It orchestrates account
configuration, local maildir operations, addressbooks, filtering,
indexing and SMTP/IMAP actions. Contacts live in `contacts.sqlite` in the
JaroMail directory and are shared across its accounts. Recipient completion
searches allow and custom lists.

## Options

`-a ACCOUNT` use a specific account instead of `default`.

`-l LIST` select a contact list by exact name (default: `allow`).

`-n` dry-run mode where supported.

`-q` quiet mode.

`-D` debug mode.

`-h` show help.

`-v` or `--version` prints the assigned release version and exits. The startup
banner and help use the same installed metadata. Source builds derive it from
release tags, with the v6.0.0 development baseline when no v6 tag exists.

`-f` force mode for selected operations.

## Filter engine selection

Pigeonhole executes local filters. The default and the `sieve`, `native` and
`legacy` values of `JARO_FILTER_ENGINE` select the same installed runtime.
Run `jaro fresh-filters` to migrate an older generation from your existing
`Filters.txt`, aliases, accounts and addressbooks. Until then, filter and fetch
report the required update before touching messages or starting transport.
An unsuccessful migration preserves the last coherent policy and all mail.
Historical messages stay in `known` and `zz.spam`. New known and spam deliveries
use `INBOX` and `Spam` by default. Custom Sieve mailbox mappings apply locally
and to the exported script. Existing files in the destination Maildir are preserved.

`jaro fresh-filters` publishes the shared local generation and portable Sieve
export. `jaro sieve` can also regenerate the policy after migration.
`jaro filter [MAILDIR]` applies it to incoming mail. `jaro fetch` applies it
after transport and refreshes the index once after the batch, including partial
failures. Filter alone does not index. `jaro filter --explain [MAILDIR|MESSAGE]`,
`jaro -n filter` and `jaro -n fetch` inspect the selected generation without
publication, delivery or indexing. A missing or stale generation requires
`jaro fresh-filters` before preview. A fresh `jaro init` installs commented typed
examples and active builtins. Supported legacy EREs work with the default profile.

Native `filter --compare FILE [MAILDIR]` compares retained actual final legacy
observations against native classification. It never applies either engine.
FILE uses complete fingerprint-bound JLC1 NUL framing described in
`filter-interfaces.md`. Its producer must retain unchanged-corpus hashes and
actual legacy provenance. Ambiguous dry-run proposals and unsupported files
fail; this command does not infer a legacy winner from proposal text.

## Commands

### Core mail flow

`doctor [--json]` report local setup problems without initialization, network requests,
credential retrieval or repair. `jaro -a ACCOUNT doctor` checks a selected account.
It continues through independently discoverable problems even if mail tools or
version metadata are missing. Exit status is 0 when there are no blocking
findings, 1 for actionable blockers, and 2 for invalid invocation or an internal
diagnostic failure. Human output suggests commands without running them.
`--json` emits schema version 1 with stable check IDs, status, redacted
`explanation` and `next_action` strings, resolved paths and `exit_status`.
Warnings and unknown checks do not make the exit status nonzero. See the
manual's offline diagnostics section for the complete schema and setup flow.

`wizard` interactive account setup.

`fetch` fetch unread messages for an account.

`send` send queued messages from `outbox/`.

`outbox list` print stable queue IDs, delivery states and classified outcomes
without message content or recipients. This route does not initialize files,
consult credentials or require the mail transport tools.

`outbox retry ID --confirm-duplicate` explicitly retry only a held uncertain
item. The required flag acknowledges possible duplicate delivery. `-a ACCOUNT`
selects its account. A new uncertain result is held again.

`outbox archive ID` record externally verified delivery and complete the local
sent copy without SMTP or account/credential access. Accepted, verified and
complete items can also finish local recovery this way.

Both actions accept `-n` for validation without writes or transport. Unknown,
stale, ambiguous and content-mismatched IDs are refused. See
[delivery recovery](jaromail-manual.md#inspect-and-reconcile-held-outbox-messages)
for states, examples and damaged-state handling. Bare `outbox` still opens
the outbox Maildir in Mutt.

`queue` read a full message from stdin and queue it.

`smtp` read a full message from stdin and send via SMTP.

`peek` open remote IMAP mailbox view.

`open` open a local maildir folder.

`serve` expose local Maildirs through Dovecot 2.4.x on `127.0.0.1:61443`.
Run as the mail owner. Same-user clients can read the mode-0600
`MAIL_ROOT/.imap/passwd` credential file, containing `username:{PLAIN}password`.
The `Credentials:` startup line gives its path. `JARO_SERVE_USER` defaults to
`jaro`; `JARO_SERVE_PASSWORD` overrides the otherwise randomly generated
password. Read the current file for each new server session. See the manual's
local IMAP section for credential lifetime and client behavior.

### IMAP folder shell

The IMAP terminal opens a persistent connection to a remote account. Use it
to browse folders, inspect message headers, measure mailbox sizes, export
messages, and reorganize folders. Browsing, sizing, and exporting are read-only
on the server. The commands below run at the `imap>` prompt unless marked
as system-shell commands.

#### Start a session

Run `jaro -a ACCOUNT imap shell` from your system shell. Replace `ACCOUNT`
with the name of a file in `$JAROMAILDIR/Accounts/`. Omit `-a ACCOUNT` to use
`default`. The shell uses Jaro Mail's account login and credential lookup.
For implicit TLS, set `proto imap`, `imap HOST`, the provider's IMAP port
(usually `imap_port 993`), `transport implicit`, and `cert check`.
For STARTTLS use the provider's STARTTLS port (usually 143) and
`transport starttls`. Certificate checks use system trust or the CA bundle
in `certfile /path/to/ca.pem`. A failed certificate check or STARTTLS
negotiation ends the connection attempt. Legacy `cert ignore` disables
identity checks, and `transport plain` sends credentials unencrypted.

Python 3.9 or newer is required. `JARO_IMAP_PYTHON` selects the interpreter
(default `python3`), and downloads also require its `sqlite3` module.
`JARO_IMAP_TIMEOUT` sets the deadline for each network operation in seconds
(1–300, default 30). Start from the directory where you want relative archive
paths to be written.

#### Folder paths

The session starts at the logical root `/`. A leading slash makes a path
absolute. Other paths start at the folder shown by `pwd`. Use `.` for the
current folder and `..` for its parent. Separate components with `/` even
when the server uses dots. For example, `Projects.Café team` on a dot-delimited
server appears as `/Projects/Café%20team`. A flat folder has no children.
`[container]` marks a folder you can navigate through whose messages cannot
be selected.

Copy percent escapes as displayed. Examples include `%20` for a space,
`%2F` for a literal slash within a name, `%25` for a percent sign, and
`%2A` for a literal asterisk. A folder named `.` or `..` is written
`%2E` or `%2E%2E`. You can also put spaces inside quotes, as in
`cd 'Café team'`. The prompt reads one command per line and treats
shell expansions such as `$HOME` and `~` as literal text.

#### Command reference

`help`

:   List commands and their options.

`quit` or `exit`

:   Close the session. Ctrl-D at an empty prompt also exits.

`pwd` and `cd [PATH]`

:   Print or change the current folder. With no argument, `cd` returns to `/`.

`folders` and `refresh`

:   List immediate children of the current folder. `refresh` rereads them
    from the server and resets header paging.

`ls [--folders | --messages] [--limit N]` and `more`

:   Show immediate children and, in a selectable folder, a page of message
    headers. `--folders` shows children only, and `--messages` shows headers
    only. Use `--limit N` for 1–500 headers per page (default 50).
    `more` continues the same header listing toward lower UIDs.

`du [PATH] [--exact] [--recursive] [--depth N] [--top N]`

:   Measure logical message bytes in `PATH`, or in the current folder when
    omitted. `--exact` counts size metadata for a captured UID inventory.
    `--recursive` includes descendants. `--depth N` limits the scan to
    `N` child levels, with `0` for the starting folder only. `--top N`
    shows the `N` largest selectable mailboxes by their own message bytes,
    where `N` is positive. Both options enable recursive mode.

`mkdir PATH`

:   Create one folder under an existing parent.

`mv OLD NEW` or `rename OLD NEW`

:   Rename or move an exact folder and its descendants. Supply the complete
    new path, including the new leaf name. The destination must be unused
    and its parent must exist.

`rename-glob SOURCE TEMPLATE` and `apply ID`

:   Preview wildcard matches and then apply the saved mapping in the same
    session. Quote both arguments. The source's parent path is literal.
    Its final component contains one unescaped `*`, which captures zero or
    more characters case-sensitively. Use `{1}` in the template for the
    capture and `%2A` for a literal star. Destinations must have existing
    parents. The preview reports every mapping and any reason it is blocked.
    Applying requires confirmation and a fresh check of the saved mapping.

`rmdir EXACT-PATH [--recursive] [--yes]`

:   Delete a folder and its messages. Root and INBOX are protected.
    `--recursive` includes descendants and deletes them first.
    The default prompt shows the targets and available message counts.
    Use `--yes` only for a deliberate deletion of a target you have checked.
    It bypasses the confirmation prompt.

`download FOLDER --output PATH.tar.gz [--recursive] [--resume]`

:   Export a mailbox to a verified local archive. Use `.` for the current
    mailbox. `--recursive` includes descendants and allows starting from
    a nonselectable container. Choose a folder below the virtual root.
    `--resume` reuses the staging directory for the same export.

#### Browse and measure

The examples use a disposable account with a dot hierarchy, three messages in
`Projects.Café team`, and a folder named `Projects.List-Dev`.

```text
folders
cd /Projects
folders
cd 'Café team'
pwd
ls --messages --limit 2
more
du /Projects --depth 1 --top 3
```

Headers show UID, message size, date, sender, and subject. Highest UID appears
first. The displayed Date header can be older or newer than its neighbors.
A missing or invalid date falls back to the server's INTERNALDATE. Each header
listing keeps its starting upper UID bound. Run `ls` again for new arrivals,
after a UIDVALIDITY change, or after navigation, `refresh`, or connection
recovery resets paging.

`du` uses server-reported mailbox bytes where available. Both its fallback
and `--exact` read only size metadata. The direct column covers a single
mailbox, and the subtree column adds its descendants within the requested
depth. Check the coverage labels before comparing totals. A partial total
is a lower bound, and shared copies can count twice. Account quota, filesystem
use, and compressed archive size have different accounting. Ctrl-C can stop
a recursive scan and leave a partial report. Run `du` again for a fresh scan.

#### Download, resume, and verify an archive

From the IMAP prompt, write a new archive on a filesystem supporting hard
links. Choose an unused filename under an existing directory. Output paths
refer to the local filesystem, with relative paths anchored to the directory
where you started Jaro Mail. Use ordinary path components. Symlinks and
`..` are refused. Allow space for raw message bytes plus the compressed
tar, inventory, and tar headers. For incompressible mail, budget roughly
twice the message bytes plus overhead and extra headroom for many small
messages. Add `--recursive` to include descendants.

```text
download '/Projects/Café team' --output cafe-team.tar.gz
```

After an interruption keep `.cafe-team.tar.gz.incomplete` next to the intended
output. Reconnect if the session ended. Use the same account, mailbox,
recursive setting, and output path.

```text
download '/Projects/Café team' --output cafe-team.tar.gz --resume
```

Resume verifies completed messages locally and restarts any partial message
from byte zero. If packaging failed after all messages were staged, resume
can rebuild the archive from those files. Free space before retrying a
disk-full failure. Missing captured UIDs or a changed UIDVALIDITY require
a new filename and a fresh inventory. The export records each folder's
captured UID range separately. Concurrent delivery or folder changes can
affect what a later export sees.

Publication verifies the archive before exposing the final filename and
preserves any existing output. In your system shell, extract the archive
into a fresh private directory and check the message bytes against the manifest.

```sh
umask 077
gzip -t cafe-team.tar.gz &&
tar -tzf cafe-team.tar.gz &&
mkdir verified-copy &&
tar -xzf cafe-team.tar.gz -C verified-copy &&
(cd verified-copy && python3 - <<'PY'
import hashlib, json
with open("manifest.jsonl", encoding="utf-8") as manifest:
    for line in manifest:
        record = json.loads(line)
        if record["type"] == "message":
            digest, size = hashlib.sha256(), 0
            with open(record["path"], "rb") as message:
                for block in iter(lambda: message.read(131072), b""):
                    size += len(block)
                    digest.update(block)
            if (size, digest.hexdigest()) != (record["size"], record["sha256"]):
                raise SystemExit("Verification failed for " + record["path"])
print("All listed messages verified")
PY
)
```

The archive contains raw `.eml` files and a versioned `manifest.jsonl`.
Folder records describe the original paths and names, including empty folders.
Message records give the generated member path, folder ID, UID, INTERNALDATE,
flags, byte length, and SHA-256 digest. The export preserves the message bytes
the server returns and leaves its Seen flags alone.

#### Reorganize or delete folders

After verifying the copy, use the following IMAP commands to reorganize the
disposable account.

```text
mkdir /Projects/Lists
mv '/Projects/Café team' '/Projects/Café archive'
rename-glob '/Projects/List-*' '/Projects/Lists/{1}'
```

Read every old and new path in the preview. To accept this mapping, replace
`ID` with the printed ID in `apply ID` in the same session, then type `yes`
on the controlling terminal. The preview prints every mapping in one pass.
For a large preview, use terminal scrollback or capture the session's
standard output to a file and read it before confirming. A new preview
replaces the old ID. Every apply attempt consumes its ID, including
cancellations. A refusal, a detected change, or an unknown reply stops the
serial renames. Inspect completed, failed, unknown, and pending pairs before
making a fresh preview.

For deletion practice, create a new empty folder in the disposable account.
Issue the second command only after the first succeeds and you intend to
remove that exact folder.

```text
mkdir /Projects/Practice
rmdir /Projects/Practice
```

Check the target and message count, then type `yes` on the controlling
terminal. A blank answer or Ctrl-C at the confirmation prompt cancels the
operation. Descendants require `--recursive`, which lists the target set
before confirmation. Keep archive verification and deletion as separate
steps. The shell checks the scope again after confirmation. Another client
or new mail can still change it between the last check and the server write.
If a write returns an unknown outcome, inspect the refreshed folders and
any surviving or unknown paths before retrying. Folder writes may change
subscriptions on the server. The shell reports any differences it can see.

#### Batch input and exit status

Read one command per line from a file with
`jaro -a ACCOUNT imap shell < commands.txt > results.txt`.
Piped input omits the prompt. Redirected header rows use tab-separated
fields. Progress and diagnostics go to stderr. The `apply` and `rmdir`
confirmation prompts read from the controlling terminal. Lines on command
stdin cannot confirm a write. For a run without a controlling terminal,
use `rename-glob` to inspect mappings. To apply them, generate a fresh
preview in an interactive session and use its ID. An explicit
`rmdir EXACT-PATH --yes` can delete in batch mode.

A normal interactive exit returns `0`. In batch mode, `0` indicates success
and `1` records any failed, cancelled, or partial command, even when a
later command succeeds. A connection or authentication failure also exits
with `1`. An unhandled network deadline exits with `124`. When Ctrl-C ends
the session, it exits with `130`.

If startup fails, check the account name, credentials, host, port, transport,
and certificate settings. If a folder is missing, check its parent with
`folders` or `refresh` and copy the displayed spelling and escapes. Server
permissions can hide folders.

### Message snapshots

`snapshot create DEST` copy regular Maildir `new`/`cur` messages and filename
flags from `$JAROMAILDIR` into a new private destination outside the source.
Stop all mail writers first; detected source changes fail creation. This is
a message-only copy, excluding accounts, credentials, configuration, indexes,
temporary messages and delivery journals. Symlinks and existing destinations
are refused. A failed capture may leave an incomplete private destination;
choose a fresh destination after inspecting/removing it.

`snapshot verify SNAPSHOT` check the complete manifest and every payload using
sizes and SHA-256. Invalid paths, missing/altered messages and malformed
manifests fail.

`snapshot restore SNAPSHOT DEST` verify the full artifact, stage and verify
message copies privately on the destination filesystem, then publish into an
absent or owned empty mail root. Existing populated roots and symlink paths are
refused. Keep snapshot/destination writers stopped until it returns. Outbox
messages go under `.jaro-recovered-outbox-N/outbox`, outside the normal send
queue; inspect and explicitly requeue individual messages only after checking
delivery history. Accounts, credentials and configuration need separate
recovery; rebuild the mail index after setup. Handled failures clean staging;
a parent-sync failure rolls back publication. Power loss or failed cleanup can
require inspection before retrying into a fresh root.

All three commands work without initialization or network access;
global options, including `-n`, are unsupported. Exit status is 0 on success,
1 for validation/storage failure, 2 for invalid invocation, and 128+signal for
a handled interruption. No encryption, live snapshot or full configuration
recovery is provided. See the manual's message snapshot section for the
versioned artifact format and consistency requirements.

### Recipient groups

Local recipient lists live in `$JAROMAILDIR/Groups/` (for example
`Mail/Groups/team`) and are addressed as `team@jaromail.group`. The
first line may set `#mode individual`, `#mode cc`/`#mode carboncopy`, or
`#mode bcc`; when no `#mode` is set, groups use `#mode individual`.

### Account and secrets

`passwd` set or update account password in the configured keyring.

`askpass` print account password resolved from keyring/prompt.

`isonline` check network reachability.

### Search and indexing

`index` update the search index.

`search` run search queries over local mail archives.

`header`, `headers` print mail headers.

### Addressbook and contacts

`abook` edit addressbook.

`addr`, `list` list addresses from addressbook.

`extract`, `parse` extract addresses from supported inputs.

`import` import addresses from stdin.

`export [vcard]` export the selected contact list as vCard records.

`learn`, `isknown`, `complete` contact learning/query/completion helpers.

### Filtering and Sieve

`update` install a newer stable JaroMail release into the current Linux prefix.

`fresh [MAIL_ROOT]` refresh filters and the existing search index.

`fresh-filters [MAIL_ROOT]` rebuild local filtering cache and regenerate Sieve.

`fresh-search [MAIL_ROOT]` refresh the existing notmuch index.

`filter` apply filters to a maildir.

`sieve` publish the shared filter generation and its portable `Filters.sieve` export.

The native Linux Sieve package also installs
`jaro-sieve-filter [--dry-run] --from INCOMING --to ROOT SCRIPT`.
It files messages from the source Maildir into the named mailboxes under
ROOT. Give it absolute directories. Share the exported source with a
Sieve-enabled delivery server so that each host compiles its own binary.

`sieve-import FILE` import a Jaromail-generated Sieve file into local
filter/addressbook files. The bounded token
parser validates and stages the complete import before publication. `-n`
validates without writes; `--legacy-generated` explicitly recovers older generated
header-contains scripts with a migration diagnostic. Arbitrary provider scripts
are unsupported. See [the import contract](filter-interfaces.md#rollout-and-bounded-import)
for supported grammar, profile/mapping requirements and recovery behavior.

### Maildir and utility commands

`backup` move search results between maildirs.

`merge` merge one maildir into another.

`deliver` deliver a message from stdin into a maildir.

`dedup [--scratch-dir DIR] [--] MAILDIR...` removes extra copies of messages
whose stored bytes match exactly, including duplicates in a single Maildir.
The first listed folder wins. Within it, `cur` wins over `new`, then the
smallest filename in unsigned byte order wins. The survivor keeps its name
and flags. Use `jaro -n dedup Primary Archive` to preview each proposed
keep/remove pair. Pause clients that move or edit existing entries during apply.
Delivery under fresh names may continue. See the user manual for scratch
placement, output escaping, accounting and retry behavior.

`rmdupes` uses the legacy `formail` Message-ID cache.
`rmdupes lastlog` obtains its list of destinations from `maildirs_lastlog`.

`remember`, `replay`, `preview`, `edit`, `vim`, `imap`,
`alot`, `alot-config`, `notmuch`, `notmuch-config`, `isml`, `ismd`.

## Files

`$JAROMAILDIR` mail root (default usually `$HOME/Mail`).

`$JAROMAILDIR/Accounts/` account configuration files.

`$JAROMAILDIR/Groups/` local recipient group files.

`$JAROMAILDIR/Filters.txt` filtering rules.

`$JAROMAILDIR/Filters.sieve` generated Sieve rules.

## See Also

`jaromail(1)`
