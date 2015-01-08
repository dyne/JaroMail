# Roundcube Majordomo
## Addressbook based whitelist

Server-side extension to use the addressbook as whitelist.

The whitelist helps prioritize messages received from known people, creating different folders for first-contact and non recipient emails.

The user interface of Roundcube stays the same, more IMAP folders are created automatically.

anaging the whitelist is done normally via the addressbook, which can also import and export VCard files from and to mobile phones.

This is not a Roundcube plugin: it is a server-side ZShell script to be run by Cron, generating a Sieve script usually processed by the Dovecot2 or Procmail LDA. Beware folders created are likely to be accessible only via IMAP.

# Raison d'être

Roundcube majordomo is there to organize the e-mail workflow so that one's attention is dedicated to important communications, rather than being constantly distracted by various degrees of spam and the need to weed it out of the mailbox. This ambitious task is pursued by realizing an integrated approach consisting of flexible whitelisting and the distinction between mails from known people and the rest.


Folder         | What goes in there
---------------|--------------------------------------------------
**INBOX**      | Mails whose sender is known (Whitelist)
**priv**       | Unknown sender, we are the explicit destination
**unsorted**   | Unknown sender, we are in cc: or somehow reached
**lists.**     | Mailinglists grouped per address and domain name
**zz.spam**    | Spam detected and marked as such in the headers
**zz.bounces** | Warnings and mailman bounces of sorts

The advantage using such a folder organization is that every time we open up the mail reader (quickly checking INBOX from our phone, for instance) we will be presented with something we are likely to be most interested in (known people replying our mails) and progressively, as we will have the time to scroll through, mails from "new people" or mass mailings of sort. Please note this organization does not includes spam, which is supposedly weeded out on the server via spamlists: White/Blacklisting has more to do with our own selection of content sources than with the generic protection from random pieces of information.


For more information about this 3-tier imap folder system, see JaroMail's manual on:
https://files.dyne.org/jaromail/jaromail-manual.pdf

## Installation

Roundcube Majordomo, or Roundomo in brief, is a backend agent for Roundcube webmail setups that takes advantage of its easy to use addressbook interface and the Sieve scripting language for server-side filtering into folders.

Roundomo is developed for the Dyne.org webmail and its targeted to be used on a Postfix/Dovecot2 setup supporting multiple domains.  An example of such a configuration is [detailed here](https://www.digitalocean.com/community/tutorials/how-to-configure-a-mail-server-using-postfix-dovecot-mysql-and-spamassasin).

# Disclaimer

Roundcube Majordomo is Copyright (C) 2014 Denis Roio <jaromil@dyne.org>

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
