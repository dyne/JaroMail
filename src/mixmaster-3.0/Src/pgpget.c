/* Mixmaster version 3.0  --  (C) 1999 - 2006 Anonymizer Inc. and others.

   Mixmaster may be redistributed and modified under certain conditions.
   This software is distributed on an "AS IS" basis, WITHOUT WARRANTY OF
   ANY KIND, either express or implied. See the file COPYRIGHT for
   details.

   Read OpenPGP packets
   $Id: pgpget.c 934 2006-06-24 13:40:39Z rabbi $ */


#include "mix3.h"
#ifdef USE_PGP
#include "pgp.h"
#include "crypto.h"
#include <time.h>
#include <assert.h>
#include <string.h>

int pgp_getmsg(BUFFER *in, BUFFER *key, BUFFER *sig, char *pubring,
	       char *secring)
{
  BUFFER *p;
  BUFFER *out;
  int type, algo = 0;
  int err = PGP_NOMSG;
  pgpsig signature = {0, NULL, 0, 0, {0,} };

  p = buf_new();
  out = buf_new();

  if (sig)
    signature.userid = buf_new();

  while ((type = pgp_getpacket(in, p)) > 0)
    switch (type) {
    case PGP_LITERAL:
      pgp_getliteral(p);
      buf_move(out, p);
      err = 0;
      break;
    case PGP_COMPRESSED:
      err = pgp_uncompress(p);
      if (err == 0)
	err = pgp_getmsg(p, key, sig, pubring, secring);
      if (err != PGP_ERR && err != PGP_PASS)
	buf_move(out, p);
      break;
    case PGP_ENCRYPTED:
    case PGP_ENCRYPTEDMDC:
      if (!key) {
	err = -1;
	break;
      }
      if (/*key->length > 0 &&*/ algo == 0) {
	algo = PGP_K_IDEA;
	digest_md5(key, key);
      }
      if (key->length > 0)
	err = pgp_getsymmetric(p, key, algo, type==PGP_ENCRYPTEDMDC);
      else
	err = -1;
      if (err == 0)
	err = pgp_getmsg(p, NULL, sig, pubring, secring);
      if (err != PGP_ERR)
	buf_move(out, p);
      break;
    case PGP_SESKEY:
      if (!key) {
	err = -1;
	break;
      }
      err = pgp_getsessionkey(p, key, secring);
      if (err >= 0) {
	algo = err;
	err = 0;
	buf_set(key, p);
      }
      break;
    case PGP_SYMSESKEY:
      if (!key) {
	err = -1;
	break;
      }
      err = pgp_getsymsessionkey(p, key);
      if (err >= 0) {
	algo = err;
	err = 0;
	if (key) buf_set(key, p);
      }
      break;
    case PGP_MARKER:
	err = 0;
	break; /* ignore per RFC2440 */
    case PGP_SIG:
      pgp_getsig(p, &signature, pubring);
      /* fallthru */
    default:
      if (err == PGP_NOMSG)
	err = PGP_NODATA;
    }

  if (signature.ok == PGP_SIGVRFY)
    pgp_verify(out, sig, &signature);
  if (signature.ok == PGP_SIGOK) {
    char line[LINELEN];
    time_t t;
    struct tm *tc;

    t = signature.sigtime;
    tc = localtime(&t);
#if 0
    strftime(line, LINELEN, "[%Y-%m-%d %H:%M:%S]", tc);
#else /* end of 0 */
    strftime(line, LINELEN, "[%a %b %d %H:%M:%S %Y]", tc);
#endif /* else if not 0 */
    if (sig) {
      buf_cat(sig, signature.userid);
      buf_appendc(sig, ' ');
      buf_appends(sig, line);
    }
  }
  if (sig) {
    if (signature.ok == PGP_SIGNKEY)
      buf_appendf(sig, "%02X%02X%02X%02X", signature.userid->data[4],
		  signature.userid->data[5], signature.userid->data[6],
		  signature.userid->data[7]);
    buf_free(signature.userid);
  }

  if ((err == 0 || err == PGP_NODATA) && signature.ok != 0)
    err = signature.ok;

  buf_move(in, out);
  buf_free(out);
  buf_free(p);

  return (err);
}

int pgp_ispacket(BUFFER *b)
{
  return ((b->data[b->ptr] >> 6) == 2 || (b->data[b->ptr] >> 6) == 3);
}

