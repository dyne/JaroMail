/* Mixmaster version 3.0  --  (C) 1999 - 2006 Anonymizer Inc. and others.

   Mixmaster may be redistributed and modified under certain conditions.
   This software is distributed on an "AS IS" basis, WITHOUT WARRANTY OF
   ANY KIND, either express or implied. See the file COPYRIGHT for
   details.

   Send messages from pool
   $Id: pool.c 934 2006-06-24 13:40:39Z rabbi $ */

#include "mix3.h"
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <sys/types.h>
#include <time.h>
#ifdef POSIX
#include <unistd.h>
#else /* end of POSIX */
#include <io.h>
#endif /* else if not POSIX */
#ifndef _MSC
#include <dirent.h>
#endif /* not _MSC */
#include <assert.h>
#include <sys/stat.h>
#include <sys/types.h>

#ifdef USE_PCRE
#include "pcre.h"
#endif /* USE_PCRE */

int msg_send(char *name);

int mix_send(void)
{
  return (mix_regular(FORCE_POOL));
}

/* Message pool:       Unix  DOS
 * latent messages:      l*  *.lat
 * pooled messages:      m*  *.msg
 * messages to be sent:  s*  *.snd
 * temporary files:      t*  *.tmp
 * files in user editor: x*
 * incoming mail:        i*  *.inf
 * partial messages:     p*  p*.*
 * error messages:       e*  *.err
 * outgoing messages:    out *.txt (to be used by external program)
 */

static int is(char *path, char *type)
{
#ifdef SHORTNAMES
  int s;

  s = strlen(path);
  if (s <= 4)
    return 0;
  return (path[s - 4] == '.' && streq(path + s - 3, type));
#else /* end of SHORTNAMES */
  return (path[0] == type[0]);
#endif /* else if not SHORTNAMES */
}

static void mv(char *name, char *newtype)
{
  char old[PATHMAX], new[PATHMAX];

  sprintf(old, "%s%c%s", POOLDIR, DIRSEP, name);
#ifdef SHORTNAMES
  assert(strlen(name) > 4);
  strcpy(name + strlen(name) - 3, newtype);
#else /* end of SHORTNAMES */
  name[0] = newtype[0];
#endif /* else if not SHORTNAMES */
  sprintf(new, "%s%c%s", POOLDIR, DIRSEP, name);
  rename(old, new);
}

int latent_read(void)
{
  char path[PATHMAX];
  DIR *d;
  FILE *f;
  struct dirent *e;
  int size = 0;
  long now, then;

  now = time(NULL);
  d = opendir(POOLDIR);
  if (d != NULL)
    for (;;) {
      e = readdir(d);
      if (e == NULL)
	break;
      if (is(e->d_name, "lat")) {
	sprintf(path, "%s%c%s", POOLDIR, DIRSEP, e->d_name);
	f = fopen(path, "rb");
	if (f != NULL) {
	  fscanf(f, "%*d %ld\n", &then);
	  fclose(f);
	  if (now > then)
	    mv(e->d_name, "msg");
	}
      }
    }
  closedir(d);
  return (size);
}

int infile_read(void)
{
  char path[PATHMAX];
  BUFFER *msg;
  DIR *d;
  FILE *f;
  struct dirent *e;
  int size = 0;

  msg = buf_new();
  d = opendir(POOLDIR);
  if (d != NULL)
    for (;;) {
      e = readdir(d);
      if (e == NULL)
	break;
      if (is(e->d_name, "inf")) {
	mv(e->d_name, "tmp");
	sprintf(path, "%s%c%s", POOLDIR, DIRSEP, e->d_name);
	f = fopen(path, "rb");
	if (f != NULL) {
	  buf_clear(msg);
	  buf_read(msg, f);
	  fclose(f);
	  unlink(path);
	  mix_decrypt(msg);
	}
      }
    }
  closedir(d);
  buf_free(msg);
  return (size);
}

