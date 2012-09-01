/* Mixmaster version 3.0  --  (C) 1999 - 2006 Anonymizer Inc. and others.

   Mixmaster may be redistributed and modified under certain conditions.
   This software is distributed on an "AS IS" basis, WITHOUT WARRANTY OF
   ANY KIND, either express or implied. See the file COPYRIGHT for
   details.

   Menu-based user interface
   $Id: menu.c 934 2006-06-24 13:40:39Z rabbi $ */


#include "menu.h"
#include "mix3.h"
#include <string.h>
#include <ctype.h>
#include <stdlib.h>
#include <fcntl.h>
#ifdef POSIX
#include <unistd.h>
#else /* end of POSIX */
#include <io.h>
#endif /* else if not POSIX */
#include <assert.h>

void menu_folder(char command, char *foldername)
{
  mix_init(NULL);
  if (foldername)
    menu_init();
  read_folder(command, foldername, ANON);
  menu_exit();
}

void read_folder(char command, char *foldername, char *nym)
{
#ifdef USE_NCURSES
  char path[PATHMAX] = "stdin", path_with_tilde[PATHMAX], l[LINELEN];
#else /* end of USE_NCURSES */
  char path[PATHMAX] = "stdin", l[LINELEN];
#endif /* else if not USE_NCURSES */
  char *h;
  FILE *f;
  BUFFER *folder;
  BUFFER *line, *field, *content, *name;
  BUFFER *index;
  BUFFER *mail, *log;
  int mailfolder = -1;	/* -1 = unknown, 0 = no mailfolder, 1 = mailfolder */
  int num = 0;
  long from = -1, subject = -1;
  int folder_has_changed;
#ifdef USE_NCURSES
  BUFFER *deleted_message;
  BUFFER *new_folder;
  BUFFER *new_index;
  long length;
  char sub[LINELEN], str[LINELEN], search[LINELEN] = "";
  long p;
  int display, range, selected, i, redraw, c, q;

#endif /* USE_NCURSES */
  int ispgp = 0, eof = 0;
  folder_has_changed = 0;

  line = buf_new();
  field = buf_new();
  content = buf_new();
  index = buf_new();
  mail = buf_new();
  name = buf_new();
  folder = buf_new();
  log = buf_new();

  if (foldername == NULL)
    f = stdin;
  else {
    if (foldername[0] == '~' && (h = getenv("HOME")) != NULL) {
      strncpy(path, h, PATHMAX);
      strcatn(path, foldername + 1, PATHMAX);
    } else
      strncpy(path, foldername, PATHMAX);
    f = fopen(path, "r");
  }
  if (f == NULL) {
#ifdef USE_NCURSES
    if (foldername)
      beep();
#endif /* USE_NCURSES */
    mix_status("Can't read %s.\n", path);
    goto end;
  }
  for (;;) {
    if (fgets(l, sizeof(l), f) == NULL)
      eof = 1;
    else if (mailfolder == -1) {
      if (strleft(l, "From "))
	mailfolder = 1;
      else if (strileft(l, "from:") || strileft(l, "path:")
	  || strileft(l, "xref:") || strileft(l, "return-path"))
	mailfolder = 0;
      else
	break;
    }
    if (eof || (mailfolder && strleft(l, "From ")) ||
	(mailfolder == 0 && from != -1 &&
	 (strileft(l, "path:") ||
	  strileft(l, "xref:") || strileft(l,"return-path")))) {
      if (num > 1)
	mix_status("Reading message %d", num);
#ifdef USE_PGP
      if (ispgp)
#ifdef NYMSUPPORT
	switch (nym_decrypt(mail, NULL, log)) {
	case 2:
	  from = -1, subject = -1;
	  while (buf_getline(mail, line) == 0) {
	    if (bufileft(line, "from:"))
	      from = folder->length + mail->ptr - line->length - 1;
	    if (bufileft(line, "subject:"))
	      subject = folder->length + mail->ptr - line->length - 1;
	  }
	  folder_has_changed = 1;
	  break;
	case -1:
	  buf_clear(mail);
	  from = -1, subject = -1;
	  continue;
	default:
	  ;
	}
#else
	if (!eof) {
	  buf_clear(mail);
	  from = -1, subject = -1;
	  continue;
	}
#endif /* NYMSUPPORT */
#endif /* USE_PGP */
      buf_cat(folder, mail);
      buf_clear(mail);
      ispgp = 0;
      if (num > 0) {
	buf_appendl(index, from);
	buf_appendl(index, subject);
      }
      if (eof)
	break;

      buf_appendl(index, folder->length);
      from = subject = -1;
      num++;
    }
    if (from == -1 && strileft(l, "from:"))
      from = folder->length + mail->length;

    if (subject == -1 && strileft(l, "subject:"))
      subject = folder->length + mail->length;

    buf_appends(mail, l);
    if (strleft(l, begin_pgp))
      ispgp = 1;
  }

  if (foldername)
    fclose(f);
  else {
    dup2(open("/dev/tty", O_RDWR), fileno(stdin));
    menu_init();
  }

  mix_status("");
  if (folder->length == 0) {
#ifdef USE_NCURSES
    clear();
    beep();
#endif /* USE_NCURSES */
    mix_status("%s is empty.\n", path);
    goto end;
  }
  if (mailfolder == -1) {
#ifdef USE_NCURSES
    clear();
    beep();
#endif /* USE_NCURSES */
    mix_status("%s is not a mail folder.\n", path);
    goto end;
  }
#ifndef USE_NCURSES
  if (command == 0) {
    buf_write(folder, stdout);
    goto end;
  }
  if (num > 1) {
    mix_status("Folder contains several messages.");
    goto end;
  }
#endif /* not USE_NCURSES */

  if (num < 2) {
    folder->ptr = 0;
    mimedecode(folder);

    if (command != 0)
      send_message(command, nym, folder);
#ifdef USE_NCURSES
    else
      read_message(folder, nym);

    clear();
#endif /* USE_NCURSES */
    goto end;
  }
#ifdef USE_NCURSES
  display = selected = 0;
  range = LINES - 3;
  redraw = 2;

  for (;;) {
    if (selected < 0)
      selected = 0;
    if (selected >= num)
      selected = num - 1;

    if (selected < display) {
      display = selected - range / 2;
      redraw = 2;
    }
    if (selected >= display + range) {
      display = selected - range / 2;
      redraw = 2;
    }
    if (display >= num - 5)
      display = num - 5;
    if (display < 0)
      display = 0;

    if (redraw) {
      if (redraw == 2) {
	clear();
	standout();
	mvprintw(0, 0, "Mixmaster %s", VERSION);
	printw("   %.20s reading %.50s", nym, path);
	standend();
      }
      for (i = display; i < display + range; i++) {
	if (i < num) {
	  index->ptr = 12 * i;
	  p = buf_getl(index);
	  buf_clear(name);
	  folder->ptr = buf_getl(index);
	  if (folder->ptr < 0)
	    folder->ptr = 0;
	  else {
	    buf_getheader(folder, field, line);
	    if (line->length) {
	      decode_header(line);
	      rfc822_name(line, name);
	    }
	  }
	  if (i == selected)
	    standout();

	  mvaddnstr(i - display + 2, 0, name->data, 18);

	  sub[0] = '\0';
	  folder->ptr = buf_getl(index);
	  if (folder->ptr < 0)
	    folder->ptr = 0;
	  else {
	    buf_getheader(folder, field, content);
	    if (content->length) {
	      decode_header(content);
	      strncpy(sub, content->data, sizeof(sub));
	    }
	  }
	  if (sub[0] == '\0')
	    strcpy(sub, "(no subject)");
	  mvaddnstr(i - display + 2, 20, sub, COLS - 21);

	  if (i == selected)
	    standend();
	}
      }
    }
    move(LINES - 1, COLS - 1);
    refresh();
    redraw = 0;

    c = getch();
    switch (c) {
    case '\014':
      display = selected - range / 2;
      redraw = 2;
      break;
    case 'q':
      clear();
      goto end;
    case '/':
      echo();
      cl(LINES - 1, 0);
      printw("Search: ");
      refresh();
      wgetnstr(stdscr, str, LINELEN);
      if (str[0] != '\0')
	strncpy(search, str, LINELEN);
      noecho();
      for (i = (selected < num ? selected + 1 : 0); i < num; i++) {
	index->ptr = 12 * i + 4;
	folder->ptr = buf_getl(index);
	if (folder->ptr < 0)
	  folder->ptr = 0;
	else {
	  buf_getheader(folder, field, line);
	  if (line->length) {
	    decode_header(line);
	    if (bufifind(line, search))
	      break;
	  }
	}
	folder->ptr = buf_getl(index);
	if (folder->ptr < 0)
	  folder->ptr = 0;
	else {
	  buf_getheader(folder, field, line);
	  if (line->length) {
	    decode_header(line);
	    if (bufifind(line, search))
	      break;
	  }
	}
      }
      if (i < num)
	selected = i;
      else
	beep();
      redraw = 1;
      break;
    case '\r':			/* read message */
    case '\n':
    case 'r':			/* reply to message */
    case 'g':
    case 'f':
    case 'm':
    case 'p':
    case 's':
      index->ptr = 12 * selected;
      p = buf_getl(index);
      if (selected < num - 1) {
	index->ptr = 12 * (selected + 1);
	q = buf_getl(index) - p;
      } else
	q = folder->length - p;
      buf_clear(mail);
      buf_append(mail, folder->data + p, q);
      mimedecode(mail);
      if(c == 's')
	savemsg(mail);
      else{
	if (c == '\r' || c == '\n')
	  read_message(mail, nym);
	else
	  send_message(c, nym, mail);
      }
      redraw = 2;
      break;
    case 'd':                   /* delete message */
      /* Remove message from folder */
      if(num > 0){
	index->ptr = 12 * selected;
	p = buf_getl(index);
	if (selected < num - 1) {
	  index->ptr = 12 * (selected + 1);
	  q = buf_getl(index) - p;
	} else
	  q = folder->length - p;
	deleted_message = buf_new();
	new_folder = buf_new();
	buf_cut_out(folder, deleted_message, new_folder, p, q);
	buf_free(deleted_message);
	buf_free(folder);
	folder = new_folder;
	/* Update index file */
	new_index = buf_new();
	index->ptr = 0;
	if(selected > 0)
	  buf_get(index, new_index, 12 * selected);
	index->ptr = 12 * (selected + 1);
	while((from = buf_getl(index)) != -1){
	  subject = buf_getl(index);
	  length = buf_getl(index);
	  buf_appendl(new_index, from - q);
	  buf_appendl(new_index, subject - q);
	  buf_appendl(new_index, length - q);
	}
	buf_free(index);
	index = new_index;
	/* Done */
	num--;
	folder_has_changed = 1;
      }
      redraw = 2;
      break;
    case KEY_UP:
      selected--;
      redraw = 1;
      break;
    case KEY_DOWN:
    case 'n':			/* nym ???? */
      selected++;
      redraw = 1;
      break;
    case KEY_PPAGE:
      selected -= range;
      redraw = 1;
      break;
    case KEY_NPAGE:
      selected += range;
      redraw = 1;
      break;
    default:
      beep();
    }
  }
#endif /* USE_NCURSES */

end:
#ifdef USE_NCURSES
  /* If folder has changed, ask user about saving new folder. */
  if (folder_has_changed && !streq(path, "stdin")) {
    mvprintw(LINES - 2, 0, "Buffer has changed. Save [y/n]? ");
    c = getch();
    cl(LINES - 2, 0);
    if ((c == 'y') || (c == 'Y')){
      strncpy(path_with_tilde, path, PATHMAX-1);
      strcat(path_with_tilde, "~");
      rename(path, path_with_tilde); /* Rename folder to folder~ */
      f = fopen(path, "w"); /* Write new folder */
      if (f == NULL)
	mix_status("Can't write to %s.", path);
      else{
	buf_write(folder, f);
	mix_status("Wrote %s.", path);
	fclose(f);
      }
    }
    else{
      mix_status("%s was not saved.", path);
    }
  }
#endif /* USE_NCURSES */
  buf_free(folder);
  buf_free(line);
  buf_free(field);
  buf_free(content);
  buf_free(index);
  buf_free(mail);
  buf_free(name);
  buf_free(log);
}

