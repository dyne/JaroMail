/* Mixmaster version 3.0  --  (C) 1999 - 2006 Anonymizer Inc. and others.

   Mixmaster may be redistributed and modified under certain conditions.
   This software is distributed on an "AS IS" basis, WITHOUT WARRANTY OF
   ANY KIND, either express or implied. See the file COPYRIGHT for
   details.

   Parse RFC 822 headers
   $Id: rfc822.c 934 2006-06-24 13:40:39Z rabbi $ */


#include "mix3.h"

static int is_specials(int c);
static int is_qtext(char c);
static int is_ctext(char c);
static void wsc(BUFFER *in, BUFFER *xomment);
static int word(BUFFER *in, BUFFER *word, BUFFER *x);
static int atom(BUFFER *in, BUFFER *atom, BUFFER *x);
static int quoted_string(BUFFER *in, BUFFER *string, BUFFER *x);
static int comment(BUFFER *in, BUFFER *string);
static int local_part(BUFFER *in, BUFFER *addr, BUFFER *x);
static int domain(BUFFER *in, BUFFER *domain, BUFFER *x);
static int sub_domain(BUFFER *in, BUFFER *sub, BUFFER *x);
static int domain_ref(BUFFER *in, BUFFER *dom, BUFFER *x);
static int domain_literal(BUFFER *in, BUFFER *dom, BUFFER *x);
static int addr_spec(BUFFER *in, BUFFER *addr, BUFFER *x);
static int route_addr(BUFFER *in, BUFFER *addr, BUFFER *x);
static int phrase(BUFFER *in, BUFFER *phr, BUFFER *x);
static int mailbox(BUFFER *in, BUFFER *mailbox, BUFFER *name, BUFFER *x);
static int group(BUFFER *in, BUFFER *group, BUFFER *name, BUFFER *x);

static void backtrack(BUFFER *b, int len)
{
  if (b) {
    b->length = len;
    b->data[b->length] = '\0';
  }
}

/* white space and comments */
static void wsc(BUFFER *in, BUFFER *string)
{
  int c;

  for (;;) {
    c = buf_getc(in);
    if (c == -1)
      break;
    else if (c == '\n') {
      c = buf_getc(in);
      if (c != ' ' && c != '\t') {
	if (c != -1)
	  buf_ungetc(in), buf_ungetc(in);
	break;
      }
    } else {
      if (c != ' ' && c != '\t') {
	buf_ungetc(in);
	if (!comment(in, string))
	  break;
      }
    }
  }
}

/*          specials    =  "(" / ")" / "<" / ">" / "@"  ; Must be in quoted-
 *                      /  "," / ";" / ":" / "\" / <">  ;  string, to use
 *                      /  "." / "[" / "]"              ;  within a word.
 */

static int is_specials(int c)
{
  return (c == '(' || c == ')' || c == '<' || c == '>' || c == '@' ||
	  c == ',' || c == ';' || c == ':' || c == '\\' || c == '\"' ||
	  c == '.' || c == '[' || c == ']');
}

/*           qtext       =  <any CHAR excepting <">,     ; => may be folded
 *                          "\" & CR, and including
 *                          linear-white-space>
 */
static int is_qtext(char c)
{
  return (c != '\"' && c != '\\' && c != '\n');
}

/*           ctext       =  <any CHAR excluding "(",     ; => may be folded
 *                          ")", "\" & CR, & including
 *                          linear-white-space>
 */
static int is_ctext(char c)
{
  return (c != '(' && c != ')' && c != '\\' && c != '\n');
}

/*           word        =  atom / quoted-string
 */
static int word(BUFFER *in, BUFFER *word, BUFFER *x)
{
  return (atom(in, word, x) || quoted_string(in, word, x));
}

/*          atom        =  1*<any CHAR except specials, SPACE and CTLs>
 */
static int atom(BUFFER *in, BUFFER *atom, BUFFER *x)
{
  int c;

  buf_clear(atom);
  wsc(in, x);
  for (;;) {
    c = buf_getc(in);
    if (c == -1)
      break;
    else if (is_specials(c) || c == ' ' || c < 32 || c == 127) {
      buf_ungetc(in);
      break;
    } else
      buf_appendc(atom, c);
  }
  if (atom->length)
    wsc(in, x);
  return (atom->length);
}