int mailin_maildir_one(char *dir)
/** Read mails from one directory
    This function reads all files from the directory passed and passes
    them on to mix_decrypt(). Each file is unlinked when its is read.

    @param dir	The directory in which files are to be read. No path finding
    		voodoo is done to the path; It's passed it opendir() as is.
    @author	PP
    @return	Number of files read
 */
{
  BUFFER *msg;
  DIR *d;
  FILE *f;
  struct dirent *e;
  int size = 0;
  char path[PATHMAX];

  msg = buf_new();
  d = opendir(dir);
  if (d != NULL)
    for (;;) {
      e = readdir(d);
      if (e == NULL)
	break;
      if (e->d_name[0] != '.') {
	sprintf(path, "%s%c%s", dir, DIRSEP, e->d_name);
	path[PATHMAX-1]='\0';
	f = fopen(path, "rb");
	if (f != NULL) {
	  buf_clear(msg);
	  buf_read(msg, f);
	  fclose(f);
	  unlink(path);
	  mix_decrypt(msg);
	  size++;
	}
      }
    }
  closedir(d);
  buf_free(msg);
  return (size);
}

int mailin_maildir(char *maildir)
/** Read mails from a mail folder in Maildir format
    Reads all files from the Maildir using mailin_maildir_one().
    All mails are removed after this function returns.

    @param maildir	The Maildir to open. mixfile() is called to normalize the path.
    @author	PP
    @return	0
 */
{
  char normalized[PATHMAX];
  char path[PATHMAX];

  mixfile(normalized, maildir);
  sprintf(path, "%s%c%s", normalized, DIRSEP, "new");
  path[PATHMAX-1]='\0';
  mailin_maildir_one(path);
  sprintf(path, "%s%c%s", normalized, DIRSEP, "cur");
  path[PATHMAX-1]='\0';
  mailin_maildir_one(path);
  return (0);
}

int mailin_mbox(char *path)
/** Read mails from a mail folder in mbox format
    Reads all messages from the mbox filder passes as an argument. Mails are
    handed over to mix_decrypt. After all mails have been read the mailbox
    is truncated to zero size i.e. all mails are deleted. The mbox is
    locked using lock() and unlock() during this operation.

    @param maildir	Path to the mbox mail folder.
    @author	PP
    @return	0 on sucess, other on error
 */
{
  char line[LINELEN];
  FILE *f;
  int state, eof;
  BUFFER *msg;
  int err=0;

  msg = buf_new();

  f = mix_openfile(path, "r+");
  if (f != NULL) {
    if (lock(f) != 0) {
      /* Locking failed */
      err = 1;
      goto end;
    }
    /* State machine
     * 1 - Look for the first ^From_ line
     * 2 - add messages as they come
     */
    state = 1;
    eof = 0;
    for(;;) {
      if (fgets(line, sizeof(line), f) == NULL)
	eof = 1;

      switch (state) {
	case 1:
	  /* Initial state - Looking for first appearance of From_ */
	  if (eof)
	    goto end_state;
	  if (strleft(line, "From ")) {
#if 0
	    buf_appends(msg, line);
#endif /* 0 */
	    state = 2;
	    break;
	  };
	  break;
	case 2:
	  /* Within one mail - Adding lines to mail until we encounter another From_ or eof */
	  if (eof || strleft(line, "From ")) {
	    mix_decrypt(msg);
	    buf_clear(msg);
	  }
	  if (eof)
	    goto end_state;
	  if (!strleft(line, "From "))
	    buf_appends(msg, line);
	  break;
	default:
	  assert(0);
	  err=1;
	  goto end_state;
      }
    }
end_state:
#ifndef WIN32
    rewind(f);
    ftruncate(fileno(f), 0);
#else /* end of not WIN32 */
    chsize(fileno(f), 0);
#endif /* else if WIN32 */
    unlock(f);
    fclose(f);
  }
end:
  buf_free(msg);
  return (err);
}

/** Process MAILIN if applicable
    If MAILIN is defined this function calls either mailin_maildir() or
    mailin_mbox() depending on whether the last character of MAILIN
    is DIRSEP.

    @param mailbox	Path to the mbox or Maildir mail folder.
    @author	PP
    @return	0 on sucess, other on error
 */