#ifdef USE_NCURSES
static int sortrel(const void *a, const void *b)
{
  int na, ra, nb, rb;

  na = *(int *) a;
  nb = *(int *) b;

  ra = *((int *) a + 1);
  rb = *((int *) b + 1);
  return rb - ra;
}

void menu_main(void)
{
  int y, x;
  int pool, n;
  int c;
  int space;
  BUFFER *chainlist, *line;
  char nym[LINELEN] = ANON;

  chainlist = buf_new();
  line = buf_new();

  mix_init(NULL);
  menu_init();

menu_redraw:
  clear();
  for (;;) {
    standout();
    mvprintw(0, 0, "Mixmaster %s", VERSION);
#if 0
    mvprintw(0, COLS - sizeof(COPYRIGHT), COPYRIGHT);
#endif
    for (space = 0; space < (COLS - 10 - sizeof(VERSION)); space++) {
        printw(" ");
    };
    standend();
    mix_status(NULL);

#ifdef NYMSUPPORT
    mvprintw(8, 4, "n)ym: %s", nym);
#endif /* NYMSUPPORT */
    y = 12, x = 25;
    mvprintw(y++, x, "m)ail");
    mvprintw(y++, x, "p)ost to Usenet");
    mvprintw(y++, x, "r)ead mail (or news article)");
    mvprintw(y++, x, "d)ummy message");
    mvprintw(y++, x, "s)end messages from pool");
    mvprintw(y++, x, "e)dit configuration file");
    mvprintw(y++, x, "u)pdate stats");
    mvprintw(y++, x, "q)uit");

    pool = pool_read(NULL);
    if (pool == 1)
      mvprintw(4, 2, "%3d outgoing message in the pool. \n", pool);
    else
      mvprintw(4, 2, "%3d outgoing messages in the pool.\n", pool);

    move(LINES - 1, COLS - 1);
    refresh();
    c = getch();
    if (c != ERR) {
      mix_status("");
      switch (c) {
      case '\014':
	mix_status("");
	goto menu_redraw;
#ifdef NYMSUPPORT
      case 'n':
	menu_nym(nym);
	goto menu_redraw;
#endif /* NYMSUPPORT */
      case 'm':
      case 'p':
	send_message(c, nym, NULL);
	break;
      case 'd':
	mix_status("Creating message...");
	if (mix_encrypt(MSG_NULL, NULL, NULL, 1, chainlist) != 0) {
	  if (chainlist->length > 0)
	    mix_status("%s", chainlist->data);
	  else
	    mix_genericerror();
	  beep();
	} else {
	  for (n = 0; buf_getline(chainlist, line) == 0; n++) ;
	  if (n > 1)
	    mix_status("Done (%d packets).", n);
	  else
	    mix_status("Chain: %s", chainlist->data);
	}
	break;
      case 's':
	mix_status("Mailing messages...");
	mix_send();
	mix_status("Done.");
	break;
      case 'r':
	{
	  char name[LINELEN];

	  cl(LINES - 3, 0);
	  if (getenv("MAIL"))
	    printw("File name [%s]: ", getenv("MAIL"));
	  else
	    printw("File name: ");
	  echo();
	  wgetnstr(stdscr, name, LINELEN);
	  noecho();
	  cl(LINES - 3, 0);
	  if (name[0] == '\0') {
	    if (getenv("MAIL"))
	      read_folder(0, getenv("MAIL"), nym);
	  } else
	    read_folder(0, name, nym);
	}
	break;
      case 'e':
	do {
	  char path[PATHMAX];
	  mixfile(path, MIXCONF);
	  menu_spawn_editor(path, 0);
	  mix_config();
	} while (0);
	break;
      case 'u':
	update_stats();
	break;
      case 'q':
      case 'Q':
	goto quit;
      default:
	beep();
      }
    }
  }

quit:
  menu_exit();
  buf_free(chainlist);
  buf_free(line);
}

