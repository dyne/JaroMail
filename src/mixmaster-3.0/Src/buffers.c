/* Mixmaster version 3.0  --  (C) 1999 - 2006 Anonymizer Inc. and others.

   Mixmaster may be redistributed and modified under certain conditions.
   This software is distributed on an "AS IS" basis, WITHOUT WARRANTY OF
   ANY KIND, either express or implied. See the file COPYRIGHT for
   details.

   Dynamically allocated buffers
   $Id: buffers.c 934 2006-06-24 13:40:39Z rabbi $ */


#include "mix3.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdarg.h>
#ifdef WIN32
#include <io.h>
#endif /* WIN32 */
#include <assert.h>
#ifdef POSIX
#include <unistd.h>
#endif /* POSIX */

static void fail(void)
{
  errlog(ERRORMSG, "Out of memory!\n");
  abort();
}

#define space 128		/* allocate some additional space */

static void alloc(BUFFER *b)
{
  b->data = malloc(space);
  if (b->data == NULL)
    fail();
  b->data[0] = 0;
  b->size = space;
}

#undef buf_new /* DEBUG */
BUFFER *buf_new(void)
{
  BUFFER *b;

  b = malloc(sizeof(BUFFER));
  if (b == NULL)
    fail();
  alloc(b);
  b->length = 0;
  b->ptr = 0;
  b->sensitive = 0;

  return (b);
}

#ifdef DEBUG
static void sanity_check(BUFFER *b)
{
  assert(b != NULL);
  assert(b->size > 0);
  assert(b->data != NULL);
  assert(b->length >= 0 && b->length < b->size);
  assert(b->ptr >= 0 && b->ptr <= b->length);
}
#else /* not DEBUG */
#define sanity_check(arg)
#endif /* else not DEBUG */

int buf_reset(BUFFER *buffer)
{
  sanity_check(buffer);

  buffer->length = 0;
  buffer->ptr = 0;
  if (buffer->sensitive)
    memset(buffer->data, 0, buffer->size);
  free(buffer->data);
  alloc(buffer);
  return (0);
}

int buf_free(BUFFER *buffer)
{
  int err = 0;

  if (buffer->sensitive)
    memset(buffer->data, 0, buffer->size);
  free(buffer->data);
  free(buffer);
  return (err);
}

int buf_clear(BUFFER *buffer)
{
  sanity_check(buffer);
  buffer->data[0] = '\0';
  buffer->length = 0;
  buffer->ptr = 0;
  return (0);
}

int buf_append(BUFFER *buffer, byte *msg, int len)
{
  assert(len >= 0);
  sanity_check(buffer);

  if (buffer->length + len >= buffer->size) {
    register byte *new;
    register long newsize;

    newsize = 2 * buffer->length;	/* double buffer size */
    if (newsize < buffer->length + len + space)
      newsize = buffer->length + len + space;
    new = malloc(newsize);
    if (new == NULL)
      fail();
    memcpy(new, buffer->data, buffer->length);
    if (buffer->sensitive)
      memset(buffer->data, 0, buffer->size);
    free(buffer->data);
    buffer->data = new;
    buffer->size = newsize;
  }
  if (msg != NULL)
    memcpy(buffer->data + buffer->length, msg, len);
  buffer->length += len;

  buffer->data[buffer->length] = 0;
  return (0);
}

int buf_appendrnd(BUFFER *to, int n)
{
  buf_append(to, NULL, n);
  rnd_bytes(to->data + to->length - n, n);
  return (0);
}

int buf_appendzero(BUFFER *to, int n)
{
  buf_append(to, NULL, n);
  memset(to->data + to->length - n, 0, n);
  return (0);
}

int buf_setrnd(BUFFER *b, int n)
{
  buf_prepare(b, n);
  rnd_bytes(b->data, n);
  return (0);
}

int buf_cat(BUFFER *to, BUFFER *from)
{
  return (buf_append(to, from->data, from->length));
}

int buf_set(BUFFER *to, BUFFER *from)
{
  buf_reset(to);
  return (buf_cat(to, from));
}

