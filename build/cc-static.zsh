#!/usr/bin/env zsh

# Static build wrapper
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
# this script modifies compiler commands to include all static
# libraries that are found.


# configuration variables

typeset -a arguments cflags libdirs deps skip
typeset -lA libraries # map (name, static|dynamic)
typeset -h argc

# check the operating system
case `uname` in
    Linux)
	cc=gcc
#	cc=clang
	libdirs+=/lib
	libdirs+=/usr/lib
	libdirs+=/usr/local/lib
	cflags+=(-O2)
	libraries+=(ld-linux-x86 skip)
	libraries+=(linux-vdso   skip)
	libraries+=(c            skip)
#	libraries+=(rt           skip)
	libraries+=(pthread      shared)
	libraries+=(dl           shared)
	libraries+=(m            shared)
	;;
    Darwin)
	cc=clang
	libdirs+=(/opt/local/lib)
	cflags+=(-arch x86_64)
	cflags+=(-arch i386)
	;;
esac

##########################

arguments=($@)

linkdep() {
    # # descend lib dependencies
    dep="`basename ${1}`"
    { test "$dep" = "" } && { echo "Error: linkdep called with void argument"; return 1 }

    dep=${dep/lib/} # strip lib- prefix

    # check if known
    { test "${libraries[$dep]}" = "skip" } && {
	# to be skipped
	print "$dep :: skip"
	return 3
    }

    { test "${libraries[$dep]}" = "shared" } && {
	# force shared
	arguments+=("-l${dep}")
	isshared=1
	print "$dep :: shared"
	return 1
    }

    # check if a static lib is present
    for l in ${=libdirs}; do
	static="${l}/lib${dep}.a"	
	shared="${l}/lib${dep}.so"
	{ test -r "$shared" } || { shared="${l}/lib${dep}" }

	{ ! test -r "$shared" } && { ! test -r "$static" } && { continue }

	# save deps
	{ test -r "$shared" } && {
	    for dd in `ldd $shared | awk '{print $1}'`; do
		deps+=($dd); done
	}

	if [ -r "$static" ];   then
	    print "$dep :: static :: $static <> ${arguments[$argc]}"
	    arguments[$argc]="" # remove from arguments
	    arguments+=($static);
	    libraries+=($dep static)
	    return 0

	elif [ -r "$shared" ]; then
	    print "$dep :: shared :: $shared <> ${arguments[$argc]}"
	    arguments[$argc]="" # remove from arguments
	    arguments+=($shared);
	    isshared=true
	    libraries+=($dep dynamic)

	    return 1

	else echo "$dep not found in ${l}";  return 10

	fi
    done

}

# check if we are linking
echo $arguments | grep -i ' -l' > /dev/null
{ test $? = 0 } && {

    argc=0
    for i in ${=arguments}; do
	argc=$(( $argc + 1 ))

	# is it a new library path?
	{ test "$i[1,2]" = "-L" } && {
	    lib=${i/-L/}
	    libdirs=($lib $libdirs)
	    continue
	}

	# is it a shared library link argument?
	{ test "$i[1,2]" = "-l" } && {
	    lib=${i/-l/}

	    linkdep "lib${lib}"
	}
    done
    
    echo "-= Process sub dependencies"
    # eliminate duplicates
    typeset -U deps

    for d in ${=deps}; do

	# check if it was already made static
	ds="`print ${d/lib/} | cut -d. -f1`"

	{ test "$libraries[$ds]" = "static" } && { continue }
	linkdep "lib${ds}"
    done

    # check if full static
    { test -z $isshared } && { cflags+=(-static) }
}

# remove duplicate arguments
typeset -U arguments

# execute
echo "$cc ${=cflags} ${=arguments}"
$cc ${=cflags} ${=arguments}



# TODO

	    # 	Darwin)
	    # 	    deps1=`otool -L ${libdir}/$lib.dylib | awk '
	    # 	    /^\// {next} /\/opt\/local/ {print $1}'`
	    # 	    for d1 in ${(f)deps1}; do
	    # 		dep=`basename $d1`
	    # 		sdep=${dep/.*/.a}
	    # 		ddep=${dep/lib/-l}
	    # 		ddep=${ddep/.*/}
	    # 		if [ -r ${libdir}/${sdep} ]; then
	    # 		    arguments+=(${libdir}/${sdep})
	    # 		else arguments+=($ddep); fi
	    # 	    done