int mailin(char *mailbox)
{
  if (mailbox != NULL && (strcmp(mailbox, "") != 0))
    if (mailbox[strlen(mailbox)-1] == DIRSEP)
      return mailin_maildir(mailbox);
    else
      return mailin_mbox(mailbox);
  else
    return 0;
};

int pool_add(BUFFER *msg, char *type)
{
  char path[PATHMAX], pathtmp[PATHMAX];
  FILE *f;
  int err = -1;

  f = pool_new(type, pathtmp, path);
  if (f != NULL) {
    err = buf_write(msg, f);
    fclose(f);
  }
  if (err == 0) {
    rename(pathtmp, path);
    errlog(DEBUGINFO, "Added %s file to pool.\n", type);
  }
  return (err);

}

FILE *pool_new(char *type, char *tmpname, char *path)
{
  FILE *f;
  struct stat buf;
  int err;

  assert(strlen(type) == 3);
#ifdef SHORTNAMES
  sprintf(tmpname, "%s%c%02x%02x%02x%02x.tmp", POOLDIR, DIRSEP, rnd_byte(), rnd_byte(),
	  rnd_byte(), rnd_byte());
  strcpy(path, tmpname);
  memcpy(path + strlen(path) - 3, type, 3);
#else /* end of SHORTNAMES */
  sprintf(tmpname, "%s%ct%02x%02x%02x%02x%02x%02x%01x", POOLDIR, DIRSEP, rnd_byte(),
	  rnd_byte(), rnd_byte(), rnd_byte(), rnd_byte(),
	  rnd_byte(), rnd_byte() & 15);
  strcpy(path, tmpname);
  strrchr(path, DIRSEP)[1] = type[0];
#endif /* else if not SHORTNAMES */
  err = stat(tmpname, &buf);
  if (err == 0)
    errlog(WARNING, "Overwriting file %s\n", tmpname);
  f = fopen(tmpname, "wb");
  if (f == NULL)
    errlog(ERRORMSG, "Error creating temporary file %s\n", tmpname);
  return (f);
}

int pool_read(BUFFER *pool)
{
  DIR *d;
  struct dirent *e;
  int size = 0;

  d = opendir(POOLDIR);
  if (d != NULL) {
    for (;;) {
      e = readdir(d);
      if (e == NULL)
	break;
      if (is(e->d_name, "msg")) {
	if (pool != NULL) {
	  buf_appends(pool, e->d_name);
	  buf_appendc(pool, 0);
	}
	size++;
      }
    }
    closedir(d);
  } else
    errlog(WARNING, "Error reading pool dir %s\n", POOLDIR);
  return (size);
}

void pool_dosend(void)
{
  DIR *d;
  struct dirent *e;
  char path[PATHMAX];

  d = opendir(POOLDIR);
  if (d != NULL) {
    for (;;) {
      e = readdir(d);
      if (e == NULL)
	break;
      if (is(e->d_name, "snd")) {
	sendmail_begin();
	mv(e->d_name, "tmp");
	sprintf(path, "%s%c%s", POOLDIR, DIRSEP, e->d_name);
	if (msg_send(path) == 1)
	  mv(e->d_name, "err");
      }
    }
    closedir(d);
  } else
    errlog(WARNING, "Error reading pool dir %s\n", POOLDIR);
  sendmail_end();
}

int process_mailin()
{
  mailin(MAILIN);
  infile_read();
  return(0);
}

int create_dummy_mailout()
{
  while (rnd_number(100) < OUTDUMMYP) {
    errlog(DEBUGINFO, "Generating dummy message with outgoing mail.\n");
    if (mix_encrypt(MSG_NULL, NULL, NULL, 1, NULL) == -1)
      return -1;
  }
  return 0;
}