int buf_appendc(BUFFER *to, byte b)
{
  return (buf_append(to, &b, 1));
}

int buf_rest(BUFFER *to, BUFFER *from)
{
  assert(from != to);
  return (buf_append(to, from->data + from->ptr, from->length - from->ptr));
}

int buf_appends(BUFFER *buffer, char *s)
{
  return (buf_append(buffer, s, strlen(s)));
}

int buf_sets(BUFFER *buffer, char *s)
{
  buf_clear(buffer);
  return (buf_appends(buffer, s));
}

int buf_setc(BUFFER *buffer, byte c)
{
  buf_clear(buffer);
  return (buf_appendc(buffer, c));
}

int buf_nl(BUFFER *b)
{
  return (buf_append(b, "\n", 1));
}

int buf_vappendf(BUFFER *b, char *fmt, va_list args)
{
  for (; *fmt != '\0'; fmt++)
    if (*fmt == '%') {
      int lzero = 0;
      int longvar = 0;
      int len = 0;

      for (;;) {
	if (*++fmt == '\0')
	  return (-1);
	if (*fmt == '%') {
	  buf_appendc(b, '%');
	  break;
	} else if (*fmt == 'b') {	/* extension of printf */
	  buf_cat(b, va_arg(args, BUFFER *));

	  break;
	} else if (*fmt == 'c') {
	  buf_appendc(b, va_arg(args, int));

	  break;
	} else if (*fmt == 's') {
	  buf_appends(b, va_arg(args, char *));

	  break;
	} else if (*fmt == 'd' || *fmt == 'X') {
	  int base, val, sign = 0;
	  BUFFER *out;

	  out = buf_new();
	  base = *fmt == 'd' ? 10 : 16;
	  if (longvar)
	    val = va_arg(args, long);

	  else
	    val = va_arg(args, int);

	  if (val < 0)
	    sign = 1, val = -val;
	  do {
	    if (val % base > 9)
	      buf_appendc(out, val % base - 10 + 'A');
	    else
	      buf_appendc(out, val % base + '0');
	    val /= base;
	  } while (val > 0);
	  if (sign)
	    len--;
	  while (len-- > out->length)
	    buf_appendc(b, lzero ? '0' : ' ');
	  if (sign)
	    buf_appendc(b, '-');
	  for (len = out->length - 1; len >= 0; len--)
	    buf_appendc(b, out->data[len]);
	  buf_free(out);
	  break;
	} else if (*fmt == 'l')
	  longvar = 1;
	else if (*fmt == '0' && len == 0)
	  lzero = 1;
	else if (isdigit(*fmt))
	  len = len * 10 + *fmt - '0';
	else
	  assert(0);
      }
    } else
      buf_appendc(b, *fmt);
  return (0);
}

int buf_setf(BUFFER *b, char *fmt, ...)
{
  va_list args;
  int ret;

  va_start(args, fmt);
  buf_clear(b);
  ret = buf_vappendf(b, fmt, args);
  va_end(args);
  return (ret);
}

int buf_appendf(BUFFER *b, char *fmt, ...)
{
  va_list args;
  int ret;

  va_start(args, fmt);
  ret = buf_vappendf(b, fmt, args);
  va_end(args);
  return (ret);
}

int buf_pad(BUFFER *buffer, int size)
{
  assert(size - buffer->length >= 0);
  buf_appendrnd(buffer, size - buffer->length);
  return (0);
}

int buf_prepare(BUFFER *buffer, int size)
{
  buf_clear(buffer);
  buf_append(buffer, NULL, size);
  return (0);
}

int buf_read(BUFFER *outmsg, FILE *infile)
{
  char buf[BUFSIZE];
  int n;
  int err = -1;

  assert(infile != NULL);
  sanity_check(outmsg);

  for (;;) {
    n = fread(buf, 1, BUFSIZE, infile);
    if (n > 0)
      err = 0;
    buf_append(outmsg, buf, n);
    if (n < BUFSIZE)
      break;
#ifdef BUFFER_MAX
    if (outmsg->length > BUFFER_MAX) {
      errlog(ERRORMSG, "Message file too large. Giving up.\n");
      return (1);
    }
#endif /* BUFFER_MAX */
  }

#ifdef WIN32
  if (isatty(fileno(infile)) && isatty(fileno(stdout)))
    printf("\n");
#endif /* WIN32 */

  return (err);
}

