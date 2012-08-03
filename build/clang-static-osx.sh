#!/usr/bin/env zsh

# Static build wrapper to be used with clang on osx
#
# Copyleft (C) 2012 Denis Roio <jaromil@dyne.org>
#
# This source code is free software; you can redistribute it and/or
# modify it under the terms of the GNU Public License as published by
# the Free Software Foundation; either version 3 of the License, or
# (at your option) any later version.
#
# This source code is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
# Please refer to the GNU Public License for more details.
#
# You should have received a copy of the GNU Public License along with
# this source code; if not, write to:
# Free Software Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
#
# this script modifies compiler commands to include all
# static libraries that are found.


# configuration variables

cc=clang
libdir="/opt/local/lib"

typeset -a arguments
typeset -a cflags

cflags+=(-arch x86_64)
cflags+=(-arch i386)

##########################

arguments=($@)

# linking
echo $arguments | grep ' -l' > /dev/null
{ test $? = 0 } && {
    c=1; for i in ${=arguments}; do
	{ test "$i[1,2]" = "-l" } && {
	    lib=${i/-l/lib}
	    arguments[$c]="${libdir}/${lib}.a"
	    # descend lib dependencies
	    deps1=`otool -L ${libdir}/$lib.dylib | awk '
	    /^\// {next} /\/opt\/local/ {print $1}'`
	    for d1 in ${(f)deps1}; do
		dep=`basename $d1`
		sdep=${dep/.*/.a}
		ddep=${dep/lib/-l}
		ddep=${ddep/.*/}
		if [ -r ${libdir}/${sdep} ]; then
		    arguments+=(${libdir}/${sdep})
		else arguments+=($ddep); fi
	    done
	}
	c=$(( $c + 1 ))
    done
}

# remove duplicate arguments
typeset -U arguments

# execute
$cc ${=cflags} ${=arguments}
