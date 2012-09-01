/* Mixmaster version 3.0  --  (C) 1999 - 2006 Anonymizer Inc. and others.

   Mixmaster may be redistributed and modified under certain conditions.
   This software is distributed on an "AS IS" basis, WITHOUT WARRANTY OF
   ANY KIND, either express or implied. See the file COPYRIGHT for
   details.

   OpenPGP data
   $Id: pgpdata.c 934 2006-06-24 13:40:39Z rabbi $ */


#include "mix3.h"
#ifdef USE_PGP
#include "pgp.h"
#include "crypto.h"
#include <assert.h>
#include <time.h>
#include <string.h>

int pgp_keylen(int symalgo)
{
  switch (symalgo) {
#ifdef USE_AES
  case PGP_K_AES256:
    return (32);
  case PGP_K_AES192:
    return (24);
  case PGP_K_AES128:
#endif /* USE_AES */
  case PGP_K_IDEA:
  case PGP_K_CAST5:
  case PGP_K_BF:
    return (16);
  case PGP_K_3DES:
    return (24);
  default:
    return (0);
  }
}

int pgp_blocklen(int symalgo)
{
  switch (symalgo) {
#ifdef USE_AES
  case PGP_K_AES256:
  case PGP_K_AES192:
  case PGP_K_AES128:
    return (16);
#endif /* USE_AES */
  case PGP_K_IDEA:
  case PGP_K_CAST5:
  case PGP_K_BF:
  case PGP_K_3DES:
    return (8);
  default:
    return (16);
  }
}

int mpi_get(BUFFER *b, BUFFER *mpi)
{
  int l;

  l = buf_geti(b);
  buf_clear(mpi);

  if (l <= 0 || b->ptr + (l + 7) / 8 > b->length)
    return (-1);
  buf_get(b, mpi, (l + 7) / 8);
  return (l);
}


int mpi_bitcount(BUFFER *mpi)
{
  int i, l;
  while (!mpi->data[0] && mpi->length) /* remove leading zeros from mpi */
    memmove(mpi->data, mpi->data+1, --mpi->length);
  l = mpi->length * 8;
  for (i = 7; i >= 0; i--)
    if (((mpi->data[0] >> i) & 1) == 1) {
      l -= 7 - i;
      break;
    }
  return l;
}

int mpi_put(BUFFER *b, BUFFER *mpi)
{
  buf_appendi(b, mpi_bitcount(mpi));
  buf_cat(b, mpi);
  return (0);
}

int skcrypt(BUFFER *data, int skalgo, BUFFER *key, BUFFER *iv, int enc)
{
  switch (skalgo) {
  case 0:
    return (0);
#ifdef USE_IDEA
  case PGP_K_IDEA:
    return (buf_ideacrypt(data, key, iv, enc));
#endif /* USE_IDEA */
#ifdef USE_AES
  case PGP_K_AES128:
  case PGP_K_AES192:
  case PGP_K_AES256:
    return (buf_aescrypt(data, key, iv, enc));
#endif /* USE_AES */
  case PGP_K_3DES:
    return (buf_3descrypt(data, key, iv, enc));
  case PGP_K_BF:
    return (buf_bfcrypt(data, key, iv, enc));
  case PGP_K_CAST5:
    return (buf_castcrypt(data, key, iv, enc));
  default:
    return (-1);
  }
}

int pgp_csum(BUFFER *key, int start)
{
  int i, csum = 0;
  for (i = start; i < key->length; i++)
    csum = (csum + key->data[i]) % 65536;
  return (csum);
}

int pgp_rsa(BUFFER *in, BUFFER *k, int mode)
{
  BUFFER *mpi, *out;
  int err = -1;
  RSA *key;

  assert(mode == PK_ENCRYPT || mode == PK_VERIFY || mode == PK_DECRYPT
	 || mode == PK_SIGN);
  key = RSA_new();
  out = buf_new();
  mpi = buf_new();

  mpi_get(k, mpi);
  key->n = BN_bin2bn(mpi->data, mpi->length, NULL);

  if (mpi_get(k, mpi) < 0)
    goto end;
  key->e = BN_bin2bn(mpi->data, mpi->length, NULL);

  if (mode == PK_DECRYPT || mode == PK_SIGN) {
    if (mpi_get(k, mpi) < 0)
      goto end;
    key->d = BN_bin2bn(mpi->data, mpi->length, NULL);

#if 1
    /* compute auxiluary parameters */
    mpi_get(k, mpi);		/* PGP'p is SSLeay's q */
    key->q = BN_bin2bn(mpi->data, mpi->length, NULL);

    mpi_get(k, mpi);
    key->p = BN_bin2bn(mpi->data, mpi->length, NULL);

    if (mpi_get(k, mpi) < 0)
      goto end;
    key->iqmp = BN_bin2bn(mpi->data, mpi->length, NULL);

    {
      BIGNUM *i;
      BN_CTX *ctx;

      ctx = BN_CTX_new();
      i = BN_new();
      key->dmp1 = BN_new();
      key->dmq1 = BN_new();

      BN_sub(i, key->p, BN_value_one());
      BN_mod(key->dmp1, key->d, i, ctx);

      BN_sub(i, key->q, BN_value_one());
      BN_mod(key->dmq1, key->d, i, ctx);

      BN_free(i);
    }
#endif /* 1 */
  }
  buf_prepare(out, RSA_size(key));

  switch (mode) {
  case PK_ENCRYPT:
    out->length = RSA_public_encrypt(in->length, in->data, out->data, key,
				     RSA_PKCS1_PADDING);
    break;
  case PK_VERIFY:
    out->length = RSA_public_decrypt(in->length, in->data, out->data, key,
				     RSA_PKCS1_PADDING);
    break;
  case PK_SIGN:
    out->length = RSA_private_encrypt(in->length, in->data, out->data, key,
				      RSA_PKCS1_PADDING);
    break;
  case PK_DECRYPT:
    out->length = RSA_private_decrypt(in->length, in->data, out->data, key,
				      RSA_PKCS1_PADDING);
    break;
  }
  if (out->length == -1)
    err = -1, out->length = 0;
  else
    err = 0;

  buf_move(in, out);
end:
  RSA_free(key);
  buf_free(out);
  buf_free(mpi);
  return (err);
}

