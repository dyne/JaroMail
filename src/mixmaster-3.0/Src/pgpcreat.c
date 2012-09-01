/* Mixmaster version 3.0  --  (C) 1999 - 2006 Anonymizer Inc. and others.

   Mixmaster may be redistributed and modified under certain conditions.
   This software is distributed on an "AS IS" basis, WITHOUT WARRANTY OF
   ANY KIND, either express or implied. See the file COPYRIGHT for
   details.

   Create OpenPGP packets
   $Id: pgpcreat.c 934 2006-06-24 13:40:39Z rabbi $ */


#include "mix3.h"
#ifdef USE_PGP
#include "pgp.h"
#include "crypto.h"
#include <assert.h>
#include <time.h>
#include <string.h>

int pgp_packet(BUFFER *in, int type)
{
  int ctb;
  BUFFER *out;

  out = buf_new();
 if (type > 15) {
    ctb = 0xC0 | type; /* make v4 packet */
    buf_setc(out, ctb);
    if (in->length > 8383) {
      buf_appendc(out, 0xFF);
      buf_appendl(out, in->length);
    } else if (in->length > 191) {
#if 0
      buf_appendc(out, ((in->length-192) >> 8) + 192);
      buf_appendc(out,  (in->length-192) & 0xFF);
#else /* end of 0 */
      buf_appendi(out, in->length - 0xC0 + 0xC000);
#endif /* else if not 0 */
    } else {
      buf_appendc(out, in->length);
    }
 } else {
  ctb = 128 + (type << 2);
  if (in->length < 256 && type != PGP_PUBKEY && type != PGP_SECKEY &&
      type != PGP_SIG && type != PGP_PUBSUBKEY && type != PGP_SECSUBKEY
#ifdef MIMIC
      && type != PGP_ENCRYPTED
#endif /* MIMIC */
    ) {
    buf_setc(out, ctb);
    buf_appendc(out, in->length);
  }
#ifndef MIMIC
  else if (in->length < 65536)
#else /* end of not MIMIC */
  else if ((type == PGP_PUBKEY || type == PGP_SECKEY || type == PGP_SIG
	   || type == PGP_SESKEY || type == PGP_PUBSUBKEY ||
	   type == PGP_SECSUBKEY) && in->length < 65536)
#endif /* else if MIMIC */
  {
    buf_appendc(out, ctb | 1);
    buf_appendi(out, in->length);
  } else {
    buf_appendc(out, ctb | 2);
    buf_appendl(out, in->length);
  }
 }
  buf_cat(out, in);
  buf_move(in, out);
  buf_free(out);
  return (0);
}

int pgp_subpacket(BUFFER *in, int type)
{
  BUFFER *out;
  int len;

  out = buf_new();
  len = in->length + 1;
  if (len < 192)
    buf_setc(out, len);
  else {
    buf_setc(out, 255);
    buf_appendl(out, len);
  }
  buf_appendc(out, type);
  buf_cat(out, in);
  buf_move(in, out);
  buf_free(out);
  return (0);
}

int pgp_packet3(BUFFER *in, int type)
{
#ifdef MIMIC
  int ctb;
  BUFFER *out;

  out = buf_new();
  ctb = 128 + (type << 2);
  buf_setc(out, ctb | 3);
  buf_cat(out, in);
  buf_move(in, out);
  buf_free(out);
  return (0);
#else /* end of MIMIC */
  return pgp_packet(in, type);
#endif /* else if not MIMIC */
}