int pool_send(void)
{
  int size, max, i, r;
  BUFFER *pool;
  long int *ptr;

  create_dummy_mailout();

  latent_read();
  pool = buf_new();
  size = pool_read(pool);
  if (size <= POOLSIZE)
    goto end;

  ptr = malloc(size * sizeof(long int));

  if (ptr == NULL)
    goto end;
  for (i = 0; i < size; i++) {
    ptr[i] = pool->ptr;
    buf_getline(pool, NULL);
  }

  max = size * RATE / 100;	/* send no more than RATE % of the messages */
  if (max < 0)
    max = 1;

  for (i = 0; i < size - POOLSIZE && i < max; i++) {
    do
      r = rnd_number(size);	/* chose a new random message */
    while (is(pool->data + ptr[r], "snd"));
    mv(pool->data + ptr[r], "snd");
  }
  stats_out(size - --i);
  pool_dosend();
  free(ptr);

end:
  buf_free(pool);
  return (size);
}

int msg_send(char *name)
{
  FILE *f;
  int type = -1;
  BUFFER *m, *addr;
  int err = 0;
  char line[LINELEN];
  int userfrom = 0;

  m = buf_new();
  addr = buf_new();
  if ((f = fopen(name, "rb")) == NULL) {
    err = -1;
    goto end;
  }
  fscanf(f, "%d %*d\n", &type);
  if (type == INTERMEDIATE) {
    fgets(line, sizeof(line), f);
    buf_sets(addr, line);
    buf_chop(addr);
    err = buf_read(m, f);
    if (err == -1)
      goto end;
    err = mix_armor(m);
    if (err == -1)
      goto end;
    err = sendmail(m, REMAILERADDR, addr);
    stats_log(3);
  } else if (type == MSG_MAIL || type == MSG_POST) {
    err = buf_read(m, f);
    if (err == -1)
      goto end;
    if (MIDDLEMAN && ! allowmessage(m)) {
      mix2_encrypt(type, m, FORWARDTO, 1, 1, NULL);
      stats_log(6);
    } else {
      err = filtermsg(m);
      if (err == 1)
	userfrom = 1, err = 0;
      if (err != -1) {
	/* message has recipients */
	errlog(DEBUGINFO, "Sending message (%ld bytes)\n", m->length);

	if (type == MSG_MAIL) {
	  err = sendmail(m, userfrom ? NULL : ANONNAME, NULL);
	  stats_log(4);
	} else if (type == MSG_POST) {
	  if (strchr(NEWS, '@') && !strchr(NEWS, ' ')) {
	    errlog(LOG, "Mailing article to %s.\n", NEWS);
	    buf_sets(addr, NEWS);
	    err = sendmail(m, userfrom ? NULL : ANONNAME, addr);
	  } else if (NEWS[0] != '\0') {
	    FILE *f;

	    f = openpipe(NEWS);
	    if (f == NULL)
	      goto end;
	    errlog(LOG, "Posting article.\n");
	    if (!userfrom)
	      fprintf(f, "From: %s\n", ANONNAME);
	    if (ORGANIZATION[0] != '\0')
	      fprintf(f, "Organization: %s\n", ORGANIZATION);
	    buf_write(m, f);
	    closepipe(f);
	  } else
	    errlog(NOTICE, "Rejecting news article.\n");
	  stats_log(5);
	}
      } else
	errlog(ERRORMSG, "Bad message file.\n");
    }
  }
end:
  if (f != NULL)
    fclose(f);
  if (err != 1)			/* problem sending mail */
    unlink(name);
  buf_free(m);
  buf_free(addr);
  return (err);
}

