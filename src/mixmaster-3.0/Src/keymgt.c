/* Mixmaster version 3.0  --  (C) 1999 - 2006 Anonymizer Inc. and others.

   Mixmaster may be redistributed and modified under certain conditions.
   This software is distributed on an "AS IS" basis, WITHOUT WARRANTY OF
   ANY KIND, either express or implied. See the file COPYRIGHT for
   details.

   Key management
   $Id: keymgt.c 934 2006-06-24 13:40:39Z rabbi $ */


#include "mix3.h"
#include <string.h>
#include <time.h>
#include <assert.h>

int getv2seckey(byte keyid[], BUFFER *key);
static int getv2pubkey(byte keyid[], BUFFER *key);

int db_getseckey(byte keyid[], BUFFER *key)
{
  if (getv2seckey(keyid, key) == -1)
    return (-1);
  else
    return (0);
}

int db_getpubkey(byte keyid[], BUFFER *key)
{
  if (getv2pubkey(keyid, key) == -1)
    return (-1);
  else
    return (0);
}

/* now accepts NULL keyid too, with NULL keyid any key
 * will be matched, with valid passphrase of course */
int getv2seckey(byte keyid[], BUFFER *key)
{
  FILE *keyring;
  BUFFER *iv, *pass, *temp;
  char idstr[KEY_ID_LEN+2];
  char line[LINELEN];
  int err = -1;
  char *res;
  time_t created, expires;

  pass = buf_new();
  iv = buf_new();
  temp = buf_new();
  if (keyid)
    id_encode(keyid, idstr);
  else
    idstr[0] = 0;
  strcat(idstr, "\n");
  if ((keyring = mix_openfile(SECRING, "r")) == NULL) {
    errlog(ERRORMSG, "No secret key file!\n");
  } else {
    while (err == -1) {
      buf_clear(key);
      if (fgets(line, sizeof(line), keyring) == NULL)
	break;
      if (strleft(line, begin_key)) {
	expires = 0;
	created = 0;
	do {
	  res = fgets(line, sizeof(line), keyring);
	  if (strileft(line, "created:")) {
	    created = parse_yearmonthday(strchr(line, ':')+1);
	    if (created == -1)
	      created = 0;
	  } else if (strileft(line, "expires:")) {
	    expires = parse_yearmonthday(strchr(line, ':')+1);
	    if (expires == -1)
	      expires = 0;
	  }
	  /* Fetch lines until we fail or get a non-header line */
	} while ( res != NULL && strchr(line, ':') != NULL );
	if (res == NULL)
	  break;
	if (keyid && (strncmp(line, idstr, KEY_ID_LEN) != 0))
	  continue;
	if (created != 0 && (created > time(NULL))) {
	  errlog(ERRORMSG, "Key is not valid yet (creation date in the future): %s", idstr);
	  break;
	}
	if (expires != 0 && (expires + KEYGRACEPERIOD < time(NULL))) {
	  errlog(ERRORMSG, "Key is expired: %s", idstr);
	  break;
	}
	fgets(line, sizeof(line), keyring);
	fgets(line, sizeof(line), keyring);
	buf_sets(iv, line);
	decode(iv, iv);
	for (;;) {
	  if (fgets(line, sizeof(line), keyring) == NULL)
	    break;
	  if (strleft(line, end_key)) {
	    if (decode(key, key) == -1) {
	      errlog(ERRORMSG, "Corrupt secret key.\n");
	      break;
	    }
	    buf_sets(pass, PASSPHRASE);
	    digest_md5(pass, pass);
	    buf_crypt(key, pass, iv, DECRYPT);
	    err = check_seckey(key, keyid);
	    if (err == -1)
	      errlog(ERRORMSG, "Corrupt secret key. Bad passphrase?\n");
	    break;
	  }
	  buf_append(key, line, strlen(line) - 1);
	}
	break;
      }
    }
    fclose(keyring);
  }

  buf_free(pass);
  buf_free(iv);
  buf_free(temp);
  return (err);
}