#ifdef USE_IDEA
static int pgp_ideaencrypt(BUFFER *in, BUFFER *out, BUFFER *key, int mdc)
{
  byte iv[8];
  int i, n = 0;
  IDEA_KEY_SCHEDULE ks;
  SHA_CTX c;

  assert(key->length == 17);

  for (i = 0; i < 8; i++)
    iv[i] = 0;

  idea_set_encrypt_key(key->data + 1, &ks);

  if (mdc) {
    mdc = 1;
    out->data[0] = 1;
  }
  rnd_bytes(out->data + mdc, 8);
  out->data[8 + mdc] = out->data[6 + mdc], out->data[9 + mdc] = out->data[7 + mdc];
  if (mdc) {
    SHA1_Init(&c);
    SHA1_Update(&c, out->data + 1, 10);
    SHA1_Update(&c, in->data, in->length);
  }
  n = 0;
  idea_cfb64_encrypt(out->data + mdc, out->data + mdc, 10, &ks, iv, &n, IDEA_ENCRYPT);
  if (!mdc) {
    iv[6] = iv[0], iv[7] = iv[1];
    memcpy(iv, out->data + 2, 6);
    n = 0;
  }
  idea_cfb64_encrypt(in->data, out->data + 10 + mdc, in->length, &ks, iv, &n,
		     IDEA_ENCRYPT);
  if (mdc) {
    SHA1_Update(&c, "\xD3\x14", 2); /* 0xD3 = 0xC0 | PGP_MDC */
    idea_cfb64_encrypt("\xD3\x14", out->data + 11 + in->length, 2, &ks, iv, &n,
		       IDEA_ENCRYPT);
    SHA1_Final(out->data + 13 + in->length, &c);
    idea_cfb64_encrypt(out->data + 13 + in->length, out->data + 13 + in->length, 20, &ks, iv, &n,
		       IDEA_ENCRYPT);
  }
  return (0);
}
#endif /* USE_IDEA */

static int pgp_3desencrypt(BUFFER *in, BUFFER *out, BUFFER *key, int mdc)
{
  des_cblock iv;
  int i, n = 0;
  des_key_schedule ks1;
  des_key_schedule ks2;
  des_key_schedule ks3;
  SHA_CTX c;

  assert(key->length == 25);

  for (i = 0; i < 8; i++)
    iv[i] = 0;

  des_set_key((const_des_cblock *) (key->data + 1), ks1);
  des_set_key((const_des_cblock *) (key->data + 9), ks2);
  des_set_key((const_des_cblock *) (key->data+ 17), ks3);

  if (mdc) {
    mdc = 1;
    out->data[0] = 1;
  }
  rnd_bytes(out->data + mdc, 8);
  out->data[8 + mdc] = out->data[6 + mdc], out->data[9 + mdc] = out->data[7 + mdc];
  if (mdc) {
    SHA1_Init(&c);
    SHA1_Update(&c, out->data + 1, 10);
    SHA1_Update(&c, in->data, in->length);
  }
  n = 0;
  des_ede3_cfb64_encrypt(out->data + mdc, out->data + mdc, 10, ks1, ks2, ks3, &iv, &n,
			 ENCRYPT);
  if (!mdc) {
    iv[6] = iv[0], iv[7] = iv[1];
    memcpy(iv, out->data + 2, 6);
    n = 0;
  }
  des_ede3_cfb64_encrypt(in->data, out->data + 10 + mdc, in->length, ks1, ks2, ks3,
			 &iv, &n, ENCRYPT);
  if (mdc) {
    SHA1_Update(&c, "\xD3\x14", 2); /* 0xD3 = 0xC0 | PGP_MDC */
    des_ede3_cfb64_encrypt("\xD3\x14", out->data + 11 + in->length, 2, ks1, ks2, ks3,
		       &iv, &n, ENCRYPT);
    SHA1_Final(out->data + 13 + in->length, &c);
    des_ede3_cfb64_encrypt(out->data + 13 + in->length, out->data + 13 + in->length, 20, ks1, ks2, ks3,
		       &iv, &n, ENCRYPT);
  }
  return (0);
}

