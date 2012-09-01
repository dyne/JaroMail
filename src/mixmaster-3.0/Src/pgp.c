/* Mixmaster version 3.0  --  (C) 1999 - 2006 Anonymizer Inc. and others.

   Mixmaster may be redistributed and modified under certain conditions.
   This software is distributed on an "AS IS" basis, WITHOUT WARRANTY OF
   ANY KIND, either express or implied. See the file COPYRIGHT for
   details.

   OpenPGP messages
   $Id: pgp.c 934 2006-06-24 13:40:39Z rabbi $ */


#include "mix3.h"
#ifdef USE_PGP
#include "pgp.h"
#include <ctype.h>
#include <string.h>

int pgp_decrypt(BUFFER *in, BUFFER *pass, BUFFER *sig, char *pubring,
		char *secring)
{
  BUFFER *key;
  int err;

  key = buf_new();
  if (pass)
    buf_set(key, pass);
  if (!pgp_ispacket(in))
    pgp_dearmor(in, in);
  err = pgp_getmsg(in, key, sig, pubring, secring);
  buf_free(key);
  return (err);
}

static void appendaddr(BUFFER *to, BUFFER *addr)
{
  if (bufifind(addr, "<")) {
    for (addr->ptr = 0; addr->ptr < addr->length; addr->ptr++)
      if (addr->data[addr->ptr] == '<') {
	buf_rest(to, addr);
	break;
      }
  } else {
    buf_appendc(to, '<');
    buf_cat(to, addr);
    buf_appendc(to, '>');
  }
  buf_nl(to);
  buf_clear(addr);
}

int pgp_mailenc(int mode, BUFFER *msg, char *sigid,
		BUFFER *pass, char *pubring, char *secring)
{
  BUFFER *hdr, *body, *line, *uid, *field, *content;
  int err = -1;

  hdr = buf_new();
  body = buf_new();
  line = buf_new();
  uid = buf_new();
  field = buf_new();
  content = buf_new();

  buf_appendc(uid, '<');
  buf_appends(uid, sigid);
  if (sigid[strlen(sigid) - 1] != '@')
    buf_appendc(uid, '>');

  while (buf_getline(msg, line) == 0)
    buf_cat(hdr, line), buf_nl(hdr);

  if ((mode & PGP_SIGN) && !(mode & PGP_ENCRYPT))
    while (buf_getheader(hdr, field, content) == 0)
      if (bufileft(field, "content-") || bufieq(field, "mime-version")) {
	/* Is MIME message */
	err = pgpmime_sign(msg, uid, pass, secring);
	goto end;
      }

  buf_rest(body, msg);

  if ((mode & PGP_SIGN) && !(mode & PGP_ENCRYPT)) {
    err = pgp_signtxt(body, uid, pass, secring, mode & PGP_REMAIL);
  }

  if (mode & PGP_ENCRYPT) {
    BUFFER *plainhdr, *encrhdr, *to, *addr;
    int encapsulate = 0;

    plainhdr = buf_new();
    encrhdr = buf_new();
    to = buf_new();
    addr = buf_new();
    while (buf_getheader(hdr, field, content) == 0) {
      if (bufieq(field, "to") || bufieq(field, "cc") || bufieq(field, "bcc")) {
	buf_appendheader(plainhdr, field, content);
	rfc822_addr(content, addr);
	while (buf_getline(addr, content) != -1)
	  appendaddr(to, content);
      } else
	buf_appendheader(encrhdr, field, content);
    }
#if 1
    /* encrypt the headers */
    buf_appends(plainhdr, "Subject: PGP encrypted message\n");
    if (encrhdr->length) {
      buf_nl(encrhdr);
      buf_cat(encrhdr, body);
      buf_move(body, encrhdr);
      encapsulate = 1;
    }
#else /* end of 1 */
    /* send headers as plain text */
    buf_cat(plainhdr, encrhdr);
#endif /* not 1 */
    buf_move(hdr, plainhdr);

    buf_clear(line);
    if (encapsulate)
      buf_sets(line, "Content-Type: message/rfc822\n");
    else if (strlen(DEFLTENTITY))
      buf_setf(line, "Content-Type: %s\n", DEFLTENTITY);
    buf_nl(line);
    buf_cat(line, body);
    buf_move(body, line);

    /* Use the user keyring if pubring == NULL */
    err = pgp_encrypt(mode, body, to, uid, pass,
	              pubring ? pubring : PGPPUBRING, secring);
    buf_free(plainhdr);
    buf_free(encrhdr);
    buf_free(to);
    buf_free(addr);
  }
  if (err == 0) {
    if (mode & PGP_ENCRYPT) {
#if 1
      buf_sets(field, "+--");
#else /* end of 1 */
    buf_setrnd(mboundary, 18);
    encode(mboundary, 0);
#endif /* else if not 1 */

      buf_appendf(hdr,
		  "Content-Type: multipart/encrypted; boundary=\"%b\"; "
		  "protocol=\"application/pgp-encrypted\"\n\n"
		  "--%b\n"
		  "Content-Type: application/pgp-encrypted\n\n"
		  "Version: 1\n\n"
		  "--%b\n"
		  "Content-Type: application/octet-stream\n",
		  field, field, field);
      buf_appendf(body, "\n--%b--\n", field);
    }
    buf_move(msg, hdr);
    buf_nl(msg);
    buf_cat(msg, body);
  }
 end:
  buf_free(hdr);
  buf_free(body);
  buf_free(line);
  buf_free(uid);
  buf_free(field);
  buf_free(content);
  return (err);
}

