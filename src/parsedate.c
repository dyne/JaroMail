/* Jaro Mail
 *
 *  (C) Copyright 2014 Denis Roio <jaromil@dyne.org>
 *      
 * Minimalist date reformatted learned from Mairix and Mutt
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
#include <stdlib.h>
#include <string.h>
#include <time.h>
/* #include <sys/types.h> */
/* #include <sys/stat.h> */

time_t parse_rfc822_date(char *date_string)/*{{{*/
{
  struct tm tm;
  char *s, *z;
  /* Format [weekday ,] day-of-month month year hour:minute:second timezone.

     Some of the ideas, sanity checks etc taken from parse.c in the mutt
     sources, credit to Michael R. Elkins et al
     */

  s = date_string;
  z = strchr(s, ',');
  if (z) s = z + 1;
  while (*s && isspace(*s)) s++;
  /* Should now be looking at day number */
  if (!isdigit(*s)) goto tough_cheese;
  tm.tm_mday = atoi(s);
  if (tm.tm_mday > 31) goto tough_cheese;

  while (isdigit(*s)) s++;
  while (*s && isspace(*s)) s++;
  if (!*s) goto tough_cheese;
  if      (!strncasecmp(s, "jan", 3)) tm.tm_mon =  0;
  else if (!strncasecmp(s, "feb", 3)) tm.tm_mon =  1;
  else if (!strncasecmp(s, "mar", 3)) tm.tm_mon =  2;
  else if (!strncasecmp(s, "apr", 3)) tm.tm_mon =  3;
  else if (!strncasecmp(s, "may", 3)) tm.tm_mon =  4;
  else if (!strncasecmp(s, "jun", 3)) tm.tm_mon =  5;
  else if (!strncasecmp(s, "jul", 3)) tm.tm_mon =  6;
  else if (!strncasecmp(s, "aug", 3)) tm.tm_mon =  7;
  else if (!strncasecmp(s, "sep", 3)) tm.tm_mon =  8;
  else if (!strncasecmp(s, "oct", 3)) tm.tm_mon =  9;
  else if (!strncasecmp(s, "nov", 3)) tm.tm_mon = 10;
  else if (!strncasecmp(s, "dec", 3)) tm.tm_mon = 11;
  else goto tough_cheese;

  while (!isspace(*s)) s++;
  while (*s && isspace(*s)) s++;
  if (!isdigit(*s)) goto tough_cheese;
  tm.tm_year = atoi(s);
  if (tm.tm_year < 70) {
    tm.tm_year += 100;
  } else if (tm.tm_year >= 1900) {
    tm.tm_year -= 1900;
  }

  while (isdigit(*s)) s++;
  while (*s && isspace(*s)) s++;
  if (!*s) goto tough_cheese;

  /* Now looking at hms */
  /* For now, forget this.  The searching will be vague enough that nearest day is good enough. */

  tm.tm_hour = 0;
  tm.tm_min = 0;
  tm.tm_sec = 0;
  tm.tm_isdst = 0;
  return mktime(&tm);

tough_cheese:
  return (time_t) -1; /* default value */
}

int main(int argc, char **argv) {
  time_t res;
  if(argc<2) {
    printf("usage: parsedate date_string_from_mail_header\n");
    printf("returns date in seconds since the Epoch.\n");
    exit(0); }
  res = parse_rfc822_date(argv[1]);
  if(res<0) exit(1);
  //  printf("Date: %d (seconds since the Epoch)\n",res);
  printf("%lld\n", (long long) res);
  exit(0);
}