/* Contrary to RFC 2440, old PGP versions use this for clearsign only.
 * If the text is included in the OpenPGP message, the application will
 * typically provide the text in the proper format (whatever that is);
 * we use "canonic" format so everybody will be able to read our messages.
 * In clearsigned messages, trailing whitespace is always ignored.
 * Detached signatures are the problematic case. For PGP/MIME, we always
 * escape trailing whitespace as quoted-printable.
 */
void pgp_sigcanonic(BUFFER *msg)
{
  BUFFER *line, *out;

  out = buf_new();
  line = buf_new();

  while (buf_getline(msg, line) != -1) {
    while (line->length > 0 && (line->data[line->length - 1] == ' '
#if 0
				|| line->data[line->length - 1] == '\t'
#endif /* 0 */
	))
      line->length--;
    line->data[line->length] = '\0';
    buf_cat(out, line);
    buf_appends(out, "\r\n");
  }
  buf_move(msg, out);
  buf_free(out);
  buf_free(line);
}

static void mpi_bnput(BUFFER *o, BIGNUM *i)
{
  BUFFER *b;

  b = buf_new();
  buf_prepare(b, BN_num_bytes(i));
  b->length = BN_bn2bin(i, b->data);
  mpi_put(o, b);
  buf_free(b);
}

static void mpi_bnputenc(BUFFER *o, BIGNUM *i, int ska, BUFFER *key,
			 BUFFER *iv)
{
  BUFFER *b;
  int ivlen = iv->length;

  b = buf_new();
  buf_prepare(b, BN_num_bytes(i));
  b->length = BN_bn2bin(i, b->data);
  buf_appendi(o, mpi_bitcount(b));
  if (key && key->length) {
    skcrypt(b, ska, key, iv, ENCRYPT);
    buf_clear(iv);
    buf_append(iv, b->data+b->length-ivlen, ivlen);
  }
  buf_cat(o, b);
  buf_free(b);
}

static int getski(BUFFER *p, BUFFER *pass, BUFFER *key, BUFFER *iv)
{
  int skalgo;
  BUFFER *salt, *temp;

  if (!pass)
    return(-1);

  salt = buf_new();
  temp = buf_new();

  skalgo = buf_getc(p);
  switch (skalgo) {
  case 0:
    /* none */
    goto end;
  case 255:
    /* S2K specifier */
    skalgo = pgp_getsk(p, pass, key);
    break;
  default:
    /* simple */
    digest_md5(pass, key);
    break;
  }

  buf_get(p, iv, pgp_blocklen(skalgo));

 end:
  buf_free(salt);
  buf_free(temp);
  return (skalgo);
}

static void makeski(BUFFER *secret, BUFFER *pass, int remail)
{
  BUFFER *out, *key, *iv;
  out = buf_new();
  key = buf_new();
  iv = buf_new();
  if (pass == NULL || pass->length == 0 || remail == 2) {
    buf_appendc(out, 0);
    buf_cat(out, secret);
  } else {
    buf_appendc(out, 255);
    pgp_makesk(out, key, PGP_K_CAST5, 3, PGP_H_SHA1, pass);
    buf_setrnd(iv, pgp_blocklen(PGP_K_CAST5));
    buf_cat(out, iv);
    skcrypt(secret, PGP_K_CAST5, key, iv, 1);
    buf_cat(out, secret);
  }
  buf_move(secret, out);
  buf_free(out);
  buf_free(key);
  buf_free(iv);
}

int pgp_nummpi(int algo)
{
  switch (algo) {
   case PGP_ES_RSA:
    return (2);
   case PGP_S_DSA:
    return (4);
   case PGP_E_ELG:
    return (3);
   default:
    return (0);
  }
}

int pgp_numsecmpi(int algo)
{
  switch (algo) {
   case PGP_ES_RSA:
    return (4);
   case PGP_S_DSA:
    return (1);
   case PGP_E_ELG:
    return (1);
   default:
    return (0);
  }
}

/* store key's ID in keyid */
int pgp_keyid(BUFFER *key, BUFFER *keyid)
{
  BUFFER *i, *k;
  int version, algo, j, ptr;

  i = buf_new();
  k = buf_new();

  ptr = key->ptr;
  key->ptr = 0;
  switch (version = buf_getc(key)) {
  case 2:
  case 3:
    buf_getl(key);
    buf_geti(key);
    buf_getc(key);
    mpi_get(key, i);
    break;
  case 4:
    buf_appendc(k, version);
    buf_appendl(k, buf_getl(key));
    algo = buf_getc(key);
    buf_appendc(k, algo);
    if (pgp_nummpi(algo) == 0)
      buf_rest(k, key); /* works for public keys only */
    else
      for (j = 0; j < pgp_nummpi(algo); j++) {
	mpi_get(key, i);
	mpi_put(k, i);
      }
    buf_clear(i);
    buf_appendc(i, 0x99);
    buf_appendi(i, k->length);
    buf_cat(i, k);
    digest_sha1(i, i);
    break;
  }
  buf_clear(keyid);
  buf_append(keyid, i->data + i->length - 8, 8);
  buf_free(i);
  buf_free(k);
  key->ptr = ptr;
  return(0);
}

static int pgp_iskeyid(BUFFER *key, BUFFER *keyid)
{
  BUFFER *thisid;
  int ret;

  thisid = buf_new();
  pgp_keyid(key, thisid);
  ret = buf_eq(keyid, thisid);
  buf_free(thisid);
  return(ret);
}

static int pgp_get_sig_subpacket(BUFFER * p1, BUFFER *out)
{
  int suptype, len = buf_getc(p1);
  if (len > 192 && len < 255)
    len = (len - 192) * 256 + buf_getc(p1) + 192;
  else if (len == 255)
    len = buf_getl(p1);
  suptype = buf_getc(p1);
  if (len)
    buf_get(p1, out, len-1); /* len-1 - exclude type */
  else
    buf_clear(out);
  return suptype;
}

typedef struct _UIDD {
  struct _UIDD * next;
  long created, expires;
  int revoked, sym, mdc, uid, primary;
  BUFFER *uidstr;
} UIDD;

static UIDD * new_uidd_c(UIDD *uidd_c, int uidno)
{
  UIDD * tmp;

  if (!uidd_c || uidd_c->uid < uidno) {
    tmp = (UIDD *)malloc(sizeof(UIDD));
    if (!tmp)
	return uidd_c;
    if (uidd_c) {
      uidd_c->next = tmp;
      uidd_c = uidd_c->next;
    } else
      uidd_c = tmp;
    if (uidd_c) {
      memset(uidd_c, 0, sizeof(UIDD));
      uidd_c->uid = uidno;
    }
  }
  return uidd_c;
}

