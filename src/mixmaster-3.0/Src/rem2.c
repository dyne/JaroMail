/* Mixmaster version 3.0  --  (C) 1999 - 2006 Anonymizer Inc. and others.

   Mixmaster may be redistributed and modified under certain conditions.
   This software is distributed on an "AS IS" basis, WITHOUT WARRANTY OF
   ANY KIND, either express or implied. See the file COPYRIGHT for
   details.

   Process Mixmaster remailer messages
   $Id: rem2.c 934 2006-06-24 13:40:39Z rabbi $ */


#include "mix3.h"
#include <string.h>
#include <time.h>
#include <ctype.h>
#include <sys/types.h>
#include <sys/stat.h>
#ifdef POSIX
#include <unistd.h>
#else /* end of POSIX */
#include <io.h>
#endif /* else if not POSIX */
#ifndef _MSC
#include <dirent.h>
#endif /* not _MSC */
#include <assert.h>

int mix_dearmor(BUFFER *in, BUFFER *out)
{
  BUFFER *line, *md;
  int tempbuf = 0;
  int err = 0;

  line = buf_new();
  md = buf_new();

  if (in == out) {
    tempbuf = 1;
    out = buf_new();
  }
  do {
    err = buf_getline(in, line);
    if (err == -1)
      goto end;
  }
  while (!bufeq(line, begin_remailer));

  do {
    /* skip lines before message digest */
    if (buf_getline(in, md) == -1)
      break;
  } while (strlen(md->data) != 24);

  decode(in, out);

  err = buf_getline(in, line);
  if (err != 0 || !bufeq(line, end_remailer))
    err = -1;
  else {
    digest_md5(out, line);
    encode(line, 0);
    if (!buf_eq(md, line))
      err = -1;
    if (out->length != 20480)
      err = -1;
  }

end:
  if (err == -1)
    errlog(NOTICE, "Malformatted message.\n");

  if (tempbuf) {
    buf_move(in, out);
    buf_free(out);
  }
  buf_free(line);
  buf_free(md);
  return (err);
}

static int isnewid(BUFFER *id, long timestamp)
/* return values:
 *   0: ignore message, no error
 *   1: ok, process message
 *  -1: bad message, send reply
 */
{
  FILE *f;
  int ret = 1;
  long now, old = 0;
  LOCK *i = NULL;
  idlog_t idbuf;

  if (REMAIL == 0)
    return (1); /* don't keep statistics for the client */

  now = time(NULL);

  if ((f = mix_openfile(IDLOG, "rb+")) != NULL) {
    fread(&idbuf,1,sizeof(idlog_t),f);
    old = idbuf.time;
  } else {
    if (IDEXP == 0) {
      if (timestamp > 0 && timestamp <= now - 7 * SECONDSPERDAY) {
	errlog(LOG, "Ignoring old message.\n");
	return (0);
      }
    } else {
      if ((f = mix_openfile(IDLOG, "wb")) != NULL) {
	memset(idbuf.id,0,sizeof(idbuf.id));
	idbuf.time = now;
	fwrite(&idbuf,1,sizeof(idlog_t),f);
	memcpy(idbuf.id,id->data,sizeof(idbuf.id));
	idbuf.time = now;
	fwrite(&idbuf,1,sizeof(idlog_t),f);
	fclose(f);
	errlog(NOTICE, "Creating %s\n", IDLOG);
      } else {
	errlog(ERRORMSG, "Can't create %s\n", IDLOG);
      }
      return (1);
    }
  }

  if (now - old < 5 * SECONDSPERDAY)	/* never reject messages less than */
    old = now - 5 * SECONDSPERDAY;	/* 5 days old (== minimum IDEXP) */

  if (timestamp > 0 && timestamp <= old) {
    errlog(LOG, "Ignoring old message.\n");
    ret = 0;
    goto end;
  }
  i = lockfile(IDLOG);
  while (fread(&idbuf, 1, sizeof(idlog_t), f) == sizeof(idlog_t)) {
    if (!memcmp(idbuf.id, id->data, sizeof(idbuf.id))) {
      char idstr[33];
      id_encode(id->data, idstr);
      errlog(LOG, "Ignoring redundant message: %s.\n", idstr);
      ret = 0;
      goto end;
    }
  }
  if (timestamp > now) {
    errlog(LOG, "Ignoring message with future timestamp.\n");
    ret = -1;
    goto end;
  }
  if (ftell(f)%sizeof(idlog_t)) fseek(f,0-(ftell(f)%sizeof(idlog_t)),SEEK_CUR); /* make sure that we're on sizeof(idlog_t) byte boundary */
  memcpy(idbuf.id,id->data,sizeof(idbuf.id));
  idbuf.time = now;
  fwrite(&idbuf,1,sizeof(idlog_t),f);
end:
  if (i)
    unlockfile(i);
  fclose(f);
  return (ret);
}