static int getv2pubkey(byte keyid[], BUFFER *key)
{
  FILE *keyring;
  BUFFER *b, *temp, *iv;
  char idstr[KEY_ID_LEN+2];
  char line[LINELEN];
  int err = 0;

  b = buf_new();
  iv = buf_new();
  temp = buf_new();
  id_encode(keyid, idstr);
  if ((keyring = mix_openfile(PUBRING, "r")) == NULL) {
    errlog(ERRORMSG, "Can't open %s!\n", PUBRING);
    err = -1;
    goto end;
  }
  for (;;) {
    if (fgets(line, sizeof(line), keyring) == NULL)
      break;
    if (strleft(line, begin_key)) {
      if (fgets(line, sizeof(line), keyring) == NULL)
	break;
      if ((strlen(line) > 0) && (line[strlen(line)-1] == '\n'))
	line[strlen(line)-1] = '\0';
      if ((strlen(line) > 0) && (line[strlen(line)-1] == '\r'))
	line[strlen(line)-1] = '\0';
      if (strncmp(line, idstr, KEY_ID_LEN) != 0)
	continue;
      fgets(line, sizeof(line), keyring);	/* ignore length */
      for (;;) {
	if (fgets(line, sizeof(line), keyring) == NULL)
	  goto done;
	if (strleft(line, end_key))
	  goto done;
	buf_append(key, line, strlen(line));
      }
      break;
    }
  }
done:
  fclose(keyring);

  if (key->length == 0) {
    errlog(ERRORMSG, "No such public key: %s\n", idstr);
    err = -1;
    goto end;
  }
  err = decode(key, key);
  if (err != -1)
    err = check_pubkey(key, keyid);
  if (err == -1)
    errlog(ERRORMSG, "Corrupt public key %s\n", idstr);
end:
  buf_free(b);
  buf_free(iv);
  buf_free(temp);
  return (err);
}

int key(BUFFER *out)
{
  int err = -1;
  FILE *f;
  BUFFER *tmpkey;

  tmpkey = buf_new();

  buf_sets(out, "Subject: Remailer key for ");
  buf_appends(out, SHORTNAME);
  buf_appends(out, "\n\n");

  keymgt(0);

  conf_premail(out);
  buf_nl(out);

#ifdef USE_PGP
  if (PGP) {
    if (pgp_latestkeys(tmpkey, PGP_ES_RSA) == 0) {
      buf_appends(out, "Here is the RSA PGP key:\n\n");
      buf_cat(out, tmpkey);
      buf_nl(out);
      err = 0;
    }
    if (pgp_latestkeys(tmpkey, PGP_S_DSA) == 0) {
      buf_appends(out, "Here is the DSA PGP key:\n\n");
      buf_cat(out, tmpkey);
      buf_nl(out);
      err = 0;
    }
  }
#endif /* USE_PGP */
  if (MIX) {
    if ((f = mix_openfile(KEYFILE, "r")) != NULL) {
      buf_appends(out, "Here is the Mixmaster key:\n\n");
      buf_appends(out, "=-=-=-=-=-=-=-=-=-=-=-=\n");
      buf_read(out, f);
      buf_nl(out);
      fclose(f);
      err = 0;
    }
  }
  if (err == -1 && UNENCRYPTED) {
    buf_appends(out, "The remailer accepts unencrypted messages.\n");
    err = 0;
  }
  if (err == -1)
    errlog(ERRORMSG, "Cannot create remailer keys!");

  buf_free(tmpkey);

  return (err);
}

int adminkey(BUFFER *out)
{
	int err = -1;
	FILE *f;

	buf_sets( out, "Subject: Admin key for the " );
	buf_appends( out, SHORTNAME );
	buf_appends( out, " remailer\n\n" );

	if ( (f = mix_openfile( ADMKEYFILE, "r" )) != NULL ) {
	        buf_read( out, f );
	        buf_nl( out );
	        fclose( f );
	        err = 0;
	}

	if ( err == -1 )
	        errlog( ERRORMSG, "Can not read admin key file!\n" );

	return err;
}

