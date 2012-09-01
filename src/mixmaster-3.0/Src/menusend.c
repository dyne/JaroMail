/* Mixmaster version 3.0  --  (C) 1999 - 2006 Anonymizer Inc. and others.

   Mixmaster may be redistributed and modified under certain conditions.
   This software is distributed on an "AS IS" basis, WITHOUT WARRANTY OF
   ANY KIND, either express or implied. See the file COPYRIGHT for
   details.

   Menu-based user interface -- send message
   $Id: menusend.c 934 2006-06-24 13:40:39Z rabbi $ */


#include "menu.h"
#include "mix3.h"
#include <string.h>
#include <ctype.h>
#include <stdlib.h>
#ifdef POSIX
#include <unistd.h>
#else /* end of POSIX */
#include <io.h>
#endif /* else if not POSIX */

void send_message(int type, char *nym, BUFFER *in)
{
  char dest[LINELEN] = "", subject[LINELEN] = "";
  char chain[CHAINMAX], thisnym[LINELEN], path[PATHMAX];
  BUFFER *chainlist, *msg, *txt, *tmp, *field, *content, *cc, *cite;
  int numcopies;
  int hdr = 0;			/* txt buffer contains header lines */
  FILE *f;
  int n, err;

#ifdef USE_PGP
  int sign = 0, encrypt = 0, key = 0;

#endif /* USE_PGP */
#ifdef USE_NCURSES
  char reliability[9];
  int c;
  char line[LINELEN];

#endif /* USE_NCURSES */
  msg = buf_new();
  tmp = buf_new();
  txt = buf_new();
  field = buf_new();
  content = buf_new();
  chainlist = buf_new();
  cc = buf_new();
  cite = buf_new();
  strncpy(chain, CHAIN, CHAINMAX);
  numcopies = NUMCOPIES;

  mix_status("");
  strncpy(thisnym, nym, sizeof(thisnym));

  if (in != NULL)
    buf_set(txt, in);

  if (bufileft(txt, "From "))
    buf_getline(txt, field);	/* ignore envelope From */

  if (type == 'p' || type == 'm') {
#ifndef USE_NCURSES
    mix_status("Invalid option to -f");
    mix_exit();
    exit(1);
#else /* end of not USE_NCURSES */
    clear();
    echo();
    if (in != NULL)
      mvprintw(1, 0, "%s forwarding message...", thisnym);
    if (type == 'p')
      mvprintw(3, 0, "Newsgroups: ");
    else
      mvprintw(3, 0, "Send message to: ");
    refresh();
    wgetnstr(stdscr, dest, LINELEN);
    if (dest[0] == '\0') {
      noecho();
      cl(3, 0);
      goto quit;
    }
    if (txt->length == 0) {
      mvprintw(4, 0, "Subject: ");
      refresh();
      wgetnstr(stdscr, subject, LINELEN);
    } else {
      strcpy(subject, "Forwarded message");
      while (buf_getheader(txt, field, content) == 0) {
	if (bufieq(field, "subject")) {
	  strncpy(subject, content->data, sizeof(subject));
	  strcatn(subject, " (fwd)", sizeof(subject));
	}
	if (bufieq(field, "from") || bufieq(field, "subject") ||
	    bufieq(field, "date"))
	  buf_appendheader(tmp, field, content);
      }
      buf_nl(tmp);
      buf_rest(tmp, txt);
      buf_move(txt, tmp);
    }
    noecho();
#endif /* else if USE_NCURSES */
  } else {
    strcpy(subject, "Re: your mail");
    while (buf_getheader(txt, field, content) == 0) {
      if (bufieq(field, "subject")) {
	if (bufileft(content, "Re:"))
	  subject[0] = '\0';
	else
	  strcpy(subject, "Re: ");
	strcatn(subject, content->data, sizeof(subject));
      }
      if (bufieq(field, "from"))
	buf_set(cite, content);
      if (type == 'p' || type == 'f') {
	if (dest[0] == '\0' && bufieq(field, "newsgroups"))
	  strncpy(dest, content->data, sizeof(dest));
	if (bufieq(field, "followup-to") && !bufieq(content, "poster"))
	  strncpy(dest, content->data, sizeof(dest));
	if (bufieq(field, "message-id"))
	  buf_appendf(tmp, "References: %b\n", content);
      } else {
	if (dest[0] == '\0' && bufieq(field, "from"))
	  strncpy(dest, content->data, sizeof(dest));
	if (bufieq(field, "reply-to"))
	  strncpy(dest, content->data, sizeof(dest));
	if (type == 'g' && (bufieq(field, "to") || bufieq(field, "cc"))) {
	  if (cc->length)
	    buf_appends(cc, ", ");
	  buf_cat(cc, content);
	}
	if (bufieq(field, "message-id"))
	  buf_appendf(tmp, "In-Reply-To: %b\n", content);
      }
    }
    if (cc->length)
      buf_appendf(tmp, "Cc: %b\n", cc);
    if (tmp->length > 0)
      hdr = 1;
    if (hdr)
      buf_nl(tmp);

    if ((type == 'f' || type == 'g') && cite->length) {
      buf_appendf(tmp, "%b wrote:\n\n", cite);
    }
    if (type == 'r')
      buf_appends(tmp, "You wrote:\n\n");

    while (buf_getline(txt, content) != -1)
      buf_appendf(tmp, "> %b\n", content);
    buf_set(txt, tmp);
    if (dest[0] == '\0') {
#ifdef USE_NCURSES
      beep();
      mix_status("No recipient address found.");
#endif /* USE_NCURSES */
      goto quit;
    }
    goto edit;
  }

#ifdef USE_NCURSES
redraw:
  clear();

  for (;;) {
    standout();
    mvprintw(0, 0, "Mixmaster %s - ", VERSION);
    printw(type == 'p' || type == 'f' ? "posting to Usenet" : "sending mail");
    standend();
    mix_status(NULL);
    cl(2, 0);
#ifdef NYMSUPPORT
    printw("n)ym: %s", thisnym);
#endif /* NYMSUPPORT */
    if (!strleft(thisnym, NONANON)) {
      chain_reliability(chain, 0, reliability);   /* chaintype 0=mix */
      cl(4, 0);
      printw("c)hain: %-35s (reliability: %s)", chain, reliability);
      cl(5, 0);
      printw("r)edundancy: %3d copies ", numcopies);
    }
    cl(7, 0);
    printw("d)estination: %s", dest);
    cl(8, 0);
    printw("s)ubject: %s", subject);
#ifdef USE_PGP
    if (type != 'p' && type != 'f') {
      cl(10, 0);
      printw("pgp encry)ption: ");
      if (encrypt)
	printw("yes");
      else
	printw("no");
    }
    if (!streq(thisnym, ANON)) {
      cl(11, 0);
      printw("p)gp signature: ");
      if (sign)
	printw("yes");
      else
	printw("no");
      cl(12, 0);
      if (key == 0)
	printw("attach pgp k)ey: no");
    }
#endif /* USE_PGP */

    if (txt->length == 0)
      mvprintw(LINES - 3, 18,
	       "e)dit message           f)ile      q)uit w/o sending");
    else
      mvprintw(LINES - 3, 0,
	       "m)ail message      e)dit message         f)ile          q)uit w/o sending");
    move(LINES - 1, COLS - 1);
    refresh();
    c = getch();
    if (c != ERR) {
      mix_status("");
      if (c == '\r' || c == '\n') {	/* default action is edit or mail */
	if (txt->length == 0)
	  c = 'e';
	else
	  c = 'm';
      }
      switch (c) {
#ifdef NYMSUPPORT
      case 'n':
	menu_nym(thisnym);
	goto redraw;
#endif /* NYMSUPPORT */
      case '\014':
	goto redraw;
      case 'd':
	echo();
	cl(LINES - 3, 20);
	cl(7, 14);
	wgetnstr(stdscr, dest, LINELEN);
	noecho();
	break;
      case 's':
	echo();
	cl(LINES - 3, 20);
	cl(8, 10);
	wgetnstr(stdscr, subject, LINELEN);
	noecho();
	break;
      case 'c':
	menu_chain(chain, 0, (type == 'p' || type == 'f')
		   && streq(thisnym, ANON));
	goto redraw;
      case 'r':
	echo();
	cl(LINES - 5, 20);
	cl(5, 13);
	wgetnstr(stdscr, line, LINELEN);
	numcopies = strtol(line, NULL, 10);
	if (numcopies < 1 || numcopies > 10)
	  numcopies = 1;
	noecho();
	break;
      case 'f':
	cl(LINES - 3, 0);
	askfilename(path);
	cl(LINES - 3, 0);
	if (txt->length) {
	  buf_sets(tmp, path);
	  buf_clear(msg);
	  if (!hdr)
	    buf_nl(msg);
	  buf_cat(msg, txt);
	  if (attachfile(msg, tmp) == -1)
	    beep();
	  else {
	    buf_move(txt, msg);
	    hdr = 1;
	  }
	} else {
	  if ((f = fopen(path, "r")) != NULL) {
	    buf_clear(txt);
	    buf_read(txt, f);
	    fclose(f);
	  } else
	    beep();
	}
	break;
      case 'e':
#endif /* USE_NCURSES */
	{
	  int linecount;

	edit:
	  linecount = 1;
	  sprintf(path, "%s%cx%02x%02x%02x%02x.txt", POOLDIR, DIRSEP,
		  rnd_byte(), rnd_byte(), rnd_byte(), rnd_byte());
	  f = fopen(path, "w");
	  if (f == NULL) {
#ifdef USE_NCURSES
	    beep();
#endif /* USE_NCURSES */
	  } else {
	    if (type == 'f' || type == 'p')
	      fprintf(f, "Newsgroups: %s\n", dest);
	    if (type == 'r' || type == 'g' || type == 'm')
	      fprintf(f, "To: %s\n", dest);
	    fprintf(f, "Subject: %s\n", subject);
	    linecount += 2;
	    if (hdr)
	      while (buf_getline(txt, NULL) == 0) linecount++;
	    else
	      fprintf(f, "\n");
	    linecount++;
	    if (txt->length == 0)
	      fprintf(f, "\n");

	    buf_write(txt, f);
	    fclose(f);
	  }

	  menu_spawn_editor(path, linecount);

	  f = fopen(path, "r");
	  if (f == NULL) {
#ifdef USE_NCURSES
	    clear();
	    beep();
	    continue;
#else /* end of USE_NCURSES */
	    goto quit;
#endif /* else if not USE_NCURSES */
	  }
	  buf_reset(txt);
	  hdr = 0;

	  buf_reset(tmp);
	  buf_read(tmp, f);
	  while (buf_getheader(tmp, field, content) == 0) {
	    if (bufieq(field, "subject"))
	      strncpy(subject, content->data,
		      sizeof(subject));
	    else if ((type == 'p' || type == 'f') &&
		     bufieq(field, "newsgroups"))
	      strncpy(dest, content->data, sizeof(dest));
	    else if (bufieq(field, "to"))
	      strncpy(dest, content->data, sizeof(dest));
	    else {
	      buf_appendheader(txt, field, content);
	      hdr = 1;
	    }
	  }
	  if (hdr)
	    buf_nl(txt);
	  buf_rest(txt, tmp);

	  fclose(f);
	  unlink(path);
	  strcatn(path, "~", PATHMAX);
	  unlink(path);
#ifndef USE_NCURSES
	  {
	    char line[4];

	    fprintf(stderr, "Send message [y/n]? ");
	    scanf("%3s", line);
	    if (!strleft(line, "y"))
	      goto quit;
	  }
#else /* end of not USE_NCURSES */
	  goto redraw;
	}
	break;
      case 'm':
	if (txt->length == 0)
	  beep();
	else if (dest[0] == '\0') {
	  mix_status("No destination given.");
	  goto redraw;
	} else {
	  mix_status("Creating message...");
#endif /* else if USE_NCURSES */
	  buf_reset(msg);

	  if (type == 'p' || type == 'f')
	    buf_appends(msg, "Newsgroups: ");
	  else
	    buf_appends(msg, "To: ");
	  buf_appends(msg, dest);
	  buf_nl(msg);
	  buf_appends(msg, "Subject: ");
	  if (subject[0] == '\0')
	    buf_appends(msg, "(no subject)");
	  else
	    buf_appends(msg, subject);
	  buf_nl(msg);
	  if (!hdr)
	    buf_nl(msg);
	  buf_cat(msg, txt);
#ifdef USE_PGP
	  {
	    BUFFER *p;

	    p = buf_new();
	    if (streq(thisnym, ANON))
	      sign = 0;
	    if (sign || (key && !strileft(thisnym, NONANON)))
	      user_pass(p);

	    if (encrypt || sign) {
	      if (pgp_mailenc((encrypt ? PGP_ENCRYPT : 0)
			      | (sign ? PGP_SIGN : 0) | PGP_TEXT
			      | (strleft(thisnym, NONANON) ? 0 : PGP_REMAIL),
			      msg, strleft(thisnym, NONANON) ?
			      ADDRESS : thisnym, p, PGPPUBRING,
			      strleft(thisnym, NONANON) ?
			      PGPSECRING : NYMSECRING) == -1) {
		mix_genericerror();
#ifdef USE_NCURSES
		beep();
		goto redraw;
#endif /* USE_NCURSES */
	      }
	    }
	    buf_free(p);
	  }
#endif /* USE_PGP */

	  if (strleft(thisnym, NONANON)) {
	    FILE *f = NULL;

	    if (type == 'p' || type == 'f') {
	      if (strchr(NEWS, '@')) {
		/*  NOT_IMPLEMENTED; */
	      } else
		f = openpipe(NEWS);
	    } else {
	      if (NAME[0]) {
		buf_sets(tmp, NAME);
		buf_appends(tmp, " <");
		buf_appends(tmp, ADDRESS);
		buf_appends(tmp, ">");
	      } else
		buf_sets(tmp, ADDRESS);
	      mail_encode(msg, 0);
	      if (sendmail(msg, tmp->data, NULL) != 0) {
#ifdef USE_NCURSES
		clear();
#endif /* USE_NCURSES */
		mix_status("Error sending message.");
#ifdef USE_NCURSES
		goto redraw;
#else /* end of USE_NCURSES */
		goto quit;
#endif /* else if not USE_NCURSES */
	      }
	    }
#ifdef USE_NCURSES
	    clear();
#endif /* USE_NCURSES */
	    mix_status("Message sent non-anonymously.");
	    goto quit;
	  } else {
#ifdef USE_PGP
#ifdef NYMSUPPORT
	    if (!streq(thisnym, ANON)) {
	      if (nym_encrypt(msg, thisnym, (type == 'p' || type == 'f') ?
			      MSG_POST : MSG_MAIL) == 0)
		type = 'm';
	    }
#endif /* NYMSUPPORT */
#endif /* USE_PGP */
	    err = mix_encrypt((type == 'p' || type == 'f') ?
			      MSG_POST : MSG_MAIL,
			      msg, chain, numcopies, chainlist);
	    if (err == 0) {
#ifdef USE_NCURSES
	      clear();
#endif /* USE_NCURSES */
	      for (n = 0; buf_getline(chainlist, tmp) == 0; n++) ;
	      if (n > 1)
		mix_status("Done. (%d packets)", n);
	      else
		mix_status("Chain: %s", chainlist->data);
	      goto quit;
	    } else {
#ifdef USE_NCURSES
	      beep();
#endif /* USE_NCURSES */
	      if (chainlist->length)
		mix_status("%s", chainlist->data);
	      else
		mix_genericerror();
	    }
	  }
	}
#ifdef USE_NCURSES
	break;
      case 'q':
      case 'Q':
	clear();
	goto quit;
#ifdef USE_PGP
      case 'p':
	if (!streq(thisnym, ANON))
	  sign = !sign;
	break;
      case 'y':
	encrypt = !encrypt;
	break;
      case 'k':
	if (!streq(thisnym, ANON)) {
	  BUFFER *p, *keytxt, *uid;

	  key = 1;
	  p = buf_new();
	  keytxt = buf_new();
	  uid = buf_new();

	  buf_appendf(uid, "<%s>", strleft(thisnym, NONANON) ? ADDRESS :
		      thisnym);
	  user_pass(p);
	  pgp_pubkeycert(uid, strleft(thisnym, NONANON) ?
			 PGPSECRING : NYMSECRING, p, keytxt, PGP_ARMOR_NYMKEY);

	  buf_clear(msg);
	  if (!hdr)
	    buf_nl(msg);
	  buf_cat(msg, txt);
	  buf_sets(p, "application/pgp-keys");
	  mime_attach(msg, keytxt, p);
	  hdr = 1;
	  buf_move(txt, msg);

	  buf_free(p);
	  buf_free(keytxt);
	  buf_free(uid);
	}
	break;
#endif /* USE_PGP */
      default:
	beep();
      }
    }
  }
#endif /* USE_NCURSES */
quit:
  buf_free(cc);
  buf_free(cite);
  buf_free(msg);
  buf_free(txt);
  buf_free(field);
  buf_free(content);
  buf_free(chainlist);
  buf_free(tmp);
}
