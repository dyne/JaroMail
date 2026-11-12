jaro(1)
=======

## Name

jaro - command line interface for Jaromail workflows

## Synopsis

`jaro [options] [command] [command-options]`

## Description

`jaro` is the operational CLI for Jaromail. It orchestrates account
configuration, local maildir operations, addressbooks, filtering,
indexing and SMTP/IMAP actions.

## Options

`-a ACCOUNT` use a specific account instead of `default`.

`-l LIST` use a specific addressbook/list (default: `whitelist`).

`-n` dry-run mode where supported.

`-q` quiet mode.

`-D` debug mode.

`-h` show help.

`-v` or `--version` prints the assigned release version and exits. The startup
banner and help use the same installed metadata. Source builds derive it from
release tags, with the v6.0.0 development baseline when no v6 tag exists.

`-f` force mode for selected operations.

## Filter engine selection

`JARO_FILTER_ENGINE=legacy|native` selects compilation and filter/fetch dispatch;
unset selects native. Explicit `JARO_FILTER_ENGINE=legacy` selects rollback.
Empty/unknown values fail. Native `update`/`sieve` invoke the installed compiler
with explicit mail/account roots and return its failures without fallback.
No profile file is required: built-in defaults enable fileinto, automatic mailbox
creation and the qualified legacy ERE subset. Regex is declared only when used;
partial addresses keep matching without wildcard suffixes. Unsupported ERE
exports fail with a source-line diagnostic. Explicit legacy rejects typed/domain records. Sieve
generation writes the local export only, without remote deployment.

Native `filter [MAILDIR]` compiles/verifies once and applies one pinned batch.
The default Maildir is `$JAROMAILDIR/incoming`. Native `fetch` uses the same
batch after transport and refreshes the index once, even after partial failure;
batch errors remain nonzero. Filter itself does not index. Native Sieve import
accepts the bounded Jaromail-generated grammar described below.

Fresh `init` under unset/explicit native installs a comment-only typed-example
policy with builtins active. Explicit legacy keeps the original 14 ERE defaults.
Existing policies are never replaced or translated. Supported legacy EREs need
no profile configuration; patterns outside the qualified subset need a deliberate
rewrite. `save` and `move` are aliases. Invalid selectors or
missing required templates fail before bootstrap configuration writes.

Native `filter --explain [MAILDIR]` and `-n filter [MAILDIR]` classify only,
checking the selected generation against current policy without publishing.
Missing/stale generations return 5 and request an explicit `jaro update`.
Native `-n fetch` explains local incoming mail without transport or indexing.

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

`export` export addresses to another format.

`learn`, `isknown`, `complete` contact learning/query/completion helpers.

### Filtering and Sieve

`update` rebuild local filtering cache and regenerate Sieve.

`filter` apply filters to a maildir.

`sieve` regenerate `Filters.sieve` only.

`sieve-import FILE` import a Jaromail-generated Sieve file into local
filter/addressbook files. With `JARO_FILTER_ENGINE=native`, the bounded token
parser validates and stages the complete import before publication. `-n`
validates without writes; `--legacy-generated` explicitly recovers older generated
header-contains scripts with a migration diagnostic. Arbitrary provider scripts
are unsupported. See [the import contract](filter-interfaces.md#rollout-and-bounded-import)
for supported grammar, profile/mapping requirements and recovery behavior.

### Maildir and utility commands

`backup` move search results between maildirs.

`merge` merge one maildir into another.

`deliver` deliver a message from stdin into a maildir.

`rmdupes` remove duplicate messages.

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
