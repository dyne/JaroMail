/* mpgp -- (C) 2000 - 2006 Ulf Moeller and others.

   mpgp may be redistributed and modified under certain conditions.
   This software is distributed on an "AS IS" basis, WITHOUT WARRANTY OF
   ANY KIND, either express or implied. See the file COPYRIGHT for
   details.

   Test application for OpenPGP features
   $Id: mpgp.c 934 2006-06-24 13:40:39Z rabbi $ */

#define MPGPVERSION "0.3.0"

#include "mix3.h"
#include "pgp.h"
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <assert.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <time.h>
#include <fcntl.h>
#ifdef POSIX
#include <unistd.h>
#include <termios.h>
#endif /* POSIX */

int pass(BUFFER *b)
{
  char p[LINELEN];
  int fd;
  int n;

#ifdef HAVE_TERMIOS
  struct termios attr;
#endif /* HAVE_TERMIOS */

  fprintf(stderr, "enter passphrase: ");
  fflush(stderr);
#ifdef HAVE_TERMIOS
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
  p[n - 1] = 0;

#else /* end of HAVE_TERMIOS */
  fgets(p, LINELEN, stdin);
  if (p[strlen(p)-1]=='\n')
    p[strlen(p)-1] = 0;
#endif /* else if not HAVE_TERMIOS */

  fprintf(stderr, "\n");
  buf_appends(b, p);
  return (0);
}

void usage(char *n)
{
  fprintf(stderr, "Usage: %s -e [-b] user@domain\n", n);
  fprintf(stderr, "       %s -s [-b] [yourname@domain]\n", n);
  fprintf(stderr, "       %s -c [-b]\n", n);
  fprintf(stderr, "       %s -C [-b]\n", n);
  fprintf(stderr, "       %s -d [passphrase]\n", n);
  fprintf(stderr, "       %s -g[r] yourname@domain [bits]\n", n);
  fprintf(stderr, "       %s -a[+-] [-b]\n", n);
  fprintf(stderr, "       %s -V\n\n", n);
  fprintf(stderr, "PGP public key ring: %s\n", PGPPUBRING);
  fprintf(stderr, "PGP secret key ring: %s\n", PGPSECRING);
}

int decrypt(BUFFER *u, BUFFER *option, char *n)
{
  BUFFER *v;
  BUFFER *sig;
  int err = 0;

  v = buf_new();
  sig = buf_new();

  buf_set(v, u);
  err = pgp_decrypt(v, NULL, sig, PGPPUBRING, PGPSECRING);
  if (err >= 0 || err == PGP_SIGBAD)
    buf_move(u, v);

  if (err == PGP_ERR) {
    pass(option);
    err = pgp_decrypt(u, option, sig, PGPPUBRING, PGPSECRING);
  }
  switch (err) {
  case PGP_NOMSG:
    fprintf(stderr, "%s: Not a PGP message.\n", n);
    break;
  case PGP_ERR:
    fprintf(stderr, "%s: Can't read message.\n", n);
    break;
  case PGP_SIGOK:
    fprintf(stderr, "%s: Valid signature: %s\n", n, sig->data);
    err = 0;
    break;
  case PGP_SIGNKEY:
    fprintf(stderr, "%s: Unknown signature key %s, cannot verify.\n", n, sig->data);
    err = 1;
    break;
  case PGP_SIGBAD:
    fprintf(stderr, "%s: Bad signature.\n", n);
    err = 1;
    break;
  }

  buf_free(v);
  buf_free(sig);

  return (err);
}