int pgp_getkey(int mode, int algo, int *psym, int *pmdc, long *pexpires, BUFFER *keypacket, BUFFER *key,
	       BUFFER *keyid, BUFFER *userid, BUFFER *pass)
/* IN:  mode   - PK_SIGN, PK_VERIFY, PK_DECRYPT, PK_ENCRYPT
 *	algo   - PGP_ANY, PGP_ES_RSA, PGP_E_ELG, PGP_S_DSA
 *	psym   - reyested sym PGP_K_ANY, PGP_K_IDEA, PGP_K_3DES, ... or NULL
 *	pass   - passprase or NULL
 *	keypacket - key, with key uid sig subkey packets, possibly encrypted
 *	keyid  - reyested (sub)keyid or empty buffer or NULL
 * OUT: psym   - found sym algo (or NULL)
 *	pmdc   - found mdc flag (or NULL)
 *	key    - found key, only key packet, decrypted
 *	           may be the same buffer as keypacket (or NULL)
 *	keyid  - found (sub)keyid (or NULL)
 *	userid - found userid (or NULL)
 *	pexpires - expiry time, or 0 if don't expire (or NULL)
 */
{
  int tempbuf = 0, dummykey = 0;
  int keytype = -1, type, j;
  int thisalgo = 0, version, skalgo;
  int needsym = 0, symfound = 0, mdcfound = 0;
  BUFFER *p1, *iv, *sk, *i, *thiskeyid, *mainkeyid;
  int ivlen;
  int csstart;
  long now = time(NULL);
  long created = 0, expires = 0, subexpires = 0;
  int uidno = 0, primary = 0, subkeyno = 0, subkeyok = 0;
  UIDD * uidd_1 = NULL, * uidd_c = NULL;

  p1 = buf_new();
  i = buf_new();
  iv = buf_new();
  sk = buf_new();
  thiskeyid = buf_new();
  mainkeyid = buf_new();
  if (psym)
    needsym = *psym;
  if (keypacket == key) {
    key = buf_new();
    tempbuf = 1;
  }
  if (! key) {
    key = buf_new();
    dummykey = 1;
  };
  if (userid)
    buf_clear(userid);

  while ((type = pgp_getpacket(keypacket, p1)) > 0) {
    switch (type) {
    case PGP_SIG:
    {
      /* it is assumed that only valid keys have been imported */
      long a;
      int self = 0, certexpires = 0, suptype;
      int sigtype = 0, sigver = buf_getc(p1);
      created = 0, expires = 0, primary = 0;
      if (sigver == 4) {
	 sigtype = buf_getc(p1);
	if (isPGP_SIG_CERT(sigtype) || sigtype == PGP_SIG_BINDSUBKEY || sigtype == PGP_SIG_CERTREVOKE) {
	  int revoked = (sigtype == PGP_SIG_CERTREVOKE), sym = PGP_K_3DES, mdc = 0;
	  buf_getc(p1); /* pk algo */
	  buf_getc(p1); /* hash algo */
	  j = buf_geti(p1); /* length of hashed signature subpackets */
	  j += p1->ptr;
	  while (p1->ptr < j) {
	    suptype = pgp_get_sig_subpacket(p1, i);
	    switch (suptype & 0x7F) {
	    case PGP_SUB_PSYMMETRIC:
	      while ((a = buf_getc(i)) != -1)
		if ((a == PGP_K_3DES || a == PGP_K_CAST5 || a == PGP_K_BF
#ifdef USE_IDEA
		     || a == PGP_K_IDEA
#endif /* USE_IDEA */
#ifdef USE_AES
		     || a ==  PGP_K_AES128 || a ==  PGP_K_AES192 || a ==  PGP_K_AES256
#endif /* USE_AES */
		     ) && (a == needsym || needsym == PGP_K_ANY)) {
		  sym = a;
		  break; /* while ((a = buf_getc(i)) != -1) */
		} /* if ((a == PGP_K_3DES)... */
	      break;
	    case PGP_SUB_FEATURES:
	      if ((a = buf_getc(i)) != -1)
		if (a & 0x01)
		  mdc = 1;
	      break;
	    case PGP_SUB_CREATIME:
	      if ((a = buf_getl(i)) != -1)
		created = a;
	      break;
	    case PGP_SUB_KEYEXPIRETIME:
	      if ((a = buf_getl(i)) != -1)
		expires = a;
	      break;
	    case PGP_SUB_CERTEXPIRETIME:
	      if ((a = buf_getl(i)) != -1)
		certexpires = a;
	      break;
	    case PGP_SUB_ISSUER: /* ISSUER normaly is in unhashed data, but check anyway */
	      if (i->length == 8)
		self = buf_eq(i, mainkeyid);
	      break;
	    case PGP_SUB_PRIMARY:
	      if ((a = buf_getl(i)) != -1)
		primary = a;
	      break;
	    default:
	      if (suptype & 0x80) {
		; /* "critical" bit set! now what? */
	      }
	    } /* switch (suptype) */
	  } /* while (p1->ptr < j) */
	  if (p1->ptr == j) {
	      j = buf_geti(p1); /* length of unhashed signature subpackets */
	      j += p1->ptr;
	      while (p1->ptr < j) {
		suptype = pgp_get_sig_subpacket(p1, i);
		if (suptype == PGP_SUB_ISSUER) {
		  if (i->length == 8)
		    self = buf_eq(i, mainkeyid);
		} /* if (suptype == PGP_SUB_ISSUER) */
	      } /* while (p1->ptr < j) #2 */
	  } /* if (p1->ptr == j) */
	  if (p1->ptr != j) /* sig damaged ? */
	    break; /* switch (type) */
	  if (self) {
	    if (certexpires)
		certexpires = ((created + certexpires < now) || (created + certexpires < 0));
	    if ((isPGP_SIG_CERT(sigtype) && !certexpires) || sigtype == PGP_SIG_CERTREVOKE) {
	      uidd_c = new_uidd_c(uidd_c, uidno);
	      if (!uidd_1)
		uidd_1 = uidd_c;
	      if (uidd_c && uidd_c->uid == uidno) {
		if (uidd_c->created <= created) {
		  /* if there is several selfsigs on that uid, find the newest one */
		  uidd_c->created = created;
		  uidd_c->expires = expires;
		  uidd_c->revoked = revoked;
		  uidd_c->primary = primary;
		  uidd_c->sym = sym;
		  uidd_c->mdc = mdc;
		}
	      }
	    } /* if ((isPGP_SIG_CERT(sigtype) && !certexpires) || sigtype == PGP_SIG_CERTREVOKE) */
	    else if (sigtype == PGP_SIG_BINDSUBKEY) {
	      if (!subkeyok) {
		subexpires = expires ? created + expires : 0;
		if (expires && ((created + expires < now) || (created + expires < 0))) {
		  if (mode == PK_ENCRYPT) { /* allow decrypt with expired subkeys, but not encrypt */
		    keytype = -1;
		  }
		}
		if (keytype != -1)
		  subkeyok = subkeyno;
	      }
	    } /* if (sigtype == PGP_SIG_BINDSUBKEY) */
	  } /* if (self) */
	} /* if (isPGP_SIG_CERT(sigtype) || sigtype == PGP_SIG_BINDSUBKEY || sigtype == PGP_SIG_CERTREVOKE) */
      } /* if (sigver == 4) */
      else if (sigver == 2 || sigver == 3) {
	buf_getc(p1); /* One-octet length of following hashed material.  MUST be 5 */
	sigtype = buf_getc(p1);
      } /* if (sigver == 2 || sigver == 3) */
      if (sigtype == PGP_SIG_KEYREVOKE) {
	/* revocation can be either v3 or v4. if v4 we could check issuer, but we don't do it... */
	if (mode == PK_SIGN || mode == PK_ENCRYPT) { /* allow verify and decrypt with revokeded keys, but not sign and encrypt */
	  keytype = -1;
	}
      } /* if (sigtype == PGP_SIG_KEYREVOKE) */
      else if (sigtype == PGP_SIG_SUBKEYREVOKE) {
      if (!subkeyok || subkeyok == subkeyno)
	  if (mode == PK_ENCRYPT) { /* allow decrypt with revokeded subkeys, but not encrypt */
	    keytype = -1;
	  }
      } /* if (sigtype == PGP_SIG_SUBKEYREVOKE) */
      break; /* switch (type) */
    } /* case PGP_SIG: */
    case PGP_USERID:
      uidno++;
      uidd_c = new_uidd_c(uidd_c, uidno);
      if (!uidd_1)
	uidd_1 = uidd_c;
      if (uidd_c && uidd_c->uid == uidno) {
	uidd_c->uidstr = buf_new();
	buf_set(uidd_c->uidstr, p1);
      }
      if (userid)
	buf_move(userid, p1);
      break;
    case PGP_PUBSUBKEY:
    case PGP_SECSUBKEY:
      subkeyno++;
      if (keytype != -1 && subkeyno > 1) {
	/* usable subkey already found, don't bother to check other */
	continue;
      }
      if (keytype != -1 && (mode == PK_SIGN || mode == PK_VERIFY))
	continue;
    case PGP_PUBKEY:
      if ((type == PGP_PUBKEY || type == PGP_PUBSUBKEY) &&
	  (mode == PK_DECRYPT || mode == PK_SIGN))
	continue;
    case PGP_SECKEY:
      if (type == PGP_PUBKEY || type == PGP_SECKEY)
	pgp_keyid(p1, mainkeyid);
      keytype = type;
      version = buf_getc(p1);
      switch (version) {
      case 2:
      case 3:
	created = buf_getl(p1);			/* created */
	expires = buf_geti(p1) * (24*60*60);	/* valid */
	if (uidno == 0) {
	  uidd_c = new_uidd_c(uidd_c, uidno);
	  if (!uidd_1)
	    uidd_1 = uidd_c;
	  if (uidd_c && uidd_c->uid == uidno) {
	    uidd_c->created = created;
	    uidd_c->expires = expires;
	    uidd_c->sym = PGP_K_IDEA;
	  }
	}
	thisalgo = buf_getc(p1);
	if (thisalgo != PGP_ES_RSA) {
	  keytype = -1;
	  goto end;
	}
	symfound = PGP_K_IDEA;
	mdcfound = 0;
	break;
      case 4:
	buf_appendc(key, version);
	buf_appendl(key, buf_getl(p1));
	thisalgo = buf_getc(p1);
	buf_appendc(key, thisalgo);
	if (symfound == 0)
	  symfound = PGP_K_3DES; /* default algorithm */
	break;
      default:
	keytype = -1;
	goto end;
      } /* switch (version) */
      if (algo != PGP_ANY && thisalgo != algo) {
	keytype = -1;
	continue;
      }
      if (keyid && keyid->length && !pgp_iskeyid(p1, keyid))
	continue;
      pgp_keyid(p1, thiskeyid);
      if (key) {
	buf_clear(key);
	for (j = 0; j < pgp_nummpi(thisalgo); j++) {
	  if (mpi_get(p1, i) == -1)
	    goto end;
	  mpi_put(key, i);
	}
	if (keytype == PGP_SECKEY || keytype == PGP_SECSUBKEY) {
	  csstart = key->length;
	  skalgo = getski(p1, pass, sk, iv);
	  switch (version) {
	   case 2:
	   case 3:
	    ivlen = pgp_blocklen(skalgo);
	    for (j = 0; j < pgp_numsecmpi(thisalgo); j++) {
	      unsigned char lastb[16];
	      if (mpi_get(p1, i) == -1) {
		keytype = -1;
		goto end;
	      }
	      assert(ivlen <= 16);
	      memcpy(lastb, i->data+i->length-ivlen, ivlen);
	      skcrypt(i, skalgo, sk, iv, DECRYPT);
	      buf_clear(iv);
	      buf_append(iv, lastb, ivlen);
	      mpi_put(key, i);
	    } /* for */
	    break; /* switch (version) */
	   case 4:
	    buf_clear(i);
	    buf_rest(i, p1);
	    skcrypt(i, skalgo, sk, iv, DECRYPT);
	    buf_move(p1, i);
	    for (j = 0; j < pgp_numsecmpi(thisalgo); j++) {
	      if (mpi_get(p1, i) == -1) {
		keytype = PGP_PASS;
		goto end;
	      }
	      mpi_put(key, i);
	    }
	    break;
	  } /* switch (version) */
	  if (pgp_csum(key, csstart) != buf_geti(p1)) {
	    keytype = PGP_PASS;
	    goto end;
	  }
	}
      } /* if (key) */
      break; /* switch (type) */
     default:
      /* ignore trust packets etc */
      break;
    } /* switch (type) */
  } /* while ((type = pgp_getpacket(keypacket, p1)) > 0) */
 end:
  if (keyid) buf_set(keyid, thiskeyid);
  if (tempbuf) {
    buf_move(keypacket, key);
    buf_free(key);
  }
  if (dummykey) {
    buf_free(key);
  }
  buf_free(p1);
  buf_free(i);
  buf_free(iv);
  buf_free(sk);
  buf_free(thiskeyid);
  buf_free(mainkeyid);

  if (uidd_1) {
    primary = 0;
    created = expires = 0;
    while (uidd_1) {
      /* find newest uid which is not revoked or expired */
      if (primary <= uidd_1->primary && created <= uidd_1->created && !uidd_1->revoked) {
	created = uidd_1->created;
	expires = uidd_1->expires;
	primary = uidd_1->primary;
	symfound = uidd_1->sym;
	mdcfound = uidd_1->mdc;
	if (userid && uidd_1->uidstr)
	  buf_set(userid, uidd_1->uidstr);
      }
      uidd_c = uidd_1;
      uidd_1 = uidd_1->next;
      if (uidd_c->uidstr)
	buf_free(uidd_c->uidstr);
      free(uidd_c);
    }
    if (expires && ((created + expires < now) || (created + expires < 0))) {
      if (mode == PK_SIGN || mode == PK_ENCRYPT) { /* allow verify and decrypt with expired keys, but not sign and encrypt */
	keytype = -1;
      }
    }
  } /* if (uidd_1) */
  expires = expires ? created + expires : 0;
  if (subexpires > 0 && expires > 0 && subexpires < expires)
    expires = subexpires;
  if (pexpires)
    *pexpires = expires;

  if (!subkeyok && keytype == PGP_E_ELG && (mode == PK_DECRYPT || mode == PK_ENCRYPT))
    keytype = -1; /* no usable subkey found, one with valid binding */

  if (needsym != PGP_K_ANY && needsym != symfound)
    keytype = -1;
  else if (psym && *psym == PGP_K_ANY)
    *psym = symfound;
  if (pmdc)
    *pmdc = mdcfound;

  return (keytype <= 0 ? keytype : thisalgo);
}

