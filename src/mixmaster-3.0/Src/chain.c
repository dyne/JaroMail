/* Mixmaster version 3.0  --  (C) 1999 - 2006 Anonymizer Inc. and others.

   Mixmaster may be redistributed and modified under certain conditions.
   This software is distributed on an "AS IS" basis, WITHOUT WARRANTY OF
   ANY KIND, either express or implied. See the file COPYRIGHT for
   details.

   Prepare messages for remailer chain
   $Id: chain.c 934 2006-06-24 13:40:39Z rabbi $ */


#include "mix3.h"
#include <string.h>
#include <ctype.h>
#include <assert.h>
#include <stdlib.h>

void clienterr(BUFFER *msgbuf, char *err)
{
  if (msgbuf) {
    buf_sets(msgbuf, "Error: ");
    buf_appends(msgbuf, err);
  } else
    errlog(ERRORMSG, "%s\n", err);
}

void parse_badchains(int badchains[MAXREM][MAXREM], char *file, char *startindicator, REMAILER *remailer, int maxrem) {
  int i,j;
  FILE *list;
  char line[LINELEN];

  if (!badchains)
    return;

  for (i = 0; i < maxrem; i++ )
    for (j = 0; j < maxrem; j++ )
      badchains[i][j] = 0;
  list = mix_openfile(TYPE2REL, "r");
  if (list != NULL) {
    while (fgets(line, sizeof(line), list) != NULL &&
      !strleft(line, startindicator)) ;
    while (fgets(line, sizeof(line), list) != NULL &&
      strleft(line, "(")) {
      char *left, *right, *tmp;
      int lefti, righti;

      left = line + 1;
      while (*left == ' ')
	left ++;

      tmp = left + 1;
      while (*tmp != ' ' && *tmp != '\0' && *tmp != ')')
	tmp ++;
      if (*tmp == '\0' || *tmp == ')')
	/* parsing this line failed */
	continue;
      *tmp = '\0';

      right = tmp+1;
      while (*right == ' ')
	right ++;
      tmp = right + 1;
      while (*tmp != ' ' && *tmp != '\0' && *tmp != ')')
	tmp ++;
      if (*tmp == '\0')
	/* parsing this line failed */
	continue;
      *tmp = '\0';

      lefti = -1;
      righti = -1;
      for (i = 1; i < maxrem; i++) {
	if (strcmp(remailer[i].name, left) == 0)
	  lefti = i;
	if (strcmp(remailer[i].name, right) == 0)
	  righti = i;
      }
      if (strcmp(left, "*") == 0)
	lefti = 0;
      if (strcmp(right, "*") == 0)
	righti = 0;

      if (lefti == -1 || righti == -1)
	/* we don't know about one or both remailers */
	continue;
      badchains[lefti][righti] = 1;
    }
    fclose(list);
    /* If some broken chain includes all remailers (*) mark it broken for
     * every single remailer - this simplifies handling in other places */
    for (i=1; i < maxrem; i++ ) {
      if (badchains[0][i])
	for (j=1; j < maxrem; j++ )
	  badchains[j][i] = 1;
      if (badchains[i][0])
	for (j=1; j < maxrem; j++ )
	  badchains[i][j] = 1;
    }
  }
}


int chain_select(int hop[], char *chainstr, int maxrem, REMAILER *remailer,
		 int type, BUFFER *feedback)
{
  int len = 0;
  int i, j, k;
  BUFFER *chain, *selected, *addr;
  chain = buf_new();
  selected = buf_new();
  addr = buf_new();

  if (chainstr == NULL || chainstr[0] == '\0')
    buf_sets(chain, CHAIN);
  else
    buf_sets(chain, chainstr);

  /* put the chain backwards: final hop is in hop[0] */

  for (i = chain->length; i >= 0; i--)
    if (i == 0 || chain->data[i - 1] == ','
	|| chain->data[i - 1] == ';' || chain->data[i - 1] == ':') {
      for (j = i; isspace(chain->data[j]);)	/* ignore whitespace */
	j++;
      if (chain->data[j] == '\0')
	break;

      if (chain->data[j] == '*')
	k = 0;
#if 0
      else if (isdigit(chain->data[j]))
	k = atoi(chain->data + j);
#endif /* 0 */
      else {
	buf_sets(selected, chain->data + j);
	rfc822_addr(selected, addr);
	buf_clear(selected);
	buf_getline(addr, selected);
	if (!selected->length)
	  buf_sets(selected, chain->data + j);

	for (k = 0; k < maxrem; k++)
	  if (((remailer[k].flags.mix && type == 0) ||
	       (remailer[k].flags.cpunk && type == 1) ||
	       (remailer[k].flags.newnym && type == 2)) &&
	      (streq(remailer[k].name, selected->data) ||
	       strieq(remailer[k].addr, selected->data) ||
	       (selected->data[0] == '@' && strifind(remailer[k].addr,
					    selected->data))))
	    break;
      }
      if (k < 0 || k >= maxrem) {
	if (feedback != NULL) {
		buf_appendf(feedback, "No such remailer: %b", selected);
		buf_nl(feedback);
	}
#if 0
	k = 0;
#else /* end of 0 */
	len = -1;
	goto end;
#endif /* else not 0 */
      }
      hop[len++] = k;
      if (len >= 20) {          /* array passed in is has length 20 */
	if (feedback != NULL) {
		buf_appends(feedback, "Chain too long.\n");
	}
	break;
      }
      if (i > 0)
	chain->data[i - 1] = '\0';
    }
end:
  buf_free(chain);
  buf_free(selected);
  buf_free(addr);
  return len;
}