/*           quoted-string = <"> *(qtext/quoted-pair) <">; Regular qtext or
 *                                                       ;   quoted chars.
 */
static int quoted_string(BUFFER *in, BUFFER *string, BUFFER *x)
{
  int ptr, xlen;
  int c;

  ptr = in->ptr, xlen = x ? x->length : 0;
  buf_clear(string);
  wsc(in, NULL);
  c = buf_getc(in);
  if (c == '\"') {
#if 0
    buf_appendc(string, c);
#endif
    for (;;) {
      c = buf_getc(in);
      if (c == -1)              /* catch unterminated quoted string */
	break;
      if (is_qtext(c))
	buf_appendc(string, c);
      else if (c == '\n') {
	c = buf_getc(in);
	if (c != ' ' && c != '\n')
	  break;
      } else if (c == '\\') {
	c = buf_getc(in);
	if (c == -1)
	  break;
	else
	  buf_appendc(string, c);
      } else if (c == '\"') {
#if 0
	buf_appendc(string, c);
#endif
	wsc(in, NULL);
	return (1);
      } else
	break;
    }
  }
  in->ptr = ptr, backtrack(x, xlen);
  return (0);
}

/*           comment     =  "(" *(ctext / quoted-pair / comment) ")"
 */
static int comment(BUFFER *in, BUFFER *string)
{
  int ptr, xlen;
  int separator = 0;
  int c;

  ptr = in->ptr;
  xlen = string ? string->length : 0;
  if (xlen)
    separator = 1;
  c = buf_getc(in);
  if (c == '(') {
    for (;;) {
      c = buf_getc(in);
      if (c == -1)
	return(1); /* unterminated comment, bail out */
      if (is_ctext(c)) {
	if (string != NULL) {
	  if (separator)
	    buf_appendc(string, ' '), separator = 0;
	  buf_appendc(string, c);
	}
      } else if (c == '\n') {
	c = buf_getc(in);
	if (c != ' ' && c != '\n')
	  break;
      } else if (c == '\\') {
	c = buf_getc(in);
	if (c != -1) {
	  if (string != NULL) {
	    if (separator)
	      buf_appendc(string, ' '), separator = 0;
	    buf_appendc(string, c);
	  }
	}
      } else if (c == ')')
	return (1);
      else {
	BUFFER *s;
	int o;

	s = buf_new();
	buf_ungetc(in);
	o = comment(in, s);
	if (o && string != NULL) {
	  if (separator)
	    buf_appendc(string, ' '), separator = 0;
	  buf_cat(string, s);
	}
	buf_free(s);
	if (!o)
	  break;
      }
    }
  }
  in->ptr = ptr;
  backtrack(string, xlen);
  return (0);
}

/*          local-part  =  word *("." word)             ; uninterpreted
 *                                                      ; case-preserved
 */
static int local_part(BUFFER *in, BUFFER *addr, BUFFER *x)
{
  BUFFER *w;
  int c;

  buf_clear(addr);
  if (!word(in, addr, x))
    return (0);
  w = buf_new();
  for (;;) {
    c = buf_getc(in);
    if (c == -1)
      break;
    if (c == '.' && (word(in, w, x)))
      buf_appendc(addr, '.'), buf_cat(addr, w);
    else {
      buf_ungetc(in);
      break;
    }
  }
  buf_free(w);
  return (addr->length);
}

/*           domain      =  sub-domain *("." sub-domain)
 */
static int domain(BUFFER *in, BUFFER *domain, BUFFER *x)
{
  BUFFER *sub;
  int c;

  if (!sub_domain(in, domain, x))
    return (0);
  sub = buf_new();
  for (;;) {
    c = buf_getc(in);
    if (c == -1)
      break;
    if (c == '.' && (sub_domain(in, sub, x)))
      buf_appendc(domain, '.'), buf_cat(domain, sub);
    else {
      buf_ungetc(in);
      break;
    }
  }
  buf_free(sub);
  return (domain->length);
}

/*             sub-domain  =  domain-ref / domain-literal
 */