int pgp_packettype(BUFFER *b, long *len, int *partial)
{
  int ctb;

  ctb = buf_getc(b);
  switch (ctb >> 6) {
  case 2:
    /* old packet type */
    switch (ctb & 3) {
    case 0:
      *len = buf_getc(b);
      break;
    case 1:
      *len = buf_geti(b);
      break;
    case 2:
      *len = buf_getl(b);
      break;
    case 3:
      *len = b->length - b->ptr;
      break;
    }
    *partial = 0;
    return (ctb >> 2) & 15;
  case 3:
  case 1: /* in GnuPG secret key ring */
    /* new packet type */
    *len = buf_getc(b);
    if (*len >= 192 && *len <= 223)
      *len = (*len - 192) * 256 + buf_getc(b) + 192;
    else if (*len == 255)
      *len = buf_getl(b);
    else if (*len > 223) {
      *len = 1 <<(*len & 0x1f);
      *partial = 1;
    }
    return (ctb & 63);
  }
  return (-1);
}

int pgp_packetpartial(BUFFER *b, long *len, int *partial)
{
  *partial = 0;
  *len = buf_getc(b);
  if (*len >= 192 && *len <= 223)
    *len = (*len - 192) * 256 + buf_getc(b) + 192;
  else if (*len == 255)
    *len = buf_getl(b);
  else if (*len > 223) {
    *len = 1 <<(*len & 0x1f);
    *partial = 1;
  }
  return 1;
}

int pgp_isconventional(BUFFER *b)
{
  int type;
  BUFFER *p;
  p = buf_new();

  type = pgp_getpacket(b, p);
  if (type == PGP_MARKER)
    type = pgp_getpacket(b, p);
  buf_rewind(b);
  buf_free(p);
  return (type == PGP_ENCRYPTED || type == PGP_SYMSESKEY);
}

int pgp_getpacket(BUFFER *in, BUFFER *p)
     /*  returns <0 = error, >0 = packet type */
{
  int type;
  long len;
  int partial = 0;
  BUFFER *tmp;

  tmp = buf_new();
  type = pgp_packettype(in, &len, &partial);
  if (type > 0 && len > 0) {
    buf_clear(p);
    while(partial && len > 0) {
      buf_get(in, tmp, len);
      buf_cat(p, tmp);
      pgp_packetpartial(in, &len, &partial);
    }
    if (len > 0) {
      buf_get(in, tmp, len);
      buf_cat(p, tmp);
    }
  }

  buf_free(tmp);
  return (type);
}

int pgp_getsig(BUFFER *p, pgpsig *sig, char *pubring)
{
  BUFFER *sigkey, *id, *i;
  int algo, hashalgo;
  int hash;

  sigkey = buf_new();
  id = buf_new();
  i = buf_new();

  sig->ok = PGP_SIGBAD;

  if (buf_getc(p) > 3)
    goto end;
  if (buf_getc(p) != 5)
    goto end;
  sig->sigtype = buf_getc(p);
  sig->sigtime = buf_getl(p);
  buf_get(p, id, 8);
  algo = buf_getc(p);
  hashalgo = buf_getc(p);
  if (hashalgo != PGP_H_MD5)
    goto end;
  hash = buf_geti(p);
  if (pgpdb_getkey(PK_VERIFY, algo, NULL, NULL, NULL, sigkey, NULL, sig->userid, id,
		   pubring, NULL) < 0) {
    sig->ok = PGP_SIGNKEY;
    if (sig->userid)
      buf_set(sig->userid, id);
    goto end;
  }
  switch (algo) {
  case PGP_ES_RSA:
    mpi_get(p, i);
    if (pgp_rsa(i, sigkey, PK_VERIFY) == -1 ||
	memcmp(i->data, MD5PREFIX, sizeof(MD5PREFIX) - 1) != 0)
      goto end;
    memcpy(sig->hash, i->data + sizeof(MD5PREFIX) - 1, 16);
    if (sig->hash[0] * 256 + sig->hash[1] != hash)
      goto end;
    sig->ok = PGP_SIGVRFY;
    break;
  default:
    break;
  }
end:
  buf_free(sigkey);
  buf_free(id);
  buf_free(i);
  return (sig->ok);
}