int pgp_makepkpacket(int type, BUFFER *p, BUFFER *outtxt, BUFFER *out,
		     BUFFER *key, BUFFER *pass, time_t *created)
{
  BUFFER *i, *id;
  char txt[LINELEN], algoid;
  int version, algo, valid = 0, err = 0;
  int len, j;
  struct tm *tc;

  i = buf_new();
  id = buf_new();

  version = buf_getc(p);
  buf_clear(key);
  switch (version) {
  case 2:
  case 3:
    *created = buf_getl(p);
    valid = buf_geti(p);
    algo = buf_getc(p);
    if (algo != PGP_ES_RSA)
      return(-1);
    break;
  case 4:
    *created = buf_getl(p);
    algo = buf_getc(p);
    break;
  default:
    return(-1);
  }

  switch (version) {
  case 2:
  case 3:
    buf_appendc(key, version);
    buf_appendl(key, *created);
    buf_appendi(key, valid);
    buf_appendc(key, algo);
    break;
  case 4:
    buf_appendc(key, version);
    buf_appendl(key, *created);
    buf_appendc(key, algo);
    break;
  }

  pgp_keyid(p, id);
  len = mpi_get(p, i);
  mpi_put(key, i);
  for (j = 1; j < pgp_nummpi(algo); j++) {
    if (mpi_get(p, i) == -1) {
      err = -1;
      goto end;
    }
    mpi_put(key, i);
  }
  pgp_packet(key, type);
  buf_cat(out, key);

  if (outtxt != NULL) {
    switch(algo) {
     case PGP_ES_RSA:
      algoid = 'R';
      break;
     case PGP_S_DSA:
      algoid = 'D';
      break;
     case PGP_E_ELG:
      algoid = 'g';
      break;
     default:
      algoid = '?';
    }
    buf_appendf(outtxt, "%s %5d%c/%02X%02X%02X%02X ",
		type == PGP_PUBSUBKEY ?  "sub" :
		type == PGP_PUBKEY ? "pub" :
		type == PGP_SECKEY ? "sec" :
		type == PGP_SECSUBKEY ? "ssb" :
		"???", len, algoid,
		id->data[4], id->data[5], id->data[6], id->data[7]);
    tc = localtime(created);
    strftime(txt, LINELEN, "%Y-%m-%d ", tc);
    buf_appends(outtxt, txt);
  }
 end:
  buf_free(i);
  buf_free(id);
  return(err == 0 ? algo : err);
}

