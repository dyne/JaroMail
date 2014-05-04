
# TODO notes for Jaro Mail

  Contribute code or donate to complete this TODO
  https://dyne.org/donate


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

## Peek to open imap folders with reverse Date ordering
   when open, imap folders should list emails without threading
   this is really intended for a peek
   
## CC: detection
   filter in known/ also emails sent to private addresses in cc:

## Resident indexing
   Mairix search now re-indexes all mailfolder before search
   split this into two operations: index (to refresh) and search
   most times we search for old stuff which might be already indexed

   MAYBE: add possibility to re-index after fetch

   CAVEAT: progressive indexing in Mairix does not work,
   	   must start a new one every time

## Sieve filters for first level naming of mailinglists
   consolidate the use of first level naming of filtered maildirs
   (a la newsgroups) and use it also for sieve filters so that imap
   folders will be created to contain those.

   the peek function then should be started up with a list of those
   folders so that imap can be peeked with the same hierarchy of
   downloaded emails.
   
## Solve imap idle timeout on Mutt
  peek command should use fetchmail --idle instead of mutt -f imaps
  this way we can also implement desktop notifications
  
  PLAN: use isync and open local maildirs

## Substitute procmail with a simple C filter
 we just filter From, To:, CC: and mailman headers

 a small C program using sqlite3 should be enough

## Investigate integration with Mailpile's frontend

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



