
  oo                                                oo dP
                                                       88
  dP .d8888b. 88d888b. .d8888b. 88d8b.d8b. .d8888b. dP 88
  88 88'  `88 88'  `88 88'  `88 88'`88'`88 88'  `88 88 88
  88 88.  .88 88       88.  .88 88  88  88 88.  .88 88 88
  88 `88888P8 dP       `88888P' dP  dP  dP `88888P8 dP dP
  88~ooooooooooooooooooooooooooooooooooooooooooooooooooooo
 odP   your humble and faithful electronic postman   v 1.2

A commandline tool to easily and privately handle your e-mail

Homepage with more information and links to the manual:

	      	   	             http://jaromail.dyne.org



* INSTALL

Apple/OSX users can simply drag JaroMail into /Applications

Nevertheless, you need to read the Manual: this software is not
graphical, yes has a high productive potential. JaroMail is operated via
Terminal, configured in plain text and overall made by geeks for geeks.
Beware.

GNU/Linux users can use the build/build-gnu.sh script to install
dependencies and build all necessary binaries. This is still dirty, it
will likely never be really packaged by zealots into their distros, but
works well for those who use it. It might get better in some future.
After build, use the ./install.sh script to put JaroMail in $HOME/Mail
or use an argument to install it in some other location.


Jaro Mail is completely reentrant: if you install it in multiple
locations you can have completely separated setups, with multiple
identities and different filters.

* USAGE instructions

See the commandline help:

 jaro -h

When in doubt, make sure you read the User's Manual, it is important.



* DEVELOPERS

All revisioned in Git, see https://github.com/dyne/JaroMail

Pull requests and patches welcome.

Come on channel #dyne on https://irc.dyne.org to get in touch with devs

Make sure to idle in that channel, answers take some time to come.

We are all idling artists.


* DONATE

Money donations are very welcome and well needed, they will encourage
further development even beyond my own needs for this software.

 https://dyne.org/donate



* ACKNOWLEDGEMENTS

JaroMail is conceived, designed and put together with a substantial
amount of ZShell scripts and some C code by Jaromil @ Dyne.org

The Mutt sourcecode included is maintained by Antonio Radici
<antonio@dyne.org> who is also the original maintainer of the Debian
package. Here below the list of main Mutt authors:
Mutt is
 Copyright (C) 1996-2007 Michael R. Elkins <me@cs.hmc.edu>
 Copyright (C) 1996-2002 Brandon Long <blong@fiction.net>
 Copyright (C) 1997-2008 Thomas Roessler <roessler@does-not-exist.org>
 Copyright (C) 1998-2005 Werner Koch <wk@isil.d.shuttle.de>
 Copyright (C) 1999-2009 Brendan Cully <brendan@kublai.com>
 Copyright (C) 1999-2002 Tommi Komulainen <Tommi.Komulainen@iki.fi>
 Copyright (C) 2000-2004 Edmund Grimley Evans <edmundo@rano.org>
 Copyright (C) 2006-2008 Rocco Rutte <pdmef@gmx.net>

Mairix, the search engine we use in Jaro Mail, is licensed GNU GPL v2
Mairix is
 Copyright (C) Richard P. Curnow  2002,2003,2004,2005,2006,2007,2008
 Copyright (C) Sanjoy Mahajan 2005
 Copyright (C) James Cameron 2005
 Copyright (C) Paul Fox 2006
And received contributions from: Anand Kumria André Costa, Andreas
Amann, Andre Costa, Aredridel, Balázs Szabó, Bardur Arantsson,
Benj. Mako Hill, Chris Mason, Christoph Dworzak, Christopher Rosado,
Chung-chieh Shan, Claus Alboege, Corrin Lakeland, Dan Egnor, Daniel
Jacobowitz, Dirk Huebner, Ed Blackman, Emil Sit, Felipe Gustavo de
Almeida, Ico Doornekamp, Jaime Velasco Juan, James Leifer, Jerry
Jorgenson, Joerg Desch, Johannes Schindelin, Johannes Weißl, John
Arthur Kane, John Keener, Jonathan Kamens, Josh Purinton, Karsten
Petersen, Kevin Rosenberg, Mark Hills, Martin Danielsson, Matthias
Teege, Mikael Ylikoski, Mika Fischer, Oliver Braun, Paramjit Oberoi,
Paul Fox, Peter Chines, Peter Jeremy, Robert Hofer, Roberto Boati,
Samuel Tardieu, Sanjoy Mahajan, Satyaki Das, Steven Lumos, Tim Harder,
Tom Doherty, Vincent Lefevre, Vladimir V. Kisil, Will Yardley,
Wolfgang Weisselberg.

Procmail was originally designed and developed by Stephen R. van den
Berg. The Procmail library collection included in Jaro Mail is developed
and maintained by Jari Aalto.

MSmtp is developed and maintained by Martin Lambers.

The RFC 822 address parser (fetchaddr) is originally written by
Michael Elkins for the Mutt MUA.

The gateway to Apple/OSX addressbook (ABQuery) was written by Brendan
Cully and just slightly updated for our distribution.

We are also including some (experimental, still) modules for statistical
visualization using JQuery libraries:

Timecloud is Copyright (C) 2008-2009 by Stefan Marsiske
TagCloud version 1.1.2 (c) 2006 Lyo Kato <lyo.kato@gmail.com>
ExCanvas is Copyright 2006 Google Inc.
jQuery project is distributed by the JQuery Foundation under the
 terms of either the GNU General Public License (GPL) Version 2.
The Sizzle selector engine is held by the Dojo Foundation and is
 licensed under the MIT, GPL, and BSD licenses.
JQuery.sparkline 2.0 is licensed under the New BSD License
Visualize.JQuery is written by Scott Jehl Copyright (c) 2009 Filament Group 

# JaroMail is Copyright (C) 2010-2013 Denis Roio <jaromil@dyne.org>
#
# This source  code is free  software; you can redistribute  it and/or
# modify it under the terms of  the GNU Public License as published by
# the Free  Software Foundation; either  version 3 of the  License, or
# (at your option) any later version.
#
# This source code is distributed in  the hope that it will be useful,
# but  WITHOUT ANY  WARRANTY;  without even  the  implied warranty  of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
# Please refer to the GNU Public License for more details.
#
# You should have received a copy of the GNU Public License along with
# this source code; if not, write to:
# Free Software Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