static void pgp_setkey(BUFFER *key, int algo)
{
  buf_setc(key, algo);
  buf_appendrnd(key, pgp_keylen(algo));
}

int pgp_encrypt(int mode, BUFFER *in, BUFFER *to, BUFFER *sigid,
		BUFFER *pass, char *pubring, char *secring)
{
  BUFFER *dek, *out, *sig, *dest, *tmp;
  int err = 0, sym = PGP_K_ANY, mdc = 0;
  int text;

  out = buf_new();
  tmp = buf_new();
  dek = buf_new();
  sig = buf_new();
  dest = buf_new();

  text = mode & PGP_TEXT ? 1 : 0;

  if (mode & (PGP_CONV3DES | PGP_CONVCAST))
    mode |= PGP_NCONVENTIONAL;

  if (mode & PGP_SIGN) {
    err = pgp_sign(in, NULL, sig, sigid, pass, text, 0, 0,
		   mode & PGP_REMAIL ? 1 : 0, NULL, secring);
    if (err < 0)
      goto end;
    if (mode & PGP_DETACHEDSIG) {
      buf_move(in, sig);
      if (!(mode & PGP_NOARMOR))
	pgp_armor(in, PGP_ARMOR_NYMSIG);
      goto end;
    }
  }
  if (mode & PGP_ENCRYPT) {
    err = buf_getline(to, dest);
    if (err == -1)
      goto end;
    if (to->ptr == to->length) {
      if ((err = pgpdb_getkey(PK_ENCRYPT, PGP_ANY, &sym, &mdc, NULL, NULL, dest, NULL,
			      NULL, pubring, NULL)) < 0)
	goto end;
      pgp_setkey(dek, sym);
      err = pgp_sessionkey(out, dest, NULL, dek, pubring);
#ifdef USE_IDEA
      if (err < 0 && dek->data[0] == PGP_K_IDEA) {
	pgp_setkey(dek, PGP_K_3DES);
	err = pgp_sessionkey(out, dest, NULL, dek, pubring);
      }
#endif /* USE_IDEA */
    } else {
      /* multiple recipients */
      pgp_setkey(dek, PGP_K_3DES);
      buf_rewind(to);
      while (buf_getline(to, dest) != -1)
	if (dest->length) {
	  err = pgp_sessionkey(tmp, dest, NULL, dek, pubring);
#ifdef USE_IDEA
	  if (err < 0 && dek->data[0] != PGP_K_IDEA) {
	    buf_rewind(to);
	    buf_clear(out);
	    pgp_setkey(dek, PGP_K_IDEA);
	    continue;
	  }
#endif /* USE_IDEA */
	  if (err < 0)
	    goto end;
	  buf_cat(out, tmp);
	}
    }
  } else if (mode & PGP_NCONVENTIONAL) {
    /* genereate DEK in pgp_symsessionkey */
    buf_setc(dek, mode & PGP_CONVCAST ? PGP_K_CAST5 : PGP_K_3DES);
    pgp_marker(out);
    err = pgp_symsessionkey(tmp, dek, to);
    buf_cat(out, tmp);
  } else if (mode & PGP_CONVENTIONAL) {
    digest_md5(to, tmp);
    buf_setc(dek, PGP_K_IDEA);
    buf_cat(dek, tmp);
  }

  pgp_literal(in, NULL, text);
  if (sig->length) {
    buf_cat(sig, in);
    buf_move(in, sig);
  }
  pgp_compress(in);
  if (mode & (PGP_ENCRYPT | PGP_CONVENTIONAL | PGP_NCONVENTIONAL))
    pgp_symmetric(in, dek, mdc);
  if (mode & (PGP_ENCRYPT | PGP_NCONVENTIONAL)) {
    buf_cat(out, in);
    buf_move(in, out);
  }
  if (!(mode & PGP_NOARMOR))
    pgp_armor(in, (mode & PGP_REMAIL) ? PGP_ARMOR_REM : PGP_ARMOR_NORMAL);

end:
  buf_free(out);
  buf_free(tmp);
  buf_free(dek);
  buf_free(sig);
  buf_free(dest);
  return (err);
}

