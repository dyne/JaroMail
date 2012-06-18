/* Jaro Mail
 *
 *  (C) Copyright 2012 Denis Roio <jaromil@dyne.org>
 *      
 * This is just a simple use of Mairix API to extract formatted dates
 *
 * This source code is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Public License as published 
 * by the Free Software Foundation; either version 3 of the License,
 * or (at your option) any later version.
 *
 * This source code is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * Please refer to the GNU Public License for more details.
 *
 * You should have received a copy of the GNU Public License along with
 * this source code; if not, write to:
 * Free Software Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 *
 */

#include <stdio.h>
#include <mairix.h>

int verbose = 0;
int do_hardlinks = 0;
void out_of_mem(char *file, int line, size_t size);
void report_error(const char *str, const char *filename);
void emit_int(int x);

static struct rfc822 *parsed;

int main (int argc, char **argv) {
  if( argc < 3 ) {
    printf("usage: fetchdate strftime_format filename\n");
    printf("see man strftime(5) for format options");
    exit(1);
  }

  //  printf("Parsing email: %s\n",argv[1]);
  parsed = make_rfc822(argv[2]);

  if (parsed) {
    char datebuf[64];
    struct tm *thetm;
#ifdef TEST
    if (parsed->hdrs.to)      printf("  To:         %s\n", parsed->hdrs.to);
    if (parsed->hdrs.cc)      printf("  Cc:         %s\n", parsed->hdrs.cc);
    if (parsed->hdrs.from)    printf("  From:       %s\n", parsed->hdrs.from);
    if (parsed->hdrs.subject) printf("  Subject:    %s\n", parsed->hdrs.subject);
    if (parsed->hdrs.message_id)
      printf("  Message-ID: %s\n", parsed->hdrs.message_id);
    thetm = gmtime(&parsed->hdrs.date);
    strftime(datebuf, sizeof(datebuf), "%a, %d %b %Y", thetm);
    printf("  Date:        %s\n", datebuf);
#else
    thetm = gmtime(&parsed->hdrs.date);
    strftime(datebuf, sizeof(datebuf), argv[1], thetm);

    /* needed by timecloud:

   [["YYYY-MM-DD",[["TAG1","WEIGHT1"],
                   ["TAG2","WEIGHT2"],
                   ...
                  ]
    ],
    ["YYYY-MM-DD",[....]]
   ]

    */      

    printf("%s\n",datebuf);
    
#endif
    free_rfc822(parsed);
  }

  exit(0);
}

void out_of_mem(char *file, int line, size_t size)/*{{{*/
{
  /* Hairy coding ahead - can't use any [s]printf, itoa etc because
   * those might try to use the heap! */

  int filelen;
  char *p;

  static char msg1[] = "Out of memory (at ";
  static char msg2[] = " bytes)\n";
  /* Perhaps even strlen is unsafe in this situation? */
  p = file;
  while (*p) p++;
  filelen = p - file;
  write(2, msg1, sizeof(msg1));
  write(2, file, filelen);
  write(2, ":", 1);
  emit_int(line);
  write(2, ", ", 2);
  emit_int(size);
  write(2, msg2, sizeof(msg2));
  exit(2);
}
/*}}}*/
void report_error(const char *str, const char *filename)/*{{{*/
{
  if (filename) {
    int len = strlen(str) + strlen(filename) + 4;
    char *t;
    t = new_array(char, len);
    sprintf(t, "%s '%s'", str, filename);
    perror(t);
    free(t);
  } else {
    perror(str);
  }
}
void emit_int(int x)/*{{{*/
{
  char buf1[20], buf2[20];
  char *p, *q;
  int neg=0;
  p = buf1;
  *p = '0'; /* In case x is zero */
  if (x < 0) {
    neg = 1;
    x = -x;
  }
  while (x) {
    *p++ = '0' + (x % 10);
    x /= 10;
  }
  p--;
  q = buf2;
  if (neg) *q++ = '-';
  while (p >= buf1) {
    *q++ = *p--;
  }
  write(2, buf2, q-buf2);
  return;
}
