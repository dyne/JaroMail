/* Mixmaster version 3.0  --  (C) 1999 - 2006 Anonymizer Inc. and others.

   Mixmaster may be redistributed and modified under certain conditions.
   This software is distributed on an "AS IS" basis, WITHOUT WARRANTY OF
   ANY KIND, either express or implied. See the file COPYRIGHT for
   details.

   Get randomness from device or user
   $Id: rndseed.c 934 2006-06-24 13:40:39Z rabbi $ */


#include "mix3.h"
#include <assert.h>
#include <time.h>
#include <fcntl.h>
#include <time.h>
#include <stdlib.h>
#ifdef POSIX
#include <unistd.h>
#include <termios.h>
#else /* end of POSIX */
#include <io.h>
#include <process.h>
#endif /* else if not POSIX */
#if defined(WIN32) || defined(MSDOS)
#include <conio.h>
#endif /* defined(WIN32) || defined(MSDOS) */
#ifdef WIN32
#include <windows.h>
#endif /* WIN32 */

#define NEEDED 128

#ifndef O_NDELAY
#define O_NDELAY 0
#endif /* not O_NDELAY */

int kbd_noecho(void)
{
#ifdef HAVE_TERMIOS
  int fd;
  struct termios attr;

  setbuf(stdin, NULL);
  fd = fileno(stdin);
  if (tcgetattr(fd, &attr) != 0)
    return (-1);
  attr.c_lflag &= ~(ECHO | ICANON);
  if (tcsetattr(fd, TCSAFLUSH, &attr) != 0)
    return (-1);
#endif /* HAVE_TERMIOS */
  return (0);
}

int kbd_echo(void)
{
#ifdef HAVE_TERMIOS
  int fd;
  struct termios attr;

  setvbuf(stdin, NULL, _IOLBF, BUFSIZ);
  fd = fileno(stdin);
  if (tcgetattr(fd, &attr) != 0)
    return (-1);
  attr.c_lflag |= ECHO | ICANON;
  if (tcsetattr(fd, TCSAFLUSH, &attr) != 0)
    return (-1);
#endif /* HAVE_TERMIOS */
  return (0);
}

void rnd_error(void)
{
  errlog(ERRORMSG,
	 "Random number generator not initialized. Aborting.\n\
Run the program interactively to seed the generator.\n");
  exit(3);
}

/* get randomness from system or user. If the application has promised that
   it will seed the RNG later, we do not ask for user input */

int rnd_seed(void)
{
  int fd = -1;
  byte b[512], c = 0;
  int bytes = 0;

#ifdef DEV_RANDOM
  fd = open(DEV_RANDOM, O_RDONLY | O_NDELAY);
#endif /* DEV_RANDOM */
  if (fd == -1) {
#if 1
    if (rnd_state == RND_WILLSEED)
      return(-1);
    if (!isatty(fileno(stdin)))
      rnd_error();
#else /* end of 1 */
#error "should initialize the prng from system ressources"
#endif /* else if not 1 */
    fprintf(stderr, "Please enter some random characters.\n");
    kbd_noecho();
    while (bytes < NEEDED) {
      fprintf(stderr, "  %d     \r", NEEDED - bytes);
#ifdef HAVE_GETKEY
      if (kbhit(), *b = getkey())
#else /* end of HAVE_GETKEY */
      if (read(fileno(stdin), b, 1) > 0)
#endif /* else if not HAVE_GETKEY */
	{
	  rnd_add(b, 1);
	  rnd_time();
	  if (*b != c)
	    bytes++;
	  c = *b;
	}
    }
    fprintf(stderr, "Thanks.\n");
    sleep(1);
    kbd_echo();
  }
#ifdef DEV_RANDOM
  else {
    bytes = read(fd, b, sizeof(b));
    if (bytes > 0) {
      rnd_add(b, bytes);
    } else {
      bytes = 0;
    }
    close(fd);
    if (bytes < NEEDED) {
      fd = open(DEV_RANDOM, O_RDONLY);	/* re-open in blocking mode */
      if (isatty(fileno(stdin))) {
	fprintf(stderr,
		"Please move the mouse, enter random characters, etc.\n");
	kbd_noecho();
      }
      while (bytes < NEEDED) {
	if (isatty(fileno(stdin)))
	  fprintf(stderr, "  %d     \r", NEEDED - bytes);
	if (read(fd, b, 1) > 0) {
	  rnd_add(b, 1);
	  bytes++;
	}
      }
      if (isatty(fileno(stdin))) {
	fprintf(stderr, "Thanks.\n");
	sleep(1);
	kbd_echo();
      }
      close(fd);
    }
  }
#endif /* DEV_RANDOM */
  rnd_state = RND_SEEDED;
  return (0);
}
