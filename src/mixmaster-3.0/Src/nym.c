/* Mixmaster version 3.0  --  (C) 1999 - 2006 Anonymizer Inc. and others.

   Mixmaster may be redistributed and modified under certain conditions.
   This software is distributed on an "AS IS" basis, WITHOUT WARRANTY OF
   ANY KIND, either express or implied. See the file COPYRIGHT for
   details.

   Create nym server messages
   $Id: nym.c 934 2006-06-24 13:40:39Z rabbi $ */


#ifdef NYMSUPPORT

#include "mix3.h"
#include "pgp.h"
#include <string.h>
#include <time.h>
#include <assert.h>

int nym_config(int mode, char *nym, char *nymserver, BUFFER *pseudonym,
	       char *sendchain, int sendnumcopies, BUFFER *chains,
	       BUFFER *options)
{
#ifndef USE_PGP
  return (-1);
#else /* end of not USE_PGP */
  REMAILER remailer[MAXREM];
  int badchains[MAXREM][MAXREM];
  KEYRING *r;
  int maxrem;
  int chain[20];
  char rchain[CHAINMAX];
  BUFFER *userid, *msg, *req, *k, *line, *ek, *eklist, *key, *pubkey, *out,
      *oldchains;
  int latency;
  int err = -1;
  int status;
  int desttype = MSG_MAIL;
  int rblock = 0;
  BUFFER *nymlist, *userpass, *config;
  LOCK *nymlock;

  userid = buf_new();
  msg = buf_new();
  req = buf_new();
  k = buf_new();
  line = buf_new();
  ek = buf_new();
  eklist = buf_new();
  key = buf_new();
  pubkey = buf_new();
  out = buf_new();
  config = buf_new();
  nymlist = buf_new();
  userpass = buf_new();
  oldchains = buf_new();

  for (;;) {
    user_pass(userpass);
    if (user_confirmpass(userpass))
      break;
    user_delpass();
  }

  if (nymserver) {
    maxrem = t1_rlist(remailer, badchains);
    if (maxrem < 1)
      return (-1);
    if (chain_select(chain, nymserver, maxrem, remailer, 2, NULL) != 1)
      return (-1);
    if (chain[0] == 0)
      chain[0] = chain_randfinal(MSG_MAIL, remailer, badchains, maxrem, 2, NULL, -1);
    if (chain[0] == -1)
      return (-1);
    assert(strchr(nym, '@') == NULL && strchr(remailer[chain[0]].addr, '@'));
    strcatn(nym, strchr(remailer[chain[0]].addr, '@'), LINELEN);
    buf_appends(config, remailer[chain[0]].addr);
  } else
    assert(strchr(nym, '@') != NULL);

  status = nymlist_getnym(nym, config->length ? NULL : config, eklist, NULL,
			  NULL, oldchains);
  if (mode == NYM_CREATE && status == NYM_OK)
    mode = NYM_MODIFY;

  buf_appendc(userid, '<');
  buf_appends(userid, nym);
  buf_appendc(userid, '>');

  buf_sets(req, "Config:\nFrom: ");
  buf_append(req, nym, strchr(nym, '@') - nym);
  buf_appends(req, "\nNym-Commands:");
  if (mode == NYM_CREATE)
    buf_appends(req, " create?");
  if (mode == NYM_DELETE)
    buf_appends(req, " delete");
  else {
    if (options && options->length > 0) {
      if (!bufleft(options, " "))
	buf_appendc(req, ' ');
      buf_cat(req, options);
    }
    if (pseudonym && pseudonym->length > 0) {
      buf_appends(req, " name=\"");
      buf_cat(req, pseudonym);
      buf_appendc(req, '\"');
    }
  }
  buf_nl(req);
  if (mode == NYM_CREATE) {
    buf_appends(req, "Public-Key:\n");

  getkey:
    r = pgpdb_open(NYMSECRING, userpass, 0, PGP_TYPE_PRIVATE);
    if (r == NULL) {
      err = -3;
      goto end;
    }
    if (r->filetype == -1)
      r->filetype = 0;

    while (pgpdb_getnext(r, key, NULL, userid) != -1)
      if (pgp_makepubkey(key, NULL, pubkey, userpass, 0) == 0)
	err = 0;
    pgpdb_close(r);
    if (err != 0) {
      if (err == -2)
	goto end;
      err = -2;
      if (pseudonym && pseudonym->length) {
	buf_set(userid, pseudonym);
	buf_appendc(userid, ' ');
      } else
	buf_clear(userid);
      buf_appendf(userid, "<%s>", nym);
      pgp_keygen(PGP_ES_RSA, 0, userid, userpass, NULL, NYMSECRING, 2);
      goto getkey;
    }
    pgp_armor(pubkey, PGP_ARMOR_NYMKEY);
    buf_cat(req, pubkey);
  }
  if (mode != NYM_DELETE) {
    if (nymlist_read(nymlist) == -1) {
      user_delpass();
      err = -1;
      goto end;
    }
    if (chains)
      for (;;) {
	err = buf_getheader(chains, k, line);
	if (err == -1 && rblock == 0)
	  break;
	if (err != 0 && rblock == 1) {
	  buf_setrnd(ek, 16);
	  if (t1_encrypt(desttype, msg, rchain, latency, ek, NULL) != 0) {
	    err = -2;
	    goto end;
	  }
	  encode(ek, 0);
	  buf_cat(eklist, ek);
	  buf_nl(eklist);
	  buf_appends(req, "Reply-Block:\n");
	  buf_cat(req, msg);
	  buf_clear(msg);
	  rblock = 0;
	  continue;
	}
	if (bufieq(k, "Chain"))
	  strncpy(rchain, line->data, sizeof(rchain));
	else if (bufieq(k, "Latency"))
	  sscanf(line->data, "%d", &latency);
	else if (bufieq(k, "Null"))
	  desttype = MSG_NULL, rblock = 1;
	else {
	  buf_appendheader(msg, k, line);
	  if (bufieq(k, "To"))
	    desttype = MSG_MAIL, rblock = 1;
	  if (bufieq(k, "Newsgroups"))
	    desttype = MSG_POST, rblock = 1;
	}
      }
  }
  nymlock = lockfile(NYMDB);
  if (nymlist_read(nymlist) == 0) {
    nymlist_del(nymlist, nym);
    nymlist_append(nymlist, nym, config, options, pseudonym,
		   chains ? chains : oldchains, eklist,
		   mode == NYM_DELETE ? NYM_DELETED :
		   (status == -1 ? NYM_WAITING : status));
    nymlist_write(nymlist);
  } else
    err = -1;
  unlockfile(nymlock);

#ifdef DEBUG
  buf_write(req, stderr);
#endif /* DEBUG */
  buf_clear(line);
  buf_appendc(line, '<');
  buf_cat(line, config);
  buf_appendc(line, '>');

  err = pgp_encrypt(PGP_ENCRYPT | PGP_SIGN | PGP_TEXT | PGP_REMAIL,
		    req, line, userid, userpass, NULL, NYMSECRING);
  if (err != 0)
    goto end;
#ifdef DEBUG
  buf_write(req, stderr);
#endif /* DEBUG */
  buf_sets(out, "To: ");
  buf_cat(out, config);
  buf_nl(out);
  buf_nl(out);
  buf_cat(out, req);

  err = mix_encrypt(desttype, out, sendchain, sendnumcopies, line);
  if (err)
    mix_status("%s\n", line->data);

end:
  if (strchr(nym, '@')) *strchr(nym, '@') = '\0';
  buf_free(userid);
  buf_free(msg);
  buf_free(req);
  buf_free(k);
  buf_free(line);
  buf_free(ek);
  buf_free(eklist);
  buf_free(key);
  buf_free(pubkey);
  buf_free(out);
  buf_free(nymlist);
  buf_free(userpass);
  buf_free(oldchains);
  buf_free(config);
  return (err);
#endif /* else if USE_PGP */
}