int buf_write(BUFFER *buffer, FILE *out)
{
  assert(out != NULL);
  sanity_check(buffer);

  return (fwrite(buffer->data, 1, buffer->length, out) == buffer->length
	  ? 0 : -1);
}

int buf_write_sync(BUFFER *buffer, FILE *out)
{
    int ret = 0;

    if (buf_write(buffer, out) == -1) {
      fclose(out);
      return -1;
    }

    if (fflush(out) != 0)
      ret = -1;

#ifdef POSIX
    /* dir entry not synced */
    if (fsync(fileno(out)) != 0)
      ret = -1;
#endif /* POSIX */

    if (fclose(out) != 0)
      ret = -1;

    return ret;
}

int buf_rewind(BUFFER *buffer)
{
  sanity_check(buffer);

  buffer->ptr = 0;
  return (0);
}

int buf_get(BUFFER *buffer, BUFFER *to, int n)
{
  sanity_check(buffer);
  sanity_check(to);
  assert(n > 0);
  assert(buffer != to);

  buf_clear(to);
  if (buffer->length - buffer->ptr < n)
    return (-1);
  buf_append(to, buffer->data + buffer->ptr, n);
  buffer->ptr += n;
  return (0);
}

int buf_getc(BUFFER *buffer)
{
  sanity_check(buffer);
  if (buffer->ptr == buffer->length)
    return (-1);
  else
    return (buffer->data[buffer->ptr++]);
}

void buf_ungetc(BUFFER *buffer)
{
  sanity_check(buffer);
  if (buffer->ptr > 0)
    buffer->ptr--;
}

int buf_getline(BUFFER *buffer, BUFFER *line)
{
  register int i;
  int ret = 0;
  int nl = 0;

  sanity_check(buffer);

  if (line != NULL)
    buf_clear(line);
  if (buffer->ptr == buffer->length)
    return (-1);

  for (i = buffer->ptr; i < buffer->length; i++) {
    if (buffer->data[i] > '\r')
      continue;
    if (buffer->data[i] == '\0' || buffer->data[i] == '\n') {
      nl = 1;
      break;
    }
    if (buffer->data[i] == '\r' &&
	i + 1 <= buffer->length && buffer->data[i + 1] == '\n') {
      nl = 2;
      break;
    }
  }

  if (line != NULL)
    buf_append(line, buffer->data + buffer->ptr, i - buffer->ptr);
  if (i == buffer->ptr)
    ret = 1;
  buffer->ptr = i + nl;

  return (ret);
}

int buf_chop(BUFFER *b)
{
  int i;

  sanity_check(b);

  for (i = 0; i < b->length; i++)
    if (b->data[i] == '\0' || b->data[i] == '\n' ||
	(b->data[i] == '\r' && i + 1 < b->length &&
	 b->data[i + 1] == '\n'))
      b->length = i;
  b->data[b->length] = 0;
  return (0);
}

int buf_isheader(BUFFER *buffer)
{
  BUFFER *line;
  long p;
  int i;
  int err;
  int ret;

  line = buf_new();
  p = buffer->ptr;
  ret = 0;
  err = buf_getline(buffer, line);
  if (err != 0)
    goto end;

  for (i = 0; i < line->length; i++) {
    if (line->data[i] == ' ' || line->data[i] == '\t')
      break;
    if (line->data[i] == ':') {
      ret = 1;
      break;
    }
  }

end:
  buffer->ptr = p;
  buf_free(line);
  return(ret);
}

