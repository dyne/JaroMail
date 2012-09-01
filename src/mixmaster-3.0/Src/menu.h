/* Mixmaster version 3.0  --  (C) 1999 - 2006 Anonymizer Inc. and others.

   Mixmaster may be redistributed and modified under certain conditions.
   This software is distributed on an "AS IS" basis, WITHOUT WARRANTY OF
   ANY KIND, either express or implied. See the file COPYRIGHT for
   details.

   Menu-based user interface
   $Id: menu.h 934 2006-06-24 13:40:39Z rabbi $ */


#ifndef _MENU_H
#define _MENU_H
#include "mix3.h"
#ifdef USE_NCURSES
#ifdef HAVE_NCURSES_H
#include <ncurses.h>
#else /* end of HAVE_NCURSES_H */
#include <curses.h>
#endif /* else if not HAVE_NCURSES_H */
#endif /* USE_NCURSES */

#define NONANON "non-anonymous"
#define ANON "Anonymous"

void send_message(int type, char *nym, BUFFER *txt);
void read_folder(char command, char *foldername, char *nym);
void menu_init(void);
void menu_exit(void);

void menu_spawn_editor(char *path, int lineno);

#ifdef USE_NCURSES
void read_message(BUFFER *message, char *nym);
void menu_nym(char *);
void menu_chain(char *chain, int type, int post);
void cl(int y, int x);
void askfilename(char *fn);
void savemsg(BUFFER *message);
int menu_replychain(int *d, int *l, char *mdest, char *pdest, char *psub,
		    char *r);
void update_stats(void);

#endif /* USE_NCURSES */

#define maxnym 30

#endif /* not _MENU_H */
