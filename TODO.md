# TODO notes for Jaro Mail

  Contribute code or donate to complete this TODO
  https://www.dyne.org/donate

## Notmuch web

Fire up the web interface for notmuch searches

## Dovecot local service

Serve maildirs to all kinds of imap clients (MUA) locally

## DIME specification

Jaro Mail will support DIME
https://darkmail.info/downloads/dark-internet-mail-environment-december-2014.pdf

## Vacation trigger
sieve script example

```

require ["fileinto", "vacation", "variables"];

if header :is "X-Spam-Flag" "YES" {
    fileinto "Spam";
}

if header :matches "Subject" "*" {
	set "subjwas" ": ${1}";
}

vacation
  :days 1
  :subject "Out of office reply${subjwas}"
"I'm out of office, please contact Joan Doe instead.
Best regards
John Doe";
```

## substitute mairix with mu (maildir-utils)
   has all functions and now also date ranges
   to have a list of hits use mu find -l f
   will output only filenames, this way symlink maildirs
   of results can be generated and browsed with mutt

## Sieve filters for first level naming of mailinglists
   consolidate the use of first level naming of filtered maildirs
   (a la newsgroups) and use it also for sieve filters so that imap
   folders will be created to contain those.

   the peek function then should be started up with a list of those
   folders so that imap can be peeked with the same hierarchy of
   downloaded emails.

   eventually substitute the main usage of procmail in jaromail
   with sieve filters and then sync every mailbox (full server-side
   filtering)

## Solve imap idle timeout on Mutt (??)
   use isync/mbsync and open local maildirs

## Serve local maildirs over imap using dovecot
	to enable use of any MUA frontend supporting imap

## Import of addresses from GnuPG keyring

## TBT
   time based text, all included in html mails

## Speedmail or Quickmail
  write down a mail from commandline and send it right away (if online)
  doesn't uses Mutt to generate the mail body
  but might read Mutt.txt configuration for headers and such

## WIP Stats
 * Moar some fancy statistics

   use timecloud for a jquery visualization

 * include anu arg's mailinglist statistics
