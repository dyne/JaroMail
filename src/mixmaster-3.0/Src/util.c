/* Mixmaster version 3.0  --  (C) 1999 - 2006 Anonymizer Inc. and others.

   Mixmaster may be redistributed and modified under certain conditions.
   This software is distributed on an "AS IS" basis, WITHOUT WARRANTY OF
   ANY KIND, either express or implied. See the file COPYRIGHT for
   details.

   Utility functions
   $Id: util.c 934 2006-06-24 13:40:39Z rabbi $ */

#include "mix3.h"
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>
#ifdef POSIX
#include <signal.h>
#include <errno.h>
#include <unistd.h>
#include <sys/file.h>
#include <termios.h>
#else /* end of POSIX */
#include <io.h>
#endif /* else if not POSIX */
#ifdef HAVE_GETKEY
#include <pc.h>
#endif /* HAVE_GETKEY */
#include <assert.h>

/** string comparison functions. return 1 on match, 0 otherwise ********/

int strileft(const char *string, const char *keyword)
{
  register unsigned int i;

  for (i = 0; keyword[i] != '\0'; i++)
    if (tolower(string[i]) != tolower(keyword[i]))
      return 0;
  return 1;
}

int striright(const char *string, const char *keyword)
{
  int l;
  l = strlen(string) - strlen(keyword);
  return (l >= 0 ? strieq(string + l, keyword) : -1);
}

int strleft(const char *string, const char *keyword)
{
  register unsigned int i;

  for (i = 0; keyword[i] != '\0'; i++)
    if (string[i] != keyword[i])
      return 0;
  return 1;
}

int strifind(const char *string, const char *keyword)
{
  register unsigned int i, j;
  char k;

  k = tolower(keyword[0]);
  for (i = 0; string[i] != '\0'; i++) {
    if (tolower(string[i]) == k) {
      for (j = 1; keyword[j] != '\0'; j++)
	if (tolower(string[i + j]) != tolower(keyword[j]))
	  goto next;
      return 1;
    }
  next:
    ;
  }
  return 0;
}

int strieq(const char *s1, const char *s2)
{
  register unsigned int i = 0;

  do
    if (tolower(s1[i]) != tolower(s2[i]))
      return 0;
  while (s1[i++] != '\0') ;
  return 1;
}

int streq(const char *a, const char *b)
{
  return (strcmp(a, b) == 0);
}

int strfind(const char *a, const char *keyword)
{
  return (strstr(a, keyword) != NULL);
}

void strcatn(char *dest, const char *src, int n)
{
  int l;
  l = strlen(dest);
  if (l < n)
    strncpy(dest + l, src, n - l - 1);
  dest[n-1] = '\0';
}

/** files **************************************************************/

int mixfile(char *path, const char *name)
{
  char *h;
  assert(path != NULL && name != NULL);

#ifdef POSIX
  if (name[0] == '~' && name[1] == DIRSEP && (h = getenv("HOME")) != NULL) {
    strncpy(path, h, PATHMAX);
    path[PATHMAX-1] = '\0';
    strcatn(path, name + 1, PATHMAX);
  } else
#endif /* POSIX */
  if (name[0] == DIRSEP || (isalpha(name[0]) && name[1] == ':') || MIXDIR == NULL) {
    strncpy(path, name, PATHMAX);
    path[PATHMAX-1] = '\0';
  } else {
    strncpy(path, MIXDIR, PATHMAX);
    path[PATHMAX-1] = '\0';
    strcatn(path, name, PATHMAX);
  }
  return (0);
}

FILE *mix_openfile(const char *name, const char *a)
{
  char path[PATHMAX];

  mixfile(path, name);
  return (fopen(path, a));
}

FILE *openpipe(const char *prog)
{
  FILE *p = NULL;

#ifdef POSIX
  p = popen(prog, "w");
#endif /* POSIX */
#ifdef _MSC
  p = _popen(prog, "w");
#endif /* _MSC */

  if (p == NULL)
    errlog(ERRORMSG, "Unable to open pipe to %s\n", prog);
  return p;
}