static int pgp_castencrypt(BUFFER *in, BUFFER *out, BUFFER *key, int mdc)
{
  byte iv[8];
  int i, n = 0;
  CAST_KEY ks;
  SHA_CTX c;

  assert(key->length == 17);

  for (i = 0; i < 8; i++)
    iv[i] = 0;

  CAST_set_key(&ks, 16, key->data + 1);

  if (mdc) {
    mdc = 1;
    out->data[0] = 1;
  }
  rnd_bytes(out->data + mdc, 8);
  out->data[8 + mdc] = out->data[6 + mdc], out->data[9 + mdc] = out->data[7 + mdc];
  if (mdc) {
    SHA1_Init(&c);
    SHA1_Update(&c, out->data + 1, 10);
    SHA1_Update(&c, in->data, in->length);
  }
  n = 0;
  CAST_cfb64_encrypt(out->data + mdc, out->data + mdc, 10, &ks, iv, &n, CAST_ENCRYPT);
  if (!mdc) {
    iv[6] = iv[0], iv[7] = iv[1];
    memcpy(iv, out->data + 2, 6);
    n = 0;
  }
  CAST_cfb64_encrypt(in->data, out->data + 10 + mdc, in->length, &ks, iv, &n,
		     CAST_ENCRYPT);
  if (mdc) {
    SHA1_Update(&c, "\xD3\x14", 2); /* 0xD3 = 0xC0 | PGP_MDC */
    CAST_cfb64_encrypt("\xD3\x14", out->data + 11 + in->length, 2, &ks, iv, &n,
		       CAST_ENCRYPT);
    SHA1_Final(out->data + 13 + in->length, &c);
    CAST_cfb64_encrypt(out->data + 13 + in->length, out->data + 13 + in->length, 20, &ks, iv, &n,
		       CAST_ENCRYPT);
  }
  return (0);
}

static int pgp_bfencrypt(BUFFER *in, BUFFER *out, BUFFER *key, int mdc)
{
  byte iv[8];
  int i, n = 0;
  BF_KEY ks;
  SHA_CTX c;

  assert(key->length == 17);

  for (i = 0; i < 8; i++)
    iv[i] = 0;

  BF_set_key(&ks, 16, key->data + 1);

  if (mdc) {
    mdc = 1;
    out->data[0] = 1;
  }
  rnd_bytes(out->data + mdc, 8);
  out->data[8 + mdc] = out->data[6 + mdc], out->data[9 + mdc] = out->data[7 + mdc];
  if (mdc) {
    SHA1_Init(&c);
    SHA1_Update(&c, out->data + 1, 10);
    SHA1_Update(&c, in->data, in->length);
  }
  n = 0;
  BF_cfb64_encrypt(out->data + mdc, out->data + mdc, 10, &ks, iv, &n, BF_ENCRYPT);
  if (!mdc) {
    iv[6] = iv[0], iv[7] = iv[1];
    memcpy(iv, out->data + 2, 6);
    n = 0;
  }
  BF_cfb64_encrypt(in->data, out->data + 10 + mdc, in->length, &ks, iv, &n,
		     BF_ENCRYPT);
  if (mdc) {
    SHA1_Update(&c, "\xD3\x14", 2); /* 0xD3 = 0xC0 | PGP_MDC */
    BF_cfb64_encrypt("\xD3\x14", out->data + 11 + in->length, 2, &ks, iv, &n,
		       BF_ENCRYPT);
    SHA1_Final(out->data + 13 + in->length, &c);
    BF_cfb64_encrypt(out->data + 13 + in->length, out->data + 13 + in->length, 20, &ks, iv, &n,
		       BF_ENCRYPT);
  }
  return (0);
}