int allowmessage(BUFFER *in)
/* Only called if remailer is middleman. Checks whether all Recipient
 * addresses are in dest.allow. If yes return 1; 0 otherwhise
 */
{
  BUFFER *out, *allow, *allow2, *line, *line2;
  int err=1;
  FILE *f;

  allow = buf_new();
  allow2 = buf_new();
  out = buf_new();
  line = buf_new();
  line2 = buf_new();

  f = mix_openfile(DESTALLOW, "r");
  if (f != NULL) {
    buf_read(allow, f);
    fclose(f);
  }
  f = mix_openfile(DESTALLOW2, "r");
  if (f != NULL) {
    buf_read(allow2, f);
    fclose(f);
    buf_cat(allow, allow2);
  }

  /* Do header lines */
  while (buf_getline(in, line) == 0) {
    for (;;) {
      buf_lookahead(in, line2);
      if (!bufleft(line2, " ") && !bufleft(line2, "\t"))
	break;
      buf_getline(in, line2);
      buf_cat(line, line2);
    }

    if (bufileft(line, "to:") || bufileft(line, "cc:") ||
	bufileft(line, "bcc:") || bufileft(line, "newsgroups:"))
      if (! doallow(line, allow))
      	err = 0;

    if (line->length > 0) {
      if (!buffind(line, ":"))
	 buf_appends(out, "X-Invalid: ");
      buf_cat(out, line);
      buf_nl(out);
    }
  }
  buf_nl(out);

  /* Rest of the message */
  buf_append(out, in->data + in->ptr, in->length - in->ptr);

  buf_move(in, out);
  buf_free(out);
  buf_free(allow);
  buf_free(allow2);
  buf_free(line);
  buf_free(line2);
  return (err);
}

int doallow(BUFFER *line, BUFFER *filter)
/* line is a To, CC or BCC line.
 * problem is: there may be multiple addresses in one header
 * line but we only want to allow if _all_ are allowed
 *
 * So to not send direct if we do not want, we _never_ send
 * direct if there is more than one address: This is
 * assumed to be the case when there is a
 * comma in the header line.
 *
 * this should probably be rewritten somehwhen. therefore: FIXME
 *
 * returns: 1 if allowed
 *          0 if message should be send indirectly
 */
{
  if (strchr( line->data, ',')) return 0;
  return doblock(line, filter, 0);
}

