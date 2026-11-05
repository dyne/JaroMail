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

/* $Id: rfc822.h,v 1.3 2005-10-29 14:48:11 roland Exp $ */

#ifndef rfc822_h
#define rfc822_h

#include <stddef.h>

/* possible values for RFC822Error */
enum
{
  ERR_MEMORY = 1,
  ERR_MISMATCH_PAREN,
  ERR_MISMATCH_QUOTE,
  ERR_BAD_ROUTE,
  ERR_BAD_ROUTE_ADDR,
  ERR_BAD_ADDR_SPEC
};

typedef struct address_t
{
#ifdef EXACT_ADDRESS
  char *val;		/* value of address as parsed */
#endif
  char *personal;	/* real name of address */
  char *mailbox;	/* mailbox and host address */
  int group;		/* group mailbox? */
  struct address_t *next;
}
ADDRESS;

void rfc822_free_address (ADDRESS **);
void rfc822_qualify (ADDRESS *, const char *);
ADDRESS *rfc822_parse_adrlist (ADDRESS *, const char *s);
ADDRESS *rfc822_cpy_adr (ADDRESS *addr);
ADDRESS *rfc822_cpy_adr_real (ADDRESS *addr);
ADDRESS *rfc822_append (ADDRESS **a, ADDRESS *b);
void rfc822_write_address (char *, size_t, ADDRESS *);
void rfc822_write_list (char *, size_t, ADDRESS *);
void rfc822_free_address (ADDRESS **addr);
void rfc822_cat (char *, size_t, const char *, const char *);

extern int RFC822Error;
extern const char *RFC822Errors[];

#define rfc822_error(x) RFC822Errors[x]
#define rfc822_new_address() calloc(1,sizeof(ADDRESS))

/* Strict bounded mailbox extraction for native filtering. The existing ADDRESS
 * API remains available to legacy helpers. Input is NUL-terminated at length,
 * with no earlier NUL; scratch_capacity must exceed length. No allocation,
 * global error state or display-name decoding is used. Quoted local parts and
 * quoted-pair bytes are preserved. Comments/phrases/groups never emit addresses.
 * The callback receives tentative mailboxes: discard all outputs on failure.
 * Return 0, RFC822_BOUNDED_SYNTAX, RFC822_BOUNDED_LIMIT or RFC822_BOUNDED_EMIT.
 */
enum { RFC822_BOUNDED_SYNTAX=1, RFC822_BOUNDED_LIMIT, RFC822_BOUNDED_EMIT };
typedef int (*rfc822_mailbox_emit)(void *, const char *, size_t);
int rfc822_parse_mailboxes(const char *, size_t, char *, size_t,
                          rfc822_mailbox_emit, void *);

#endif /* rfc822_h */