#ifdef USE_AES
static int pgp_aesencrypt(BUFFER *in, BUFFER *out, BUFFER *key, int mdc)
{
  byte iv[16];
  int i, n = 0;
  AES_KEY ks;
  SHA_CTX c;

  assert(key->length == 17 || key->length == 25 || key->length == 33);

  for (i = 0; i < 16; i++)
    iv[i] = 0;

  AES_set_encrypt_key(key->data + 1, (key->length-1)<<3, &ks);

  if (mdc) {
    mdc = 1;
    out->data[0] = 1;
  }
  rnd_bytes(out->data + mdc, 16);
  out->data[16 + mdc] = out->data[14 + mdc], out->data[17 + mdc] = out->data[15 + mdc];
  if (mdc) {
    SHA1_Init(&c);
    SHA1_Update(&c, out->data + 1, 18);
    SHA1_Update(&c, in->data, in->length);
  }
  n = 0;
  AES_cfb128_encrypt(out->data + mdc, out->data + mdc, 18, &ks, iv, &n, AES_ENCRYPT);
  if (!mdc) {
    iv[14] = iv[0], iv[15] = iv[1];
    memcpy(iv, out->data + 2, 14);
    n = 0;
  }
  AES_cfb128_encrypt(in->data, out->data + 18 + mdc, in->length, &ks, iv, &n,
		     AES_ENCRYPT);
  if (mdc) {
    SHA1_Update(&c, "\xD3\x14", 2); /* 0xD3 = 0xC0 | PGP_MDC */
    AES_cfb128_encrypt("\xD3\x14", out->data + 19 + in->length, 2, &ks, iv, &n,
		       AES_ENCRYPT);
    SHA1_Final(out->data + 21 + in->length, &c);
    AES_cfb128_encrypt(out->data + 21 + in->length, out->data + 21 + in->length, 20, &ks, iv, &n,
		       AES_ENCRYPT);
  }
  return (0);
}
#endif /* USE_AES */

int pgp_symmetric(BUFFER *in, BUFFER *key, int mdc)
{
  BUFFER *out;
  int sym;

  out = buf_new();
  if (pgp_blocklen(sym = buf_getc(key)) > 8)
    mdc = 1; /* force MDC for AES */
  buf_prepare(out, in->length + (mdc?(1+2+22):2) + pgp_blocklen(sym));
  switch (sym) {
#ifdef USE_IDEA
   case PGP_K_IDEA:
    pgp_ideaencrypt(in, out, key, mdc);
    break;
#endif /* USE_IDEA */
#ifdef USE_AES
   case PGP_K_AES128:
   case PGP_K_AES192:
   case PGP_K_AES256:
    pgp_aesencrypt(in, out, key, mdc);
    break;
#endif /* USE_AES */
   case PGP_K_3DES:
    pgp_3desencrypt(in, out, key, mdc);
    break;
  case PGP_K_CAST5:
    pgp_castencrypt(in, out, key, mdc);
    break;
  case PGP_K_BF:
    pgp_bfencrypt(in, out, key, mdc);
    break;
   default:
    errlog(ERRORMSG, "Unknown symmetric algorithm.\n");
  }
  pgp_packet(out, mdc?PGP_ENCRYPTEDMDC:PGP_ENCRYPTED);

  buf_move(in, out);
  buf_free(out);
  return (0);
}

int pgp_literal(BUFFER *b, char *filename, int text)
{
  BUFFER *out;
  BUFFER *line;

  if (filename == NULL)
    filename = "stdin";

  if (strlen(filename) > 255)
    return (-1);

  out = buf_new();
  line = buf_new();

  if (text)
    buf_setc(out, 't');
  else
    buf_setc(out, 'b');
  buf_appendc(out, strlen(filename));
  buf_appends(out, filename);
  buf_appendl(out, 0);		/* timestamp */

  if (b->length > 0) {
    if (text)
      while (buf_getline(b, line) != -1) {
	buf_cat(out, line);
	buf_appends(out, "\r\n");
      } else
	buf_cat(out, b);
  }
  pgp_packet(out, PGP_LITERAL);
  buf_move(b, out);
  buf_free(out);
  buf_free(line);

  return (0);
}

int pgp_compress(BUFFER *in)
{
  int err;
  BUFFER *out;

  out = buf_new();
  buf_setc(out, 1);
  err = buf_zip(out, in, 13);
  if (err == 0) {
    pgp_packet3(out, PGP_COMPRESSED);
    buf_move(in, out);
  }
  buf_free(out);
  return (err);
}

