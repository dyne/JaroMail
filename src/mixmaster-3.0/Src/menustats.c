/* Mixmaster version 3.0  --  (C) 1999 - 2006 Anonymizer Inc. and others.

   Mixmaster may be redistributed and modified under certain conditions.
   This software is distributed on an "AS IS" basis, WITHOUT WARRANTY OF
   ANY KIND, either express or implied. See the file COPYRIGHT for
   details.

   Menu-based user interface
   $Id: menustats.c 934 2006-06-24 13:40:39Z rabbi $ */


#if 0
void errlog(int type, char *format, ...) { };
char MIXDIR[512];
#include "util.c"
#include "buffers.c"
int menu_getuserpass(BUFFER *p, int mode) { return 0; };
#endif

#include <string.h>
#include <assert.h>
#include <stdlib.h>

#include "menu.h"
#ifdef WIN32
#include <urlmon.h>
#pragma comment(lib,"urlmon.lib")
#else
#include <sys/wait.h>
#endif /* WIN32 */


int url_download(char *url, char *dest) {
  int err;
#ifdef WIN32
  err = URLDownloadToFile(NULL, url, dest, BINDF_GETNEWESTVERSION, NULL);

  if (err != S_OK)
    return -1;
  else
    return 0;
#else
  char s[PATHMAX];
  snprintf(s, PATHMAX, "%s -q %s -O %s", WGET, url, dest);
  err = system(s);

  if (err != -1) {
    if (WIFEXITED(err) == 0)
      return -1; /* abnormal child exit */
    else
      return 0;
  }
  else
    return -1;
#endif /* WIN32 */
}

/** read the allpingers file into the buffer */
void read_allpingers(BUFFER *allpingers) {
  FILE *f;

  f = mix_openfile(ALLPINGERSFILE, "r");
  if (f != NULL) {
    buf_clear(allpingers);
    buf_read(allpingers, f);
    fclose(f);
  }
}

/* Get all sections from inifile.
 *
 * They are put into the sections buffer, separated by newlines
 */
void get_sections(BUFFER *inifile, BUFFER  *sections) {
  BUFFER *line;
  int err;

  line = buf_new();

  buf_rewind(inifile);
  buf_reset(sections);

  while ((err = buf_getline(inifile, line)) != -1) {
    if (bufileft (line, "[") &&
	bufiright(line, "]")) {
      line->data[line->length-1] = '\0';
      buf_appends(sections, line->data+1);
      buf_nl(sections);
    };
  }
  buf_free (line);
}

/* Get value of an attribute
 *
 * returns -1 if it isn't found.
 */
int get_attribute(BUFFER *inifile, char *section, char *attribute, BUFFER *value) {
  BUFFER *line;
  int err = -1;
  int insection = 0;

  line = buf_new();

  buf_rewind(inifile);
  buf_reset(value);

  while (buf_getline(inifile, line) != -1) {
    if (bufileft (line, "[") &&
	bufiright(line, "]")) {
      if (insection)
	break;

      line->data[line->length-1] = '\0';
      if (strcasecmp(section, line->data+1) == 0) {
	insection = 1;
      }
    } else if (insection && bufileft(line, attribute)) {
      /* we are in the right section and this attribute name
       * at least starts with what we want */
      char *ptr = line->data + strlen(attribute);
      /* eat up whitespace */
      while ((*ptr == ' ') || (*ptr == '\t'))
	ptr++;
      if (*ptr != '=')
	continue;
      ptr++;
      while ((*ptr == ' ') || (*ptr == '\t'))
	ptr++;
      buf_appends(value, ptr);
      err = 0;
      break;
    }
  }
  buf_free (line);
  return (err);
}





static char *files[] = { "mlist", "rlist", "mixring", "pgpring"};
#define NUMFILES sizeof(files)/sizeof(*files)

/* Download all the needed files from the specified source */
/* returns -1 on error */
int stats_download(BUFFER *allpingers, char *sourcename, int curses) {
  char *localfiles[] = { TYPE2REL, TYPE1LIST, PUBRING, PGPREMPUBASC };
  char path[PATHMAX];
  char path_t[PATHMAX];
  BUFFER *value;
  int ret = 0;
  int err;
  int i;

  value = buf_new();

  if (curses) {
    clear();
  }

  err = get_attribute(allpingers, sourcename, "base", value);
  if (err == 0) {
    if (curses) {
      standout();
      printw("%s", value->data);
      standend();
    }
    else
      printf("%s\n\r", value->data);
  }

/* Loop to get each file in turn to a temp file */

  for (i=0; i<NUMFILES; i++) {
    err = get_attribute(allpingers, sourcename, files[i], value);
    if (err < 0) {
      /* the attribute vanished under us */
      ret = -1;
      break;
    }
    mixfile(path, localfiles[i]);
    if (curses) {
      mvprintw(i+3, 0, "downloading %s from %s...", localfiles[i], value->data);
      refresh();
    }
    else
      printf("downloading %s from %s...", localfiles[i], value->data);
    err = url_download(value->data, strcat(path, ".t"));
    if (err < 0) {
      if (curses)
	printw("failed to download.\n\rTry using another stats source.");
      else
	printf("failed to download.\n\rTry using another stats source.\n\r");
      ret = -1;
      break;
    }
    if (curses)
      printw("done");
    else
      printf("done\n\r");
  }

/* We got them all ok - so rename them to their correct names */

  for (i=0; i<NUMFILES; i++) {
    mixfile(path, localfiles[i]);
    mixfile(path_t, localfiles[i]);
    strcat(path_t, ".t");
    rename(path_t, path);
  }
  
  if (curses) {  
    printw("\n\n\n\n\rPress any key to continue");
    getch();
    clear();
  }
  buf_free(value);
  return ret;
}
/* Checks whether the stats source has all the required files.
 *
 * 1 if it has,
 * 0 otherwise
 */
