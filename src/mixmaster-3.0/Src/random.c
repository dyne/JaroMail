/* Mixmaster version 3.0  --  (C) 1999 - 2006 Anonymizer Inc. and others.

   Mixmaster may be redistributed and modified under certain conditions.
   This software is distributed on an "AS IS" basis, WITHOUT WARRANTY OF
   ANY KIND, either express or implied. See the file COPYRIGHT for
   details.

   Randomness
   $Id: random.c 934 2006-06-24 13:40:39Z rabbi $ */


#include "mix3.h"
#include "crypto.h"
#include <fcntl.h>
#ifdef POSIX
#include <sys/time.h>
#include <unistd.h>
#else /* end of POSIX */
#include <io.h>
#include <process.h>
#endif /* else if not POSIX */
#ifdef WIN32
#include <windows.h>
#endif /* WIN32 */
#include <assert.h>
#include <string.h>

int rnd_state = RND_NOTSEEDED;

#ifdef USE_OPENSSL
int rnd_init(void)
{
  char r[PATHMAX];
  int n;
  LOCK *rndlock;

  if (rnd_state == RND_SEEDED)
    return(0);
  rndlock = lockfile(MIXRAND);
  mixfile(r, MIXRAND);
  n = RAND_load_file(r, 1024);
  if (n < 256 && rnd_seed() == -1)
    goto err;
  rnd_time();
  RAND_write_file(r);
  rnd_state = RND_SEEDED;
 err:
  unlockfile(rndlock);
  return (rnd_state == RND_SEEDED ? 0 : -1);
}

int rnd_final(void)
{
  int err = 0;
  char r[PATHMAX];
  LOCK *rndlock;

  if (rnd_state != RND_SEEDED)
    return(-1);

  rnd_update(NULL, 0);
  rndlock = lockfile(MIXRAND);
  mixfile(r, MIXRAND);
  RAND_load_file(r, 1024);	/* add seed file again in case other instances
				   of the program have used it */
  if (RAND_write_file(r) < 1)
    err = -1;
  unlockfile(rndlock);
  RAND_cleanup();
  return (err);
}

int rnd_add(byte *b, int l)
{
  RAND_seed(b, l);
  return (0);
}
#endif /* USE_OPENSSL */

void rnd_time(void)
{
  int pid;

#ifdef WIN32
  SYSTEMTIME t;
#endif /* WIN32 */

#ifdef HAVE_GETTIMEOFDAY
  struct timeval tv;

  gettimeofday(&tv, 0);
  rnd_add((byte *) &tv, sizeof(tv));
#elif defined(WIN32) /* end of HAVE_GETTIMEOFDAY */
  GetSystemTime(&t);
  rnd_add((byte *) &t, sizeof(t));
#else /* end of defined(WIN32) */
  rnd_add((byte *) time(NULL), sizeof(time_t));
#endif /* else if not defined(WIN32), HAVE_GETTIMEOFDAY */
  pid = getpid();
  rnd_add((byte *) &pid, sizeof(pid));
}

void rnd_update(byte *seed, int l)
{
  int fd = -1;
  byte b[512];

  rnd_time();
  if (seed)
    rnd_add(seed, l);
#ifdef DEV_URANDOM
  fd = open(DEV_URANDOM, O_RDONLY);
  if (fd != -1) {
    ssize_t ret;

    ret = read(fd, b, sizeof(b));
    if (ret > 0) {
      rnd_add(b, ret);
    }
    close(fd);
  }
#endif /* DEV_URANDOM */
}

int rnd_bytes(byte *b, int n)
{
  /* we frequently need to get small amounts of random data.
     speed up by pre-generating dating data */

  static byte rand[BUFSIZE];
  static int idx = BUFSIZE;

  if (rnd_state != RND_SEEDED)
    rnd_error();

  if (n + idx < BUFSIZE) {
    memcpy(b, rand + idx, n);
    idx += n;
  } else
    RAND_bytes(b, n);

  if (idx + 256 > BUFSIZE) {
    RAND_bytes(rand, BUFSIZE);
    idx = 0;
  }
  return (0);
}

int rnd_number(int n)
{
  int r;

  assert(n > 0);
  if (n > 65535)
    do
      r = rnd_byte() * 65536 +
	rnd_byte() * 256 + rnd_byte();
    while (r >= n);
  else if (n > 255)
    do
      r = rnd_byte() * 256 + rnd_byte();
    while (r >= n);
  else
    do
      r = rnd_byte();
    while (r >= n);
  return r;
}

byte rnd_byte()
{
  byte b;

  rnd_bytes(&b, 1);
  return b;
}

void rnd_initialized(void)
{
  rnd_state = RND_SEEDED;
}

#ifdef WIN32

#define NEEDED 256

int rnd_mouse(UINT i, WPARAM w, LPARAM l)
{
  static int entropy = 0;
  static int x, y, dx, dy;
  int newx, newy, newdx, newdy;
  int rnd[4];

  if (i == WM_MOUSEMOVE) {
    newx = LOWORD(l);
    newy = HIWORD(l);
    newdx = x - newx;
    newdy = y - newy;
    if (dx != 0 && dy != 0 && dx - newdx != 0 && dy - newdy != 0) {
      entropy++;
      if (entropy >= NEEDED)
	rnd_state = RND_SEEDED;
    }
    x = newx, y = newy, dx = newdx, dy = newdy;
    rnd[0] = x; rnd[1] = y; rnd[2] = dx; rnd[3] = dy;
    rnd_update((byte*)rnd, 4 * sizeof(int));
  }
  return (rnd_state == RND_SEEDED ? 100 : entropy * 100 / NEEDED);
}
#endif /* WIN32 */