int pgp_sessionkey(BUFFER *out, BUFFER *user, BUFFER *keyid, BUFFER *seskey,
		   char *pubring)
{
  BUFFER *encrypt, *key, *id;
  int algo, sym, err = -1;
  int i, csum = 0;
  int tempbuf = 0;

  encrypt = buf_new();
  key = buf_new();
  id = buf_new();
  if (keyid == NULL) {
    keyid = buf_new();
    tempbuf = 1;
  }
  sym = seskey->data[0];
  if ((algo = pgpdb_getkey(PK_ENCRYPT, PGP_ANY, &sym, NULL, NULL, key, user, NULL, keyid,
			   pubring, NULL)) == -1)
    goto end;

  buf_setc(out, 3);		/* type */
  buf_cat(out, keyid);
  buf_appendc(out, algo);	/* algorithm */

  buf_set(encrypt, seskey);

  for (i = 1; i < encrypt->length; i++)
    csum = (csum + encrypt->data[i]) % 65536;
  buf_appendi(encrypt, csum);

  switch (algo) {
  case PGP_ES_RSA:
    err = pgp_rsa(encrypt, key, PK_ENCRYPT);
    mpi_put(out, encrypt);
    break;
   case PGP_E_ELG:
    err = pgp_elgencrypt(encrypt, key);
    buf_cat(out, encrypt);
    break;
  default:
    errlog(NOTICE, "Unknown encryption algorithm.\n");
    err = -1;
    goto end;
  }
  if (err == -1) {
    errlog(ERRORMSG, "Encryption failed!\n");
    goto end;
  }
  pgp_packet(out, PGP_SESKEY);
end:
  if (tempbuf)
    buf_free(keyid);
  buf_free(id);
  buf_free(encrypt);
  buf_free(key);
  return (err);
}

void pgp_marker(BUFFER *out)
{
  buf_clear(out);
  buf_append(out, "PGP", 3);
  pgp_packet(out, PGP_MARKER);
}

int pgp_symsessionkey(BUFFER *out, BUFFER *seskey, BUFFER *pass)
{
  BUFFER *key;
  int sym;
  key = buf_new();

  sym = seskey->data[0];
  buf_setc(out, 4); /* version */
#ifdef MIMICPGP5
  pgp_makesk(out, key, sym, 1, PGP_H_MD5, pass);
#else /* end of MIMICPGP5 */
  pgp_makesk(out, key, sym, 3, PGP_H_SHA1, pass);
#endif /* else if not MIMICPGP5 */
  if (seskey->length > 1)
    buf_cat(out, seskey);
  else {
    buf_setc(seskey, sym);
    buf_cat(seskey, key);
  }
  pgp_packet(out, PGP_SYMSESKEY);
  buf_free(key);
  return (0);
}

int pgp_digest(int hashalgo, BUFFER *in, BUFFER *d)
{
  switch (hashalgo) {
   case PGP_H_MD5:
    digest_md5(in, d);
    return (0);
   case PGP_H_SHA1:
    digest_sha1(in, d);
    return (0);
  case PGP_H_RIPEMD:
    digest_rmd160(in, d);
    return (0);
   default:
    return (-1);
  }
}

int asnprefix(BUFFER *b, int hashalgo)
{
  switch (hashalgo) {
  case PGP_H_MD5:
    buf_append(b, MD5PREFIX, sizeof(MD5PREFIX) - 1);
    return (0);
  case PGP_H_SHA1:
    buf_append(b, SHA1PREFIX, sizeof(SHA1PREFIX) - 1);
    return (0);
  default:
    return (-1);
  }
}

int pgp_expandsk(BUFFER *key, int skalgo, int hashalgo, BUFFER *data)
{
  BUFFER *temp;
  int keylen;
  int err = 0;
  temp = buf_new();

  keylen = pgp_keylen(skalgo);
  buf_clear(key);
  while (key->length < keylen) {
    if (pgp_digest(hashalgo, data, temp) == -1) {
      err = -1;
      goto end;
    }
    buf_cat(key, temp);

    buf_setc(temp, 0);
    buf_cat(temp, data);
    buf_move(data, temp);
  }

  if (key->length > keylen) {
    buf_set(temp, key);
    buf_get(temp, key, keylen);
  }
 end:
  buf_free(temp);
  return(err);
}

