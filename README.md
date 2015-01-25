
     oo                                                oo dP
                                                          88
     dP .d8888b. 88d888b. .d8888b. 88d8b.d8b. .d8888b. dP 88
     88 88'  `88 88'  `88 88'  `88 88'`88'`88 88'  `88 88 88
     88 88.  .88 88       88.  .88 88  88  88 88.  .88 88 88
     88 `88888P8 dP       `88888P' dP  dP  dP `88888P8 dP dP
     88~ooooooooooooooooooooooooooooooooooooooooooooooooooooo
    odP    your humble and faithful electronic postman 

*A commandline tool to easily and privately handle your e-mail*

Version: **3.2**

Updates on: http://dyne.org/software/jaro-mail

# INTRODUCTION

Jaro Mail is an integrated suite of interoperable tools for GNU/Linux
and Apple/OSX to manage e-mail communication in a private and efficient
way, without relying too much on on-line services, in fact encouraging
users to store e-mail locally.

Rather than reinventing the wheel, Jaro Mail reuses some existing free
and open source tools working since more than 10 years, generating
their configurations and setting up integrations automatically.

 executable | function
 ---------- | --------------------
  ZShell    | Scripting language
  Mutt      | Mail User Agent
  Fetchmail | Mail Transport (fetch)
  Vim       | Mail editor
  GnuPG     | Content encryption
  MSmtp     | Mail Transport (send)
  Notmuch   | Search engine
  ABook     | Addressbook
  Elinks    | HTML rendering

A round-up on Jaro Mail features follows:

![Jaro Mail functions diagram](http://files.dyne.org/jaromail/diagram.png)

* Minimalistic and efficient interface with message threading
* Targets intensive usage of e-mails and mailinglists
* Stores e-mails locally in a reliable format (maildir)
* Integrates whitelisting and blacklisting, local and remote
* Can do search and backup by advanced expressions
* Automatically generates filter rules (sieve)
* Imports and exports VCard contacts to addressbook
* Computes and shows statistics on mail traffic
* Encrypted password storage using OS native keyrings
* Advanced maildir tools (merge, backup, address extraction)
* Defers connections for off-line operations
* Checks SSL/TLS certificates when fetching and sending mails
* Supports strong encryption messaging (GnuPG)
* Multi platform: GNU/Linux/BSD, Apple/OSX
* Old school, used by its author for the past 10 years

# INSTALL

**GNU/Linux** users can run `make` to install all needed components
  (done automatically, requires root) and compile auxiliary
  tools. Once compiled then `make install` will put Jaro Mail in
  `/usr/local`. Or To install it as user into a self-contained place:

```
   PREFIX=$HOME/Postbox make install
```

Will create the directory `Postbox` in your home directory and install
Jaro Mail inside it, with the `jaro` executable in `Postbox/bin`.

When Jaro Mail is installed system-wide, the `JAROMAILDIR`
environmental variable can be changed to point to where all emails
will be stored, by default it is `$HOME/Mail`.

The dependencies to be installed on the system for Jaro Mail are
* build: `make gcc libglib2.0-dev libgnome-keyring-dev`
* run: `fetchmail msmtp mutt notmuch pinentry-curses abook wipe alot`

Keep in mind **you need to read the Manual**: this software is not
graphical, it is not meant to be intuitive, does not contains
eyecandies (except for stats on mail traffic). Jaro Mail is operated
via Terminal, configured in plain text and overall made by geeks for
geeks.


**Apple/OSX** Jaro Mail 3.0 has not yet been updated to Apple/OSX. With
  2.0 users can simply drag Jaro Mail into /Applications When started
  Jaro Mail opens a Terminal window preconfigured with its environment,
  to activate it for any terminal add this to `~/.profile`:
```
export PATH=/Applications/JaroMail.app/Contents/Resources/jaro/bin:$PATH
```

# USAGE MANUAL

For a brief overview see the commandline help:
```
 jaro -h
```
When in doubt, make sure you read the User's Manual, it is important.

Download the PDF: https://files.dyne.org/jaromail/jaromail-manual.pdf

Or browse online the latest version:
https://github.com/dyne/JaroMail/blob/master/doc/jaromail-manual.org

# DEVELOPERS

All revisioned in Git, see: https://github.com/dyne/JaroMail

Pull requests and patches welcome, for an overview of current plans
see [TODO](TODO.md)

Our chat channel is **#dyne** on https://irc.dyne.org

Make sure to idle in that channel, answers take some time to come.

We are all idling artists.

# DONATE

Donations are very welcome and well needed.

By donating you will encourage further development.

 https://www.dyne.org/donate

# ACKNOWLEDGEMENTS

The Jaro Mail software and user's manual is conceived, designed and put
together with a substantial amount of ZShell scripts and some C code
by Denis Roio aka [Jaromil](http://jaromil.dyne.org).

Jaro Mail would have never been possible without the incredible amount of Love shared by the free and open source community, a more complete list of contributors is included in the [user manual](https://files.dyne.org/jaromail/jaromail-manual.pdf) in the `Acknowledgments` section.

Jaro Mail is Copyright (C) 2010-2014 Denis Roio <jaromil@dyne.org>

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