int good_stats_source (BUFFER *allpingers, char *sourcename) {
  BUFFER *value;
  int i;
  int res = 1;
  int err;

  value = buf_new();

  for (i = 0; i < NUMFILES; i++) {
    err = get_attribute(allpingers, sourcename, files[i], value);
    if (err < 0) {
      res = 0;
      break;
    }
  }

  buf_free (value);
  return (res);
}

/* Do a stats download update and report status */
/* 0  on success              */
/* -1 File download failed    */
/* -2 Error writing file      */
/* -3 Stats source incomplete */

int download_stats(char *sourcename) {
  BUFFER *inifile;
  FILE *f;
  int ret;
  inifile = buf_new();
  read_allpingers(inifile);
  if (good_stats_source(inifile, sourcename) == 1) {
    if (stats_download(inifile, sourcename, 0) == 0) {
      f = mix_openfile(STATSSRC, "w+");
      if (f != NULL) {
        fprintf(f, "%s", sourcename);
        fclose(f);
	ret = 0;
      } else {
        ret = -2;
	errlog(ERRORMSG, "Could not open stats source file for writing.\n");
      }
    } else {
        ret = -1;
	errlog(ERRORMSG, "Stats source download failed.\n");
    }
  } else {
      ret = -3;
      errlog(ERRORMSG, "Stats source does not include all required files.\n");
  }

  buf_free(inifile);
  return (ret);
}


/* Download allpingers.txt */
/* -1 on error */
static int download_list() {
  char path[PATHMAX];

  mixfile(path, ALLPINGERSFILE);

  clear();
  standout();
  printw(ALLPINGERSURL);
  standend();

  mvprintw(3,0,"downloading %s...", ALLPINGERSURL);
  refresh();
  if (url_download(ALLPINGERSURL, path) < 0) {
    printw("failed to download.\n\rTry again later.");
    printw("\n\n\rPress any key to continue");
    getch();
    return -1;
  }
  return 0;
}

/* Displays the choice of stats sources */
#define MAXPING (26 * 2)
void update_stats() {
  char c;
  BUFFER *inifile;
  BUFFER *pingernames;
  BUFFER *goodpingers;
  BUFFER *line;
  BUFFER *statssrc;
  FILE *f;
  int num;
  int err;
  int x, y;
  int i;

  inifile = buf_new();
  pingernames = buf_new();
  goodpingers = buf_new();
  line = buf_new();
  statssrc = buf_new();

  while (1) {
    clear();
    standout();
    buf_clear(statssrc);
    f = mix_openfile(STATSSRC, "r");
    if (f != NULL) {
      buf_read(statssrc, f);
      fclose(f);
    }
    printw("Select stats source:");
    standend();
    if (statssrc->length > 0)
      printw("       Current: %s (Enter to download)", statssrc->data);
    printw("\n\n");

    read_allpingers(inifile);
    get_sections (inifile, pingernames);

    num = 0;
    buf_reset(goodpingers);
    buf_rewind(pingernames);
    while ((buf_getline(pingernames, line) != -1) && num < MAXPING) {
      if (good_stats_source (inifile, line->data)) {
	buf_cat(goodpingers, line);
	buf_nl(goodpingers);
	num++;
      }
    }

    x = 0;
    buf_rewind(goodpingers);
    for (i=0; i<num; i++) {
      err = buf_getline(goodpingers, line);
      assert (err != -1);
      y = i;
      if (y >= LINES - 6)
	y -= LINES - 6, x = 40;
      mvprintw(y + 2, x, "%c", i < 26 ? i + 'a' : i - 26 + 'A');
      mvprintw(y + 2, x + 2, "%s", line->data);
    }
    y = i + 3;
    if (y > LINES - 4)
      y = LINES - 4;
    mvprintw(y, 0, "*  update list of pingers from web     =  edit list       <space> to exit");
    c = getch();
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')) {
      if (c >= 'a')
	c -= 'a';
      else
	c = c - 'A' + 26;
      if (c < num) {
	buf_rewind(goodpingers);
	while (c >= 0) {
	  err = buf_getline(goodpingers, line);
	  assert (err != -1);
	  c--;
	}
	if (stats_download(inifile, line->data, 1) == 0) {
	  f = mix_openfile(STATSSRC, "w+");
	  if (f != NULL) {
	    fprintf(f, "%s", line->data);
	    fclose(f);
	  } else
	    fprintf(stderr, "Could not open stats source file for writing\n");
	  break;
	}
      }
    }
    else if (c == '*') {
      download_list();
    }
    else if (c == '=') {
      char path[PATHMAX];
      mixfile(path, ALLPINGERSFILE);
      menu_spawn_editor(path, 0);
    }
    else if ((c == '\r') && statssrc->length) {
      stats_download(inifile, statssrc->data, 1);
      break;
    }
    else if (c == ' ') {
      break;
    }
  }
  clear();

  buf_free(statssrc);
  buf_free(inifile);
  buf_free(line);
  buf_free(pingernames);
  buf_free(goodpingers);
}

#if 0
int main() {
  strcpy(MIXDIR,"./");

  BUFFER *allpingers;
  BUFFER *pingernames;
  BUFFER *value;

  allpingers = buf_new();
  pingernames = buf_new();
  value = buf_new();

  read_allpingers (allpingers);
  get_sections (allpingers, pingernames);

  printf("%s", pingernames->data);

  get_attribute (allpingers, "noreply", "rlist", value);
  printf("%s\n", value->data);


  exit(0);
}

#endif
