     oo                                                oo dP
                                                          88
     dP .d8888b. 88d888b. .d8888b. 88d8b.d8b. .d8888b. dP 88
     88 88'  `88 88'  `88 88'  `88 88'`88'`88 88'  `88 88 88
     88 88.  .88 88       88.  .88 88  88  88 88.  .88 88 88
     88 `88888P8 dP       `88888P' dP  dP  dP `88888P8 dP dP
     88~ooooooooooooooooooooooooooooooooooooooooooooooooooooo
    odP    your humble and faithful electronic postman

*Privacy enhanced terminal application for off-line E-mail management*

[![software by Dyne.org](https://files.dyne.org/software_by_dyne.png)](https://dyne.org/software/jaro-mail)

Updates on: https://dyne.org/software/jaro-mail

# INTRODUCTION

Jaromail is an integrated suite of interoperable tools for GNU/Linux,
Apple/OSX and MS/WSL to manage e-mail communication in a private and
efficient way, without relying too much on on-line services, in fact
encouraging users to store E-Mail locally and remove it from servers.

Rather than reinventing the wheel, Jaromail adopts a set of robust
free and open source tools and integrates them automatically. It also
comes with a complete usage manual.

Overview of internal components:

 executable | function
 ---------- | --------------------
  ZSH       | Shell interpreter
  Neomutt   | Mail User Agent
  mSmtp     | Mail Transport (send)
  Fetchmail | Mail Transport (receive)
  Vim       | Mail editor
  Mblaze    | Maildir manipulation
  GnuPG     | Encrypt and sign
  Mixmaster | Anonymity
  Notmuch   | Local Search engine
  ABook     | Local Addressbook
  Elinks    | HTML rendering

Jaromail architecture diagram:

![Jaromail functions diagram](http://files.dyne.org/jaromail/diagram.png)

List of core features

* Privacy by design to facilitate local storage and backup
* Helps to empty an on-line INBOX, free space and avoid bills
* Supports strong encryption and signing messages (gpg)
* Minimalistic and efficient interface with message threading
* Targets intensive usage of e-mails and mailinglists (neomutt)
* Stores E-Mails in a very reliable UNIX format (maildir)
* Integrates filtering locally as well remotely with (sieve)
* Implements graylisting to filter known and new contacts
* Can do search and backup by advanced expressions (notmuch)
* Automatically generates filter rules (sieve)
* Imports and exports VCard contacts to addressbook (abook)
* Computes and shows statistics on mail traffic
* Encrypted password storage OS independent (pass)
* Advanced analysis tools and address extraction (mblaze)
* Group contacts into secret BCC: lists of recipients
* Checks fingerprints of SSL/TLS server certificates
* Can send anonymous emails (mixmaster)
* Renders HTML with links, without images (elinks)
* May run on a remove machine connected via terminal (ssh)
* Many languages! so exotic! such UTF-8!
* Multi platform: GNU/Linux/BSD, Apple/OSX, Microsoft/WSL
* Old school, used by its author for the past 20 years

# Install

Make sure you have installed the following dependencies on your operating system, they should be named in a similar way across distributions, here below the names of the packages are referring to those found in APT distros:

```
fetchmail msmtp neomutt notmuch pinentry-curses abook \
wipe mblaze vim netcat-traditional
```

To compile and install jaromail on your system as root:
```
make install
```

When Jaromail is installed system-wide, the `JAROMAILDIR`
environmental variable can be changed to point to where all emails
will be stored, by default it is `$HOME/Mail`.

Read the manual:
```
man jaromail
```

# Usage

For a brief overview see the commandline help:
```
 jaro -h
```
When in doubt, make sure you read the [Jaromail usage manual](https://files.dyne.org/jaromail/jaromail-manual.pdf).

Note for experienced Mutt users: keys will be familiar, but there are
things to learn in the manual on how jaromail enhances the workflow.

# Developers

All development happens on [github.com/jaromail](https://github.com/dyne/JaroMail)

We are reachable on [many Dyne.org chats and channels](https://dyne.org/linktree).

# License
Jaromail is Copyright (C) 2010-2023 by the Dyne.org foundation

Designed, written and maintained by [@jaromil](https://github.com/jaromil)

This source code is free software; you can redistribute it and/or
modify it under the terms of the GNU Public License as published by
the Free Software Foundation; either version 3 of the License, or (at
your option) any later version.

This source code is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  Please refer to
the GNU Public License for more details.

You should have received a copy of the GNU Public License along with
this source code; if not, write to: Free Software Foundation, Inc.,
675 Mass Ave, Cambridge, MA 02139, USA.