void read_message(BUFFER *message, char *nym)
{
  int l = 0;
  int c;
  int inhdr = 1, txtbegin;
  BUFFER *field, *content, *line, *hdr;
  char sub[LINELEN] = "mail";
  char thisnym[LINELEN] = "";

  field = buf_new();
  content = buf_new();
  line = buf_new();
  hdr = buf_new();

  if (thisnym[0] == '\0')
    strncpy(thisnym, nym, LINELEN);

  if (bufleft(message, "From nymserver ")) {
    /* select nym if Nym: pseudo header is in the first line */
    buf_getline(message, line);
    buf_getheader(message, field, content);
    if (bufieq(field, "Nym"))
      strncpy(thisnym, content->data, sizeof(thisnym));
    buf_rewind(message);
  }
  while (buf_getheader(message, field, content) == 0) {
    if (bufieq(field, "received") || bufleft(field, "From "))
      continue;
    if (bufieq(field, "subject"))
      strncpy(sub, content->data, sizeof(sub));
    buf_appendheader(hdr, field, content);
  }
  if (strlen(sub) > COLS - strlen(VERSION) - strlen(thisnym) - 23)
    sub[COLS - strlen(VERSION) - strlen(thisnym) - 24] = '\0';
  txtbegin = message->ptr;

loop:
  clear();
  standout();
  mvprintw(0, 0, "Mixmaster %s", VERSION);
  printw("   %.20s reading %.50s\n", thisnym, sub);
  standend();
  mix_status(NULL);

  while (l < LINES - 2) {
    if (inhdr) {
      if (buf_getline(hdr, line) == -1)
	buf_clear(line), inhdr = 0;
    } else {
      if (buf_getline(message, line) == -1) {
	standout();
	mvprintw(LINES - 1, 0, "Command");
	standend();
	refresh();
	for (;;) {
	  c = getch();
	  switch (c) {
	  case 'm':
	  case 'p':
	  case 'r':
	  case 'g':
	  case 'f':
	    send_message(c, thisnym, message);
	    goto end;
	  case 'u':
	    inhdr = 0;
	    message->ptr = txtbegin;
	    l = 0;
	    goto loop;
	  case 'h':
	    inhdr = 1;
	    hdr->ptr = 0;
	    message->ptr = txtbegin;
	    l = 0;
	    goto loop;
	  case 's':
	    savemsg(message);
	    /* fallthru */
	  case 'q':
	  case 'i':
	  case '\n':
	  case '\r':
	    goto end;
	  case '\014':
	    refresh();
	    continue;
	  default:
	    beep();
	    refresh();
	  }
	}
      }
    }
    mvprintw(l + 1, 0, "%s\n", line->data);
    l += (line->length - 1) / COLS + 1;
  }
  standout();
  mvprintw(LINES - 1, 0, "MORE");
  standend();
  refresh();
  for (;;) {
    c = getch();
    switch (c) {
    case 'm':
    case 'p':
    case 'r':
    case 'g':
    case 'f':
      send_message(c, thisnym, message);
      goto end;
    case 'u':
      inhdr = 0;
      message->ptr = txtbegin;
      l = 0;
      goto loop;
    case 'h':
      inhdr = 1;		/* show full header */
      hdr->ptr = 0;
      message->ptr = txtbegin;
      l = 0;
      goto loop;
    case 's':
      savemsg(message);
      /* fallthru */
    case 'q':
    case 'i':
      goto end;
    case ' ':
    case '\n':
    case '\r':
      l = 0;
      goto loop;
    case '\014':
      refresh();
      continue;
    default:
      beep();
      refresh();
    }
  }
end:
  buf_free(field);
  buf_free(content);
  buf_free(line);
  buf_free(hdr);
}