int nym_encrypt(BUFFER *msg, char *nym, int type)
{
#ifndef USE_PGP
  return (-1);
#else /* end of not USE_PGP */
  BUFFER *out, *userpass, *sig, *config;
  int err = -1;

  out = buf_new();
  userpass = buf_new();
  sig = buf_new();
  config = buf_new();

  if (nymlist_getnym(nym, config, NULL, NULL, NULL, NULL) == NYM_OK) {
    buf_appends(out, "From: ");
    buf_append(out, nym, strchr(nym, '@') - nym);
    buf_nl(out);
    if (type == MSG_POST) {
      buf_appends(out, "To: ");
      buf_appends(out, MAILtoNEWS);
      buf_nl(out);
    }
    buf_cat(out, msg);
    mail_encode(out, 0);
    buf_appendc(sig, '<');
    buf_appends(sig, nym);
    buf_appendc(sig, '>');
#ifdef DEBUG
    buf_write(out, stderr);
#endif /* DEBUG */
    user_pass(userpass);
    err = pgp_encrypt(PGP_ENCRYPT | PGP_SIGN | PGP_TEXT | PGP_REMAIL,
		      out, config, sig, userpass, NULL, NYMSECRING);
    if (err == 0) {
      buf_clear(msg);
      buf_appends(msg, "To: send");
      buf_appends(msg, strchr(nym, '@'));
      buf_nl(msg);
      buf_nl(msg);
      buf_cat(msg, out);
    }
  }
  buf_free(out);
  buf_free(config);
  buf_free(userpass);
  buf_free(sig);
  return (err);
#endif /* else if USE_PGP */
}