void pgp_verify(BUFFER *msg, BUFFER *detached, pgpsig *sig)
{
  MD5_CTX c;
  BUFFER *t;
  byte md[16];

  t = buf_new();
  sig->ok = PGP_SIGBAD;

  if (msg->length == 0) {	/* detached signature */
    if (detached && detached->length) {
      buf_move(msg, detached);
      if (sig->sigtype == PGP_SIG_CANONIC)
	pgp_sigcanonic(msg); /* for cleartext signatures */
    } else
      sig->ok = PGP_NODATA;
  }
  MD5_Init(&c);
  switch (sig->sigtype) {
  case PGP_SIG_BINARY:
    MD5_Update(&c, msg->data, msg->length);
    break;
  case PGP_SIG_CANONIC:
    while (buf_getline(msg, t) != -1) {
#if 0
      pgp_sigcanonic(t); /* according to OpenPGP */
#else /* end of 0 */
      buf_appends(t, "\r\n");
#endif /* else if not 0 */
      MD5_Update(&c, t->data, t->length);
    }
    break;
  default:
    sig->ok = PGP_SIGBAD;
  }
  MD5_Update(&c, &(sig->sigtype), 1);
  buf_appendl(t, sig->sigtime);
  MD5_Update(&c, t->data, 4);
  MD5_Final(md, &c);
  if (memcmp(md, sig->hash, 16) == 0)
    sig->ok = PGP_SIGOK;
  buf_free(t);
}

#ifdef USE_IDEA
static int pgp_ideadecrypt(BUFFER *in, BUFFER *out, BUFFER *key, int mdc)
{
  int err = 0;
  byte iv[8];
  byte hdr[10];
  int i, n;
  IDEA_KEY_SCHEDULE ks;
  SHA_CTX c;
  char md[20]; /* we could make hdr 20 bytes long and reuse it for md */

  if (key->length != 16 || in->length <= (mdc?(1+10+22):10))
    return (-1);

  if (mdc) {
    mdc = 1;
    if (in->data[0] != 1)
      return (-1);
  }

  buf_prepare(out, in->length - 10 - mdc);

  for (i = 0; i < 8; i++)
    iv[i] = 0;

  idea_set_encrypt_key(key->data, &ks);

  n = 0;
  idea_cfb64_encrypt(in->data + mdc, hdr, 10, &ks, iv, &n, IDEA_DECRYPT);
  if (n != 2 || hdr[8] != hdr[6] || hdr[9] != hdr[7]) {
    err = -1;
    goto end;
  }
  if (mdc) {
    SHA1_Init(&c);
    SHA1_Update(&c, hdr, 10);
  } else {
    iv[6] = iv[0], iv[7] = iv[1];
    memcpy(iv, in->data + 2, 6);
    n = 0;
  }
  idea_cfb64_encrypt(in->data + 10 + mdc, out->data, in->length - 10 - mdc, &ks, iv, &n,
		     IDEA_DECRYPT);
  if (mdc) {
    if (out->length > 22) {
      out->length -= 22;
      if (out->data[out->length] == 0xD3 && out->data[out->length + 1] == 0x14) {
	SHA1_Update(&c, out->data, out->length + 2);
	SHA1_Final(md, &c);
	if (memcmp(out->data + out->length + 2, md, 20))
	  err = -1;
      } else
	err = -1;
    } else
      err = -1;
  }
end:
  return (err);
}
#endif /* USE_IDEA */

