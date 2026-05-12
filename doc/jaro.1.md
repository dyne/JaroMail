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

`-v` show version.

`-f` force mode for selected operations.

## Commands

### Core mail flow

`wizard` interactive account setup.

`fetch` fetch unread messages for an account.

`send` send queued messages from `outbox/`.

`queue` read a full message from stdin and queue it.

`smtp` read a full message from stdin and send via SMTP.

`peek` open remote IMAP mailbox view.

`open` open a local maildir folder.

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
filter/addressbook files.

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