static int sub_domain(BUFFER *in, BUFFER *sub, BUFFER *x)
{
  return (domain_ref(in, sub, x) || domain_literal(in, sub, x));
}

/*           domain-ref  =  atom                         ; symbolic reference
 */
static int domain_ref(BUFFER *in, BUFFER *d, BUFFER *x)
{
  return (atom(in, d, x));
}

/*           addr-spec   =  local-part "@" domain        ; global address
 */
static int addr_spec(BUFFER *in, BUFFER *addr, BUFFER *x)
{
  BUFFER *dom;
  int ptr, xlen;

  ptr = in->ptr, xlen = x ? x->length : 0;
  dom = buf_new();
  buf_clear(addr);
  if (local_part(in, addr, x) && buf_getc(in) == '@' && domain(in, dom, x))
    buf_appendc(addr, '@'), buf_cat(addr, dom);
  else
    buf_clear(addr), in->ptr = ptr, backtrack(x, xlen);
  buf_free(dom);
  return (addr->length);
}

/*           route-addr  =  "<" [route] addr-spec ">"
 */
static int route_addr(BUFFER *in, BUFFER *addr, BUFFER *x)
{
  int c;
  int ptr, xlen;

  ptr = in->ptr, xlen = x ? x->length : 0;
  c = buf_getc(in);
  if (c == -1)
    return (0);
  if (c != '<') {
    buf_ungetc(in);
    return (0);
  }
  if (addr_spec(in, addr, x) && buf_getc(in) == '>')
    return (1);
  in->ptr = ptr, backtrack(x, xlen);
  return (0);
}

/*           phrase      =  1*word                       ; Sequence of words
 */
static int phrase(BUFFER *in, BUFFER *phr, BUFFER *x)
{
  BUFFER *w;

  buf_clear(phr);
  w = buf_new();
  while (word(in, w, x)) {
    if (phr->length)
      buf_appendc(phr, ' ');
    buf_cat(phr, w);
  }
  buf_free(w);
  return (phr->length);
}

/*          mailbox     =  addr-spec                    ; simple address
 *                      /  [phrase] route-addr          ; name & addr-spec
 *                                                        (RFC 1123)
 */
static int mailbox(BUFFER *in, BUFFER *mailbox, BUFFER *name, BUFFER *x)
{
  int ptr, xlen, ret;

  buf_clear(name);
  if (addr_spec(in, mailbox, x))
    return (1);

  ptr = in->ptr, xlen = x ? x->length : 0;
  ret = phrase(in, name, x) && route_addr(in, mailbox, x);
  if (!ret) {
    in->ptr = ptr, backtrack(x, xlen);
    ret = route_addr(in, mailbox, x);
  }

  return (ret);
}

/*          address     =  mailbox                      ; one addressee
 *                      /  group                        ; named list
 */
static int address(BUFFER *in, BUFFER *address, BUFFER *name, BUFFER *x)
{
  return (mailbox(in, address, name, x) || group(in, address, name, x));
}

/*          group       =  phrase ":" [#mailbox] ";"
 */
static int group(BUFFER *in, BUFFER *group, BUFFER *name, BUFFER *x)
{
  BUFFER *addr, *tmp;
  int ptr, xlen, ret = 0;

  ptr = in->ptr, xlen = x ? x->length : 0;
  addr = buf_new();
  tmp = buf_new();
  buf_clear(group);
  if (phrase(in, name, x) && buf_getc(in) == ':') {
    while (mailbox(in, addr, tmp, x))
      buf_cat(group, addr), buf_nl(group);
    ret = buf_getc(in) == ';';
  }
  if (!ret)
    in->ptr = ptr, backtrack(x, xlen);
  buf_free(addr);
  buf_free(tmp);
  return (ret);
}

/*         domain-literal =  "[" *(dtext / quoted-pair) "]"
 */
static int domain_literal(BUFFER *in, BUFFER *dom, BUFFER *x)
{
  return 0;			/* XXX */
}

/* local address without `@' is not specified in RFC 822 */

