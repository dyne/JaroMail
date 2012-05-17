/*
 *  Copyright (C) 1998-2000  Thomas Roessler <roessler@guug.de>
 *  Copyright (C) 2000       Roland Rosenfeld <roland@spinnaker.de>
 *
 *  This program is free software; you can redistribute
 *  it and/or modify it under the terms of the GNU
 *  General Public License as published by the Free
 *  Software Foundation; either version 2 of the License,
 *  or (at your option) any later version.
 *
 *  This program is distributed in the hope that it will
 *  be useful, but WITHOUT ANY WARRANTY; without even the
 *  implied warranty of MERCHANTABILITY or FITNESS FOR A
 *  PARTICULAR PURPOSE.  See the GNU General Public
 *  License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software Foundation,
 *  Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301,, USA.
 *
 */

/* $Id: fetchaddr.c,v 1.23 2007-10-28 16:33:35 roland Exp $ */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "helpers.h"
#include "rfc822.h"
#include "rfc2047.h"

#define MAXHDRS 21

struct header
{
  char *tag;
  char *value;
  size_t len;
  size_t taglen;
};

struct header hdr[MAXHDRS] =
{
  { "to:",		NULL, 0,  3 },
  { "from:",		NULL, 0,  5 },
  { "cc:",		NULL, 0,  3 },
  { "resent-from:",	NULL, 0, 12 },
  { "resent-to:",	NULL, 0, 10 },
  { NULL, 		NULL, 0,  0 }
};

void chop(struct header *cur)
{
  if(cur->len && cur->value[cur->len - 1] == '\n')
    cur->value[--cur->len] = '\0';
}

int writeout(struct header *h, const char *datefmt, 
	     unsigned char create_real_name)
{
  int rv = 0;
  ADDRESS *addr, *p;
  time_t timep;
  char timebuf[256];
  char *c;

  if(!h->value)
    return 0;
  
  addr = rfc822_parse_adrlist(NULL, h->value);
  time(&timep);

  rfc2047_decode_adrlist(addr);
  for(p = addr; p; p = p->next)
  {
    if(create_real_name == 1 
       && (!p->personal || !*p->personal)
       && p->mailbox)
    {
      if(p->personal)
	FREE(p->personal);
      p->personal = safe_strdup(p->mailbox);
      c=strchr(p->personal, '@');
      if (c)
	*c='\0';
    }
    if(!p->group && p->mailbox && *p->mailbox && p->personal)
    {
      if(p->personal && strlen(p->personal) > 30)
	strcpy(p->personal + 27, "...");

      if ((c=strchr(p->mailbox,'@')))
	for(c++; *c; c++)
	  *c=tolower(*c);

      strftime(timebuf, sizeof(timebuf), datefmt, localtime(&timep));
      printf("%s\t%s\t%s\n", p->mailbox, p->personal && *p->personal ? 
	     p->personal : "no realname given", timebuf);

      rv = 1;
    }
  }

  rfc822_free_address(&addr);

  return rv;
}
  
int main(int argc, char* argv[])
{
  char buff[2048];
  char *t;
  int i, rv;
  int partial = 0;
  struct header *cur_hdr = NULL;
  char *datefmt = NULL;
  char *headerlist = NULL;
  char *fieldname, *next;
  char create_real_name = 0;
#ifdef HAVE_ICONV
  const char **charsetptr = &Charset;
#endif

  /* process command line arguments: */
  if (argc > 1) {
    i = 1;
    while (i < argc) {
      if (!strcmp (argv[i], "-d") && i+1 < argc) {
	datefmt = argv[++i];
      } else if (!strcmp (argv[i], "-x") && i+1 < argc) {
	headerlist = argv[++i];
#ifdef HAVE_ICONV
      } else if (!strcmp (argv[i], "-c") && i+1 < argc) {
	*charsetptr = argv[++i];
#endif
      } else if (!strcmp (argv[i], "-a")) {
	create_real_name = 1;
      } else {
	fprintf (stderr, "%s: `%s' wrong parameter\n", argv[0], argv[i]);
      }
      i++;
    }
  }

  if (!datefmt) 
    datefmt = safe_strdup("%Y-%m-%d %H:%M");

  if (headerlist && strlen (headerlist) > 0 ) {
    fieldname = headerlist;
    i = 0;
    while ( i < MAXHDRS-1 && (next = strchr (fieldname, ':'))) {
      hdr[i].tag = safe_malloc (next - fieldname + 2);
      strncpy (hdr[i].tag, fieldname, next - fieldname);
      hdr[i].tag[next - fieldname] = ':';
      hdr[i].tag[next - fieldname + 1] = '\0';
      hdr[i].taglen = next - fieldname + 1;
      fieldname = next+1;
      i++;
    }
    
    if (i < MAXHDRS-1 && *fieldname != '\0') {
      hdr[i].tag = safe_malloc (strlen (fieldname) + 2);
      strncpy (hdr[i].tag, fieldname, strlen (fieldname));
      hdr[i].tag[strlen (fieldname)] = ':';
      hdr[i].tag[strlen (fieldname) + 1] = '\0';
      hdr[i].taglen = strlen (fieldname) + 1;
      i++;
    }
    
    hdr[i].tag = NULL;	/* end of hdr list */
  }

  while(fgets(buff, sizeof(buff), stdin))
  {
    
    if(!partial && *buff == '\n')
      break;

    if(cur_hdr && (partial || *buff == ' ' || *buff == '\t'))
    {
      size_t nl = cur_hdr->len + strlen(buff);
      
      safe_realloc((void **) &cur_hdr->value, nl + 1);
      strcpy(cur_hdr->value + cur_hdr->len, buff);
      cur_hdr->len = nl;
      chop(cur_hdr);
    }
    else if(!partial && *buff != ' ' && *buff != '\t')
    {
      cur_hdr = NULL;

      for(i = 0; hdr[i].tag; i++)
      {
	if(!strncasecmp(buff, hdr[i].tag, hdr[i].taglen))
	{
	  cur_hdr = &hdr[i];
	  break;
	}
      }
      
      if(cur_hdr)
      {
	safe_free(&cur_hdr->value);
	cur_hdr->value = safe_strdup(buff + cur_hdr->taglen);
	cur_hdr->len = strlen(cur_hdr->value);
	chop(cur_hdr);
      }
    }
    
    if(!(t = strchr(buff, '\n')))
      partial = 1;
    else
      partial = 0;
  }

  for(rv = 0, i = 0; hdr[i].tag; i++)
  {
    if(hdr[i].value)
      rv = writeout(&hdr[i], datefmt, create_real_name) || rv;
  }

  return (rv ? 0 : 1);
  
}
