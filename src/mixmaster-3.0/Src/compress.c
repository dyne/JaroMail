/* Mixmaster version 3.0  --  (C) 1999 - 2006 Anonymizer Inc. and others.

   Mixmaster may be redistributed and modified under certain conditions.
   This software is distributed on an "AS IS" basis, WITHOUT WARRANTY OF
   ANY KIND, either express or implied. See the file COPYRIGHT for
   details.

   Buffer compression (interface to zlib)
   $Id: compress.c 934 2006-06-24 13:40:39Z rabbi $ */


#include "mix3.h"
#include <stdio.h>
#include <assert.h>

static byte gz_magic[2] =
{0x1f, 0x8b};			/* gzip magic header */

/* gzip flag byte */
#define ASCII_FLAG   0x01
#define HEAD_CRC     0x02
#define EXTRA_FIELD  0x04
#define ORIG_NAME    0x08
#define COMMENT      0x10
#define RESERVED     0xE0
#define Z_DEFLATED   8

#ifdef USE_ZLIB
#include "zlib.h"

int buf_unzip(BUFFER *in, int type)
{
  BUFFER *out;
  z_stream s;
  long outstart;
  int err;
  int ret = 0;

  out = buf_new();

  s.zalloc = (alloc_func) 0;
  s.zfree = (free_func) 0;
  s.opaque = (voidpf) 0;

  s.next_in = in->data + in->ptr;
  s.avail_in = in->length + 1 - in->ptr;	/* terminating 0 byte as "dummy" */
  s.next_out = NULL;

  if (type == 1)
    err = inflateInit(&s);    /* zlib */
  else
    err = inflateInit2(&s, -MAX_WBITS);
  if (err != Z_OK) {
    ret = -1;
    goto end;
  }
  outstart = 0;
  buf_append(out, NULL, in->length * 15 / 10);

  for (;;) {
    s.next_out = out->data + s.total_out + outstart;
    s.avail_out = out->length - outstart - s.total_out;
    err = inflate(&s, Z_PARTIAL_FLUSH);
    out->length -= s.avail_out;
    if (err != Z_OK)
      break;
    buf_append(out, NULL, BUFSIZE);
  }
  if (err != Z_STREAM_END)
    errlog(WARNING, "Decompression error %d\n", err);

  err = inflateEnd(&s);
  if (err != Z_OK)
    ret = -1;
end:
  if (ret != 0)
    switch (err) {
    case Z_STREAM_ERROR:
      errlog(ERRORMSG, "Decompression error Z_STREAM_ERROR.\n", err);
      break;
    case Z_MEM_ERROR:
      errlog(ERRORMSG, "Decompression error Z_MEM_ERROR.\n", err);
      break;
    case Z_BUF_ERROR:
      errlog(ERRORMSG, "Decompression error Z_BUF_ERROR.\n", err);
      break;
    case Z_VERSION_ERROR:
      errlog(ERRORMSG, "Decompression error Z_VERSION_ERROR.\n", err);
      break;
    default:
      errlog(ERRORMSG, "Decompression error %d.\n", err);
    }
  buf_move(in, out);
  buf_free(out);
  return (ret);
}

int buf_zip(BUFFER *out, BUFFER *in, int bits)
{
  z_stream s;
  long outstart;
  int err = -1;

  assert(in != out);

  s.zalloc = (alloc_func) 0;
  s.zfree = (free_func) 0;
  s.opaque = (voidpf) 0;
  s.next_in = NULL;

  if (bits == 0)
    bits = MAX_WBITS;

  if (deflateInit2(&s, Z_DEFAULT_COMPRESSION, Z_DEFLATED, -bits, 8, 0) != Z_OK)
    goto end;

  outstart = out->length;
  /* 12 is overhead, 1.01 is maximum expansion, and 1 is there to force a round-up */
  buf_append(out, NULL, (int)13+in->length*1.01); /* fit it in one chunk */

  s.next_in = in->data;
  s.avail_in = in->length;

  for (;;) {
    s.next_out = out->data + s.total_out + outstart;
    s.avail_out = out->length - outstart - s.total_out;
    err = deflate(&s, Z_FINISH);
    out->length -= s.avail_out;
    if (err != Z_OK)
      break;
    errlog(ERRORMSG, "Compressed data did not fit in one chunk.\n");
    buf_append(out, NULL, BUFSIZE);
  }
  if (deflateEnd(&s) != Z_OK || err != Z_STREAM_END)
    err = -1;
  else
    err = 0;
end:
  if (err != 0)
    errlog(ERRORMSG, "Compression error.\n");
  return (err);
}

#else /* end of USE_ZLIB */
int buf_zip(BUFFER *out, BUFFER *in, int bits)
{
  return (-1);
}

int buf_unzip(BUFFER *b, int type)
{
  errlog(ERRORMSG, "Can't uncompress: no zlib\n");
  return (-1);
}
#endif /* else not USE_ZLIB */

int compressed(BUFFER *b)
{
  return (b->length >= 10 && b->data[0] == gz_magic[0] &&
	  b->data[1] == gz_magic[1]);
}

int buf_uncompress(BUFFER *in)
{
  int type;
  int err = -1;
  unsigned int len;

  if (!compressed(in))
    return (0);
  type = in->data[3];
  if (in->data[2] != Z_DEFLATED || (type & RESERVED) == 0) {
    in->ptr = 10;
    if ((type & EXTRA_FIELD) != 0) {
      len = buf_geti(in);
      in->ptr += len;
    }
    if ((type & ORIG_NAME) != 0)
      while (buf_getc(in) > 0) ;
    if ((type & COMMENT) != 0)
      while (buf_getc(in) > 0) ;
    if ((type & HEAD_CRC) != 0)
      buf_geti(in);
    err = buf_unzip(in, 0);
  }
  return (err);
}

int buf_compress(BUFFER *in)
{
  BUFFER *out;
  int err;

  if (compressed(in))
    return (0);

  out = buf_new();
  buf_appendc(out, gz_magic[0]);
  buf_appendc(out, gz_magic[1]);
  buf_appendc(out, Z_DEFLATED);
  buf_appendc(out, 0);		/* flags */
  buf_appendl(out, 0);		/* time */
  buf_appendc(out, 0);		/* xflags */
  buf_appendc(out, 3);		/* Unix */
  err = buf_zip(out, in, 0);
  if (err == 0)
    buf_move(in, out);
  buf_free(out);
  return (err);
}
