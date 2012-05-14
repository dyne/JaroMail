/*
 * Copyright (C) 1996-8 Michael R. Elkins <me@cs.hmc.edu>
 * 
 *     This program is free software; you can redistribute it and/or modify
 *     it under the terms of the GNU General Public License as published by
 *     the Free Software Foundation; either version 2 of the License, or
 *     (at your option) any later version.
 * 
 *     This program is distributed in the hope that it will be useful,
 *     but WITHOUT ANY WARRANTY; without even the implied warranty of
 *     MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *     GNU General Public License for more details.
 * 
 *     You should have received a copy of the GNU General Public License
 *     along with this program; if not, write to the Free Software Foundation,
 *     Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301,, USA.
 */ 

/* $Id: rfc2047.c,v 1.5 2007-10-28 16:33:36 roland Exp $ */

#include <ctype.h>
#include <string.h>
#ifdef HAVE_ICONV
#include <iconv.h>
#include <errno.h>
#include <limits.h>
#endif

#include "rfc822.h"
#include "rfc2047.h"
#include "helpers.h"

enum
{
  ENCOTHER,
  ENC7BIT,
  ENC8BIT,
  ENCQUOTEDPRINTABLE,
  ENCBASE64,
  ENCBINARY
};

const char MimeSpecials[] = "@.,;<>[]\\\"()?/=";
const char *Charset = "iso-8859-15"; /* XXX - hack */


int Index_hex[128] = {
    -1,-1,-1,-1, -1,-1,-1,-1, -1,-1,-1,-1, -1,-1,-1,-1,
    -1,-1,-1,-1, -1,-1,-1,-1, -1,-1,-1,-1, -1,-1,-1,-1,
    -1,-1,-1,-1, -1,-1,-1,-1, -1,-1,-1,-1, -1,-1,-1,-1,
     0, 1, 2, 3,  4, 5, 6, 7,  8, 9,-1,-1, -1,-1,-1,-1,
    -1,10,11,12, 13,14,15,-1, -1,-1,-1,-1, -1,-1,-1,-1,
    -1,-1,-1,-1, -1,-1,-1,-1, -1,-1,-1,-1, -1,-1,-1,-1,
    -1,10,11,12, 13,14,15,-1, -1,-1,-1,-1, -1,-1,-1,-1,
    -1,-1,-1,-1, -1,-1,-1,-1, -1,-1,-1,-1, -1,-1,-1,-1
};

int Index_64[128] = {
    -1,-1,-1,-1, -1,-1,-1,-1, -1,-1,-1,-1, -1,-1,-1,-1,
    -1,-1,-1,-1, -1,-1,-1,-1, -1,-1,-1,-1, -1,-1,-1,-1,
    -1,-1,-1,-1, -1,-1,-1,-1, -1,-1,-1,62, -1,-1,-1,63,
    52,53,54,55, 56,57,58,59, 60,61,-1,-1, -1,-1,-1,-1,
    -1, 0, 1, 2,  3, 4, 5, 6,  7, 8, 9,10, 11,12,13,14,
    15,16,17,18, 19,20,21,22, 23,24,25,-1, -1,-1,-1,-1,
    -1,26,27,28, 29,30,31,32, 33,34,35,36, 37,38,39,40,
    41,42,43,44, 45,46,47,48, 49,50,51,-1, -1,-1,-1,-1
};


#define IsPrint(c) (isprint((unsigned char)(c)) || \
	((unsigned char)(c) >= 0xa0))

#define hexval(c) Index_hex[(unsigned int)(c)]
#define base64val(c) Index_64[(unsigned int)(c)]

