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

Developer build/test commands:

```
make
make test
sudo make install
./extras/test/inittest.sh
```

The GNU/Linux build honors `CC`, `CPPFLAGS`, `CFLAGS`, `LDFLAGS`, and `LDLIBS`.
It uses `ccache` for compilation when available; set `CCACHE=` to disable it.
`make install` builds the helpers first. Set `PREFIX` for a private installation
or `DESTDIR` to stage a package while keeping its final runtime paths.

`make test` runs the source smoke test and every `extras/test/test-*.sh`
regression, reporting passed, failed, and skipped scripts. Missing optional
runtime tools produce exit 77 in individual scripts. `make test-strict` requires
every script to run without skips and is the CI entry point. Module tests use
local fixtures and never send or fetch real mail.

Only the minimal Pigeonhole `sievec`/`sieve-test` frontend sources, shared helper,
and required notices are included under [`vendor/`](vendor/README.md). The test
suite verifies their pinned digests and rejects extra vendor payload. Building
the validators requires matching external Dovecot/Pigeonhole development
libraries and remains separate from Jaromail's default build.

`extras/test/run-source.sh` validates the checked-out tree by executing
`src/jaro` against a temporary local install layout. `inittest.sh`
validates the installed command (`/usr/local/bin/jaro`).

Developer test dependencies (also listed in `.github/workflows/pr-tests.yml`) include:
`zsh`, `make`, `gcc`, `python3`, `fetchmail`, `msmtp`, `mutt` or `neomutt`,
`notmuch`, `pinentry-curses`, `abook`, `wipe`, and `mblaze` (for `maddr`).

Legacy/optional surfaces are documented in:

- `doc/optional-integrations.md`
- `doc/maintenance-inventory.md`

Current policy for this branch:

- `stats` and `publish` are retained for compatibility but treated as legacy.
- default maintenance and regression checks focus on the core CLI path:
  `init`, `update`, `filter`, `search`, `fetch`, `send`, and addressbook flows.

`JARO_FILTER_ENGINE=legacy|native` selects filter compilation. Unset retains
legacy; an empty or unknown value fails. `jaro update` and `jaro sieve` use the
native compiler only when explicitly selected and propagate its errors without
fallback. The normal build/install includes the compiler and legacy export
lock bridge; neither needs Dovecot. Custom `JAROMAILDIR`, `JAROWORKDIR` and
`JAROACCOUNT` paths are passed explicitly. Sieve generation never deploys rules
to a server. Indexing remains in the existing update/fetch workflows.

Native compilation defaults to `core-v1`, supporting typed address/domain
rules. Legacy `from`/`to` ERE exports require an independently qualified optional
profile; unqualified exports fail with migration diagnostics. Explicit legacy
also rejects typed/domain records rather than discarding them. At this compiler
milestone native batch filtering reports unavailable (127), retaining incoming
mail; its integration follows separately. Native Sieve import remains unsupported
(4). See [the interface contract](doc/filter-interfaces.md).

The optional `pigeonhole-2.4.5-posix-ere-v1` profile needs explicit fileinto,
mailbox/creation configuration, regex capability and a `regex-qualification`
record containing the SHA256 of installed `filter-regex-qualification.json`.
The compiler checks its embedded catalogue, so replacing that file or supplying
a different hash cannot grant support. The qualified subset covers shipped
defaults and benchmark patterns; unsupported EREs retain the prior generation
and fail with a source-line diagnostic. Backend requirements and the narrow
minimal-quoting correction are documented in the interface/policy contracts.

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