int mix2_decrypt(BUFFER *m)
     /*  0: ok
      * -1: error
      * -2: old message */
{
  int err = 0;
  int i;
  BUFFER *privkey;
  BUFFER *keyid;
  BUFFER *dec, *deskey;
  BUFFER *packetid, *mid, *digest, *addr, *temp, *iv, *ivvec;
  int type, packet = 0, numpackets = 0, timestamp = 0;
  BUFFER *body;
  BUFFER *header, *out;

  privkey = buf_new();
  keyid = buf_new();
  dec = buf_new();
  deskey = buf_new();
  packetid = buf_new();
  mid = buf_new();
  digest = buf_new();
  addr = buf_new();
  temp = buf_new();
  iv = buf_new();
  ivvec = buf_new();
  body = buf_new();
  header = buf_new();
  out = buf_new();

  buf_get(m, keyid, 16);
  err = db_getseckey(keyid->data, privkey);
  if (err == -1)
    goto end;
  buf_get(m, deskey, buf_getc(m));
  err = pk_decrypt(deskey, privkey);
  if (err == -1 || deskey->length != 24) {
    err = -1;
    errlog(NOTICE, "Cannot decrypt message.\n");
    goto end;
  }
  buf_get(m, iv, 8);
  buf_get(m, dec, 328);
  buf_crypt(dec, deskey, iv, DECRYPT);
  buf_get(dec, packetid, 16);
  buf_get(dec, deskey, 24);
  type = buf_getc(dec);
  switch (type) {
  case 0:
    buf_get(dec, ivvec, 152);
    buf_get(dec, addr, 80);
    break;
  case 1:
    buf_get(dec, mid, 16);
    buf_get(dec, iv, 8);
    break;
  case 2:
    packet = buf_getc(dec);
    numpackets = buf_getc(dec);
    buf_get(dec, mid, 16);
    buf_get(dec, iv, 8);
    break;
  default:
    errlog(WARNING, "Unknown message type.\n");
    err = -1;
    goto end;
  }
  if (dec->data[dec->ptr] == '0' && dec->data[dec->ptr + 1] == '0' &&
      dec->data[dec->ptr + 2] == '0' && dec->data[dec->ptr + 3] == '0' &&
      dec->data[dec->ptr + 4] == '\0') {
    dec->ptr += 5;
    timestamp = buf_geti_lo(dec);
  } else {
    errlog(LOG, "Ignoring message without timestamp.\n");
    err = -1;
    goto end;
  }
  buf_get(dec, digest, 16);

  dec->length = dec->ptr - 16;	/* ignore digest */
  dec->ptr = dec->length;

  if (!isdigest_md5(dec, digest)) {
    errlog(NOTICE, "Message digest does not match.\n");
    err = -1;
    goto end;
  }
  switch (isnewid(packetid, timestamp * SECONDSPERDAY)) {
    case  0: err = -2; /* redundant message */
	     goto end;
    case -1: err = -1; /* future timestamp */
	     goto end; 
  }
  buf_append(body, m->data + 20 * 512, 10240);

  switch (type) {
  case 0:
    buf_chop(addr);
    buf_cat(out, addr);
    buf_nl(out);
    for (i = 0; i < 19; i++) {
      buf_reset(header);
      buf_append(header, m->data + (i + 1) * 512, 512);
      buf_reset(iv);
      buf_append(iv, ivvec->data + i * 8, 8);
      buf_crypt(header, deskey, iv, DECRYPT);
      buf_cat(out, header);
    }
    buf_reset(header);
    buf_pad(header, 512);
    buf_cat(out, header);
    buf_reset(iv);
    buf_append(iv, ivvec->data + 144, 8);
    buf_crypt(body, deskey, iv, DECRYPT);
    buf_cat(out, body);
    mix_pool(out, INTERMEDIATE, -1);
    break;
  case 1:
    buf_crypt(body, deskey, iv, DECRYPT);
    err = v2body_setlen(body);
    if (err == -1)
      goto end;
    assert(body->ptr == 4);
    v2body(body);
    break;
  case 2:
    buf_crypt(body, deskey, iv, DECRYPT);
    v2partial(body, mid, packet, numpackets);
    break;
  }
end:
  buf_free(privkey);
  buf_free(keyid);
  buf_free(dec);
  buf_free(deskey);
  buf_free(packetid);
  buf_free(mid);
  buf_free(digest);
  buf_free(addr);
  buf_free(temp);
  buf_free(iv);
  buf_free(ivvec);
  buf_free(body);
  buf_free(header);
  buf_free(out);

  return (err);
}

