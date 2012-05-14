/*
 *  Copyright (c) 1998 Thomas Roessler <roessler@guug.de>
 * 
 *  This file contains the prototypes for several of the
 *  functions from helpers.c.  It is part of the "little
 *  brother database" packet.
 * 
 *  These functions have been stolen from the mutt mail
 *  user agent.  The copyright notice from there:
 * 
 *  Copyright (C) 1996-8 Michael R. Elkins <me@cs.hmc.edu>
 * 
 *     This program is free software; you can
 *     redistribute it and/or modify it under the terms
 *     of the GNU General Public License as published by
 *     the Free Software Foundation; either version 2 of
 *     the License, or (at your option) any later
 *     version.
 * 
 *     This program is distributed in the hope that it
 *     will be useful, but WITHOUT ANY WARRANTY; without
 *     even the implied warranty of MERCHANTABILITY or
 *     FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *     General Public License for more details.
 * 
 *     You should have received a copy of the GNU General Public License
 *     along with this program; if not, write to the Free Software Foundation,
 *     Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301,, USA.
 */

/* $Id: helpers.h,v 1.4 2005-10-29 14:48:08 roland Exp $ */

#ifndef _HELPERS_H
#define _HELPERS_H

#include <ctype.h>

char *safe_strdup (const char *);
void *safe_calloc (size_t, size_t);
void *safe_malloc (unsigned int);
void safe_realloc (void **, size_t);
void safe_free (void *);

#define FREE(x) safe_free(x)
#define SKIPWS(c) while (*(c) && isspace ((unsigned char) *(c))) c++;
#define strfcpy(A,B,C) strncpy(A,B,C), *(A+(C)-1)=0

#define STRING 256

#endif
