/* Mixmaster version 3.0  --  (C) 1999 - 2006 Anonymizer Inc. and others.

   Mixmaster may be redistributed and modified under certain conditions.
   This software is distributed on an "AS IS" basis, WITHOUT WARRANTY OF
   ANY KIND, either express or implied. See the file COPYRIGHT for
   details.

   Menu-based user interface - nym management
   $Id: menunym.c 934 2006-06-24 13:40:39Z rabbi $ */

#ifdef NYMSUPPORT

#include "menu.h"
#include "mix3.h"
#include <string.h>
#include <stdlib.h>
#ifdef POSIX
#include <unistd.h>
#endif /* POSIX */

#ifdef USE_NCURSES
void menu_nym(char *nnym)
{
  char nym[maxnym][LINELEN];
  char pending[maxnym][LINELEN];
  int c, i, num = 0, numpending = 0, select = -1;
  int edit = 0;
  BUFFER *nymlist;
  int s;
  int pass = 0;
  char reliability[9]; /* When printing information about a chain,
			  this variable stores the reliability. */

  nymlist = buf_new();

  strcpy(nym[0], NONANON);
  strcatn(nym[0], " (", sizeof(nym[0]));
  strcatn(nym[0], NAME, sizeof(nym[0]));
  strcatn(nym[0], ")", sizeof(nym[0]));

  strcpy(nym[1], ANON);
  num = 2;
  if (nymlist_read(nymlist) == -1) {
    user_delpass();
    mix_status("");
  } else
    pass = 1;
  while (nymlist_get(nymlist, nym[num], NULL, NULL, NULL, NULL, NULL, &s) >= 0) {
    if (s == NYM_OK) {
      if (num < maxnym)
	num++;
    } else if (s == NYM_WAITING) {
      if (numpending < maxnym)
	strncpy(pending[numpending++], nym[num], LINELEN);
    }
  }
  buf_free(nymlist);

nymselect:
  clear();
  standout();
  printw("Select nym:\n\n");
  standend();
#ifdef USE_PGP
  if (pass)
    printw("c)reate new nym\ne)dit nym\nd)elete nym\n\n");
  else
    printw("[nym passphrase is invalid]\n\n");
#endif /* USE_PGP */
  for (i = 0; i < num; i++)
    printw("%d) %s\n", i, nym[i]);
  if (numpending > 0) {
    printw("\n\nWaiting for confirmation: ");
    for (i = 0; i < numpending; i++)
      printw("%s ", pending[i]);
    printw("\n");
  }
select:
  if (select != -1)
    printw("\r%d", select);
  else
    printw("\r          \r");
  refresh();
  c = getch();
  if (c == erasechar())
    c = KEY_BACKSPACE;
  if (c >= '0' && c <= '9') {
    if (select == -1)
      select = c - '0';
    else
      select = 10 * select + c - '0';
    if (edit ? select == 0 || select >= num + numpending - 1 : select >= num) {
      beep();
      select = -1;
    }
    refresh();
    goto select;
  } else
    switch (c) {
    case KEY_BACKSPACE:
      select /= 10;
      if (select < 1)
	select = -1;
      goto select;
    case 'q':
      if (edit) {
	edit = 0;
	select = -1;
	goto nymselect;
      }
      break;
#ifdef USE_PGP
    case 'e':
      if (pass) {
	if (edit || num + numpending < 3) {
	  edit = 0;
	  select = -1;
	  goto nymselect;
	} else {
	  clear();
	  standout();
	  printw("Edit nym:\n\n");
	  standend();
	  for (i = 2; i < num + numpending; i++)
	    printw("%d) %s\n", i - 1, i < num ? nym[i] : pending[i - num]);
	  printw("\n");
	  select = -1;
	  edit = NYM_MODIFY;
	  goto select;
	}
      }
      break;
    case 'd':
      if (pass) {
	if (edit || num + numpending < 3) {
	  edit = 0;
	  select = -1;
	  goto nymselect;
	} else {
	  clear();
	  standout();
	  printw("Delete nym:\n\n");
	  standend();
	  for (i = 2; i < num + numpending; i++)
	    printw("%d) %s\n", i - 1, i < num ? nym[i] : pending[i - num]);
	  printw("\n");
	  select = -1;
	  edit = NYM_DELETE;
	  goto select;
	}
      }
      break;
    case '\r':
    case '\n':
      if (select == -1 || (edit && select == 0)) {
	beep();
	edit = 0;
	select = -1;
	goto nymselect;
      }
      if (!edit) {
	strncpy(nnym, nym[select], LINELEN);
	return;
      }
      /* fallthru */
    case 'c':
      if (pass) {
	char nymserv[LINELEN] = "*";
	char replyblock[5][CHAINMAX], dest[10][LINELEN];
	int latent[5], desttype[5];
	char mdest[LINELEN], pdest[LINELEN] = "alt.anonymous.messages",
	psub[LINELEN] = "";
	int deflatent = 0, defdesttype = MSG_MAIL;
	char alias[LINELEN] = "";
	BUFFER *name, *opt;
	char sendchain[CHAINMAX];
	int sendnumcopies = 1, rnum = 1;
	int i;
	char line[LINELEN];
	int acksend = 0, signsend = 0, fixedsize = 0, disable = 0,
	    fingerkey = 1;

	name = buf_new();
	opt = buf_new();
	strncpy(sendchain, CHAIN, CHAINMAX);
	strncpy(mdest, ADDRESS, LINELEN);
	if (edit)
	  strncpy(alias, select + 1 < num ? nym[select + 1] :
		  pending[select + 1 - num], LINELEN);
	if (edit == NYM_MODIFY) {
	  nymlist_getnym(alias, NULL, NULL, opt, name, NULL);
	  acksend = bufifind(opt, "+acksend");
	  signsend = bufifind(opt, "+signsend");
	  fixedsize = bufifind(opt, "+fixedsize");
	  disable = bufifind(opt, "+disable");
	  fingerkey = bufifind(opt, "+fingerkey");
	  rnum = -1;
	}
      newnym:
	if (!edit) {
	  clear();
	  standout();
	  printw("Create a nym:");
	  standend();

	  mvprintw(3, 0, "Alias address: ");
	  echo();
	  wgetnstr(stdscr, alias, LINELEN);
	  noecho();
	  if (alias[0] == '\0')
	    goto end;
	  for (i = 0; alias[i] > ' ' && alias[i] != '@'; i++) ;
	  alias[i] = '\0';
	  if (i == 0)
	    goto newnym;
	  mvprintw(4, 0, "Pseudonym: ");
	  echo();
	  wgetnstr(stdscr, line, LINELEN);
	  noecho();
	  buf_sets(name, line);
	  menu_chain(nymserv, 2, 0);
	}
	if (edit != NYM_DELETE) {
	  for (i = 0; i < 5; i++) {
	    desttype[i] = defdesttype;
	    latent[i] = deflatent;
	    dest[i][0] = '\0';
	    strcpy(replyblock[i], "*,*,*,*");
	  }
	  if (rnum != -1) {
	    menu_replychain(&defdesttype, &deflatent, mdest, pdest, psub,
			    replyblock[0]);
	    desttype[0] = defdesttype;
	    latent[0] = deflatent;
	    strncpy(dest[0], desttype[0] == MSG_POST ? pdest : mdest,
		    LINELEN);
	  }
	}
      redraw:
	clear();
	standout();
	switch (edit) {
	case NYM_DELETE:
	  printw("Delete nym:");
	  break;
	case NYM_MODIFY:
	  printw("Edit nym:");
	  break;
	default:
	  printw("Create a nym:");
	  break;
	}
	standend();
      loop:
	{
	  if (!edit) {
	    cl(2, 0);
	    printw("Nym: a)lias address: %s", alias);
	    cl(3, 0);
	    printw("     nym s)erver: %s", nymserv);
	  }
	  if (edit != NYM_DELETE) {
	    cl(4, 0);
	    printw("     p)seudonym: %s", name->data);
	    if (edit)
	      mvprintw(6, 0, "Nym modification:");
	    else
	      mvprintw(6, 0, "Nym creation:");
	  }
	  cl(7, 0);
	  chain_reliability(sendchain, 0, reliability); /* chaintype 0=mix */
	  printw("     c)hain to nym server: %-30s (reliability: %s)", sendchain, reliability);
	  cl(8, 0);
	  printw("     n)umber of redundant copies: %d", sendnumcopies);
	  if (edit != NYM_DELETE) {
	    mvprintw(10, 0, "Configuration:\n");
	    printw("     A)cknowledge sending: %s\n", acksend ? "yes" : "no");
	    printw("     S)erver signatures: %s\n", signsend ? "yes" : "no");
	    printw("     F)ixed size replies: %s\n", fixedsize ? "yes" :
		   "no");
	    printw("     D)isable: %s\n", disable ? "yes" : "no");
	    printw("     Finger K)ey: %s\n", fingerkey ? "yes" : "no");
	    mvprintw(17, 0, "Reply chains:");
	    cl(18, 0);
	    if (rnum == -1)
	      printw("     create new r)eply block");
	    else {
 	      printw("     number of r)eply chains: %2d                                     reliability", rnum);
	      for (i = 0; i < rnum; i++) {
		cl(i + 19, 0);
 		chain_reliability(replyblock[i], 1, reliability); /* 1=ek */
 		printw("     %d) %30s %-31s [%s]", i + 1,
  		       desttype[i] == MSG_NULL ?
 		       "(cover traffic)" : dest[i], replyblock[i],
 		       reliability);
	      }
	    }
	  }
	  move(LINES - 1, COLS - 1);
	  refresh();
	  c = getch();
	  if (edit != NYM_DELETE && c >= '1' && c <= '9' && c - '1' < rnum) {
	    menu_replychain(&defdesttype, &deflatent, mdest, pdest, psub,
			    replyblock[c - '1']);
	    desttype[c - '1'] = defdesttype;
	    latent[c - '1'] = deflatent;
	    strncpy(dest[c - '1'],
		    desttype[c - '1'] == MSG_POST ? pdest : mdest, LINELEN);
	    goto redraw;
	  }
	  switch (c) {
	  case 'A':
	    acksend = !acksend;
	    goto redraw;
	  case 'S':
	    signsend = !signsend;
	    goto redraw;
	  case 'F':
	    fixedsize = !fixedsize;
	    goto redraw;
	  case 'D':
	    disable = !disable;
	    goto redraw;
	  case 'K':
	    fingerkey = !fingerkey;
	    goto redraw;
	  case 'q':
	    edit = 0;
	    select = -1;
	    goto nymselect;
	  case '\014':
	    goto redraw;
	  case 'a':
	    cl(2, 0);
	    printw("Nym: a)lias address: ");
	    echo();
	    wgetnstr(stdscr, alias, LINELEN);
	    noecho();
	    for (i = 0; alias[i] > ' ' && alias[i] != '@'; i++) ;
	    alias[i] = '\0';
	    if (i == 0)
	      goto nymselect;
	    goto redraw;
	  case 'p':
	    cl(4, 0);
	    printw("     p)seudonym: ");
	    echo();
	    wgetnstr(stdscr, line, LINELEN);
	    noecho();
	    if (line[0] != '\0')
	      buf_sets(name, line);
	    goto redraw;
	  case 'c':
	    menu_chain(sendchain, 0, 0);
	    goto redraw;
	  case 'n':
	    cl(8, 0);
	    printw("     n)umber of redundant copies: ");
	    echo();
	    wgetnstr(stdscr, line, LINELEN);
	    noecho();
	    sendnumcopies = strtol(line, NULL, 10);
	    if (sendnumcopies < 1 || sendnumcopies > 10)
	      sendnumcopies = 1;
	    goto redraw;
	  case 'r':
	    cl(18, 0);
	    printw("     number of r)eply chains: ");
	    echo();
	    wgetnstr(stdscr, line, LINELEN);
	    noecho();
	    i = rnum;
	    rnum = strtol(line, NULL, 10);
	    if (rnum < 1)
	      rnum = 1;
	    if (rnum > 5)
	      rnum = 5;
	    for (; i < rnum; i++)
	      if (dest[i][0] == '\0') {
		desttype[i] = defdesttype;
		latent[i] = deflatent;
		strncpy(dest[i], defdesttype == MSG_POST ? pdest :
			mdest, LINELEN);
	      }
	    goto redraw;
	  case 's':
	    menu_chain(nymserv, 2, 0);
	    goto redraw;
	  case '\n':
	  case '\r':
	    {
	      BUFFER *chains;
	      int err;

	      if (rnum == -1)
		chains = NULL;
	      else {
		chains = buf_new();
		for (i = 0; i < rnum; i++)
		  if (replyblock[i][0] != '\0') {
		    if (desttype[i] == MSG_POST)
		      buf_appendf(chains, "Subject: %s\n", psub);
		    if (desttype[i] == MSG_MAIL)
		      buf_appends(chains, "To: ");
		    else if (desttype[i] == MSG_POST)
		      buf_appends(chains, "Newsgroups: ");
		    else
		      buf_appends(chains, "Null:");
		    buf_appendf(chains, "%s\n", dest[i]);
		    buf_appendf(chains, "Chain: %s\n", replyblock[i]);
		    buf_appendf(chains, "Latency: %d\n\n", latent[i]);
		  }
	      }
	    create:
	      clear();
	      buf_setf(opt,
		       " %cacksend %csignsend +cryptrecv %cfixedsize %cdisable %cfingerkey",
		       acksend ? '+' : '-',
		       signsend ? '+' : '-',
		       fixedsize ? '+' : '-',
		       disable ? '+' : '-',
		       fingerkey ? '+' : '-');
	      if (edit) {
		mix_status("Preparing nymserver configuration message...");
		err = nym_config(edit, alias, NULL,
				 name, sendchain, sendnumcopies,
				 chains, opt);
	      } else {
		mix_status("Preparing nym creation request...");
		err = nym_config(edit, alias, nymserv, name,
				 sendchain, sendnumcopies, chains,
				 opt);
	      }
	      if (err == -3) {
		beep();
		mix_status("Bad passphrase!");
		getch();
		goto create;
	      }
	      if (err != 0) {
		mix_genericerror();
		beep();
		refresh();
	      } else {
		if (edit)
		  mix_status("Nymserver configuration message completed.");
		else
		  mix_status("Nym creation request completed.");
	      }
	      if (chains)
		buf_free(chains);
	      goto end;
	    }
	  default:
	    beep();
	    goto loop;
	  }
	}
      end:
	buf_free(name);
	buf_free(opt);
	return;
      }
#endif /* USE_PGP */
    default:
      beep();
      goto select;
    }
}

#endif /* USE_NCURSES */
#endif /* NYMSUPPORT */