int nym_decrypt(BUFFER *msg, char *thisnym, BUFFER *log)
{
#ifndef USE_PGP
  return (-1);
#else /* end of not USE_PGP */
  BUFFER *pgpmsg, *out, *line;
  BUFFER *nymlist, *userpass;
  BUFFER *decr, *sig, *mid;
  BUFFER *name, *rblocks, *eklist, *config;
  int decrypted = 0;
  int err = 1;
  long ptr;
  char nym[LINELEN];
  BUFFER *ek, *opt;
  int status;
  LOCK *nymlock;
  time_t t;
  struct tm *tc;
  char timeline[LINELEN];

  pgpmsg = buf_new();
  out = buf_new();
  line = buf_new();
  nymlist = buf_new();
  userpass = buf_new();
  config = buf_new();
  ek = buf_new();
  decr = buf_new();
  sig = buf_new();
  mid = buf_new();
  opt = buf_new();
  name = buf_new();
  rblocks = buf_new();
  eklist = buf_new();

  if (thisnym)
    thisnym[0] = '\0';
  while ((ptr = msg->ptr, err = buf_getline(msg, line)) != -1) {
    if (bufleft(line, begin_pgpmsg)) {
      err = -1;
      msg->ptr = ptr;
      pgp_dearmor(msg, pgpmsg);
      if (pgp_isconventional(pgpmsg)) {
	user_pass(userpass);
	nymlock = lockfile(NYMDB);
	if (nymlist_read(nymlist) == -1)
	  user_delpass();
	while (nymlist_get(nymlist, nym, config, eklist, opt, name,
			   rblocks, &status) >= 0) {
	  while (buf_getline(eklist, ek) == 0) {
	    decode(ek, ek);
	    if (t1_getreply(pgpmsg, ek, 20) == 0) {
	      buf_clear(out);
	      err = pgp_decrypt(pgpmsg, userpass, sig, NULL,
				NYMSECRING);
	      buf_sets(out, "From nymserver ");
	      if (strchr(sig->data, '[') && strchr(sig->data, ']'))
		buf_append(out, strchr(sig->data, '[') + 1,
			   strchr(sig->data, ']') -
			   strchr(sig->data, '[') - 1);
	      else {
		t = time(NULL);
		tc = localtime(&t);
		strftime(timeline, LINELEN, "%a %b %d %H:%M:%S %Y", tc);
		buf_appends(out, timeline);
	      }
	      buf_nl(out);
	      if (err == PGP_SIGOK &&
		  bufifind(sig, config->data)) {
		buf_appends(out, "Nym: ");
		if (status == NYM_WAITING)
		  buf_appends(out, "confirm ");
		buf_appends(out, nym);
		buf_nl(out);
		if (thisnym && status == NYM_OK)
		  strncpy(thisnym, nym, LINELEN);
	      } else
		buf_appends(out, "Warning: Signature verification failed!\n");
	      buf_cat(out, pgpmsg);
	      decrypted = 2;
	      if (log) {
		digest_md5(out, mid);
		encode(mid, 0);
		if (buffind(log, mid->data)) {
		  decrypted = -1;
		  unlockfile(nymlock);
		  goto end;
		} else {
		  buf_cat(log, mid);
		  buf_nl(log);
		}
	      }
	      if (status == NYM_WAITING) {
		nymlist_del(nymlist, nym);
		nymlist_append(nymlist, nym, config, opt,
			       name, rblocks, eklist, NYM_OK);
	      }
	      break;
	    }
	  }
	}
	nymlist_write(nymlist);
	unlockfile(nymlock);
      }
      if (decrypted == 0) {
	user_pass(userpass);
	err = pgp_decrypt(pgpmsg, userpass, sig, PGPPUBRING, PGPSECRING);
	if (err == PGP_ERR)
	  err = pgp_decrypt(pgpmsg, userpass, sig, PGPPUBRING,
			    NYMSECRING);
#if 0
	if (err == PGP_PASS || err == PGP_ERR)
	  user_delpass();
#endif /* 0 */
	if (err != PGP_ERR && err != PGP_PASS && err != PGP_NOMSG &&
	    err != PGP_NODATA) {
	  buf_appends(out, info_beginpgp);
	  if (err == PGP_SIGOK)
	    buf_appendf(out, " (SIGNED)\n%s%b", info_pgpsig, sig);
	  buf_nl(out);
	  buf_cat(out, pgpmsg);
	  buf_appends(out, info_endpgp);
	  buf_nl(out);
	  decrypted = 1;
	}
      }
      if (decrypted == 0) {
	buf_cat(out, line);
	buf_nl(out);
      }
    } else {
      if (bufileft(line, info_beginpgp))
	  buf_appendc(out, ' '); /* escape info line in text */
      buf_cat(out, line);
      buf_nl(out);
    }
  }

  if (decrypted)
    buf_move(msg, out);
  else
    buf_rewind(msg);
  if (decrypted == 2)
    nym_decrypt(msg, thisnym, NULL);
end:
  buf_free(pgpmsg);
  buf_free(out);
  buf_free(line);
  buf_free(decr);
  buf_free(sig);
  buf_free(mid);
  buf_free(opt);
  buf_free(name);
  buf_free(config);
  buf_free(rblocks);
  buf_free(eklist);
  buf_free(nymlist);
  buf_free(userpass);
  buf_free(ek);
  return (decrypted);
#endif /* else if USE_PGP */
}