int pgp_makepubkey(BUFFER *keypacket, BUFFER *outtxt, BUFFER *out,
		   BUFFER *pass, int keyalgo)
{
  BUFFER *p, *pubkey, *seckey, *subkey, *sig, *tmp;
  int err = -1, type, thisalgo;
  time_t created;

  p = buf_new();
  seckey = buf_new();
  pubkey = buf_new();
  subkey = buf_new();
  sig = buf_new();
  tmp = buf_new();

  buf_set(seckey, keypacket);
  type = pgp_getpacket(keypacket, p);
  if (type != PGP_SECKEY)
    goto end;

  thisalgo = pgp_makepkpacket(PGP_PUBKEY, p, outtxt, tmp, pubkey, pass,
			      &created);
  if (thisalgo == -1 || (keyalgo != 0 && keyalgo != thisalgo))
    goto end;
  buf_cat(out, tmp);

  while ((type = pgp_getpacket(keypacket, p)) > 0) {
    if (type == PGP_SECSUBKEY) {
      if (pgp_makepkpacket(PGP_PUBSUBKEY, p, outtxt, out, subkey, pass,
			   &created) == -1)
	goto end;
      if (pgp_sign(pubkey, subkey, sig, NULL, pass, PGP_SIG_BINDSUBKEY, 0,
		   created, 0, seckey, NULL) != -1)
	buf_cat(out, sig);
      if (outtxt)
	buf_nl(outtxt);
    } else if (type == PGP_USERID) {
      if (outtxt != NULL) {
	buf_cat(outtxt, p);
	buf_nl(outtxt);
      }
      pgp_packet(p, PGP_USERID);
      err = pgp_sign(pubkey, p, sig, NULL, pass, PGP_SIG_CERT, 1, created, 0,
		     seckey, NULL);     /* maybe PGP_SIG_CERT3 ? */
      buf_cat(out, p);
      if (err == 0)
	buf_cat(out, sig);
    } else if (type == PGP_PUBKEY || type == PGP_SECKEY)
      break;
  }
end:
  buf_free(pubkey);
  buf_free(seckey);
  buf_free(subkey);
  buf_free(sig);
  buf_free(p);
  buf_free(tmp);
  return (err);
}

