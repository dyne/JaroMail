/* Mixmaster version 3.0  --  (C) 1999 - 2006 Anonymizer Inc. and others.

   Mixmaster may be redistributed and modified under certain conditions.
   This software is distributed on an "AS IS" basis, WITHOUT WARRANTY OF
   ANY KIND, either express or implied. See the file COPYRIGHT for
   details.

   Encrypt message for Cypherpunk remailer chain
   $Id: chain1.c 934 2006-06-24 13:40:39Z rabbi $ */


#include "mix3.h"
#include "pgp.h"
#include <string.h>
#include <ctype.h>

#define N(X) (isdigit(X) ? (X)-'0' : 0)

int t1_rlist(REMAILER remailer[], int badchains[MAXREM][MAXREM])
{
  FILE *list, *excl;
  int i, listed = 0;
  int n = 0;
  char line[2 * LINELEN], l2[LINELEN], name[LINELEN], *flags;
  BUFFER *starex;

  starex = buf_new();
  excl = mix_openfile(STAREX, "r");
  if (excl != NULL) {
    buf_read(starex, excl);
    fclose(excl);
  }

  list = mix_openfile(TYPE1LIST, "r");
  if (list == NULL) {
    buf_free(starex);
    return (-1);
  }

  while (fgets(line, sizeof(line), list) != NULL && n < MAXREM) {
    if (strleft(line, "$remailer") &&
	strchr(line, '<') && strchr(line, '>') &&
	strchr(line, '{') && strchr(line, '{') + 4 < strchr(line, '}')) {
      if (line[strlen(line) - 1] == '\n')
	line[strlen(line) - 1] = '\0';
      if (line[strlen(line) - 1] == '\r')
	line[strlen(line) - 1] = '\0';
      while (line[strlen(line) - 1] == ' ')
	line[strlen(line) - 1] = '\0';
      if (line[strlen(line) - 1] != ';'
	  && fgets(l2, sizeof(l2), list) != NULL)
	strcatn(line, l2, LINELEN);
      flags = strchr(line, '>');
      strncpy(name, strchr(line, '{') + 2,
	      strchr(line, '}') - strchr(line, '{') - 3);
      name[strchr(line, '}') - strchr(line, '{') - 3] = '\0';
      name[20] = '\0';

      for (i = 1; i <= n; i++)
	if (streq(name, remailer[i].name))
	  break;
      if (i > n) {
	/* not in mix list */
	n++;
	strcpy(remailer[i].name, name);
	strncpy(remailer[i].addr, strchr(line, '<') + 1,
		strchr(line, '>') - strchr(line, '<'));
	remailer[i].addr[strchr(line, '>') - strchr(line, '<') - 1]
	  = '\0';
	remailer[i].flags.mix = 0;
	remailer[i].flags.post = strifind(flags, " post");
      }
      remailer[i].flags.cpunk = strfind(flags, " cpunk");
      remailer[i].flags.pgp = strfind(flags, " pgp");
      remailer[i].flags.pgponly = strfind(flags, " pgponly");
      remailer[i].flags.latent = strfind(flags, " latent");
      remailer[i].flags.middle = strfind(flags, " middle");
      remailer[i].flags.ek = strfind(flags, " ek");
      remailer[i].flags.esub = strfind(flags, " esub");
      remailer[i].flags.newnym = strfind(flags, " newnym");
      remailer[i].flags.nym = strfind(flags, " nym");
      remailer[i].info[1].reliability = 0;
      remailer[i].info[1].latency = 0;
      remailer[i].info[1].history[0] = '\0';
      remailer[i].flags.star_ex = bufifind(starex, name);
   }
    if (strleft(line,
		"-----------------------------------------------------------------------"))
      break;
  }
  n++;				/* ?? */
  while (fgets(line, sizeof(line), list) != NULL) {
    if (strlen(line) >= 72 && strlen(line) <= 73)
      for (i = 1; i < n; i++)
	if (strleft(line, remailer[i].name) &&
	    line[strlen(remailer[i].name)] == ' ') {
	  strncpy(remailer[i].info[1].history, line + 42, 12);
	  remailer[i].info[1].history[12] = '\0';
	  remailer[i].info[1].reliability = 10000 * N(line[64])
	    + 1000 * N(line[65]) + 100 * N(line[66])
	    + 10 * N(line[68]) + N(line[69]);
	  remailer[i].info[1].latency = 36000 * N(line[55])
	    + 3600 * N(line[56]) + 600 * N(line[58])
	    + 60 * N(line[59]) + 10 * N(line[61])
	    + N(line[62]);
	  listed++;
	}
  }
  fclose(list);
  parse_badchains(badchains, TYPE1LIST, "Broken type-I remailer chains", remailer, n);
  if (listed < 4)		/* we have no valid reliability info */
    for (i = 1; i < n; i++)
      remailer[i].info[1].reliability = 10000;

#ifdef USE_PGP
  pgp_rlist(remailer, n);
#endif /* USE_PGP */
  buf_free(starex);
  return (n);
}

int t1_ek(BUFFER *key, BUFFER *seed, int num)
{
  buf_reset(key);
  buf_appendc(key, (byte) num);
  buf_cat(key, seed);
  digest_md5(key, key);
  encode(key, 0);
#ifdef DEBUG
  fprintf(stderr, "passphrase=%s (%2X%2X%2X%2X %d)\n", key->data,
	  seed->data[0], seed->data[1], seed->data[2], seed->data[3], num);
#endif /* DEBUG */
  return (0);
}