int v2body_setlen(BUFFER *body)
{
  long length;

  length = buf_getl_lo(body);
  if (length < 0 || length > body->length)
    return (-1);
  body->length = length + 4;
  return (0);
}

int v2body(BUFFER *body)
{
  int i, n;
  BUFFER *to, *newsgroups;
  BUFFER *temp, *out;
  BUFFER *line;
  int type = MSG_MAIL;
  int subject = 0;

  line = buf_new();
  to = buf_new();
  newsgroups = buf_new();
  temp = buf_new();
  out = buf_new();

  n = buf_getc(body);
  for (i = 0; i < n; i++) {
    buf_get(body, line, 80);
    buf_chop(line);
    if (bufileft(line, "null:"))
      goto end;
    if (bufileft(line, "post:")) {
      type = MSG_POST;
      if (line->length > 5) {
	int j = 5;

	while (j < line->length && isspace(line->data[j]))
	  j++;
	if (newsgroups->length > 0)
	  buf_appends(newsgroups, ",");
	buf_append(newsgroups, line->data + j, line->length - j);
      }
    } else {
      if (to->length > 0)
	buf_appends(to, ",");
      buf_cat(to, line);
    }
  }
  if (to->length > 0) {
    buf_appends(out, "To: ");
    buf_cat(out, to);
    buf_nl(out);
  }
  if (newsgroups->length > 0) {
    buf_appends(out, "Newsgroups: ");
    buf_cat(out, newsgroups);
    buf_nl(out);
  }
  n = buf_getc(body);
  for (i = 0; i < n; i++) {
    buf_get(body, line, 80);
    buf_chop(line);
    if (bufileft(line, "Subject:"))
      subject = 1;
    buf_cat(out, line);
    buf_nl(out);
  }

  buf_rest(temp, body);
  buf_uncompress(temp);
  buf_set(body, temp);
  buf_reset(temp);

  if (buf_lookahead(body, line) == 0 && isline(line, HASHMARK)) {
    buf_getline(body, line);
    while (buf_getline(body, line) == 0) {
      if (bufileft(line, "subject:"))
	subject = 1;
      buf_cat(out, line);
      buf_nl(out);
    }
  }
  if (type == MSG_POST && !subject)
    buf_appends(out, "Subject: (no subject)\n");

  buf_nl(out);
  buf_rest(out, body);
  buf_reset(body);
  mix_pool(out, type, -1);

end:
  buf_free(line);
  buf_free(to);
  buf_free(newsgroups);
  buf_free(temp);
  buf_free(out);
  return (0);
}

int v2_merge(BUFFER *mid)
{
  char fname[PATHMAX], line[LINELEN];
  BUFFER *temp, *msg;
  FILE *l, *f;
  int i, numpackets;
  struct stat sb;
  long d;
  int n;
  int err = -1;

  temp = buf_new();
  msg = buf_new();
  pool_packetfile(fname, mid, 0);
  l = fopen(fname, "a+");
  if (l != NULL)
    lock(l);

  pool_packetfile(fname, mid, 1);
  f = fopen(fname, "rb");
  if (f == NULL)
    goto end;
  fscanf(f, "%32s %ld %d %d\n", line, &d, &i, &numpackets);
  fclose(f);

  /* do we have all packets? */
  for (i = 1; i <= numpackets; i++) {
    pool_packetfile(fname, mid, i);
    if (stat(fname, &sb) != 0)
      goto end;
  }
  errlog(LOG, "Reassembling multipart message.\n");
  for (i = 1; i <= numpackets; i++) {
    pool_packetfile(fname, mid, i);
    f = fopen(fname, "rb");
    if (f == NULL)
      goto end;
    fscanf(f, "%32s %ld %d %d\n", line, &d, &n, &n);
    buf_clear(temp);
    buf_read(temp, f);
    v2body_setlen(temp);
    buf_append(msg, temp->data + 4, temp->length - 4);
    fclose(f);
    unlink(fname);
  }
  err = v2body(msg);

end:
  if (l != NULL)
    fclose(l);
  pool_packetfile(fname, mid, 0);
  unlink(fname);
  buf_free(temp);
  buf_free(msg);
  return (err);
}

int v2partial(BUFFER *m, BUFFER *mid, int packet, int numpackets)
{
  char fname[PATHMAX], idstr[33];
  FILE *f;
  int err = 1;

  pool_packetfile(fname, mid, packet);
  f = fopen(fname, "wb");
  if (f == NULL) {
    err = -1;
    goto end;
  }
  id_encode(mid->data, idstr);
  fprintf(f, "%s %ld %d %d\n", idstr, (long) time(NULL), packet,
	  numpackets);
  buf_write(m, f);
  buf_reset(m);
  fclose(f);
  v2_merge(mid);
end:
  return (err);
}