static int pgp_3desdecrypt(BUFFER *in, BUFFER *out, BUFFER *key, int mdc)
{
  int err = 0;
  des_cblock iv;
  byte hdr[10];
  int i, n;
  des_key_schedule ks1;
  des_key_schedule ks2;
  des_key_schedule ks3;
  SHA_CTX c;
  char md[20]; /* we could make hdr 20 bytes long and reuse it for md */

  if (key->length != 24 || in->length <= (mdc?(1+10+22):10))
    return (-1);

  if (mdc) {
    mdc = 1;
    if (in->data[0] != 1)
      return (-1);
  }

  buf_prepare(out, in->length - 10 - mdc);

  for (i = 0; i < 8; i++)
    iv[i] = 0;

  des_set_key((const_des_cblock *) key->data, ks1);
  des_set_key((const_des_cblock *) (key->data + 8), ks2);
  des_set_key((const_des_cblock *) (key->data+ 16), ks3);

  n = 0;
  des_ede3_cfb64_encrypt(in->data + mdc, hdr, 10, ks1, ks2, ks3, &iv, &n, DECRYPT);
  if (n != 2 || hdr[8] != hdr[6] || hdr[9] != hdr[7]) {
    err = -1;
    goto end;
  }
  if (mdc) {
    SHA1_Init(&c);
    SHA1_Update(&c, hdr, 10);
  } else {
    iv[6] = iv[0], iv[7] = iv[1];
    memcpy(iv, in->data + 2, 6);
    n = 0;
  }
  des_ede3_cfb64_encrypt(in->data + 10 + mdc, out->data, in->length - 10 + mdc, ks1,
			 ks2, ks3, &iv, &n, DECRYPT);
  if (mdc) {
    if (out->length > 22) {
      out->length -= 22;
      if (out->data[out->length] == 0xD3 && out->data[out->length + 1] == 0x14) {
	SHA1_Update(&c, out->data, out->length + 2);
	SHA1_Final(md, &c);
	if (memcmp(out->data + out->length + 2, md, 20))
	  err = -1;
      } else
	err = -1;
    } else
      err = -1;
  }
end:
  return (err);
}

static int pgp_castdecrypt(BUFFER *in, BUFFER *out, BUFFER *key, int mdc)
{
  int err = 0;
  byte iv[8];
  byte hdr[10];
  int i, n;
  SHA_CTX c;
  char md[20]; /* we could make hdr 20 bytes long and reuse it for md */

  CAST_KEY ks;

  if (key->length != 16 || in->length <= (mdc?(1+10+22):10))
    return (-1);

  if (mdc) {
    mdc = 1;
    if (in->data[0] != 1)
      return (-1);
  }

  buf_prepare(out, in->length - 10 - mdc);

  for (i = 0; i < 8; i++)
    iv[i] = 0;

  CAST_set_key(&ks, 16, key->data);

  n = 0;
  CAST_cfb64_encrypt(in->data + mdc, hdr, 10, &ks, iv, &n, CAST_DECRYPT);
  if (n != 2 || hdr[8] != hdr[6] || hdr[9] != hdr[7]) {
    err = -1;
    goto end;
  }
  if (mdc) {
    SHA1_Init(&c);
    SHA1_Update(&c, hdr, 10);
  } else {
    iv[6] = iv[0], iv[7] = iv[1];
    memcpy(iv, in->data + 2, 6);
    n = 0;
  }
  CAST_cfb64_encrypt(in->data + 10 + mdc, out->data, in->length - 10 - mdc, &ks,
		     iv, &n, CAST_DECRYPT);
  if (mdc) {
    if (out->length > 22) {
      out->length -= 22;
      if (out->data[out->length] == 0xD3 && out->data[out->length + 1] == 0x14) {
	SHA1_Update(&c, out->data, out->length + 2);
	SHA1_Final(md, &c);
	if (memcmp(out->data + out->length + 2, md, 20))
	  err = -1;
      } else
	err = -1;
    } else
      err = -1;
  }
end:
  return (err);
}

static int pgp_bfdecrypt(BUFFER *in, BUFFER *out, BUFFER *key, int mdc)
{
  int err = 0;
  byte iv[8];
  byte hdr[10];
  int i, n;
  SHA_CTX c;
  char md[20]; /* we could make hdr 20 bytes long and reuse it for md */

  BF_KEY ks;

  if (key->length != 16 || in->length <= (mdc?(1+10+22):10))
    return (-1);

  if (mdc) {
    mdc = 1;
    if (in->data[0] != 1)
      return (-1);
  }

  buf_prepare(out, in->length - 10 - mdc);

  for (i = 0; i < 8; i++)
    iv[i] = 0;

  BF_set_key(&ks, 16, key->data);

  n = 0;
  BF_cfb64_encrypt(in->data + mdc, hdr, 10, &ks, iv, &n, BF_DECRYPT);
  if (n != 2 || hdr[8] != hdr[6] || hdr[9] != hdr[7]) {
    err = -1;
    goto end;
  }
  if (mdc) {
    SHA1_Init(&c);
    SHA1_Update(&c, hdr, 10);
  } else {
    iv[6] = iv[0], iv[7] = iv[1];
    memcpy(iv, in->data + 2, 6);
    n = 0;
  }
  BF_cfb64_encrypt(in->data + 10 + mdc, out->data, in->length - 10 - mdc, &ks,
		   iv, &n, BF_DECRYPT);
  if (mdc) {
    if (out->length > 22) {
      out->length -= 22;
      if (out->data[out->length] == 0xD3 && out->data[out->length + 1] == 0x14) {
	SHA1_Update(&c, out->data, out->length + 2);
	SHA1_Final(md, &c);
	if (memcmp(out->data + out->length + 2, md, 20))
	  err = -1;
      } else
	err = -1;
    } else
      err = -1;
  }
end:
  return (err);
}