int pgp_makesk(BUFFER *out, BUFFER *key, int sym, int type, int hash,
	       BUFFER *pass)
{
  int err = 0;
  BUFFER *salted;
  salted = buf_new();

  buf_appendc(out, sym);
  buf_appendc(out, type);
  buf_appendc(out, hash);
  switch (type) {
  case 0:
    buf_set(salted, pass);
    break;
  case 1:
    buf_appendrnd(salted, 8); /* salt */
    buf_cat(out, salted);
    buf_cat(salted, pass);
    break;
  case 3:
    buf_appendrnd(salted, 8); /* salt */
    buf_cat(out, salted);
    buf_appendc(out, 96); /* encoded count value 65536 */
    pgp_iteratedsk(salted, salted, pass, 96);
    break;
  default:
    err = -1;
  }
  pgp_expandsk(key, sym, hash, salted);
  buf_free(salted);
  return (err);
}

/* PGP/MIME needs to know the hash algorithm */
int pgp_signhashalgo(BUFFER *algo, BUFFER *userid, char *secring, BUFFER *pass)
{
  int pkalgo;

  pkalgo = pgpdb_getkey(PK_SIGN, PGP_ANY, NULL, NULL, NULL, NULL, userid, NULL, NULL,
			secring, pass);
  if (pkalgo == PGP_S_DSA)
    buf_sets(algo, "sha1");
  if (pkalgo == PGP_ES_RSA)
    buf_sets(algo, "md5");
  return (pkalgo > 0 ? 0 : -1);
}

int pgp_sign(BUFFER *msg, BUFFER *msg2, BUFFER *sig, BUFFER *userid,
	     BUFFER *pass, int type, int self, long now, int remail,
	     BUFFER *keypacket, char *secring)
