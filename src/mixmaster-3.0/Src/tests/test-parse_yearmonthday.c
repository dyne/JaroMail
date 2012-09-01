#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#define LINELEN 128

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

int main()
{
	int t;
	
	t = parse_yearmonthday("2003-04-02");
	if (t == 1049241600) {
		printf("OK.\n");
		exit(0);
	} else {
		printf("Failed.\n");
		exit(1);
	}
}