int chain_randfinal(int type, REMAILER *remailer, int badchains[MAXREM][MAXREM], int maxrem, int rtype, int chain[], int chainlen, int ignore_constraints_if_necessary)
{
  int randavail;
  int i;
  int t;
  int select[MAXREM];
  int secondtolasthop = (chainlen <= 1 ? -1 : chain[1]);
  int constraints_ignored = 0;

  t = rtype;
  if (rtype == 2)
    t = 1;

start:
  randavail = 0;
  /* select a random final hop */
  for (i = 1; i < maxrem; i++) {
    select[i] = 
       ((remailer[i].flags.mix && rtype == 0) ||             /* remailer supports type */
	 (remailer[i].flags.pgp && remailer[i].flags.ek && rtype == 1) ||
	 (remailer[i].flags.newnym && rtype == 2)) &&
	(remailer[i].info[t].reliability >= 100 * RELFINAL || constraints_ignored ) && /* remailer has sufficient reliability */
	(remailer[i].info[t].latency <= MAXLAT || constraints_ignored ) &&             /* remailer has low enough latency */
	(remailer[i].info[t].latency >= MINLAT || constraints_ignored ) &&             /* remailer has high enough latency */
	(type == MSG_NULL || !remailer[i].flags.middle) &&   /* remailer is not middleman */
	!remailer[i].flags.star_ex &&                        /* remailer is not excluded from random selection */
	(remailer[i].flags.post || type != MSG_POST) &&      /* remailer supports post when this is a post */
	((secondtolasthop == -1) || !badchains[secondtolasthop][i]);
	                             /* we only have hop or the previous one can send to this (may be random) */
    randavail += select[i];
  }

  for (i = 1; i <= DISTANCE; i++)
    if (i < chainlen && select[chain[i]] && chain[i]) {
      select[chain[i]] = 0;
      randavail--;
    }

  assert(randavail >= 0);
  if (randavail == 0) {
    if (ignore_constraints_if_necessary && !constraints_ignored) {
      errlog(WARNING, "No reliable remailers. Ignoring for randhop\n");
      constraints_ignored = 1;
      goto start;
    };
    i = -1;
  } else {
    do
      i = rnd_number(maxrem - 1) + 1;
    while (!select[i]);
  }
  return (i);
}

int chain_rand(REMAILER *remailer, int badchains[MAXREM][MAXREM], int maxrem,
	       int thischain[], int chainlen, int t, int ignore_constraints_if_necessary)
     /* set random chain. returns 0 if not random, 1 if random, -1 on error */