#ifdef USE_AES
static int pgp_aesdecrypt(BUFFER *in, BUFFER *out, BUFFER *key, int mdc)
{
  int err = 0;
  byte iv[16];
  byte hdr[18];
  int i, n;
  SHA_CTX c;
  char md[20]; /* we could make hdr 20 bytes long and reuse it for md */

  AES_KEY ks;

  if ((key->length != 16 && key->length != 24 && key->length != 32) || in->length <= (mdc?(1+18+22):18))
    return (-1);

  if (mdc) {
    mdc = 1;
    if (in->data[0] != 1)
      return (-1);
  }

  buf_prepare(out, in->length - 18 - mdc);

  for (i = 0; i < 16; i++)
    iv[i] = 0;

  AES_set_encrypt_key(key->data, key->length<<3, &ks);

  n = 0;
  AES_cfb128_encrypt(in->data + mdc, hdr, 18, &ks, iv, &n, AES_DECRYPT);
  if (n != 2 || hdr[16] != hdr[14] || hdr[17] != hdr[15]) {
    err = -1;
    goto end;
  }
  if (mdc) {
    SHA1_Init(&c);
    SHA1_Update(&c, hdr, 18);
  } else {
    iv[14] = iv[0], iv[15] = iv[1];
    memcpy(iv, in->data + 2, 14);
    n = 0;
  }
  AES_cfb128_encrypt(in->data + 18 + mdc, out->data, in->length - 18 - mdc, &ks,
		   iv, &n, AES_DECRYPT);
  if (mdc) {
    if (out->length > 22) {
      out->length -= 22;
      if (out->data[out->length] == 0xD3 && out->data[out->length + 1] == 0x14) {
	SHA1_Update(&c, out->data, out->length + 2);
	SHA1_Final(md, &c);
	if (memcmp(out->data + out->length + 2, md, 20))
	  err = -1;
      } else
	err = -1;
    } else
      err = -1;
  }
end:
  return (err);
}
#endif /* USE_AES */

int pgp_getsymmetric(BUFFER *in, BUFFER *key, int algo, int mdc)
{
  int err = -1;
  BUFFER *out;

  out = buf_new();

  switch (algo) {
#ifdef USE_AES
   case PGP_K_AES128:
   case PGP_K_AES192:
   case PGP_K_AES256:
    err = pgp_aesdecrypt(in, out, key, mdc);
    break;
#endif /* USE_AES */
#ifdef USE_IDEA
   case PGP_K_IDEA:
    err = pgp_ideadecrypt(in, out, key, mdc);
    break;
#endif /* USE_IDEA */
   case PGP_K_3DES:
    err = pgp_3desdecrypt(in, out, key, mdc);
    break;
   case PGP_K_CAST5:
    err = pgp_castdecrypt(in, out, key, mdc);
    break;
  case PGP_K_BF:
    err = pgp_bfdecrypt(in, out, key, mdc);
    break;
  }

  if (err < 0)
    errlog(ERRORMSG, "PGP decryption failed.\n");

  buf_move(in, out);
  buf_free(out);
  return (err);
}

int pgp_getliteral(BUFFER *in)
{
  long fnlen;
  int err = 0;
  int mode;
  BUFFER *out;
  BUFFER *line;

  out = buf_new();
  line = buf_new();
  mode = buf_getc(in);
  fnlen = buf_getc(in);
  in->ptr += fnlen;		/* skip filename */
  if (in->ptr + 4 > in->length)
    err = -1;
  else {
    buf_getl(in);		/* timestamp */
    if (mode == 't')
      while (buf_getline(in, line) != -1) {
	buf_cat(out, line);
	buf_nl(out);
    } else
      buf_rest(out, in);
  }
  buf_move(in, out);
  buf_free(line);
  buf_free(out);
  return (err);
}

