/*
 * The functions in this file have completely been stolen
 * from the Mutt mail user agent, with some slight
 * modifications by Thomas Roessler <roessler@guug.de> to
 * use them in the "little brother database".
 *
 * The following copyright notice applies:
 *
 * Copyright (C) 1996-8 Michael R. Elkins <me@cs.hmc.edu>
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

/* $Id: helpers.c,v 1.5 2005-10-29 14:48:08 roland Exp $ */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "helpers.h"


void *safe_calloc (size_t nmemb, size_t size)
{
  void *p;

  if (!nmemb || !size)
    return NULL;
  if (!(p = calloc (nmemb, size)))
  {
    perror ("Out of memory");
    sleep (1);
    exit (1);
  }
  return p;
}

void *safe_malloc (unsigned int siz)
{
  void *p;

  if (siz == 0)
    return 0;
  if ((p = (void *) malloc (siz)) == 0)
  {
    perror ("Out of memory!");
    sleep (1);
    exit (1);
  }
  return (p);
}

void safe_realloc (void **p, size_t siz)
{
  void *r;

  if (siz == 0)
  {
    if (*p)
    {
      free (*p);
      *p = NULL;
    }
    return;
  }

  if (*p)
    r = (void *) realloc (*p, siz);
  else
  {
    /* realloc(NULL, nbytes) doesn't seem to work under SunOS 4.1.x */
    r = (void *) malloc (siz);
  }

  if (!r)
  {
    perror ("Out of memory!");
    sleep (1);
    exit (1);
  }

  *p = r;
}

void safe_free (void *ptr)
{ 
  void **p = (void **)ptr;
  if (*p)
  { 
    free (*p);
    *p = 0;
  }
}

char *safe_strdup (const char *s)
{
  char *p;
  size_t l;

  if (!s || !*s) return 0;
  l = strlen (s) + 1;
  p = (char *)safe_malloc (l);
  memcpy (p, s, l);
  return (p);
}