int
file_to_out(const char *filename)
{
    int len;
    FILE *fp;
    char chunk[1024];

    if ((fp = mix_openfile(filename, "r")) == NULL)
    	return -1;
    while ((len = fread(chunk, 1, sizeof(chunk), fp)) > 0)
    	{
	    fwrite(chunk, 1, len, stdout);
	}
    fclose (fp);
    return (len == 0 ? 0 : (-1));
}

int closepipe(FILE *p)
{
#ifdef POSIX
  return (pclose(p));
#elif defined(_MSC) /* end of POSIX */
  return (_pclose(p));
#else /* end of defined(_MSC) */
  return -1;
#endif /* else if not defined(_MSC), POSIX */
}

/** Base 64 encoding ****************************************************/

static byte bintoasc[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
static byte asctobin[] =
{
  0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80,
  0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80,
  0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80,
  0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80,
  0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80,
  0x80, 0x80, 0x80, 0076, 0x80, 0x80, 0x80, 0077,
  0064, 0065, 0066, 0067, 0070, 0071, 0072, 0073,
  0074, 0075, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80,
  0x80, 0000, 0001, 0002, 0003, 0004, 0005, 0006,
  0007, 0010, 0011, 0012, 0013, 0014, 0015, 0016,
  0017, 0020, 0021, 0022, 0023, 0024, 0025, 0026,
  0027, 0030, 0031, 0x80, 0x80, 0x80, 0x80, 0x80,
  0x80, 0032, 0033, 0034, 0035, 0036, 0037, 0040,
  0041, 0042, 0043, 0044, 0045, 0046, 0047, 0050,
  0051, 0052, 0053, 0054, 0055, 0056, 0057, 0060,
  0061, 0062, 0063, 0x80, 0x80, 0x80, 0x80, 0x80,

  0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80,
  0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80,
  0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80,
  0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80,
  0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80,
  0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80,
  0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80,
  0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80,
  0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80,
  0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80,
  0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80,
  0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80,
  0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80,
  0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80,
  0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80,
  0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80
};

void id_encode(byte id[], byte *s)
{
  sprintf
    (s, "%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x",
     id[0], id[1], id[2], id[3], id[4], id[5], id[6], id[7], id[8], id[9],
     id[10], id[11], id[12], id[13], id[14], id[15]);
}

void id_decode(byte *s, byte id[])
{
  int i, x[16];

  sscanf
    (s, "%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x",
     x, x + 1, x + 2, x + 3, x + 4, x + 5, x + 6, x + 7, x + 8,
     x + 9, x + 10, x + 11, x + 12, x + 13, x + 14, x + 15);
  for (i = 0; i < 16; i++)
    id[i] = x[i];
}

int encode(BUFFER *in, int linelen)
{
  byte *b, *e;
  int i, l, m;
  unsigned long u;
  BUFFER *out;

  out = buf_new();

  l = in->length;
  if (l % 3 != 0)
    l += 2;
  l = l / 3 * 4;

  if (linelen) {
    l += l / linelen + (l % linelen > 0 ? 1 : 0);
  }
  linelen /= 4;			/* blocks of 4 characters */

  buf_prepare(out, l);

  b = in->data;
  e = out->data;
  m = in->length - 2;
  for (i = 0, l = 0; i < m; i += 3) {
    u = ((unsigned long) b[i] << 16) | ((unsigned long) b[i + 1] << 8) |
	b[i + 2];
    *e++ = bintoasc[(u >> 18) & 0x3f];
    *e++ = bintoasc[(u >> 12) & 0x3f];
    *e++ = bintoasc[(u >> 6) & 0x3f];
    *e++ = bintoasc[u & 0x3f];
    if (linelen && ++l >= linelen) {
      l = 0;
      *e++ = '\n';
    }
  }
  if (i < in->length) {
    *e++ = bintoasc[b[i] >> 2];
    *e++ = bintoasc[((b[i] << 4) & 0x30) | ((b[i + 1] >> 4) & 0x0f)];
    if (i + 1 == in->length)
      *e++ = '=';
    else
      *e++ = bintoasc[((b[i + 1] << 2) & 0x3c) | ((b[i + 2] >> 6) & 0x03)];
    *e++ = '=';
    ++l;
  }
  if (linelen && l != 0)
    *e++ = '\n';
  *e = '\0';

  assert(out->data + out->length == e);
  buf_move(in, out);
  buf_free(out);
  return (0);
}

int decode(BUFFER *in, BUFFER *out)
{
  int err = 0;
  register byte c0 = 0, c1 = 0, c2 = 0, c3 = 0;
  register byte *a, *d, *end;
  int tempbuf = 0;
  int i;

  if (in == out) {
    out = buf_new();
    tempbuf = 1;
  }
  buf_prepare(out, 3 * (in->length - in->ptr) / 4);

  a = in->data + in->ptr;
  end = in->data + in->length - 3;
  d = out->data;
  i = 0;

  while (a < end) {
    if ((c0 = asctobin[a[0]]) & 0x80 ||
	(c1 = asctobin[a[1]]) & 0x80 ||
	(c2 = asctobin[a[2]]) & 0x80 ||
	(c3 = asctobin[a[3]]) & 0x80) {
      if (a[0] == '\n') {	/* ignore newline */
	a++;
	continue;
      } else if (a[0] == '\r' && a[1] == '\n') {	/* ignore crlf */
	a += 2;
	continue;
      } else if (a[0] == '=' && a[1] == '4' && a[2] == '6' && !(asctobin[a[5]] & 0x80) ) {
	a += 2;			/* '=46' at the left of a line really is 'F' */
	*a = 'F';		/* fix in memory ... */
	continue;
      } else if (a[2] == '=' || a[3] == '=') {
	if (a[0] & 0x80 || (c0 = asctobin[a[0]]) & 0x80 ||
	    a[1] & 0x80 || (c1 = asctobin[a[1]]) & 0x80)
	  err = -1;
	else if (a[2] == '=')
	  c2 = 0, i += 1;
	else if (a[2] & 0x80 || (c2 = asctobin[a[2]]) & 0x80)
	  err = -1;
	else
	  i += 2;
	if (err == 0) {
	  /* read the correct final block */
	  *d++ = (byte) ((c0 << 2) | (c1 >> 4));
	  *d++ = (byte) ((c1 << 4) | (c2 >> 2));
	  if (a[3] != '=')
	    *d++ = (byte) ((c2 << 6));
#if 1
	  if (a + 4 < in->data + in->length) {
	    a += 4;
	    continue;		/* support Mixmaster 2.0.3 encoding */
	  }
#endif /* 1 */
	  break;
	}
      }
      err = -1;
      break;
    }
    a += 4;

    *d++ = (byte) ((c0 << 2) | (c1 >> 4));
    *d++ = (byte) ((c1 << 4) | (c2 >> 2));
    *d++ = (byte) ((c2 << 6) | c3);
    i += 3;
  }

  in->ptr = a - in->data;

  assert(i <= out->length);
  out->length = i;

  if (tempbuf) {
    buf_move(in, out);
    buf_free(out);
  }
  return (err);
}

LOCK *lockfile(char *filename)
{
  LOCK *l;
  char name[LINELEN];

  strcpy(name, "lck");
  if (strchr(filename, DIRSEP))
    strcatn(name, strrchr(filename, DIRSEP), LINELEN);
  else
    strcatn(name, filename, LINELEN);
  l = malloc(sizeof(LOCK));

  l->name = malloc(PATHMAX);
  mixfile(l->name, name);
  l->f = mix_openfile(l->name, "w+");
  if (l->f)
    lock(l->f);
  return (l);
}

int unlockfile(LOCK *l)
{
  if (l->f) {
    unlock(l->f);
    fclose(l->f);
  }
  unlink(l->name);
  free(l->name);
  free(l);
  return (0);
}

int lock(FILE *f)
{
#ifndef WIN32
  struct flock lockstruct;

  lockstruct.l_type = F_WRLCK;
  lockstruct.l_whence = 0;
  lockstruct.l_start = 0;
  lockstruct.l_len = 0;
  return (fcntl(fileno(f), F_SETLKW, &lockstruct));
#else /* end of WIN32 */
  return (0);
#endif /* else if not WIN32 */
}

int unlock(FILE *f)
{
#ifndef WIN32

  struct flock lockstruct;

  lockstruct.l_type = F_UNLCK;
  lockstruct.l_whence = 0;
  lockstruct.l_start = 0;
  lockstruct.l_len = 0;
  return (fcntl(fileno(f), F_SETLKW, &lockstruct));
#else /* end of not WIN32 */
  return (0);
#endif /* else if WIN32 */
}

/* get passphrase ******************************************************/

static int getuserpass(BUFFER *b, int mode)
{
  char p[LINELEN];
  int fd;
  int n;

#ifdef HAVE_TERMIOS
  struct termios attr;

#endif /* HAVE_TERMIOS */

  if (mode == 0)
    fprintf(stderr, "enter passphrase: ");
  else
    fprintf(stderr, "re-enter passphrase: ");
  fflush(stderr);
#ifndef UNIX
#ifdef HAVE_GETKEY
  for (n = 0; p[n] != '\n' && n < LINELEN; n++) {
    p[n] = getkey();
  }
  p[n] = 0;
#else /* end of HAVE_GETKEY */
  scanf("%127s", p);
#endif /* else if not HAVE_GETKEY */
#else /* end of not UNIX */
  fd = open("/dev/tty", O_RDONLY);
  if (tcgetattr(fd, &attr) != 0)
    return (-1);
  attr.c_lflag &= ~ECHO;
  attr.c_lflag |= ICANON;
  if (tcsetattr(fd, TCSAFLUSH, &attr) != 0)
    return (-1);

  n = read(fd, p, LINELEN);

  attr.c_lflag |= ECHO;
  if (tcsetattr(fd, TCSAFLUSH, &attr) != 0)
    return (-1);

  close(fd);
  fprintf(stderr, "\n");
  p[n - 1] = 0;
#endif /* else if UNIX */
  if (mode == 0)
    buf_appends(b, p);
  else
    return (bufeq(b, p));
  return (0);
}

static BUFFER *userpass = NULL;

int user_pass(BUFFER *key)
{
  if (userpass == NULL) {
    userpass = buf_new();
    userpass->sensitive = 1;
    if (getenv("MIXPASS"))
      buf_sets(userpass, getenv("MIXPASS"));
    else if (menu_getuserpass(userpass, 0) == -1)
      getuserpass(userpass, 0);
  }
  buf_set(key, userpass);
  key->sensitive = 1;
  return (0);
}

int user_confirmpass(BUFFER *key)
{
  int ok;

  ok = menu_getuserpass(key, 1);
  if (ok == -1)
    ok = getuserpass(key, 1);
  return (ok);
}

void user_delpass(void)
{
  if (userpass)
    buf_free(userpass);
  userpass = NULL;
}

int write_pidfile(char *pidfile)
{
  int err = 0;
#ifdef POSIX
  FILE *f;
  char host[LINELEN], myhostname[LINELEN];
  int pid, mypid;
  int assigned;

  mypid = getpid();
  gethostname(myhostname, LINELEN);
  myhostname[LINELEN-1] = '\0';

  f = mix_openfile(pidfile, "r+");
  if (f != NULL) {
    assert(LINELEN > 71);
    assigned = fscanf(f, "%d %70s", &pid, host);
    if (assigned == 2) {
      if (strcmp(host, myhostname) == 0) {
	if (kill (pid, 0) == -1) {
	  if (errno == ESRCH) {
	    fprintf(stderr, "Rewriting stale pid file.\n");
	    rewind(f);
	    ftruncate(fileno(f), 0);
	    fprintf(f, "%d %s\n", mypid, myhostname);
	  } else {
	    fprintf(stderr, "Pid file exists and process still running.\n");
	    err = -1;
	  }
	} else {
	  fprintf(stderr, "Pid file exists and process still running.\n");
	  err = -1;
	}
      } else {
	/* Pid file was written on another host, fail */
	fprintf(stderr, "Pid file exists and was created on another host (%s).\n", host);
	err = -1;
      }
    } else {
      fprintf(stderr, "Pid file exists and and could not be parsed.\n");
      err = -1;
    }
  } else {
    if (errno == ENOENT) {
      f = mix_openfile(pidfile, "w+");
      if (f != NULL) {
	fprintf(f, "%d %s\n", mypid, myhostname);
      } else {
	fprintf(stderr, "Could not open pidfile for writing: %s\n", strerror(errno));
	err = -1;
      }
    } else {
      fprintf(stderr, "Could not open pidfile for readwrite: %s\n", strerror(errno));
      err = -1;
    };
  }
  if(f)
    fclose(f);
#endif /* POSIX */
  return (err);
}

int clear_pidfile(char *pidfile)
{
#ifdef POSIX
  char path[PATHMAX];

  mixfile(path, pidfile);
  return (unlink(path));
#else /* end of POSIX */
  return (0);
#endif /* else if not POSIX */
}

time_t parse_yearmonthday(char* str)
{
  time_t date;
  int day, month, year;

  if (sscanf( str, "%d-%d-%d", &year, &month, &day) == 3 ) {
    struct tm timestruct;
    char *tz;

    tz = getenv("TZ");
#ifdef HAVE_SETENV
    setenv("TZ", "GMT", 1);
#else /* end of HAVE_SETENV */
    putenv("TZ=GMT");
#endif /* else if not HAVE_SETENV */
    tzset();
    memset(&timestruct, 0, sizeof(timestruct));
    timestruct.tm_mday = day;
    timestruct.tm_mon = month - 1;
    timestruct.tm_year = year - 1900;
    date = mktime(&timestruct);
#ifdef HAVE_SETENV
    if (tz)
      setenv("TZ", tz, 1);
    else
      unsetenv("TZ");
#else  /* end of HAVE_SETENV */
    if (tz) {
      char envstr[LINELEN];
      snprintf(envstr, LINELEN, "TZ=%s", tz);
      putenv(envstr);
    } else
      putenv("TZ=");
#endif /* else if not HAVE_SETENV */
    tzset();
    return date;
  } else
    return -1;
}

/* functions missing on some systems *************************************/

#ifdef __RSXNT__
int fileno(FILE *f)
{
  return (f->_handle);
}

#endif /* __RSXNT__ */

#ifdef _MSC	/* Visual C lacks dirent */

DIR *opendir(const char *name)
{
  DIR *dir;
  WIN32_FIND_DATA d;
  char path[PATHMAX];

  dir = malloc(sizeof(HANDLE));

  sprintf(path, "%s%c*", name, DIRSEP);
  *dir = FindFirstFile(path, &d);
  /* first file found is "." -- can be safely ignored here */

  if (*dir == INVALID_HANDLE_VALUE) {
    free(dir);
    return (NULL);
  } else
    return (dir);
}

struct dirent e;
struct dirent *readdir(DIR *dir)
{
  WIN32_FIND_DATA d;
  int ok;

  ok = FindNextFile(*dir, &d);
  if (ok) {
    strncpy(e.d_name, d.cFileName, PATHMAX);
    return (&e);
  } else
    return (NULL);
}

int closedir(DIR *dir)
{
  if (dir) {
    FindClose(*dir);
    free(dir);
    return (0);
  }
  return (-1);
}

#endif /* _MSC */