int nymlist_read(BUFFER *list)
{
#ifdef USE_PGP
  BUFFER *key;

#endif /* USE_PGP */
  FILE *f;
  int err = 0;

  buf_clear(list);
  f = mix_openfile(NYMDB, "rb");
  if (f != NULL) {
    buf_read(list, f);
    fclose(f);
#ifdef USE_PGP
    key = buf_new();
    user_pass(key);
    if (key->length)
      if (pgp_decrypt(list, key, NULL, NULL, NULL) < 0) {
	buf_clear(list);
	err = -1;
      }
    buf_free(key);
#endif /* USE_PGP */
  }
  return (err);
}

int nymlist_write(BUFFER *list)
{
#ifdef USE_PGP
  BUFFER *key;

#endif /* USE_PGP */
  FILE *f;

  if (list->length == 0)
    return (-1);

#ifdef USE_PGP
  key = buf_new();
  user_pass(key);
  if (key->length)
    pgp_encrypt(PGP_NCONVENTIONAL | PGP_NOARMOR, list, key, NULL, NULL, NULL,
		NULL);
  buf_free(key);
#endif /* USE_PGP */
  f = mix_openfile(NYMDB, "wb");
  if (f == NULL)
    return (-1);
  else {
    buf_write(list, f);
    fclose(f);
  }
  return (0);
}