int menu_replychain(int *d, int *l, char *mdest, char *pdest, char *psub,
		    char *r)
{
  int c;
  char line[LINELEN];
  char reliability[9];

redraw:
  clear();
  standout();
  printw("Create a nym reply block:");
  standend();
  mix_status(NULL);

  mvprintw(3, 0, "Type of reply block:\n");
  if (*d == MSG_MAIL)
    standout();
  printw(" m)ail ");
  if (*d == MSG_MAIL)
    standend();

  if (*d == MSG_POST)
    standout();
  printw(" Usenet message p)ool ");
  if (*d == MSG_POST)
    standend();

  if (*d == MSG_NULL)
    standout();
  printw(" cover t)raffic ");
  if (*d == MSG_NULL)
    standend();

  if (*d != MSG_NULL)
    mvprintw(6, 0, "d)estination: %s", *d == MSG_MAIL ? mdest : pdest);
  if (psub && *d == MSG_POST)
    mvprintw(7, 0, "s)ubject: %s", psub);
  chain_reliability(r, 1, reliability); /* chaintype 1=ek */
  mvprintw(8, 0, "c)hain: %-39s (reliability: %s)", r, reliability);
  mvprintw(10, 0, "l)atency: %d hours", *l);
  move(LINES - 1, COLS - 1);

  for (;;) {
    refresh();
    c = getch();
    switch (c) {
    case 'm':
      *d = MSG_MAIL;
      goto redraw;
    case 'p':
      *d = MSG_POST;
      goto redraw;
    case 't':
      *d = MSG_NULL;
      goto redraw;
    case 'q':
      return (-1);
    case 'd':
      cl(6, 0);
      printw("d)estination: ");
      echo();
      wgetnstr(stdscr, *d == MSG_MAIL ? mdest : pdest, LINELEN);
      noecho();
      goto redraw;
    case 'l':
      cl(10, 0);
      printw("l)atency: ");
      echo();
      wgetnstr(stdscr, line, LINELEN);
      *l = strtol(line, NULL, 10);
      if (*l < 0)
	*l = 0;
      noecho();
      goto redraw;
    case 'c':
      menu_chain(r, 1, *d == MSG_POST);
      goto redraw;
    case '\014':
      goto redraw;
    case '\n':
    case '\r':
      return (0);
    case 's':
      if (*d == MSG_POST) {
	cl(7, 0);
	printw("s)ubject: ");
	echo();
	wgetnstr(stdscr, psub, LINELEN);
	noecho();
	goto redraw;
      }
    default:
      beep();
    }
  }
}