int pgp_makekeyheader(int type, BUFFER *keypacket, BUFFER *outtxt,
		   BUFFER *pass, int keyalgo)
{
  BUFFER *p, *pubkey, *seckey, *subkey, *sig, *tmp, *dummy;
  int thisalgo, err = -1;
  time_t created;

  assert(type == PGP_SECKEY || type == PGP_PUBKEY);

  p = buf_new();
  seckey = buf_new();
  pubkey = buf_new();
  subkey = buf_new();
  sig = buf_new();
  tmp = buf_new();
  dummy = buf_new();

  buf_set(seckey, keypacket);
  if (type != pgp_getpacket(keypacket, p))
    goto end;

  thisalgo = pgp_makepkpacket(type, p, outtxt, tmp, pubkey, pass,
			      &created);
  if (thisalgo == -1 || (keyalgo != 0 && keyalgo != thisalgo))
    goto end;

  while ((type = pgp_getpacket(keypacket, p)) > 0) {
    if (type == PGP_SECSUBKEY || type == PGP_PUBSUBKEY) {
      if (pgp_makepkpacket(type, p, outtxt, dummy, subkey, pass,
			   &created) == -1)
	goto end;
      buf_nl(outtxt);
    } else if (type == PGP_USERID) {
      buf_cat(outtxt, p);
      buf_nl(outtxt);
      pgp_packet(p, PGP_USERID);
    } else if (type == PGP_PUBKEY || type == PGP_SECKEY)
      break;
  }
  err = 0;
end:
  buf_free(pubkey);
  buf_free(seckey);
  buf_free(subkey);
  buf_free(sig);
  buf_free(p);
  buf_free(dummy);
  buf_free(tmp);
  return (err);
}

int pgp_rsakeygen(int bits, BUFFER *userid, BUFFER *pass, char *pubring,
	       char *secring, int remail)
     /* remail==2: encrypt the secring */
{
  RSA *k;
  KEYRING *keydb;
  BUFFER *pkey, *skey;
  BUFFER *dk, *sig, *iv, *p;
  long now;
  int skalgo = 0;
  int err = 0;

  pkey = buf_new();
  skey = buf_new();
  iv = buf_new();
  dk = buf_new();
  p = buf_new();
  sig = buf_new();

  errlog(NOTICE, "Generating OpenPGP RSA key.\n");
  k = RSA_generate_key(bits == 0 ? 1024 : bits, 17, NULL, NULL);
  if (k == NULL) {
    err = -1;
    goto end;
  }
  now = time(NULL);
  if (remail)			/* fake time in nym keys */
    now -= rnd_number(4 * 24 * 60 * 60);

  buf_appendc(skey, 3);
  buf_appendl(skey, now);
  /* until we can handle the case, where our key expires, don't create keys with expiration dates */
  buf_appendi(skey, 0);
  /* buf_appendi(skey, KEYLIFETIME/(24*60*60)); */
  buf_appendc(skey, PGP_ES_RSA);
  mpi_bnput(skey, k->n);
  mpi_bnput(skey, k->e);

#ifdef USE_IDEA
  if (pass != NULL && pass->length > 0 && remail != 2) {
    skalgo = PGP_K_IDEA;
    digest_md5(pass, dk);
    buf_setrnd(iv, pgp_blocklen(skalgo));
    buf_appendc(skey, skalgo);
    buf_cat(skey, iv);
  }
  else
#endif /* USE_IDEA */
    buf_appendc(skey, 0);

  mpi_bnputenc(skey, k->d, skalgo, dk, iv);
  mpi_bnputenc(skey, k->q, skalgo, dk, iv);
  mpi_bnputenc(skey, k->p, skalgo, dk, iv);
  mpi_bnputenc(skey, k->iqmp, skalgo, dk, iv);

  buf_clear(p);
  mpi_bnput(p, k->d);
  mpi_bnput(p, k->q);
  mpi_bnput(p, k->p);
  mpi_bnput(p, k->iqmp);
  buf_appendi(skey, pgp_csum(p, 0));

  pgp_packet(skey, PGP_SECKEY);
  buf_set(p, userid);
  pgp_packet(p, PGP_USERID);
  buf_cat(skey, p);

  if (secring == NULL)
    secring = PGPREMSECRING;
  keydb = pgpdb_open(secring, remail == 2 ? pass : NULL, 1, PGP_TYPE_PRIVATE);
  if (keydb == NULL) {
    err = -1;
    goto end;
  }
  if (keydb->filetype == -1)
    keydb->filetype = ARMORED;
  pgpdb_append(keydb, skey);
  pgpdb_close(keydb);

  if (pubring != NULL) {
    if (pgp_makepubkey(skey, NULL, pkey, pass, 0) == -1)
      goto end;
    keydb = pgpdb_open(pubring, NULL, 1, PGP_TYPE_PUBLIC);
    if (keydb == NULL)
      goto end;
    if (keydb->filetype == -1)
      keydb->filetype = ARMORED;
    pgpdb_append(keydb, pkey);
    pgpdb_close(keydb);
  }
end:
  RSA_free(k);
  buf_free(pkey);
  buf_free(skey);
  buf_free(iv);
  buf_free(dk);
  buf_free(p);
  buf_free(sig);
  return (err);
}

#define begin_param "-----BEGIN PUBLIC PARAMETER BLOCK-----"
#define end_param "-----END PUBLIC PARAMETER BLOCK-----"

