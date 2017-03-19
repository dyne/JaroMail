# JaroMail ChangeLog

## 4.3
## 19 Mar 2017

Switch notmuch support to use mutt-kz instead of alot, detected at
runtime. Fixes to password security and support for pass as password
store. Better cli operation without mutt and new zaw based completion
facility, initial support for vcal files, consolidated addressbook
operations.


## 4.2
## 14 Apr 2016

Fixes to imap folder detection, also fixing use for GMail. Keyring
fixes and cleanups, more complementary keys to accomodate Emacs users.

## 4.1
## 13 Mar 2016

Password storage refactoring: updating to this release requires users
to re-enter their passwords saved in the system's keyring.  The old
local keyring code was removed and support for the minimalist pass
keyring was contributed by Parazyd. Secret-tool is now used in place
of our own gnome-keyring tool when present in newer GNU/Linux distros.
Minor cleanups.

## 4.0
## 10 Nov 2015

This release brings a refactoring of all commands for shell pipe
interoperability, which also reduced the overall size of code. The
user manual reflect changes and shows several examples on how to
combine "search", "extract" and other commands to manage statistics,
groups and addressbooks. New fetures include: revamped terminal
statistics, improved abook conversion, improved secure protocol checks
and locking mechanisms.

## 3.2 25 Jan 2015

New ZSh completion extension, now capable of tab-completion for
addresses, maildirs and even remote imap folders. Included
emlx2maildir script in extras, to import Apple/OSX mail storage into
maildirs. New exclude config directive for accounts, to exclude
fetching certain remote imap folders. Fixes to vcard extraction and
import, filtering to priv folder and gpg email encryption.  Updates to
manual and commandline documentation.

## 3.1
### 16 Jan 2015

Fixes to locking, email fetching and anonymous mixmaster mailer.
Several improvements to code readability, stability and speed using
zsh builtins.

## 3.0
### 10 Jan 2015

New addressbook format using Abook natively for storage, improving
speed and usability, with UTF-8 encapsulation support. New search
engine using Notmuch instead of Mairix New usage scheme for
import/export of addressbooks and advanced functionalities for email
address extraction from maildirs and search results. Core refactoring
and cleanup. Updates to the user manual.

## 2.1
### 26 Dec 2014

New publish feature to render in HTML a maildir and all its contents,
making them browsable and producing an RSS/Atom feed. Several fixes to
keyring handling, UTF-8 parsing, locking and source build scripts.

## 2.0
### 12 May 2014

This release includes a major rewrite of the filter engine now using
our own system based on ZSh map arrays in place of Procmail, improving
speed and reliability.  Also the sending mechanism has been rewritte
to use maildirs and allow the review of the outbox queue. New features
are the Group configuration to maintain lists of recipients and the
Mixmaster3 support to easily send anonymous emails. Custom builds of
Mutt and Mairix have been dropped in favour of system-wide builds.
The account configuration has been simplified and the documentation
has been updated accordingly. Included are also several bugfixes and
an overall cleanup of the code.

## 1.3.1
### 13 December 2013

More fixes to OSX usage for editor and attachments, changes to
local keyring hashing, slight refactoring of packaging and install
scripts.

## 1.3
### 5 December 2013

Major fixes for the new OSX Application packaging implying a
rather important refactoring: during its operation JaroMail will
not write into $WORKDIR anymore, but only into $MAILDIR, where
most user generated files are now moved.

Includes bugfixes for the build on Fedora.

## 1.2
### 14 September 2013

Several usability improvements and bugfixes both for usage on
GNU/Linux and Apple/OSX systems. The latter has now a different
distribution format for JaroMail as a self-contained app bundle.

Bugfixes for: backup mechanism, gnupg wrapper, file attachments,
whitelisting, HTML mail rendering.

New features include a native keyring mechanism to store account
informations in a single-pass symmetric encrypted file.
Minor updates to the documentation.

## 1.1
### 18 October 2012 - codename: Slick bastard

Lots of bugfixes, code cleanups and usability improvements are
paving the way for some new powerful features.

The addressbook system has been enhanced to permit editing of the
entries in white and blacklist, plus import and export of VCards
is now possible.

The use of GnuPG in Mutt is fixed, along with performance
and usability improvements also to search.

Vim is now used as default editor, configured to properly format
emails with paragraph justification.

More improvements were done also to the management of multiple
accounts, mailbox filtering, maildir merge and backup.

## 1.0
### 18 June 2012 - codename: E-Data addict

Software development went forward in a rather passionate way,
mostly motivated by the author's need to backup his rather big
archive of e-mails. So this release's focus is on stability,
search and backup features and a rather deep refactoring of the
codebase.

Mairix has been added as an integrated search engine.

Backup is possible also using search expressions (see manual)

Some new modules in statistics implement jquery reports.

Code is modularized in pre-compiled zsh modules.

Extensive testing has proven the whole setup to be stable and
ready for production use, hence the 1.0 release.

## 0.9
### 06 May 2012 - codename: The rush of the unemployed

After getting recently unemployed because of European austerity
cuts (and because of not working in a bank), Jaromil followed up
with a full pijama coding session that lasted about a week,
hammering out most issues that make now this software enter BETA
stage, while starting to use it for real.

This is the first public release of Jaro Mail, it comes with a
User Manual and lots of usability fixes over the previous, it was
tested on GNU and OSX operating systems.

## 0.1
### 29 May 2012 - Initial release

After about a dozen years of development and use by its own author, Jaromail is released to the public as an e-mail and mailinglist console setup.

To find out what name to give to the software, Jaromil asked his friend and PhD colleague Max Kazemzadeh how it should be called and Max, an hilarious chap who always has a joke ready, replied: Jaro Mail!