int v2keymgt(int force)
/*
 * Mixmaster v2 Key Management
 *
 * This function triggers creation of mix keys (see parameter force) which are
 * stored in secring.mix. One public mix key is also written to key.txt. This
 * is the key with the latest expiration date (keys with no expiration date
 * are always considered newer if they appear later in the secret mix file 
 * - key creation appends keys).
 *
 * force:
 *   0, 1: create key when necessary:
 *          - no key exists as of yet
 *          - old keys are due to expire/already expired
 *   2: always create a new mix key.
 *
 *   (force = 0 is used in mix_daily, and before remailer-key replies)
 *   (force = 1 is used by mixmaster -K)
 *   (force = 2 is used by mixmaster -G)
 */
{
  FILE *keyring, *f;
  char line[LINELEN];
  byte k1[16], k1_found[16];
  BUFFER *b, *temp, *iv, *pass, *pk, *pk_found;
  int err = 0;
  int found, foundnonexpiring;
  time_t created, expires, created_found, expires_found;
  char *res;

  b = buf_new();
  temp = buf_new();
  iv = buf_new();
  pass = buf_new();
  pk = buf_new();
  pk_found = buf_new();

  foundnonexpiring = 0;
  for (;;) {
    found = 0;
    created_found = 0;
    expires_found = 0;

    keyring = mix_openfile(SECRING, "r");
    if (keyring != NULL) {
      for (;;) {
	if (fgets(line, sizeof(line), keyring) == NULL)
	  break;
	if (strleft(line, begin_key)) {
	  expires = 0;
	  created = 0;
	  do {
	    res = fgets(line, sizeof(line), keyring);
	    if (strileft(line, "created:")) {
	      created = parse_yearmonthday(strchr(line, ':')+1);
	      if (created == -1)
		created = 0;
	    } else if (strileft(line, "expires:")) {
	      expires = parse_yearmonthday(strchr(line, ':')+1);
	      if (expires == -1)
		expires = 0;
	    }
	    /* Fetch lines until we fail or get a non-header line */
	  } while ( res != NULL && strchr(line, ':') != NULL );
	  if (res == NULL)
	    break;
	  if (((created != 0) && (created > time(NULL))) ||
	      ((expires != 0) && (expires < time(NULL)))) {
	    /* Key already is expired or has creation date in the future */
	    continue;
	  }
	  id_decode(line, k1);
	  fgets(line, sizeof(line), keyring);
	  if (fgets(line, sizeof(line), keyring) == NULL)
	    break;
	  buf_sets(iv, line);
	  decode(iv, iv);
	  buf_reset(b);
	  for (;;) {
	    if (fgets(line, sizeof(line), keyring) == NULL)
	      break;
	    if (strleft(line, end_key))
	      break;
	    buf_append(b, line, strlen(line) - 1);
	  }
	  if (decode(b, b) == -1)
	    break;
	  buf_sets(temp, PASSPHRASE);
	  digest_md5(temp, pass);
	  buf_crypt(b, pass, iv, DECRYPT);
	  buf_clear(pk);
	  if (seckeytopub(pk, b, k1) == 0) {
	    found = 1;
	    if (expires == 0 || (expires - KEYOVERLAPPERIOD >= time(NULL)))
	      foundnonexpiring = 1;
	    if (expires == 0 || (expires_found <= expires)) {
	      buf_clear(pk_found);
	      buf_cat(pk_found, pk);
	      memcpy(&k1_found, &k1, sizeof(k1));
	      expires_found = expires;
	      created_found = created;
	    }
	  }
	}
      }
      fclose(keyring);
    }

    if (!foundnonexpiring || (force == 2)) {
      v2createkey();
      foundnonexpiring = 1;
      force = 1;
    } else
      break;
  };

  if (found) {
    if ((f = mix_openfile(KEYFILE, "w")) != NULL) {
      id_encode(k1_found, line);
      fprintf(f, "%s %s %s %s:%s %s%s", SHORTNAME,
	      REMAILERADDR, line, mixmaster_protocol, VERSION,
	      MIDDLEMAN ? "M" : "",
	      NEWS[0] == '\0' ? "C" : (strchr(NEWS, '@') ? "CNm" : "CNp"));
      if (created_found) {
	struct tm *gt;
	gt = gmtime(&created_found);
	strftime(line, LINELEN, "%Y-%m-%d", gt);
	fprintf(f, " %s", line);
	if (expires_found) {
	  struct tm *gt;
	  gt = gmtime(&expires_found);
	  strftime(line, LINELEN, "%Y-%m-%d", gt);
	  fprintf(f, " %s", line);
	}
      }
      fprintf(f, "\n\n%s\n", begin_key);
      id_encode(k1_found, line);
      fprintf(f, "%s\n258\n", line);
      encode(pk_found, 40);
      buf_write(pk_found, f);
      fprintf(f, "%s\n\n", end_key);
      fclose(f);
    }
  } else
    err = -1;

  buf_free(b);
  buf_free(temp);
  buf_free(iv);
  buf_free(pass);
  buf_free(pk);
  buf_free(pk_found);

  return (err);
}

int keymgt(int force)
{
  /* force = 0: write key file if there is none
     force = 1: update key file
     force = 2: generate new key */
  int err = 0;

  if (REMAIL || force == 2) {
    if (MIX && (err = v2keymgt(force)) == -1)
      err = -1;
#ifdef USE_PGP
    if (PGP && (err = pgp_keymgt(force)) == -1)
      err = -1;
#endif /* USE_PGP */
  }
  return (err);
}