int nymlist_get(BUFFER *list, char *nym, BUFFER *config, BUFFER *ek,
		BUFFER *opt, BUFFER *name, BUFFER *chains, int *status)
{
  BUFFER *line;
  int err = -1;

  line = buf_new();
  if (ek)
    buf_clear(ek);
  if (opt)
    buf_clear(opt);
  if (name)
    buf_clear(name);
  if (chains)
    buf_clear(chains);
  if (config)
    buf_clear(config);

  for (;;) {
    if (buf_getline(list, line) == -1)
      goto end;
    if (bufleft(line, "nym="))
      break;
  }
  strncpy(nym, line->data + 4, LINELEN);

  for (;;) {
    if (buf_getline(list, line) == -1)
      break;
    if (opt && bufleft(line, "opt="))
      line->ptr = 4, buf_rest(opt, line);
    if (name && bufleft(line, "name="))
      line->ptr = 5, buf_rest(name, line);
    if (config && bufleft(line, "config="))
      line->ptr = 7, buf_rest(config, line);
    if (bufeq(line, "ek=")) {
      while (buf_getline(list, line) == 0 && !bufeq(line, "end ek"))
	if (ek) {
	  buf_cat(ek, line);
	  buf_nl(ek);
	}
    }
    if (bufeq(line, "chains=")) {
      while (buf_getline(list, line) == 0 && !bufeq(line, "end chains"))
	if (chains) {
	  buf_cat(chains, line);
	  buf_nl(chains);
	}
    }
    if (status && bufleft(line, "status="))
      *status = line->data[7] - '0';
    if (bufeq(line, "end")) {
      err = 0;
      break;
    }
  }
end:
  buf_free(line);
  return (err);
}

int nymlist_append(BUFFER *list, char *nym, BUFFER *config, BUFFER *opt,
		   BUFFER *name, BUFFER *rblocks, BUFFER *eklist, int status)
{
  buf_appends(list, "nym=");
  buf_appends(list, nym);
  buf_nl(list);
  buf_appends(list, "config=");
  buf_cat(list, config);
  buf_nl(list);
  buf_appends(list, "status=");
  buf_appendc(list, (byte) (status + '0'));
  buf_nl(list);
  if (name) {
    buf_appends(list, "name=");
    buf_cat(list, name);
    buf_nl(list);
  }
  buf_appends(list, "opt=");
  buf_cat(list, opt);
  buf_nl(list);
  buf_appends(list, "chains=\n");
  buf_cat(list, rblocks);
  buf_appends(list, "end chains\n");
  buf_appends(list, "ek=\n");
  buf_cat(list, eklist);
  buf_appends(list, "end ek\n");
  buf_appends(list, "end\n");
  return (0);
}

int nymlist_del(BUFFER *list, char *nym)
{
  BUFFER *new;
  char thisnym[LINELEN];
  BUFFER *config, *ek, *name, *rblocks, *opt;
  int thisstatus;

  new = buf_new();
  config = buf_new();
  ek = buf_new();
  name = buf_new();
  rblocks = buf_new();
  opt = buf_new();

  buf_rewind(list);
  while (nymlist_get(list, thisnym, config, ek, opt, name, rblocks,
		     &thisstatus) >= 0)
    if (!streq(nym, thisnym))
      nymlist_append(new, thisnym, config, opt, name, rblocks, ek,
		     thisstatus);

  buf_move(list, new);
  buf_free(new);
  buf_free(name);
  buf_free(opt);
  buf_free(rblocks);
  buf_free(config);
  buf_free(ek);
  return (0);
}

int nymlist_getnym(char *nym, BUFFER *config, BUFFER *ek, BUFFER *opt,
		   BUFFER *name, BUFFER *rblocks)
     /* "nym@nymserver.domain" or "nym@" */
{
  BUFFER *nymlist, *userpass;
  char n[LINELEN];
  int err = -1;
  int status;

  nymlist = buf_new();
  userpass = buf_new();

  user_pass(userpass);
  if (nymlist_read(nymlist) != -1) {
    while (nymlist_get(nymlist, n, config, ek, opt, name, rblocks,
		       &status) >= 0)
      if (streq(nym, n) || (nym[strlen(nym) - 1] == '@' && strleft(n, nym))) {
	err = status;
	strncpy(nym, n, LINELEN);
	break;
      }
  }
  buf_free(userpass);
  buf_free(nymlist);
  return (err);
}

int nymlist_getstatus(char *nym)
{
  int status;

  if ((status = nymlist_getnym(nym, NULL, NULL, NULL, NULL, NULL)) == 0)
    return (status);
  else
    return (-1);
}

#endif /* NYMSUPPORT */