int filtermsg(BUFFER *in)
{
  BUFFER *out, *line, *line2, *mboundary, *block, *filter, *mid;
  FILE *f;
  int from = 0, dest = 0;
  int inbinary = 0, inpgp = 0, l = 80;
  int err = -1;

  line = buf_new();
  line2 = buf_new();
  filter = buf_new();
  mid = buf_new();
  mboundary = buf_new();
  out = buf_new();
  block = NULL;

  if (SIZELIMIT > 0 && in->length > SIZELIMIT * 1024) {
    errlog(NOTICE, "Message rejected: %ld bytes\n", in->length);
    goto end;
  }

  block = readdestblk( );
  if ( !block ) block = buf_new( );

  f = mix_openfile(HDRFILTER, "r");
  if (f != NULL) {
    buf_read(filter, f);
    fclose(f);
  }

  f = mix_openfile(DISCLAIMFILE, "r");
  if (f != NULL) {
    buf_read(out, f);
    fclose(f);
  } else {
    if (strfind(DISCLAIMER, "%s"))
      buf_appendf(out, DISCLAIMER, COMPLAINTS);
    else
      buf_appends(out, DISCLAIMER);
  }

  while (buf_getline(in, line) == 0) {
    for (;;) {
      buf_lookahead(in, line2);
      if (!bufleft(line2, " ") && !bufleft(line2, "\t"))
	break;
      buf_getline(in, line2);
      buf_cat(line, line2);
    }

    if (bufileft(line, "to:") || bufileft(line, "cc:") ||
	bufileft(line, "bcc:") || bufileft(line, "newsgroups:"))
      if (doblock(line, block, 1) == 0)
	dest++;
    if (doblock(line, filter, 1) == -1)
      goto end;
    if (bufileft(line, "from:"))
      from = 1;

    if (bufileft(line, "content-type:") && bufileft(line, "multipart"))
      get_parameter(line, "boundary", mboundary);

    if (line->length > 0) {
      if (!buffind(line, ":"))
	 buf_appends(out, "X-Invalid: ");
       buf_cat(out, line);
      buf_nl(out);
    }
  }

  if (MID[0] != '\0' && tolower(MID[0]) != 'n') {
    char txt[LINELEN];

    digestmem_md5(in->data + in->ptr, in->length - in->ptr, mid);
    id_encode(mid->data, txt);

    if (MID[0] == '@')
      strcatn(txt, MID, sizeof(txt));
    else {
      if (strchr(REMAILERADDR, '@'))
	strcatn(txt, strchr(REMAILERADDR, '@'), sizeof(txt));
      else if (strchr(COMPLAINTS, '@'))
	strcatn(txt, strchr(COMPLAINTS, '@'), sizeof(txt));
    }
    buf_appendf(out, "Message-ID: <%s>\n", txt);
  }
  buf_nl(out);

  if (from) {
    /* prepend Sender line to message header */
    buf_setf(line, "Sender: %s\n", ANONNAME);
    buf_cat(line, out);
    buf_move(out, line);

    f = mix_openfile(FROMDSCLFILE, "r");
    if (f != NULL) {
      buf_read(out, f);
      fclose(f);
    } else
      buf_appends(out, FROMDISCLAIMER);
  }

#if 0
  buf_append(out, in->data + in->ptr, in->length - in->ptr);
#endif /* 0 */
  while (buf_getline(in, line) != -1) {
      if (boundary(line, mboundary)) {
      buf_cat(out, line);
      buf_nl(out);
      while (buf_getline(in, line) == 0) {	/* MIME body part header */
	err = doblock(line, filter, 1);
	if (err == -1)
	  goto end;
	buf_cat(out, line);
	buf_nl(out);
      }
    }
    if (BINFILTER && l > 20 && line->length == l &&
	(bufleft(line, "M") || !buffind(line, " ")))
      inbinary++;
    else
      inbinary = 0, l = line->length;
    if (bufileft(line, begin_pgp) || bufileft(line, begin_key))
      inpgp = 1;
    if (bufileft(line, end_pgp) || bufileft(line, end_key))
      inpgp = 0;
    if (inbinary < 10 || inpgp) {
      buf_cat(out, line);
      buf_nl(out);
    } else if (inbinary == 10) {
      errlog(NOTICE, "Binary message detected.\n");
      if (BINFILTER > 1) {
	err = -1;
	goto end;
      }
      buf_appends(out, BINDISCLAIMER);
      buf_nl(out);
    }
  }

  f = mix_openfile(MSGFOOTERFILE, "r");
  if (f != NULL) {
    buf_read(out, f);
    fclose(f);
  } else
    buf_appends(out, MSGFOOTER);

  /* return 1 for user supplied From line */
  err = from;
  if (dest == 0)
    err = -1;

end:
  buf_move(in, out);
  buf_free(out);
  buf_free(line);
  buf_free(line2);
  if (block) buf_free(block);
  buf_free(filter);
  buf_free(mid);
  buf_free(mboundary);
  return (err);
}

BUFFER *readdestblk( )
{
       char *destblklst = (char *)malloc( strlen( DESTBLOCK )+1 );
       char *destblk = NULL;
       FILE *f;
       BUFFER *addresses;
       BUFFER *temp;
       int err = 1;

       addresses = buf_new( );
       temp = buf_new( );

       strcpy( destblklst, DESTBLOCK );

       while ( (destblk = strtok( destblk ? NULL : destblklst, " " )) )
       {
	       if ( (f = mix_openfile( destblk, "r" )) )
	       {
	               if ( !buf_read( temp, f ) )
	               {
	                       buf_cat( addresses, temp );
	                       err = 0;
	               }
	               fclose( f );
	       }
       }

       free( destblklst );
       buf_free( temp );

       if ( err ) { buf_free( addresses ); return NULL; }
       else return addresses;
}