int pgp_uncompress(BUFFER *in)
{
  int err = -1;

  switch(buf_getc(in)) {
  case 0:
    err = 0;
    break;
  case 1:
    err = buf_unzip(in, 0);
    break;
  case 2:
    err = buf_unzip(in, 1);
    break;
  default:
     err = -1;
     break;
  }
  return (err);
}

int pgp_getsessionkey(BUFFER *in, BUFFER *pass, char *secring)
{
  BUFFER *out;
  BUFFER *key;
  BUFFER *keyid;
  int type;
  int i, csum = 0;
  int algo = 0;
  int err = -1;
  long expires;

  out = buf_new();
  key = buf_new();
  keyid = buf_new();
  type = buf_getc(in);		/* packet type */
  if (type < 2 || type > 3)
    goto end;
  buf_get(in, keyid, 8);
  algo = buf_getc(in);
  err = pgpdb_getkey(PK_DECRYPT, algo, NULL, NULL, &expires, key, NULL, NULL, keyid,
		     secring, pass);
  if (err < 0)
    goto end;
  if (expires > 0 && (expires + KEYGRACEPERIOD < time(NULL))) {
    errlog(DEBUGINFO, "Key expired.\n"); /* DEBUGINFO ? */
    err = -1;
    goto end;
  }
  switch (algo) {
  case PGP_ES_RSA:
    mpi_get(in, out);
    err = pgp_rsa(out, key, PK_DECRYPT);
    break;
   case PGP_E_ELG:
    buf_rest(out, in);
    err = pgp_elgdecrypt(out, key);
    break;
  default:
    err = -1;
  }
  if (err == 0 && out->length > 3) {
    algo = buf_getc(out);
    buf_get(out, in, out->length - 3); /* return recovered key */
    csum = buf_geti(out);
    for (i = 0; i < in->length; i++)
      csum = (csum - in->data[i]) % 65536;
    if (csum != 0)
      err = -1;
  } else
    err = -1;
end:
  buf_free(out);
  buf_free(key);
  buf_free(keyid);
  return (err == 0 ? algo : err);
}

void pgp_iteratedsk(BUFFER *out, BUFFER *salt, BUFFER *pass, byte c)
{
  int count;
  BUFFER *temp;
  temp = buf_new();

  count = (16l + (c & 15)) << ((c >> 4) + 6);
  while (temp->length < count) {
    buf_cat(temp, salt);
    buf_cat(temp, pass);
  }
  buf_get(temp, out, count);
  buf_free(temp);
}

int pgp_getsk(BUFFER *p, BUFFER *pass, BUFFER *key)
{
  int skalgo, skspecifier, hashalgo;
  BUFFER *salted; /* passphrase with salt */

  if (!pass)
    return(-1);

  salted = buf_new();

  skalgo = buf_getc(p);
  skspecifier = buf_getc(p);
  hashalgo = buf_getc(p);
  switch (skspecifier) {
  case 0:
    buf_set(salted, pass);
    break;
  case 1:
    buf_get(p, salted, 8); /* salt */
    buf_cat(salted, pass);
    break;
  case 3:
    buf_get(p, salted, 8); /* salt */
    pgp_iteratedsk(salted, salted, pass, buf_getc(p));
    break;
  default:
    skalgo = -1;
    goto end;
  }
  if (pgp_expandsk(key, skalgo, hashalgo, salted) == -1)
    skalgo = -1;

 end:
  buf_free(salted);
  return (skalgo);
}

int pgp_getsymsessionkey(BUFFER *in, BUFFER *pass)
{
  BUFFER *temp, *key, *iv;
  int algo = -1;
  temp = buf_new();
  key = buf_new();
  iv = buf_new();

  if (buf_getc(in) == 4) {  /* version */
    algo = pgp_getsk(in, pass, key);
    buf_rest(temp, in);
    if (temp->length) {
      /* encrypted session key present */
      buf_appendzero(iv, pgp_blocklen(algo));
      skcrypt(temp, algo, key, iv, DECRYPT);
      algo = buf_getc(temp);
      buf_rest(in, temp);
    } else
      buf_set(in, key);
  }
  buf_free(temp);
  buf_free(key);
  buf_free(iv);
  return (algo);
}

#endif /* USE_PGP */