/* local_addr = "<" atom ">" */
static int local_addr(BUFFER *in, BUFFER *addr, BUFFER *x)
{
  int c;
  int ptr, xlen;

  ptr = in->ptr, xlen = x ? x->length : 0;
  c = buf_getc(in);
  if (c == -1)
    return (0);
  if (c != '<') {
    buf_ungetc(in);
    return (0);
  }
  if (atom(in, addr, x) && buf_getc(in) == '>')
    return (1);
  in->ptr = ptr, backtrack(x, xlen);
  return (0);
}

static int localaddress(BUFFER *in, BUFFER *address, BUFFER *name, BUFFER *x)
{
  int ptr, xlen;

  buf_clear(name);
  if (local_addr(in, address, x))
    return (1);
  ptr = in->ptr, xlen = x ? x->length : 0;
  if (phrase(in, name, x) && local_addr(in, address, x))
    return (1);
  in->ptr = ptr, backtrack(x, xlen);
  buf_clear(name);
  return (atom(in, address, x));
}

void rfc822_addr(BUFFER *destination, BUFFER *list)
{
  BUFFER *addr, *name;

  addr = buf_new();
  name = buf_new();

  for (;;) {
    if (!address(destination, addr, name, NULL) &&
	!localaddress(destination, addr, name, NULL))
      break;
    buf_cat(list, addr);
    buf_nl(list);
    if (buf_getc(destination) != ',')
      break;
  }
  buf_free(addr);
  buf_free(name);
}

void rfc822_name(BUFFER *line, BUFFER *name)
{
  BUFFER *addr, *comment;
  int ret;

  addr = buf_new();
  comment = buf_new();
  ret = address(line, addr, name, comment);
  if (ret == 0)
    ret = localaddress(line, addr, name, comment);
  if (ret) {
    if (name->length == 0)
      buf_set(name, comment);
    if (name->length == 0)
      buf_set(name, addr);
  }
  if (ret == 0)
    buf_set(name, line);
  buf_free(addr);
  buf_free(comment);
}

/* MIME extensions. RFC 2045 */

/*    tspecials :=  "(" / ")" / "<" / ">" / "@" /
 *                  "," / ";" / ":" / "\" / <">
 *                  "/" / "[" / "]" / "?" / "="
 */

static int is_tspecials(int c)
{
  return (c == '(' || c == ')' || c == '<' || c == '>' || c == '@' ||
	  c == ',' || c == ';' || c == ':' || c == '\\' || c == '\"' ||
	  c == '/' || c == '[' || c == ']' || c == '?' || c == '=');
}

/* token := 1*<any (US-ASCII) CHAR except SPACE, CTLs,
 *                or tspecials>
 */
static int token(BUFFER *in, BUFFER *token, BUFFER *x)
{
  int c;

  buf_clear(token);
  wsc(in, x);
  for (;;) {
    c = buf_getc(in);
    if (c == -1)
      break;
    else if (is_tspecials(c) || c == ' ' || c < 32 || c == 127) {
      buf_ungetc(in);
      break;
    } else
      buf_appendc(token, c);
  }
  if (token->length)
    wsc(in, x);
  return (token->length);
}

/*   value := token / quoted-string
 */

static int value(BUFFER *in, BUFFER *value, BUFFER *x)
{
  return (token(in, value, x) || quoted_string(in, value, x));
}

/*   parameter := attribute "=" value
 */

static int parameter(BUFFER *in, BUFFER *attribute, BUFFER *val, BUFFER *x)
{
  int ptr;
  ptr = in->ptr;
  token(in, attribute, x);
  if (buf_getc(in) != '=') {
    in->ptr = ptr;
    return(0);
  }
  return(value(in, val, x));
}

/* get type */
int get_type(BUFFER *content, BUFFER *type, BUFFER *subtype)
{
  token(content, type, NULL);
  if (buf_getc(content) == '/')
    return (token(content, subtype, NULL));
  buf_ungetc(content);
  buf_clear(type);
  return (0);
}

/* get parameter value */
void get_parameter(BUFFER *content, char *attribute, BUFFER *value)
{
  BUFFER *tok;
  tok = buf_new();
  buf_clear(value);

  get_type(content, tok, tok);
    for (;;) {
      if (buf_getc(content) != ';')
	break;
      if (parameter(content, tok, value, NULL) &&
	  strieq(attribute, tok->data))
	break; /* found */
      buf_clear(value);
    }
  buf_free(tok);
}