static int rfc2047_decode_word (char *d, const char *s, size_t dlen)
{
  char *p = safe_strdup (s);
  char *pp = p;
  char *pd = d;
  size_t len = dlen;
  int enc = 0, filter = 0, count = 0, c1, c2, c3, c4;
#ifdef HAVE_ICONV
  char *fromcharset;
  iconv_t cd;
  size_t in;
#endif

  while ((pp = strtok (pp, "?")) != NULL)
  {
    count++;
    switch (count)
    {
      case 2:
	if (strcasecmp (pp, Charset) != 0)
	{
	  filter = 1;
#ifdef HAVE_ICONV
	  fromcharset = safe_strdup (pp);
#endif
	}
	break;
      case 3:
	if (toupper (*pp) == 'Q')
	  enc = ENCQUOTEDPRINTABLE;
	else if (toupper (*pp) == 'B')
	  enc = ENCBASE64;
	else
	  return (-1);
	break;
      case 4:
	if (enc == ENCQUOTEDPRINTABLE)
	{
	  while (*pp && len > 0)
	  {
	    if (*pp == '_')
	    {
	      *pd++ = ' ';
	      len--;
	    }
	    else if (*pp == '=')
	    {
	      *pd++ = (hexval(pp[1]) << 4) | hexval(pp[2]);
	      len--;
	      pp += 2;
	    }
	    else
	    {
	      *pd++ = *pp;
	      len--;
	    }
	    pp++;
	  }
	  *pd = 0;
	}
	else if (enc == ENCBASE64)
	{
	  while (*pp && len > 0)
	  {
	    c1 = base64val(pp[0]);
	    c2 = base64val(pp[1]);
	    *pd++ = (c1 << 2) | ((c2 >> 4) & 0x3);
	    if (--len == 0) break;
	    
	    if (pp[2] == '=') break;

	    c3 = base64val(pp[2]);
	    *pd++ = ((c2 & 0xf) << 4) | ((c3 >> 2) & 0xf);
	    if (--len == 0)
	      break;

	    if (pp[3] == '=')
	      break;

	    c4 = base64val(pp[3]);
	    *pd++ = ((c3 & 0x3) << 6) | c4;
	    if (--len == 0)
	      break;

	    pp += 4;
	  }
	  *pd = 0;
	}
	break;
    }
    pp = 0;
  }
  safe_free (&p);
  if (filter)
  {
#ifdef HAVE_ICONV
    if ((cd = iconv_open (Charset, fromcharset)) == (iconv_t)(-1))
    {
#endif
      pd = d;
      while (*pd)
      {
	if (!IsPrint (*pd))
	  *pd = '?';
	pd++;
      }
#ifdef HAVE_ICONV
    } else {
      p = safe_strdup (d);
      pp = p;
      in = strlen (d) + 1;
      pd = d;
      /* maximum available buffer length for converted string */
      len = dlen;
      while (*pd && iconv (cd, &pp, &in, &pd, &len) == (size_t)(-1))
      {
	if (errno == E2BIG)
	  break;

	*pd = '?';
	pp++;
	in--;
	pd++;
	len--;
      }
      iconv (cd, NULL, NULL, &pd, &len);
      iconv_close (cd);
      safe_free (&p);
    }
    safe_free (&fromcharset);
#endif
  }
  return (0);
}

/* try to decode anything that looks like a valid RFC2047 encoded
 * header field, ignoring RFC822 parsing rules
 */
void rfc2047_decode (char *d, const char *s, size_t dlen)
{
  const char *p, *q;
  size_t n;
  int found_encoded = 0;

  dlen--; /* save room for the terminal nul */

  while (*s && dlen > 0)
  {
    if ((p = strstr (s, "=?")) == NULL ||
	(q = strchr (p + 2, '?')) == NULL ||
	(q = strchr (q + 1, '?')) == NULL ||
	(q = strstr (q + 1, "?=")) == NULL)
    {
      /* no encoded words */
      if (d != s)
	strfcpy (d, s, dlen + 1);
      return;
    }

    if (p != s)
    {
      n = (size_t) (p - s);
      /* ignore spaces between encoded words */
      if (!found_encoded || strspn (s, " \t\r\n") != n)
      {
	if (n > dlen)
	  n = dlen;
	if (d != s)
	  memcpy (d, s, n);
	d += n;
	dlen -= n;
      }
    }

    rfc2047_decode_word (d, p, dlen);
    found_encoded = 1;
    s = q + 2;
    n = strlen (d);
    dlen -= n;
    d += n;
  }
  *d = 0;
}

void rfc2047_decode_adrlist (ADDRESS *a)
{
  while (a)
  {
    if (a->personal && strstr (a->personal, "=?") != NULL)
      rfc2047_decode (a->personal, a->personal, strlen (a->personal) + 1);
    a = a->next;
  }
}