int buf_getheader(BUFFER *buffer, BUFFER *field, BUFFER *content)
{
  BUFFER *line;
  long p;
  int i;
  int err;

  line = buf_new();
  buf_reset(field);
  buf_reset(content);

  err = buf_getline(buffer, line);
  if (err != 0)
    goto end;

  err = -1;
  for (i = 0; i < line->length; i++) {
    if (line->data[i] == ' ' || line->data[i] == '\t')
      break;
    if (line->data[i] == ':') {
      err = 0;
      break;
    }
  }
  if (err == -1 && bufileft(line, "From ")) {
    buf_set(field, line);
    err = 0;
  }
  if (err == -1) {
    /* badly formatted message -- try to process anyway */
    buf_sets(field, "X-Invalid");
    buf_set(content, line);
    err = 0;
    goto end;
  }
  buf_append(field, line->data, i);
  if (i < line->length)
    i++;
  else
    err = 1;
  while (i < line->length &&
	 (line->data[i] == ' ' || line->data[i] == '\t'))
    i++;
  buf_append(content, line->data + i, line->length - i);

  for (;;) {
    p = buffer->ptr;
    if (buf_getline(buffer, line) != 0)
      break;
    if (line->data[0] != ' ' && line->data[0] != '\t')
      break;
#if 1
    buf_nl(content);
    buf_cat(content, line);
#else /* end of 1 */
    buf_appendc(content, ' ');
    buf_appends(content, line->data + 1);
#endif /* else if not 1 */
  }
  buffer->ptr = p;
end:
  buf_free(line);
  return (err);
}

int buf_appendheader(BUFFER *buffer, BUFFER *field, BUFFER *content)
{
  return buf_appendf(buffer, "%b: %b\n", field, content);
}

int buf_lookahead(BUFFER *buffer, BUFFER *line)
{
  long p;
  int e;

  p = buffer->ptr;
  e = buf_getline(buffer, line);
  buffer->ptr = p;
  return (e);
}

int buf_eq(BUFFER *b1, BUFFER *b2)
{
  sanity_check(b1);
  sanity_check(b2);

  if (b1->length != b2->length)
    return (0);
  if (b1->length > 0 && memcmp(b1->data, b2->data, b1->length) != 0)
    return (0);
  return (1);
}

int buf_ieq(BUFFER *b1, BUFFER *b2)
{
  int i;
  sanity_check(b1);
  sanity_check(b2);

  if (b1->length != b2->length)
    return (0);
  for (i = 0; i < b1->length; i++)
    if (tolower(b1->data[i]) != tolower(b2->data[i]))
      return (0);
  return (1);
}

void buf_move(BUFFER *dest, BUFFER *src)
     /* equivalent to buf_set(dest, src); buf_reset(src); */
{
  BUFFER temp;
  sanity_check(src);
  buf_reset(dest);
  temp.data = dest->data, temp.size = dest->size;
  dest->data = src->data, dest->size = src->size, dest->length = src->length;
  src->data = temp.data, src->size = temp.size, src->length = 0;
  dest->ptr = 0, src->ptr = 0;
}

int buf_appendl(BUFFER *b, long l)
{
  buf_appendc(b, (l >> 24) & 255);
  buf_appendc(b, (l >> 16) & 255);
  buf_appendc(b, (l >> 8) & 255);
  buf_appendc(b, l & 255);
  return (0);
}

int buf_appendl_lo(BUFFER *b, long l)
{
  buf_appendc(b, l & 255);
  buf_appendc(b, (l >> 8) & 255);
  buf_appendc(b, (l >> 16) & 255);
  buf_appendc(b, (l >> 24) & 255);
  return (0);
}

long buf_getl(BUFFER *b)
{
  long l;

  if (b->ptr + 4 > b->length)
    return (-1);
  l = buf_getc(b) << 24;
  l += buf_getc(b) << 16;
  l += buf_getc(b) << 8;
  l += buf_getc(b);
  return (l);
}

long buf_getl_lo(BUFFER *b)
{
  long l;

  if (b->ptr + 4 > b->length)
    return (-1);
  l = buf_getc(b);
  l += buf_getc(b) << 8;
  l += buf_getc(b) << 16;
  l += buf_getc(b) << 24;

  return (l);
}