/* t... 0 for mixmaster Type II
 *      1 for cypherpunk Type I
 */
{
  int hop;
  int err = 0;
  int constraints_ignored = 0;

  assert(t == 0 || t == 1);

start:
  for (hop = 0; hop < chainlen; hop++)
    if (thischain[hop] == 0) {
      int select[MAXREM];
      int randavail = 0;
      int i;

      err = 1;
      if (hop > 0)
	assert(thischain[hop-1]); /* we already should have chosen a remailer after this one */
      for (i = 1; i < maxrem; i++) {
	select[i] = ((remailer[i].flags.mix && t == 0) ||        /* remailer supports type */
		     (remailer[i].flags.pgp && remailer[i].flags.ek && t == 1)) &&
	  (remailer[i].info[t].reliability >= 100 * MINREL || constraints_ignored ) &&  /* remailer has sufficient reliability */
	  (remailer[i].info[t].latency <= MAXLAT || constraints_ignored ) &&            /* remailer has low enough latency */
	  (remailer[i].info[t].latency >= MINLAT || constraints_ignored ) &&            /* remailer has high enough latency */
	  !remailer[i].flags.star_ex &&                          /* remailer is not excluded from random selection */
	  !badchains[i][0] && !badchains[i][thischain[hop-1]] && /* remailer can send to the next one */
	  (hop == chainlen-1 || !badchains[thischain[hop+1]][i]);
	                           /* we are at the first hop or the previous one can send to this (may be random) */
	randavail += select[i];
      }

      for (i = hop - DISTANCE; i <= hop + DISTANCE; i++)
	if (i >= 0 && i < chainlen && select[thischain[i]] && thischain[i]) {
	  select[thischain[i]] = 0;
	  randavail--;
	}


      assert(randavail >= 0);
      if (randavail < 1) {
	if (ignore_constraints_if_necessary && !constraints_ignored) {
	  errlog(WARNING, "No reliable remailers. Ignoring for randhop\n");
	  constraints_ignored = 1;
	  goto start;
	};
	err = -1;
	goto end;
      }
      do
	thischain[hop] = rnd_number(maxrem - 1) + 1;
      while (!select[thischain[hop]]);
    }
end:
  return (err);
}

int mix_encrypt(int type, BUFFER *message, char *chainstr, int numcopies,
		BUFFER *chainlist)
{
  return (mix2_encrypt(type, message, chainstr, numcopies, 0, chainlist));
}

/* float chain_reliablity(char *chain, int chaintype,
	                  char *reliability_string);
 *
 * Compute reliablity of a chain.
 *
 * We get the reliablity of the chain by multiplying the reliablity of
 * every remailer in the chain. The return value is the reliablity of
 * the chain, or a negative number if the reliablity can not be
 * calculated. There are two reasons why may not be able to calculated
 * the reliablity: A remailer in the chain is selected randomly, or we
 * don't have statistics about one of the remailers in the chain.
 * remailer_type indicates the remailer type:
 * 0 = Mixmaster, 1 = Cypherpunk
 *
 * If reliability_string is non-NULL, the reliability is also returned
 * as a string in this variable. The size of the string must be at
 * least 9 characters!
 *
 * This function has been added by Gerd Beuster. (gb@uni-koblenz.de)
 *--------------------------------------------------------------------*/

float chain_reliability(char *chain, int chaintype,
			char *reliability_string){

  float acc_reliability = 1; /* Accumulated reliablity */
  char *name_start, *name_end; /* temporary pointers used
				 in string scanning */
  char remailer_name[20]; /* The length of the array is taken from mix3.h. */
  int error = 0;
  int maxrem;
  int i;
  int previous = -1;
  REMAILER remailer[MAXREM];
  int badchains[MAXREM][MAXREM];

  /* chaintype 0=mix 1=ek 2=newnym */
  assert((chaintype == 0) || (chaintype == 1));
  maxrem = (chaintype == 0 ? mix2_rlist(remailer, badchains) : t1_rlist(remailer, badchains));

  /* Dissect chain */
  name_start = chain;
  name_end = chain;
  while(*name_end != '\0'){ /* While string not scanned completely */
    do /* Get next remailer */
      name_end+=sizeof(char);
    while( (*name_end != ',') && (*name_end != '\0'));
    strncpy(remailer_name, name_start,
	    (name_end - name_start) / sizeof(char) + 1*sizeof(char));
    remailer_name[name_end-name_start]='\0';
    /* Lookup reliablity for remailer remailer_name */
    for(i=0;
	(i < maxrem) && (strcmp(remailer[i].name, remailer_name) != 0);
	i++);
    if(!strcmp(remailer[i].name, remailer_name)) { /* Found it! */
      acc_reliability *=
	((float) remailer[i].info[chaintype].reliability) / 10000;
      if (previous != -1) {
	if (badchains[previous][i] || badchains[0][i])
	  acc_reliability = 0;
      }
      previous = i;
    } else
      error = 1; /* Did not find this remailer. We can't calculate
		    the reliablity for the whole chain. */
    name_start = name_end+sizeof(char);
  }

  if(error || (name_start==name_end))
    acc_reliability = -1;

  /* Convert reliability into string, if appropriate */
  if(reliability_string){
    if(acc_reliability < 0)
      sprintf(reliability_string, "  n/a  ");
    else{
      sprintf(reliability_string, "%6.2f", acc_reliability*100);
      *(reliability_string+6*sizeof(char)) = '%';
    }
  }

  return acc_reliability;
}