int doblock(BUFFER *line, BUFFER *filter, int logandreset)
/* logandreset is usually 0
 * it is only set to 1 when called from doallow
 *  which only checks whether messages are allowed to
 *  be sent directly
 */
{
  int block = 0;
  BUFFER *pattern, *result;
  char *t;
#ifdef USE_PCRE
  int errptr, match;
  const char *error;
  pcre *compiled;
  int ovector[21];
  char *newstr;
#endif /* USE_PCRE */

  pattern = buf_new();
  result = buf_new();
  assert(filter != NULL);

  buf_rewind(filter);
  while (buf_getline(filter, pattern) != -1)
    if (pattern->length > 0 && !bufleft(pattern, "#")) {
      if (bufleft(pattern, "/") && (t = strchr(pattern->data + 1, '/'))
	  != NULL) {
#ifdef USE_PCRE
	*t = '\0';
	compiled = pcre_compile(pattern->data + 1, PCRE_CASELESS,
				&error, &errptr
#ifndef USE_PCRE_OLD
				,NULL
#endif /* not USE_PCRE_OLD */
	  );
	if (compiled) {
	  match = pcre_exec(compiled, NULL, line->data,
			    line->length,
#if (PCRE_MAJOR == 2 && PCRE_MINOR >= 06)
			    0,
#endif /* (PCRE_MAJOR == 2 && PCRE_MINOR >= 06) */
#if (PCRE_MAJOR >= 3)
			    0,
#endif /* (PCRE_MAJOR >= 3) */
			    0, ovector, sizeof(ovector) / sizeof(int));
	  free(compiled);

	  if (match < -1) {
	    *t = '/';
	    errlog(ERRORMSG, "Bad regexp %b\n", pattern);
	  }
	  else if (match >= 0) {
	    /* "/pattern/q" kills the entire message */
	    if (logandreset
	        && strlen(pattern->data + 1) + 1 < pattern->length
		&& pattern->data[pattern->length - 1] == 'q') {
	      *t = '/';
	      errlog(NOTICE,
		     "Message rejected: %b matches %b.\n", line, pattern);
	      block = -1;
	      break;
	    }
	    if (strlen(pattern->data + 1) + 1 < pattern->length
		&& pattern->data[pattern->length - 1] == '/') {
	      pattern->data[pattern->length - 1] = '\0';
	      newstr = pattern->data + strlen(pattern->data) + 1;
	      buf_reset(result);
	      buf_append(result, line->data, ovector[0]);
	      while (strchr(newstr, '$')) {
		strchr(newstr, '$')[0] = '\0';
		buf_appends(result, newstr);
		newstr += strlen(newstr) + 1;
		if (*newstr >= '1' && *newstr <= '9')
		  buf_append(result, line->data +
			     ovector[2 * (*newstr - '0')],
			     ovector[2 * (*newstr - '0') + 1] -
			     ovector[2 * (*newstr - '0')]);
		newstr++;
	      }
	      buf_appends(result, newstr);
	      buf_appends(result, line->data + ovector[1]);
	      buf_clear(line);
	      buf_appends(line, result->data);
	    } else {
	      block = 1;
	      *t = '/';
	      if (logandreset)
	        errlog(NOTICE, "Blocked header line: %b matches %b.\n",
		       line, pattern);
	    }
	  }
	} else {
	  *t = '/';
	  errlog(ERRORMSG, "Bad regexp %b\n", pattern);
	}
#else /* end of USE_PCRE */
	errlog(ERRORMSG, "No regexp support! Ignoring %b\n", pattern);
#endif /* else if not USE_PCRE */
      } else if (bufifind(line, pattern->data)) {
	if (logandreset )
	  errlog(NOTICE, "Blocked header line: %b matches %b.\n",
	         line, pattern);
	block = 1;
      }
    }

  if (logandreset && (block == 1))
    buf_reset(line);

  buf_free(pattern);
  buf_free(result);
  return (block);
}

int mix_armor(BUFFER *in)
{
  BUFFER *out, *md;

  md = buf_new();
  out = buf_new();

  if (in->length != 20480)
    return (-1);

  buf_sets(out, "\n::\n");
  buf_appends(out, remailer_type);
  buf_appends(out, VERSION);
  buf_nl(out);
  buf_nl(out);
  buf_appends(out, begin_remailer);
  buf_nl(out);
  buf_appends(out, "20480\n");
  digest_md5(in, md);
  encode(md, 0);
  buf_cat(out, md);
  buf_nl(out);
  encode(in, 40);
  buf_cat(out, in);
  buf_appends(out, end_remailer);
  buf_nl(out);

  buf_move(in, out);
  buf_free(out);
  buf_free(md);
  return (0);
}