/*  msg:      data to be signed (buffer is modified)
    msg2:     additional data to be signed for certain sig types
    sig:      signature is placed here
    userid:   select signing key
    pass:     pass phrase for signing key
    type:     PGP signature type
    self:     is this a self-signature?
    now:      time of signature creation
    remail:   is this an anonymous message?
    keypacket: signature key
    secring:   key ring with signature key */
{
  BUFFER *key, *id, *d, *sub, *enc;
  int algo, err = -1;
  int version = 3, hashalgo;
  int type1;

  id = buf_new();
  d = buf_new();
  sub = buf_new();
  enc = buf_new();
  key = buf_new();

  if (now == 0) {
    now = time(NULL);
    if (remail)
      now -= rnd_number(4 * 24 * 60 * 60);
  }
  if (keypacket) {
    buf_rewind(keypacket);
    algo = pgp_getkey(PK_SIGN, PGP_ANY, NULL, NULL, NULL, keypacket, key, id, NULL, pass);
  } else
    algo = pgpdb_getkey(PK_SIGN, PGP_ANY, NULL, NULL, NULL, key, userid, NULL, id, secring,
			pass);
  if (algo <= -1) {
    err = algo;
    goto end;
  }
  if (algo == PGP_S_DSA || algo == PGP_E_ELG)
    version = 4;
  if (version == 3)
    hashalgo = PGP_H_MD5;
  else
    hashalgo = PGP_H_SHA1;

  if (!self && type != PGP_SIG_BINDSUBKEY)
    version = 3;

  switch (type) {
   case PGP_SIG_CERT:
   case PGP_SIG_CERT1:
   case PGP_SIG_CERT2:
   case PGP_SIG_CERT3:
     type1 = pgp_getpacket(msg, d) == PGP_PUBKEY;
     assert (type1);
     buf_setc(msg, 0x99);
     buf_appendi(msg, d->length);
     buf_cat(msg, d);

     pgp_getpacket(msg2, d);
     switch (version) {
     case 3:
       buf_cat(msg, d);
       break;
     case 4:
       buf_appendc(msg, 0xb4);
       buf_appendl(msg, d->length);
       buf_cat(msg, d);
       break;
     }
     break;
   case PGP_SIG_BINDSUBKEY:
     type1 = pgp_getpacket(msg, d) == PGP_PUBKEY;
     assert (type1);
     buf_clear(msg);
     buf_appendc(msg, 0x99);
     buf_appendi(msg, d->length);
     buf_cat(msg, d);

     type1 = pgp_getpacket(msg2, d) == PGP_PUBSUBKEY;
     assert (type1);
     buf_appendc(msg, 0x99);
     buf_appendi(msg, d->length);
     buf_cat(msg, d);
     break;
   case PGP_SIG_BINARY:
     break;
   case PGP_SIG_CANONIC:
    pgp_sigcanonic(msg);
    break;
   default:
    NOT_IMPLEMENTED;
  }
  switch (version) {
   case 3:
    buf_set(d, msg);
    buf_appendc(d, type);
    buf_appendl(d, now);
    pgp_digest(hashalgo, d, d);
    if (algo == PGP_ES_RSA)
      asnprefix(enc, hashalgo);
    buf_cat(enc, d);
    err = pgp_dosign(algo, enc, key);

    buf_setc(sig, version);
    buf_appendc(sig, 5);
    buf_appendc(sig, type);
    buf_appendl(sig, now);
    buf_cat(sig, id);
    buf_appendc(sig, algo);
    buf_appendc(sig, hashalgo);
    buf_append(sig, d->data, 2);
    buf_cat(sig, enc);
    break;

   case 4:
    buf_setc(sig, version);
    buf_appendc(sig, type);
    buf_appendc(sig, algo);
    buf_appendc(sig, hashalgo);

    buf_clear(d);
    buf_appendl(d, now);
    pgp_subpacket(d, PGP_SUB_CREATIME);
    buf_cat(sub, d);

    if (self || type == PGP_SIG_BINDSUBKEY) {
      /* until we can handle the case where our pgp keys expire, don't create keys that expire */
      if (0 && KEYLIFETIME) { /* add key expirtaion time */
	buf_clear(d);
	buf_appendl(d, KEYLIFETIME);
	pgp_subpacket(d, PGP_SUB_KEYEXPIRETIME);
	buf_cat(sub, d);
      }
    }

    if (self) {
      buf_setc(d, PGP_K_CAST5);
#ifdef USE_AES
      buf_appendc(d, PGP_K_AES128);
#endif /* USE_AES */
      buf_appendc(d, PGP_K_3DES);
      pgp_subpacket(d, PGP_SUB_PSYMMETRIC);
      buf_cat(sub, d);

      buf_setc(d, 0x01); /* now we support MDC, so we can add MDC flag */
      pgp_subpacket(d, PGP_SUB_FEATURES);
      buf_cat(sub, d);
    }

    buf_appendi(sig, sub->length); /* hashed subpacket length */
    buf_cat(sig, sub);

    /* compute message digest */
    buf_set(d, msg);
    buf_cat(d, sig);
    buf_appendc(d, version);
    buf_appendc(d, 0xff);
    buf_appendl(d, sig->length);
    pgp_digest(hashalgo, d, d);

    pgp_subpacket(id, PGP_SUB_ISSUER);
    buf_appendi(sig, id->length); /* unhashed subpacket length */
    buf_cat(sig, id);

    buf_append(sig, d->data, 2);

    if (algo == PGP_ES_RSA)
      asnprefix(enc, hashalgo);
    buf_cat(enc, d);
    err = pgp_dosign(algo, enc, key);
    buf_cat(sig, enc);
    break;
  }
  pgp_packet(sig, PGP_SIG);

end:
  buf_free(key);
  buf_free(id);
  buf_free(d);
  buf_free(sub);
  buf_free(enc);
  return (err);
}

int pgp_pubkeycert(BUFFER *userid, char *keyring, BUFFER *pass,
		   BUFFER *out, int remail)
{
  BUFFER *key;
  KEYRING *r;
  int err = -1;

  key = buf_new();
  r = pgpdb_open(keyring, pass, 0, PGP_TYPE_UNDEFINED);
  if (r != NULL)
    while (pgpdb_getnext(r, key, NULL, userid) != -1) {
      if (pgp_makepubkey(key, NULL, out, pass, 0) != -1)
	err = 0;
    }
  if (err == 0)
    pgp_armor(out, remail);
  else
    buf_clear(out);
  buf_free(key);
  return (err);
}

#endif /* USE_PGP */