int main(int argc, char *argv[])
{
  BUFFER *u, *option, *pp;
  char *filename = NULL;
  char *cmd = NULL;
  int text = 1;
  int err = 99;
  int bits = 0;

  mix_init(NULL);
  VERBOSE = 3;

  u = buf_new();
  option = buf_new();
  pp = buf_new();

  if (argc > 1 && argv[1][0] == '-')
    cmd = argv[1];

  if (argc == 1 || (cmd > 0 && (cmd[1] == 'e' || cmd[1] == 'c' ||
				cmd[1] == 'd' || cmd[1] == 'a' ||
				cmd[1] == 's' || cmd[1] == 'C'))) {
    if ((argc > 2 && (cmd == NULL || cmd[1] == 'a')) || argc > 3) {
      FILE *f;

      f = fopen(argv[argc - 1], "rb");
      if (f == NULL) {
	fprintf(stderr, "%s: Can't open %s\n", argv[0], argv[argc - 1]);
	err = -1;
      } else {
	buf_read(u, f);
	fclose(f);
	filename = argv[argc - 1];
	argc--;
      }
    } else
      buf_read(u, stdin);
  }
  if (argc == 1)
    err = decrypt(u, option, argv[0]);

  if (argc > 2 && argv[2][0] == '-' && argv[2][1] == 'b') {
    text = 0;
    if (argc > 3)
      buf_appends(option, argv[3]);
  } else if (argc > 2)
    buf_appends(option, argv[2]);

  if (cmd)
    switch (cmd[1]) {
    case 's':
      err = pgp_encrypt(PGP_SIGN | (text ? PGP_TEXT : 0), u, NULL, option,
			NULL, PGPPUBRING, PGPSECRING);
      if (err != 0) {
	pass(pp);
	err = pgp_encrypt(PGP_SIGN | (text ? PGP_TEXT : 0), u, NULL, option,
			  pp, PGPPUBRING, PGPSECRING);
      }
      if (err != 0)
	fprintf(stderr, "Error.\n");
      break;
    case 'e':
      if (option->length) {
	err = pgp_encrypt(PGP_ENCRYPT | (text ? PGP_TEXT : 0), u, option, NULL,
			  NULL, PGPPUBRING, PGPSECRING);
	if (err < 0)
	  fprintf(stderr, "%s: can't encrypt message for %s\n",
		  argv[0], argv[2]);
      }
      break;
    case 'c':
      pass(option);
      err = pgp_encrypt(PGP_CONVENTIONAL | (text ? PGP_TEXT : 0), u, option,
			NULL, NULL, PGPPUBRING, PGPSECRING);
      if (err < 0)
	fprintf(stderr, "%s: can't encrypt message\n", argv[0]);
      break;
    case 'C':
      pass(option);
      err = pgp_encrypt(PGP_NCONVENTIONAL | (text ? PGP_TEXT : 0), u, option,
			NULL, NULL, PGPPUBRING, PGPSECRING);
      if (err < 0)
	fprintf(stderr, "%s: can't encrypt message\n", argv[0]);
      break;
    case 'g':
      if (argc < 3) {
	err = 99;
	goto end;
      }
      pass(pp);
      if (argc == 4)
	sscanf(argv[3], "%d", &bits);
      err = pgp_keygen(cmd[2] == 'r' ? PGP_ES_RSA : PGP_E_ELG,
		       bits, option, pp, PGPPUBRING, PGPSECRING, 0);
      break;
    case 'a':
      switch (cmd[2]) {
      case '-':
	err = pgp_dearmor(u, u);
	if (err == -1)
	  fprintf(stderr, "Not a PGP-armored message\n");
	goto end;
      case '+':
	break;
      default:
	pgp_literal(u, filename, text);
	pgp_compress(u);
	break;
      }
      err = pgp_armor(u, PGP_ARMOR_NORMAL);
      break;
    case 'd':
      err = decrypt(u, option, argv[0]);
      break;
    case 'h':
      usage(argv[0]);
      err = 0;
      break;
    case 'V':
      fprintf(stderr, "mpgp version %s\n", MPGPVERSION);
      fprintf(stderr, "(C) 2000 - 2004 Ulf Moeller and others.\n");
      fprintf(stderr, "See the file COPYRIGHT for details.\n");
      err = 0;
      break;
    }
end:
  if (err == 99)
    usage(argv[0]);

  if (err >= 0)
    buf_write(u, stdout);

  buf_free(option);
  buf_free(pp);
  buf_free(u);

  mix_exit();
  return (err == -1 ? 1 : err);
}