#define POLY 0X1864CFB

unsigned long crc24(BUFFER * in)
{
  unsigned long crc = 0xB704CE;
  long p;
  int i;

#if 0
  /* CRC algorithm from RFC 2440 */
  for (p = 0; p < in->length; p++) {
    crc ^= in->data[p] << 16;
    for (i = 0; i < 8; i++) {
      crc <<= 1;
      if (crc & 0x1000000)
	crc ^= POLY;
    }
  }
#else
  /* pre-computed CRC table -- much faster */
  unsigned long table[256];
  unsigned long t;
  int q = 0;

  table[0] = 0;
  for (i = 0; i < 128; i++) {
    t = table[i] << 1;
    if (t & 0x1000000) {
      table[q++] = t ^ POLY;
      table[q++] = t;
    } else {
      table[q++] = t;
      table[q++] = t ^ POLY;
    }
  }
  for (p = 0; p < in->length; p++)
    crc = crc << 8 ^ table[(in->data[p] ^ crc >> 16) & 255];
#endif
  return crc & ((1<<24)-1);
}

/* ASCII armor */

int pgp_dearmor(BUFFER *in, BUFFER *out)
{
  BUFFER *line, *temp;
  int err = 0;
  int tempbuf = 0;
  unsigned long crc1, crc2;

  line = buf_new();
  temp = buf_new();

  if (in == out) {
    out = buf_new();
    tempbuf = 1;
  }
  do
    if (buf_getline(in, line) == -1) {
      err = -1;
      goto end;
    }
  while (!bufleft(line, begin_pgp)) ;

  while (buf_getheader(in, temp, line) == 0) ;	/* scan for empty line */

  err = decode(in, out);
  crc1 = crc24(out);
  err = buf_getline(in, line);
  if (line->length == 5 && line->data[0] == '=') {	/* CRC */
    line->ptr = 1;
    err = decode(line, temp);
    crc2 = (((unsigned long)temp->data[0])<<16) | (((unsigned long)temp->data[1])<<8) | temp->data[2];
    if (crc1 == crc2)
      err = buf_getline(in, line);
    else {
      errlog(NOTICE, "Message CRC does not match.\n");
      err = -1;
    }
  } else
    err = -1;
  if (err == 0 && bufleft(line, end_pgp))
    err = 0;
  else
    err = -1;

end:
  buf_free(temp);
  buf_free(line);

  if (tempbuf) {
    buf_move(in, out);
    buf_free(out);
  }
  return (err);
}

int pgp_armor(BUFFER *in, int mode)

/* mode = 1: remailer message    (PGP_ARMOR_REM)
 *        0: normal message,     (PGP_ARMOR_NORMAL)
 *        2: key                 (PGP_ARMOR_KEY)
 *        3: nym key             (PGP_ARMOR_NYMKEY)
 *        4: nym signature       (PGP_ARMOR_NYMSIG)
 *        5: secret key          (PGP_ARMOR_SECKEY)
 */