int buf_appendi(BUFFER *b, int i)
{
  buf_appendc(b, (i >> 8) & 255);
  buf_appendc(b, i & 255);
  return (0);
}

int buf_appendi_lo(BUFFER *b, int i)
{
  buf_appendc(b, i & 255);
  buf_appendc(b, (i >> 8) & 255);
  return (0);
}

int buf_geti(BUFFER *b)
{
  int i;

  if (b->ptr + 2 > b->length)
    return (-1);
  i = buf_getc(b) << 8;
  i += buf_getc(b);
  return (i);
}

int buf_geti_lo(BUFFER *b)
{
  int i;

  if (b->ptr + 2 > b->length)
    return (-1);
  i = buf_getc(b);
  i += buf_getc(b) << 8;
  return (i);
}

int buf_getb(BUFFER *b, BUFFER *p)
{
  long l;

  l = buf_getl(b);
  return (buf_get(b, p, l));
}

int buf_appendb(BUFFER *b, BUFFER *p)
{
  long l;

  l = p->length;
  buf_appendl(b, l);
  return (buf_cat(b, p));
}

int bufleft(BUFFER *b, char *k) {
  return(strleft(b->data, k));
}

int bufileft(BUFFER *b, char *k) {
  return(strileft(b->data, k));
}

int buffind(BUFFER *b, char *k) {
  return(strfind(b->data, k));
}

int bufifind(BUFFER *b, char *k) {
  return(strifind(b->data, k));
}

int bufiright(BUFFER *b, char *k) {
  int l = strlen(k);
  if (l <= b->length)
    return (strieq(b->data + b->length - l, k));
  return(0);
}

int bufeq(BUFFER *b, char *k) {
  return(streq(b->data, k));
}

int bufieq(BUFFER *b, char *k) {
  return(strieq(b->data, k));
}

/* void buf_cut_out(BUFFER *buffer, BUFFER *cut_out, BUFFER *rest,
 *                  int from, int len);
 *
 * Cut a chunk out of the buffer.
 *
 * Starting with from, len bytes are cut out of buffer. The chunk
 * of text that has been cut out is returned in cut_out, the
 * remainings of buffer are returned in rest.
 *
 * This function was added by Gerd Beuster. (gb@uni-koblenz.de)
 */

void buf_cut_out(BUFFER *buffer, BUFFER *cut_out, BUFFER *rest,
		 int from, int len){

  assert((len >= 0) && (from >= 0));
  assert(from+len <= buffer->length);
  assert(cut_out != rest);

  buffer->ptr = 0;
  if(from > 0)
    buf_get(buffer, rest, from);
  buf_get(buffer, cut_out, len);
  buf_rest(rest, buffer);
}


#ifdef DEBUG
/* check for memory leaks */
#undef malloc
#undef free
void *malloc(size_t size);
void free(void *ptr);
#define max 100000
static int n=0;
static void *m[max];
static char *mm[max];

void mix3_leaks(void) {
  int i, ok=1;
  for (i = 0; i < n; i++)
    if (m[i]) {
      fprintf(stderr, "Leak [%d] %s\n", i, mm[i]);
      ok = 0;
    }
  if (ok)
    fprintf(stderr, "No memory leaks.\n");
}

void *mix3_malloc(size_t size) {
  void *ptr;
  if (n == 0)
    atexit(mix3_leaks);
  ptr = malloc(size);
  if (n >= max) abort();
  m[n++] = ptr;
  mm[n] = "?";
  return(ptr);
}

void mix3_free(void *ptr) {
  int i;
  for (i = 0; i < n; i++)
    if (m[i] == ptr) {
      m[i] = 0;
      break;
    }
  free(ptr);
}

BUFFER *mix3_bufnew(char *file, int line, char *func) {
  mm[n] = malloc(strlen(file) + strlen(func) + 15);
  sprintf(mm[n], "in %s %s:%d", func, file, line);
  return(buf_new());
}
#endif /* DEBUG */
