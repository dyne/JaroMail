/* Mixmaster version 3.0  --  (C) 1999 - 2006 Anonymizer Inc. and others.

   Mixmaster may be redistributed and modified under certain conditions.
   This software is distributed on an "AS IS" basis, WITHOUT WARRANTY OF
   ANY KIND, either express or implied. See the file COPYRIGHT for
   details.

   Menu-based user interface - utility functions
   $Id: menuutil.c 934 2006-06-24 13:40:39Z rabbi $ */


#include "menu.h"
#include <stdarg.h>
#include <stdlib.h>
#include <ctype.h>
#include <string.h>

int menu_initialized = 0;

#ifdef USE_NCURSES
void cl(int y, int x)
{
  move(y, x);
  hline(' ', COLS - x);
}
#endif /* USE_NCURSES */

void menu_init(void)
{
#ifdef USE_NCURSES
  initscr();
  cbreak();
  noecho();
  nonl();
  intrflush(stdscr, FALSE);
  keypad(stdscr, TRUE);
  menu_initialized = 1;
#endif /* USE_NCURSES */
}

void menu_exit(void)
{
  user_delpass();
#ifdef USE_NCURSES
  endwin();
#endif /* USE_NCURSES */
}

#ifdef USE_NCURSES
void askfilename(char *path)
{
  char line[PATHMAX];

  printw("\rFile name: ");
  echo();
  wgetnstr(stdscr, path, PATHMAX);
  noecho();
  printw("\r");
  if (path[0] == '~') {
    char *h;

    if ((h = getenv("HOME")) != NULL) {
      strncpy(line, h, PATHMAX);
      strcatn(line, "/", PATHMAX);
      strcatn(line, path + 1, PATHMAX);
      strncpy(path, line, PATHMAX);
    }
  }
}

void savemsg(BUFFER *message)
{
  char savename[PATHMAX];
  FILE *f;

  askfilename(savename);
  f = fopen(savename, "a");
  if (f != NULL) {
    buf_write(message, f);
    fclose(f);
  }
}

#endif /* USE_NCURSES */

void menu_spawn_editor(char *path, int lineno) {
#ifdef WIN32
  SHELLEXECUTEINFO sei;
  ZeroMemory(&sei, sizeof(SHELLEXECUTEINFO));
  sei.cbSize = sizeof(SHELLEXECUTEINFO);
  sei.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_FLAG_DDEWAIT;
  sei.hwnd = NULL;
  sei.lpVerb = "open";
  sei.lpFile = path;
  sei.lpParameters = NULL;
  sei.nShow = SW_SHOWNORMAL;

  if (ShellExecuteEx(&sei) == TRUE) {
    WaitForSingleObject(sei.hProcess, INFINITE);
    CloseHandle(sei.hProcess);
  }
#else /* WIN32 */
  char *editor;
  char s[PATHMAX];

/* Command line option +nn to position the cursor? */
#define cursorpos (strfind(editor, "emacs") || streq(editor, "vi") || \
		   streq(editor, "joe"))

  editor = getenv("EDITOR");
  if (editor == NULL)
    editor = "vi";

  if (lineno > 1 && cursorpos)
    snprintf(s, PATHMAX, "%s +%d %s", editor, lineno, path);
  else
    snprintf(s, PATHMAX, "%s %s", editor, path);

#ifdef USE_NCURSES
  clear();
  refresh();
  endwin();
#endif /* USE_NCURSES */
  system(s);
#ifdef USE_NCURSES
  refresh();
#endif /* USE_NCURSES */

#endif /* WIN32 */
}

int menu_getuserpass(BUFFER *b, int mode)
{
#ifdef USE_NCURSES
  char p[LINELEN];

  if (menu_initialized) {
    cl(LINES - 1, 10);
    if (mode == 0)
      printw("enter passphrase: ");
    else
      printw("re-enter passphrase: ");
    wgetnstr(stdscr, p, LINELEN);
    cl(LINES - 1, 10);
    refresh();
    if (mode == 0)
      buf_appends(b, p);
    else
      return (bufeq(b, p));
    return (0);
  }
#endif /* USE_NCURSES */
  return (-1);
}