{
  BUFFER *out;
  unsigned long crc;

  crc = crc24(in);
  encode(in, 64);

  out = buf_new();
  if (mode == PGP_ARMOR_KEY || mode == PGP_ARMOR_NYMKEY)
    buf_sets(out, begin_pgpkey);
  else if (mode == PGP_ARMOR_NYMSIG)
    buf_sets(out, begin_pgpsig);
  else if (mode == PGP_ARMOR_SECKEY)
    buf_sets(out, begin_pgpseckey);
  else
    buf_sets(out, begin_pgpmsg);
  buf_nl(out);
#ifdef CLOAK
  if (mode == PGP_ARMOR_REM || mode == PGP_ARMOR_NYMKEY || mode == PGP_ARMOR_NYMSIG)
    buf_appends(out, "Version: N/A\n");
  else
#elif MIMIC /* end of CLOAK */
  if (mode == PGP_ARMOR_REM || mode == PGP_ARMOR_NYMKEY || mode == PGP_ARMOR_NYMSIG)
    buf_appends(out, "Version: 2.6.3i\n");
  else
#endif /* MIMIC */
  {
    buf_appends(out, "Version: Mixmaster ");
    buf_appends(out, VERSION);
    buf_appends(out, " (OpenPGP module)\n");
  }
  buf_nl(out);
  buf_cat(out, in);
  buf_reset(in);
  buf_appendc(in, (crc >> 16) & 255);
  buf_appendc(in, (crc >> 8) & 255);
  buf_appendc(in, crc & 255);
  encode(in, 0);
  buf_appendc(out, '=');
  buf_cat(out, in);
  buf_nl(out);
  if (mode == PGP_ARMOR_KEY || mode == PGP_ARMOR_NYMKEY)
    buf_appends(out, end_pgpkey);
  else if (mode == PGP_ARMOR_NYMSIG)
    buf_appends(out, end_pgpsig);
  else if (mode == PGP_ARMOR_SECKEY)
    buf_appends(out, end_pgpseckey);
  else
    buf_appends(out, end_pgpmsg);
  buf_nl(out);

  buf_move(in, out);
  buf_free(out);
  return (0);
}

int pgp_keygen(int algo, int bits, BUFFER *userid, BUFFER *pass, char *pubring,
	       char *secring, int remail)
{
  switch (algo) {
  case PGP_ES_RSA:
#ifndef USE_IDEA
    errlog(WARNING, "IDEA disabled: OpenPGP RSA key cannot be used for decryption!\n");
#endif
    return (pgp_rsakeygen(bits, userid, pass, pubring, secring, remail));
  case PGP_E_ELG:
    return (pgp_dhkeygen(bits, userid, pass, pubring, secring, remail));
  default:
    return -1;
  }
}

int pgp_signtxt(BUFFER *msg, BUFFER *uid, BUFFER *pass,
		char *secring, int remail)
{
  int err;
  BUFFER *line, *sig, *out;

  sig = buf_new();
  out = buf_new();
  line = buf_new();

  buf_appends(out, begin_pgpsigned);
  buf_nl(out);
  if (pgpdb_getkey(PK_SIGN, PGP_ANY, NULL, NULL, NULL, NULL, uid, NULL, NULL, secring, pass) == PGP_S_DSA)
    buf_appends(out, "Hash: SHA1\n");
  buf_nl(out);
  while (buf_getline(msg, line) != -1) {
    if (line->data[0] == '-')
      buf_appends(out, "- ");
    buf_cat(out, line);
    buf_nl(out);
  }
  buf_nl(out);

  buf_rewind(msg);
  err = pgp_encrypt(PGP_SIGN | PGP_DETACHEDSIG | PGP_TEXT |
		    (remail ? PGP_REMAIL : 0),
		    msg, NULL, uid, pass, NULL, secring);
  if (err == -1)
    goto end;
  buf_cat(out, msg);
  buf_move(msg, out);
end:
  buf_free(line);
  buf_free(sig);
  buf_free(out);
  return (err);
}

#endif /* USE_PGP */