void menu_chain(char *chainstr, int chaintype, int post)
     /* chaintype 0=mix 1=ek 2=newnym */
{
  REMAILER remailer[MAXREM];
  int badchains[MAXREM][MAXREM];
  int rlist[2 * MAXREM];
  char newchain[CHAINMAX];
  char info[LINELEN];
  int num = 0, i, middlemanlast=0, ok = 1;
  int c, x, y;
  int nymserv = 0;
  int chain[20], chainlen = 0;
  char reliability[9];

  if (chaintype == 2)
    nymserv = 1, chaintype = 1;
  assert(chaintype == 0 || chaintype == 1);

  clear();
  standout();
  if (nymserv)
    printw("Select nym server:\n\n");
  else
    printw("Select remailer chain:\n\n");
  standend();

  if (chaintype == 1)
    num = t1_rlist(remailer, badchains);
  else
    num = mix2_rlist(remailer, badchains);

  if (num < 1) {
    mix_status("Can't read remailer list.");
    beep();
    return;
  }
  for (i = 0; i < num; i++) {
    rlist[2 * i] = i + 1;
    rlist[2 * i + 1] = remailer[i + 1].info[chaintype].reliability -
      remailer[i + 1].info[chaintype].latency / 3600;
    if (remailer[i + 1].info[chaintype].history[0] == '\0')
      rlist[2 * i + 1] = -1;
    if ((nymserv && !remailer[i + 1].flags.newnym) ||
	((chaintype == 0 && !remailer[i + 1].flags.mix) ||
	 (chaintype == 1 && !nymserv && (!remailer[i + 1].flags.ek
					 || !remailer[i + 1].flags.pgp))))
      rlist[2 * i] = 0, rlist[2 * i + 1] = -2;
  }
  qsort(rlist, num - 1, 2 * sizeof(int), sortrel);

  for (i = 0; i < num; i++)
    if (rlist[2 * i + 1] == -2) {
      num = i;
      break;
    }
  if (num < 1) {
    mix_status("No remailers found!");
    return;
  }
  if (num > 2 * (LINES - 6))
    num = 2 * (LINES - 6);
  if (num > 2 * 26)
    num = 2 * 26;

  for (i = 0; i < num && rlist[2 * i + 1] > -2; i++) {
    y = i, x = 0;
    if (y >= LINES - 6)
      y -= LINES - 6, x += 40;
    mvprintw(y + 2, x, "%c", i < 26 ? i + 'a' : i - 26 + 'A');
    mvprintw(y + 2, x + 2, "%s", remailer[rlist[2 * i]].name);
    mvprintw(y + 2, x + 16, "%s",
	     remailer[rlist[2 * i]].info[chaintype].history);
    sprintf(info, "%3.2f",
	    remailer[rlist[2 * i]].info[chaintype].reliability / 100.0);
    mvprintw(y + 2, x + 29 + 6 - strlen(info), "%s%%", info);
  }
  y = num + 3;
  if (y > LINES - 4)
    y = LINES - 4;
  mvprintw(y, 0, "*  select at random");

  for (;;) {
    newchain[0] = '\0';
    for (i = 0; i < chainlen; i++) {
      if (i)
	strcatn(newchain, ",", CHAINMAX);
      if (chain[i])
	strcatn(newchain, remailer[chain[i]].name, CHAINMAX);
      else
	strcatn(newchain, "*", CHAINMAX);
    }
    if (chainlen > 0) {
      ok = 1;
      middlemanlast = remailer[chain[chainlen - 1]].flags.middle;
      if (post && !remailer[chain[chainlen - 1]].flags.post && !(chain[chainlen - 1] == 0 /*randhop*/))
	ok = 0;
    } else
      ok = 1;

    mvprintw(LINES - 4, 40,
      middlemanlast ?
	"MIDDLEMAN   " :
	(ok ?
	  "            " :
	  "NO POSTING  "));
    cl(LINES - 3, 0);
    cl(LINES - 2, 0);
    cl(LINES - 1, 0);
    if(!nymserv){
      chain_reliability(newchain, chaintype, reliability);
      mvprintw(LINES - 4, 58, "(reliability: %s)", reliability);
    }
    mvprintw(LINES - 3, 0, nymserv ? "Nym server: %s" : "Chain: %s",
	     newchain);
    refresh();
    c = getch();
    if (c == '\n' || c == '\r') {
    /* beep and sleep in case the user made a mistake */
      if (middlemanlast) {
	beep();
	sleep(2);
      }
      if (ok || middlemanlast)
	break;
      else
	beep();
    } else if (c == '*') {
      if (chainlen > 19 || (nymserv && chainlen > 0))
	beep();
      else
	chain[chainlen++] = 0;
    } else if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')) {
      if (c >= 'a')
	c -= 'a';
      else
	c = c - 'A' + 26;

      if (chainlen > 19 || (nymserv && chainlen > 0) || c >= num)
	beep();
      else
	chain[chainlen++] = rlist[2 * c];
    } else if (c == killchar())
      chainlen = 0;
    else if ((c == KEY_BACKSPACE || c == KEY_LEFT || c == erasechar())
	     && chainlen > 0)
      --chainlen;
    else
      beep();
  }
  if (chainlen)
    strncpy(chainstr, newchain, CHAINMAX);
}

#endif /* USE_NCURSES */