int t1_encrypt(int type, BUFFER *message, char *chainstr, int latency,
	       BUFFER *ek, BUFFER *feedback)
{
  BUFFER *b, *rem, *dest, *line, *field, *content;
  REMAILER remailer[MAXREM];
  int badchains[MAXREM][MAXREM];
  int maxrem, chainlen = 0;
  int chain[20];
  int hop;
  int hashmark = 0;
  int err = 0;

  b = buf_new();
  rem = buf_new();
  dest = buf_new();
  line = buf_new();
  field = buf_new();
  content = buf_new();

  maxrem = t1_rlist(remailer, badchains);
  if (maxrem < 1) {
    clienterr(feedback, "No remailer list!");
    err = -1;
    goto end;
  }
  chainlen = chain_select(chain, chainstr, maxrem, remailer, 1, line);
  if (chainlen < 1) {
    if (line->length)
      clienterr(feedback, line->data);
    else
      clienterr(feedback, "Invalid remailer chain!");
    err = -1;
    goto end;
  }
  if (chain[0] == 0)
    chain[0] = chain_randfinal(type, remailer, badchains, maxrem, 1, chain, chainlen, 0);

  if (chain[0] == -1) {
    clienterr(feedback, "Invalid remailer chain!");
    err = -1;
    goto end;
  }
  if (chain_rand(remailer, badchains, maxrem, chain, chainlen, 1, 0) == -1) {
    clienterr(feedback, "No reliable remailers!");
    err = -1;
    goto end;
  }
  while (buf_getheader(message, field, content) == 0) {
    hdr_encode(content, 0);
    if (type == MSG_POST && bufieq(field, "newsgroups") &&
	remailer[chain[0]].flags.post) {
      buf_appendf(dest, "Anon-Post-To: %b\n", content);
    } else if (type == MSG_MAIL && bufieq(field, "to")) {
      buf_appendf(dest, "Anon-To: %b\n", content);
    } else {
      /* paste header */
      if (type == MSG_POST && bufieq(field, "newsgroups"))
	buf_appendf(dest, "Anon-To: %s\n", MAILtoNEWS);
      if (hashmark == 0) {
	buf_appends(b, "##\n");
	hashmark = 1;
      }
      buf_appendheader(b, field, content);
    }
  }
  buf_nl(b);
  buf_rest(b, message);
  buf_move(message, b);

  if (type != MSG_NULL && dest->length == 0) {
    clienterr(feedback, "No destination address!");
    err = -1;
    goto end;
  }
  if (type == MSG_NULL) {
    buf_sets(dest, "Null:\n");
  }
  for (hop = 0; hop < chainlen; hop++) {
    if (hop == 0) {
      buf_sets(b, "::\n");
      buf_cat(b, dest);
    } else {
      buf_sets(b, "::\nAnon-To: ");
      buf_appends(b, remailer[chain[hop - 1]].addr);
      buf_nl(b);
    }
    if (remailer[chain[hop]].flags.latent && latency > 0)
      buf_appendf(b, "Latent-Time: +%d:00r\n", latency);
    if (ek && remailer[chain[hop]].flags.ek) {
      t1_ek(line, ek, hop);
      buf_appendf(b, "Encrypt-Key: %b\n", line);
    }
    buf_nl(b);
    buf_cat(b, message);
#ifdef USE_PGP
    if (remailer[chain[hop]].flags.pgp) {
      buf_clear(message);
      buf_clear(rem);
      buf_setf(rem, "<%s>", remailer[chain[hop]].addr);
      err = pgp_encrypt(PGP_ENCRYPT | PGP_REMAIL | PGP_TEXT, b, rem,
			NULL, NULL, NULL, NULL);
      if (err < 0) {
	buf_setf(line, "No PGP key for remailer %s!\n",
		 remailer[chain[hop]].name);
	clienterr(feedback, line->data);
	goto end;
      }
      buf_appends(message, "::\nEncrypted: PGP\n\n");
      buf_cat(message, b);
    } else
#endif /* USE_PGP */
    {
      if (remailer[chain[hop]].flags.pgponly) {
	buf_setf(line, "PGP encryption needed for remailer %s!\n",
		 remailer[chain[hop]].name);
	clienterr(feedback, line->data);
	goto end;
      }
      buf_move(message, b);
    }
    if (ek && remailer[chain[hop]].flags.ek)
      buf_appends(message, "\n**\n");
  }
  buf_clear(b);
  if (chainlen == 0) {
    buf_appends(b, "::\n");
    buf_cat(b, dest);
  } else {
    buf_appendf(b, "%s: %s\n", ek ? "::\nAnon-To" : "To",
		remailer[chain[chainlen - 1]].addr);
  }
  buf_nl(b);
  buf_cat(b, message);
  buf_move(message, b);
end:
  buf_free(b);
  buf_free(rem);
  buf_free(dest);
  buf_free(line);
  buf_free(field);
  buf_free(content);
  return (err);
}

#ifdef USE_PGP
int t1_getreply(BUFFER *msg, BUFFER *ek, int len)
{
  BUFFER *key, *decrypt;
  int err = -1;
  int hop = 0;

  key = buf_new();
  decrypt = buf_new();

  do {
    t1_ek(key, ek, hop);
    buf_set(decrypt, msg);
    if (pgp_decrypt(decrypt, key, NULL, NULL, NULL) == 0
	&& decrypt->data != NULL)
      err = 0, buf_move(msg, decrypt);
  }
  while (hop++ < len);
  return (err);
}

#endif /* USE_PGP */