static void *params(int dsa, int bits)
{
  DSA *k = NULL;
  DH *d = NULL;
  FILE *f;
  BUFFER *p, *n;
  char line[LINELEN];
  byte b[1024];
  int m, l;

  if (bits == 0)
    bits = 1024;
  if (dsa && bits > 1024)
    bits = 1024;

  p = buf_new();
  n = buf_new();
  f = mix_openfile(dsa ? DSAPARAMS : DHPARAMS, "r");
  if (f != NULL) {
    for (;;) {
      if (fgets(line, sizeof(line), f) == NULL)
	break;
      if (strleft(line, begin_param)) {
	if (fgets(line, sizeof(line), f) == NULL)
	  break;
	m = 0;
	sscanf(line, "%d", &m);
	if (bits == m) {
	  buf_clear(p);
	  while (fgets(line, sizeof(line), f) != NULL) {
	    if (strleft(line, end_param)) {
	      decode(p, p);
	      if (dsa) {
		k = DSA_new();
		l = buf_geti(p);
		buf_get(p, n, l);
		k->p = BN_bin2bn(n->data, n->length, NULL);
		l = buf_geti(p);
		buf_get(p, n, l);
		k->q = BN_bin2bn(n->data, n->length, NULL);
		l = buf_geti(p);
		buf_get(p, n, l);
		k->g = BN_bin2bn(n->data, n->length, NULL);
	      } else {
		d = DH_new();
		l = buf_geti(p);
		buf_get(p, n, l);
		d->p = BN_bin2bn(n->data, n->length, NULL);
		l = buf_geti(p);
		buf_get(p, n, l);
		d->g = BN_bin2bn(n->data, n->length, NULL);
	      }
	      break;
	    }
	    buf_appends(p, line);
	  }
	}
      }
    }
    fclose(f);
  }

  buf_free(p);
  buf_free(n);

  if (dsa) {
    if (k == NULL) {
      errlog(NOTICE, "Generating DSA parameters.\n");
      k = DSA_generate_parameters(bits, NULL, 0, NULL, NULL, NULL, NULL);
      p = buf_new();
      l = BN_bn2bin(k->p, b);
      buf_appendi(p, l);
      buf_append(p, b, l);
      l = BN_bn2bin(k->q, b);
      buf_appendi(p, l);
      buf_append(p, b, l);
      l = BN_bn2bin(k->g, b);
      buf_appendi(p, l);
      buf_append(p, b, l);
      encode(p, 64);
      f = mix_openfile(DSAPARAMS, "a");
      if (f != NULL) {
	fprintf(f, "%s\n%d\n", begin_param, bits);
	buf_write(p, f);
	fprintf(f, "%s\n", end_param);
	fclose(f);
      } else
	errlog(ERRORMSG, "Cannot open %s!\n", DSAPARAMS);
      buf_free(p);
    }
    return (k);
  } else {
    if (d == NULL) {
      errlog(NOTICE, "Generating DH parameters. (This may take a long time!)\n");
      d = DH_generate_parameters(bits, DH_GENERATOR_5, NULL, NULL);
      p = buf_new();
      l = BN_bn2bin(d->p, b);
      buf_appendi(p, l);
      buf_append(p, b, l);
      l = BN_bn2bin(d->g, b);
      buf_appendi(p, l);
      buf_append(p, b, l);
      encode(p, 64);
      f = mix_openfile(DHPARAMS, "a");
      if (f != NULL) {
	fprintf(f, "%s\n%d\n", begin_param, bits);
	buf_write(p, f);
	fprintf(f, "%s\n", end_param);
	fclose(f);
      } else
	errlog(ERRORMSG, "Cannot open %s!\n", DHPARAMS);
      buf_free(p);
    }
    return (d);
  }
}

int pgp_dhkeygen(int bits, BUFFER *userid, BUFFER *pass, char *pubring,
	       char *secring, int remail)
     /* remail==2: encrypt the secring */
{
  DSA *s;
  DH *e;
  KEYRING *keydb;
  BUFFER *pkey, *skey, *subkey, *secret;
  BUFFER *dk, *sig, *iv, *p;
  long now;
  int err = 0;

  pkey = buf_new();
  skey = buf_new();
  subkey = buf_new();
  iv = buf_new();
  dk = buf_new();
  p = buf_new();
  sig = buf_new();
  secret = buf_new();

  s = params(1, bits);
  errlog(NOTICE, "Generating OpenPGP DSA key.\n");
  if (s == NULL || DSA_generate_key(s) != 1) {
    err = -1;
    goto end;
  }
  e = params(0, bits);
  errlog(NOTICE, "Generating OpenPGP ElGamal key.\n");
  if (e == NULL || DH_generate_key(e) != 1) {
    err = -1;
    goto end;
  }

  now = time(NULL);
  if (remail)			/* fake time in nym keys */
    now -= rnd_number(4 * 24 * 60 * 60);

  /* DSA key */
  buf_setc(skey, 4);
  buf_appendl(skey, now);
  buf_appendc(skey, PGP_S_DSA);
  mpi_bnput(skey, s->p);
  mpi_bnput(skey, s->q);
  mpi_bnput(skey, s->g);
  mpi_bnput(skey, s->pub_key);

  mpi_bnput(secret, s->priv_key);
  buf_appendi(secret, pgp_csum(secret, 0));
  makeski(secret, pass, remail);
  buf_cat(skey, secret);
  pgp_packet(skey, PGP_SECKEY);

  /* ElGamal key */
  buf_setc(subkey, 4);
  buf_appendl(subkey, now);
  buf_appendc(subkey, PGP_E_ELG);
  mpi_bnput(subkey, e->p);
  mpi_bnput(subkey, e->g);
  mpi_bnput(subkey, e->pub_key);

  buf_clear(secret);
  mpi_bnput(secret, e->priv_key);
  buf_appendi(secret, pgp_csum(secret, 0));
  makeski(secret, pass, remail);
  buf_cat(subkey, secret);

  buf_set(p, userid);
  pgp_packet(p, PGP_USERID);
  buf_cat(skey, p);

  pgp_packet(subkey, PGP_SECSUBKEY);
  buf_cat(skey, subkey);

  if (secring == NULL)
    secring = PGPREMSECRING;
  keydb = pgpdb_open(secring, remail == 2 ? pass : NULL, 1, PGP_TYPE_PRIVATE);
  if (keydb == NULL) {
    err = -1;
    goto end;
  }
  if (keydb->filetype == -1)
    keydb->filetype = ARMORED;
  pgpdb_append(keydb, skey);
  pgpdb_close(keydb);

  if (pubring != NULL) {
    pgp_makepubkey(skey, NULL, pkey, pass, 0);
    keydb = pgpdb_open(pubring, NULL, 1, PGP_TYPE_PUBLIC);
    if (keydb == NULL)
      goto end;
    if (keydb->filetype == -1)
      keydb->filetype = ARMORED;
    pgpdb_append(keydb, pkey);
    pgpdb_close(keydb);
  }
end:
  buf_free(pkey);
  buf_free(skey);
  buf_free(subkey);
  buf_free(iv);
  buf_free(dk);
  buf_free(p);
  buf_free(sig);
  buf_free(secret);
  return (err);
}

int pgp_dsasign(BUFFER *data, BUFFER *key, BUFFER *out)
{
  BUFFER *mpi, *b;
  DSA *d;
  DSA_SIG *sig = NULL;

  d = DSA_new();
  b = buf_new();
  mpi = buf_new();
  mpi_get(key, mpi);
  d->p = BN_bin2bn(mpi->data, mpi->length, NULL);
  mpi_get(key, mpi);
  d->q = BN_bin2bn(mpi->data, mpi->length, NULL);
  mpi_get(key, mpi);
  d->g = BN_bin2bn(mpi->data, mpi->length, NULL);
  mpi_get(key, mpi);
  d->pub_key = BN_bin2bn(mpi->data, mpi->length, NULL);
  if (mpi_get(key, mpi) == -1) {
    goto end;
  }
  d->priv_key = BN_bin2bn(mpi->data, mpi->length, NULL);

  sig = DSA_do_sign(data->data, data->length, d);
  if (sig) {
    buf_prepare(b, BN_num_bytes(sig->r));
    b->length = BN_bn2bin(sig->r, b->data);
    mpi_put(out, b);
    b->length = BN_bn2bin(sig->s, b->data);
    mpi_put(out, b);
  }
 end:
  buf_free(mpi);
  buf_free(b);
  DSA_SIG_free(sig);
  DSA_free(d);
  return(sig ? 0 : -1);
}

int pgp_dosign(int algo, BUFFER *data, BUFFER *key)
{
  int err;
  BUFFER *out, *r, *s;

  out = buf_new();
  r = buf_new();
  s = buf_new();
  switch (algo) {
   case PGP_ES_RSA:
    err = pgp_rsa(data, key, PK_SIGN);
    if (err == 0)
      mpi_put(out, data);
    break;
   case PGP_S_DSA:
    err = pgp_dsasign(data, key, out);
    break;
   default:
    errlog(NOTICE, "Unknown encryption algorithm!\n");
    return (-1);
  }
  if (err == -1)
    errlog(ERRORMSG, "Signing operation failed!\n");

  buf_move(data, out);
  buf_free(out);
  buf_free(r);
  buf_free(s);
  return (err);
}

int pgp_elgdecrypt(BUFFER *in, BUFFER *key)
{
  BIGNUM *a = NULL, *b = NULL, *c = NULL,
	 *p = NULL, *g = NULL, *x = NULL;
  BN_CTX *ctx;
  BUFFER *i;
  int err = -1;

  i = buf_new();
  ctx = BN_CTX_new();
  if (ctx == NULL) goto end;
  mpi_get(key, i);
  p = BN_bin2bn(i->data, i->length, NULL);
  mpi_get(key, i);
  g = BN_bin2bn(i->data, i->length, NULL);
  mpi_get(key, i); /* y */
  mpi_get(key, i);
  x = BN_bin2bn(i->data, i->length, NULL);
  mpi_get(in, i);
  a = BN_bin2bn(i->data, i->length, NULL);
  if (mpi_get(in, i) == -1)
    goto e1;
  b = BN_bin2bn(i->data, i->length, NULL);
  c = BN_new();

  if (BN_mod_exp(c, a, x, p, ctx) == 0) goto end;
  if (BN_mod_inverse(a, c, p, ctx) == 0) goto end;
  if (BN_mod_mul(c, a, b, p, ctx) == 0) goto end;

  buf_prepare(i, BN_num_bytes(c));
  i->length = BN_bn2bin(c, i->data);

  buf_prepare(in, BN_num_bytes(c));
  in->length = RSA_padding_check_PKCS1_type_2(in->data, in->length, i->data,
					       i->length, i->length + 1);
  if (in->length <= 0)
    in->length = 0;
  else
    err = 0;

 end:
  BN_free(b);
  BN_free(c);
 e1:
  buf_free(i);
  BN_free(a);
  BN_free(p);
  BN_free(g);
  BN_clear_free(x);
  BN_CTX_free(ctx);

  return (err);
}

int pgp_elgencrypt(BUFFER *in, BUFFER *key)
{
  BIGNUM *m, *k, *a, *b, *c, *p, *g, *y = NULL;
  BN_CTX *ctx;
  BUFFER *i;
  int err = -1;

  i = buf_new();
  ctx = BN_CTX_new();
  if (ctx == NULL) goto end;
  mpi_get(key, i);
  p = BN_bin2bn(i->data, i->length, NULL);
  mpi_get(key, i);
  g = BN_bin2bn(i->data, i->length, NULL);
  if (mpi_get(key, i) == -1)
    goto e1;
  y = BN_bin2bn(i->data, i->length, NULL);

  buf_prepare(i, BN_num_bytes(p));
  if (RSA_padding_add_PKCS1_type_2(i->data, i->length, in->data, in->length)
      != 1)
    goto end;
  m = BN_bin2bn(i->data, i->length, NULL);

  k = BN_new();
  BN_rand(k, BN_num_bits(p), 0, 0);

  a = BN_new();
  b = BN_new();
  c = BN_new();

  if (BN_mod_exp(a, g, k, p, ctx) == 0) goto end;
  if (BN_mod_exp(c, y, k, p, ctx) == 0) goto end;
  if (BN_mod_mul(b, m, c, p, ctx) == 0) goto end;

  buf_clear(in);
  i->length = BN_bn2bin(a, i->data);
  mpi_put(in, i);
  i->length = BN_bn2bin(b, i->data);
  mpi_put(in, i);

  err = 0;

  BN_free(a);
  BN_free(b);
  BN_free(c);
  BN_free(m);
e1:
  buf_free(i);
  BN_free(p);
  BN_free(g);
  BN_free(y);
  BN_CTX_free(ctx);
 end:

  return (err);
}

#endif /* USE_PGP */
