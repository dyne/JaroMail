/* Mixmaster version 3.0  --  (C) 1999 - 2006 Anonymizer Inc. and others.

   Mixmaster may be redistributed and modified under certain conditions.
   This software is distributed on an "AS IS" basis, WITHOUT WARRANTY OF
   ANY KIND, either express or implied. See the file COPYRIGHT for
   details.

   Socket-based mail transport services
   $Id: mail.c 934 2006-06-24 13:40:39Z rabbi $ */


#include "mix3.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(UNIX) && defined(USE_SOCK)
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#endif /* defined(UNIX) && defined(USE_SOCK) */

#include <fcntl.h>
#include <time.h>
#include <sys/stat.h>
#include <errno.h>


int sendinfofile(char *name, char *logname, BUFFER *address, BUFFER *header)
{
  FILE *f = NULL, *log = NULL;
  BUFFER *msg, *addr;
  char line[LINELEN];
  int ret = -1;

  if (bufeq(address, ANONNAME))
    return (0);			/* don't reply to our own anon messages */
  f = mix_openfile(name, "r");
  if (f == NULL)
    return (-1);

  addr = buf_new();
  rfc822_addr(address, addr);
  if (addr->length == 0)
    buf_set(addr, address), buf_nl(addr);

  if (logname != NULL) {
    if ((log = mix_openfile(logname, "r+")) != NULL) {
      /* log recipients to prevent mail loop */
      while (fgets(line, sizeof(line), log) != NULL)
	if (strieq(line, addr->data))
	  goto end;
    } else if ((log = mix_openfile(logname, "w")) == NULL) {
      errlog(ERRORMSG, "Can't create %s.\n", logname);
      ret = -1;
      goto end;
    }
    fprintf(log, "%s", addr->data);
  }
  msg = buf_new();
  if (header)
    buf_cat(msg, header), buf_nl(msg);
  while (fgets(line, sizeof(line), f) != NULL) {
    if (streq(line, "DESTINATION-BLOCK\n"))
      buf_appendf(msg, "destination-block %b", addr);
    else
      buf_appends(msg, line);
  }
  ret = sendmail(msg, REMAILERNAME, address);
  buf_free(msg);
end:
  if (f)
    fclose(f);
  if (log)
    fclose(log);
  buf_free(addr);
  return (ret);
}

int smtpsend(BUFFER *head, BUFFER *message, char *from);


int sendmail_loop(BUFFER *message, char *from, BUFFER *address)
{
  BUFFER *msg;
  int err;

  msg = buf_new();
  buf_appendf(msg, "X-Loop: %s\n", REMAILERADDR);
  buf_cat(msg, message);
  err = sendmail(msg, from, address);
  buf_free(msg);

  return(err);
}

/* Returns true if more than one of the recipients in the
 * rcpt buffer is a remailer
 */
int has_more_than_one_remailer(BUFFER *rcpts)
{
  BUFFER *newlinelist;
  BUFFER *line;
  int remailers = 0;
  REMAILER type1[MAXREM];
  REMAILER type2[MAXREM];
  int num1;
  int num2;
  int i;

  newlinelist = buf_new();
  line = buf_new();

  num1 = t1_rlist(type1, NULL);
  num2 = mix2_rlist(type2, NULL);

  rfc822_addr(rcpts, newlinelist);

  while (buf_getline(newlinelist, line) != -1) {
    int found = 0;
    printf("%s\n", line->data);

    for (i = 0; i < num2; i++)
      if (strcmp(type2[i].addr, line->data) == 0) {
	found = 1;
	break;
      }
    if (!found)
      for (i = 0; i < num1; i++)
	if (strcmp(type1[i].addr, line->data) == 0) {
	  found = 1;
	  break;
	}
    if (found)
      remailers++;
  }
  printf("found %d\n", remailers);

  buf_free(newlinelist);
  buf_free(line);

  return(remailers > 1);
}

int sendmail(BUFFER *message, char *from, BUFFER *address)
{
  /* returns: 0: ok  1: problem, try again  -1: failed */

  BUFFER *head, *block, *rcpt;
  FILE *f;
  int err = -1;
  int rcpt_cnt;

  head = buf_new();
  rcpt = buf_new();

  block = readdestblk( );
  if ( !block ) block = buf_new( );

  if (address != NULL &&
      (address->length == 0 || doblock(address, block, 1) == -1))
    goto end;

  if (from != NULL) {
    buf_setf(head, "From: %s", from);
    hdr_encode(head, 255);
    buf_nl(head);
  }
  if (address != NULL)
    buf_appendf(head, "To: %b\n", address);

  if (PRECEDENCE[0])
	buf_appendf(head, "Precedence: %s\n", PRECEDENCE);

  buf_rewind(message);

  /* search recipient addresses */
  if (address == NULL) {
    BUFFER *field, *content;
    field = buf_new();
    content = buf_new();

    rcpt_cnt = 0;
    while (buf_getheader(message, field, content) == 0) {
      if (bufieq(field, "to") || bufieq(field, "cc") || bufieq(field, "bcc")) {
	int thislinercpts = 1;
	char *tmp = content->data;
	while ((tmp = strchr(tmp+1, ',')))
	  thislinercpts ++;
	rcpt_cnt += thislinercpts;

	if ( rcpt->data[0] )
	  buf_appends(rcpt, ", ");
	buf_cat(rcpt, content);
      }
    }
    buf_free(field);
    buf_free(content);
  } else if (address->data[0]) {
    char *tmp = address->data;
    rcpt_cnt = 1;
    while ((tmp = strchr(tmp+1, ',')))
      rcpt_cnt ++;

    buf_set(rcpt, address);
  } else
    rcpt_cnt = 0;

  buf_rewind(message);

  if ( ! rcpt_cnt ) {
    errlog(NOTICE, "No recipients found.\n");
    err = 0;
  } else if ( rcpt_cnt > MAXRECIPIENTS ) { 
    errlog(NOTICE, "Too many recipients.  Dropping message.\n");
    err = 0;
  } else if ( rcpt_cnt > 1 && has_more_than_one_remailer(rcpt) ) {
    errlog(NOTICE, "Message is destined to more than one remailer.  Dropping.\n");
    err = 0;
  } else if ( REMAIL && strcmp(REMAILERADDR, rcpt->data) == 0) {
    buf_cat(head, message);
    errlog(LOG, "Message loops back to us; storing in pool rather than sending it.\n");
    err = pool_add(head, "inf");
  } else if (SMTPRELAY[0])
    err = smtpsend(head, message, from);
  else if (strieq(SENDMAIL, "outfile")) {
    char path[PATHMAX];
    FILE *f = NULL;
#ifdef SHORTNAMES
    int i;
    for (i = 0; i < 10000; i++) {
      snprintf(path, PATHMAX, "%s%cout%i.txt", POOLDIR, DIRSEP, i);
      f = fopen(path, "r");
      if (f)
	fclose(f);
      else
	break;
    }
    f = fopen(path, "w");
#else /* end of SHORTNAMES */
    static unsigned long namecounter = 0;
    struct stat statbuf;
    int count;
    char hostname[64];

    hostname[0] = '\0';
    gethostname(hostname, 63);
    hostname[63] = '\0';

    /* Step 2:  Stat the file.  Wait for ENOENT as a response. */
    for (count = 0;; count++) {
      snprintf(path, PATHMAX, "%s%cout.%lu.%u_%lu.%s,S=%lu.txt",
	POOLDIR, DIRSEP, time(NULL), getpid(), namecounter++, hostname, head->length + message->length);

      if (stat(path, &statbuf) == 0)
	errno = EEXIST;
      else if (errno == ENOENT) { /* create the file (at least try) */
	f = fopen(path, "w");
	if (f != NULL)
	  break; /* we managed to open the file */
      }
      if (count > 5)
	break; /* Too many retries - give up */
      sleep(2); /* sleep and retry */
    }
#endif /* else not SHORTNAMES */
    if (f != NULL) {
      err = buf_write(head, f);
      err = buf_write(message, f);
      fclose(f);
    } else
      errlog(ERRORMSG, "Can't create %s!\n", path);
  } else {
    if (SENDANONMAIL[0] != '\0' && (from == NULL || streq(from, ANONNAME)))
      f = openpipe(SENDANONMAIL);
    else
      f = openpipe(SENDMAIL);
    if (f != NULL) {
      err = buf_write(head, f);
      err = buf_write(message, f);
      closepipe(f);
    }
  }
  if (err != 0) {
    errlog(ERRORMSG, "Unable to execute sendmail. Check path!\n");
    err = 1;			/* error while sending, retry later */
  }

end:
  buf_free(block);
  buf_free(head);
  buf_free(rcpt);
  return (err);
}

/* socket communication **********************************************/

#ifdef USE_SOCK
#ifdef WIN32
WSADATA w;

int sock_init()
{
  if (WSAStartup(MAKEWORD(2, 0), &w) != 0) {
    errlog(ERRORMSG, "Unable to initialize WINSOCK.\n");
    return 0;
  }
  return 1;
}

void sock_exit(void)
{
  WSACleanup();
}
#endif /* WIN32 */

SOCKET opensocket(char *hostname, int port)
{
  struct hostent *hp;
  struct sockaddr_in server;
  SOCKET s;

  if ((hp = gethostbyname(hostname)) == NULL)
    return (INVALID_SOCKET);

  memset((char *) &server, 0, sizeof(server));
  server.sin_family = AF_INET;
  server.sin_addr.s_addr = *(unsigned long *) hp->h_addr;
  server.sin_port = htons((unsigned short) port);

  s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (s != INVALID_SOCKET)
    if (connect(s, (struct sockaddr *) &server, sizeof(server)) < 0) {
      closesocket(s);
      return (INVALID_SOCKET);
    }
  return (s);
}

#ifndef WIN32
int closesocket(SOCKET s)
{
  return (close(s));
}
#endif /* ifndef WIN32 */

int sock_getline(SOCKET s, BUFFER *line)
{
  char c;
  int ok;

  buf_clear(line);
  while ((ok = recv(s, &c, 1, 0)) > 0) {
    if (c == '\n')
      break;
    if (c != '\r')
      buf_appendc(line, c);
  }
  if (ok <= 0)
    return (-1);
  if (line->length == 0)
    return (1);
  return (0);
}

int sock_cat(SOCKET s, BUFFER *b)
{
  int p = 0, n;

  do {
    n = send(s, b->data, b->length, 0);
    if (n < 0)
      return (-1);
    p += n;
  } while (p < b->length);
  return (0);
}
#else /* end of USE_SOCK */
SOCKET opensocket(char *hostname, int port)
{
  return (INVALID_SOCKET);
}

int closesocket(SOCKET s)
{
  return (INVALID_SOCKET);
}

int sock_getline(SOCKET s, BUFFER *line)
{
  return (-1);
}

int sock_cat(SOCKET s, BUFFER *b)
{
  return (-1);
}
#endif /* else not USE_SOCK */

/* send messages by SMTP ************************************************/

static int sock_getsmtp(SOCKET s, BUFFER *response)
{
  int ret;
  BUFFER *line;
  line = buf_new();

  buf_clear(response);
  do {
    ret = sock_getline(s, line);
    buf_cat(response, line);
  } while (line->length >= 4 && line->data[3] == '-');
  buf_free(line);
  return (ret);
}

SOCKET smtp_open(void)
{
  int s = INVALID_SOCKET;
  int esmtp = 0;
  BUFFER *line;

#ifdef USE_SOCK
  if (SMTPRELAY[0] != '\0')
    s = opensocket(SMTPRELAY, 25);
  if (s != INVALID_SOCKET) {
    line = buf_new();
    sock_getsmtp(s, line);
    if (bufifind(line, "ESMTP"))
      esmtp = 1;
    if (line->data[0] != '2') {
      errlog(ERRORMSG, "SMTP relay not ready. %b\n", line);
      closesocket(s);
      s = INVALID_SOCKET;
    } else {
      errlog(DEBUGINFO, "Opening SMTP connection.\n");
      if (esmtp)
	buf_sets(line, "EHLO ");
      else
	buf_sets(line, "HELO ");
      if (HELONAME[0])
	buf_appends(line, HELONAME);
      else if (strchr(ENVFROM, '@'))
	buf_appends(line, strchr(ENVFROM, '@') + 1);
      else {
	struct sockaddr_in sa;
	int len = sizeof(sa);
	struct hostent *hp;

	if (getsockname(s, (struct sockaddr *) &sa, &len) == 0 &&
	    (hp = gethostbyaddr((char *) &sa.sin_addr, sizeof(sa.sin_addr),
				AF_INET)) != NULL)
	  buf_appends(line, (char *) hp->h_name);
	else if (strchr(REMAILERADDR, '@'))
	  buf_appends(line, strchr(REMAILERADDR, '@') + 1);
	else
	  buf_appends(line, SHORTNAME);
      }
      buf_chop(line);
      buf_appends(line, "\r\n");
      sock_cat(s, line);
      sock_getsmtp(s, line);
      if (line->data[0] != '2') {
	errlog(ERRORMSG, "SMTP relay refuses HELO: %b\n", line);
	closesocket(s);
	s = INVALID_SOCKET;
      } else if (SMTPUSERNAME[0] && esmtp && bufifind(line, "AUTH") && bufifind(line, "LOGIN")) {
	buf_sets(line, "AUTH LOGIN\r\n");
	sock_cat(s, line);
	sock_getsmtp(s, line);
	if (!bufleft(line, "334")) {
	  errlog(ERRORMSG, "SMTP AUTH fails: %b\n", line);
	  goto err;
	}
	buf_sets(line, SMTPUSERNAME);
	encode(line, 0);
	buf_appends(line, "\r\n");
	sock_cat(s, line);
	sock_getsmtp(s, line);
	if (!bufleft(line, "334")) {
	  errlog(ERRORMSG, "SMTP username rejected: %b\n", line);
	  goto err;
	}
	buf_sets(line, SMTPPASSWORD);
	encode(line, 0);
	buf_appends(line, "\r\n");
	sock_cat(s, line);
	sock_getsmtp(s, line);
	if (!bufleft(line, "235"))
	  errlog(ERRORMSG, "SMTP authentication failed: %b\n", line);
      }
    }
err:
    buf_free(line);
  }
#endif /* USE_SOCK */
  return (s);
}

int smtp_close(SOCKET s)
{
  BUFFER *line;
  int ret = -1;

#ifdef USE_SOCK
  line = buf_new();
  buf_sets(line, "QUIT\r\n");
  sock_cat(s, line);
  if (sock_getsmtp(s, line) == 0 && line->data[0] == '2') {
    errlog(DEBUGINFO, "Closing SMTP connection.\n");
    ret = 0;
  } else
    errlog(WARNING, "SMTP quit failed: %b\n", line);
  closesocket(s);
  buf_free(line);
#endif /* USE_SOCK */
  return (ret);
}

int smtp_send(SOCKET relay, BUFFER *head, BUFFER *message, char *from)
{
  BUFFER *rcpt, *line, *field, *content;
  int ret = -1;

#ifdef USE_SOCK
  line = buf_new();
  field = buf_new();
  content = buf_new();
  rcpt = buf_new();

  while (buf_getheader(head, field, content) == 0)
    if (bufieq(field, "to"))
#ifdef BROKEN_MTA
      if (!bufifind(rcpt, content->data))
      /* Do not add the same recipient twice.
	 Needed for brain-dead MTAs.      */
#endif /* BROKEN_MTA */
	rfc822_addr(content, rcpt);
  buf_rewind(head);

  while (buf_isheader(message) && buf_getheader(message, field, content) == 0) {
    if (bufieq(field, "to") || bufieq(field, "cc") || bufieq(field, "bcc")) {
#ifdef BROKEN_MTA
      if (!bufifind(rcpt, content->data))
      /* Do not add the same recipient twice.
	 Needed for brain-dead MTAs.      */
#endif /* BROKEN_MTA */
	rfc822_addr(content, rcpt);
    }
    if (!bufieq(field, "bcc"))
      buf_appendheader(head, field, content);
  }
  buf_nl(head);

  buf_clear(content);
  if (from) {
    buf_sets(line, from);
    rfc822_addr(line, content);
    buf_chop(content);
  }
  if (bufieq(content, REMAILERADDR) || bufieq(content, ANONADDR))
    buf_clear(content);
  if (content->length == 0)
    buf_sets(content, ENVFROM[0] ? ENVFROM : ANONADDR);

  buf_setf(line, "MAIL FROM:<%b>\r\n", content);
  sock_cat(relay, line);
  sock_getsmtp(relay, line);
  if (!line->data[0] == '2') {
    errlog(ERRORMSG, "SMTP relay does not accept mail: %b\n", line);
    goto end;
  }
  while (buf_getline(rcpt, content) == 0) {
    buf_setf(line, "RCPT TO:<%b>\r\n", content);
    sock_cat(relay, line);
    sock_getsmtp(relay, line);
    if (bufleft(line, "421")) {
      errlog(ERRORMSG, "SMTP relay error: %b\n", line);
      goto end;
    }
    if (bufleft(line, "530")) {
      errlog(ERRORMSG, "SMTP authentication required: %b\n", line);
      goto end;
    }
  }

  buf_sets(line, "DATA\r\n");
  sock_cat(relay, line);
  sock_getsmtp(relay, line);
  if (!bufleft(line, "354")) {
    errlog(WARNING, "SMTP relay does not accept message: %b\n", line);
    goto end;
  }
  while (buf_getline(head, line) >= 0) {
    buf_appends(line, "\r\n");
    if (bufleft(line, ".")) {
      buf_setf(content, ".%b", line);
      buf_move(line, content);
    }
    sock_cat(relay, line);
  }
  while (buf_getline(message, line) >= 0) {
    buf_appends(line, "\r\n");
    if (bufleft(line, ".")) {
      buf_setf(content, ".%b", line);
      buf_move(line, content);
    }
    sock_cat(relay, line);
  }
  buf_sets(line, ".\r\n");
  sock_cat(relay, line);
  sock_getsmtp(relay, line);
  if (bufleft(line, "2"))
    ret = 0;
  else
    errlog(WARNING, "SMTP relay will not send message: %b\n", line);
end:
  buf_free(line);
  buf_free(field);
  buf_free(content);
  buf_free(rcpt);
#endif /* USE_SOCK */
  return (ret);
}

static SOCKET sendmail_state = INVALID_SOCKET;

void sendmail_begin(void)
{
  /* begin mail sending session */
  if (sendmail_state == INVALID_SOCKET)
    sendmail_state = smtp_open();
}
void sendmail_end(void)
{
  /* end mail sending session */
  if (sendmail_state != INVALID_SOCKET) {
    smtp_close(sendmail_state);
    sendmail_state = INVALID_SOCKET;
  }
}

int smtpsend(BUFFER *head, BUFFER *message, char *from)
{
  SOCKET s;
  int ret = -1;

  if (sendmail_state != INVALID_SOCKET)
    ret = smtp_send(sendmail_state, head, message, from);
  else {
    s = smtp_open();
    if (s != INVALID_SOCKET) {
      ret = smtp_send(s, head, message, from);
      smtp_close(s);
    }
  }
  return (ret);
}

/* retrieve mail with POP3 **********************************************/
#ifdef USE_SOCK
int pop3_close(SOCKET s);

#define POP3_ANY 0
#define POP3_APOP 1
#define POP3_PASS 2

SOCKET pop3_open(char *user, char *host, char *pass, int auth)
{
  SOCKET server = INVALID_SOCKET;
  BUFFER *line;
  int authenticated = 0;
  char md[33];
  int c;

  line = buf_new();
  server = opensocket(host, 110);
  if (server == INVALID_SOCKET)
    errlog(NOTICE, "Can't connect to POP3 server %s.\n", host);
  else {
    sock_getline(server, line);
    if (!bufleft(line, "+")) {
      errlog(WARNING, "No POP3 service at %s.\n", host);
      closesocket(server);
      server = INVALID_SOCKET;
    }
  }
  if (server != INVALID_SOCKET) {
    errlog(DEBUGINFO, "Opening POP3 connection to %s.\n", host);
    do
      c = buf_getc(line);
    while (c != '<' && c != -1);
    while (c != '>' && c != -1) {
      buf_appendc(line, c);
      c = buf_getc(line);
    }
    if (c == '>' && (auth == POP3_ANY || auth == POP3_APOP)) {
      buf_appendc(line, c);
      buf_appends(line, pass);
      digest_md5(line, line);
      id_encode(line->data, md);
      buf_setf(line, "APOP %s %s\r\n", user, md);
      sock_cat(server, line);
      sock_getline(server, line);
      if (bufleft(line, "+"))
	authenticated = 1;
      else {
	  errlog(auth == POP3_APOP ? ERRORMSG : NOTICE,
		 "POP3 APOP auth at %s failed: %b\n", host, line);
	buf_sets(line, "QUIT\r\n");
	sock_cat(server, line);
	closesocket(server);
	server = pop3_open(user, host, pass, POP3_PASS);
	goto end;
      }
    }
    if (!authenticated) {
      buf_setf(line, "USER %s\r\n", user);
      sock_cat(server, line);
      sock_getline(server, line);
      if (!bufleft(line, "+"))
	errlog(ERRORMSG, "POP3 USER command at %s failed: %b\n", host, line);
      else {
	buf_setf(line, "PASS %s\r\n", pass);
	sock_cat(server, line);
	sock_getline(server, line);
	if (bufleft(line, "+"))
	  authenticated = 1;
	else
	  errlog(ERRORMSG, "POP3 auth at %s failed: %b\n", host, line);
      }
    }
    if (!authenticated) {
      pop3_close(server);
      closesocket(server);
      server = INVALID_SOCKET;
    }
  }
 end:
  buf_free(line);
  return (server);
}

int pop3_close(SOCKET s)
{
  BUFFER *line;
  int ret = -1;

  line = buf_new();
  buf_sets(line, "QUIT\r\n");
  sock_cat(s, line);
  sock_getline(s, line);
  if (bufleft(line, "+")) {
    ret = 0;
    errlog(DEBUGINFO, "Closing POP3 connection.\n");
  } else
    errlog(ERRORMSG, "POP3 QUIT failed:\n", line->data);
  buf_free(line);
  return (ret);
}

int pop3_stat(SOCKET s)
{
  BUFFER *line;
  int val = -1;

  line = buf_new();
  buf_sets(line, "STAT\r\n");
  sock_cat(s, line);
  sock_getline(s, line);
  if (bufleft(line, "+"))
    sscanf(line->data, "+%*s %d", &val);
  buf_free(line);
  return (val);
}

int pop3_list(SOCKET s, int n)
{
  BUFFER *line;
  int val = -1;

  line = buf_new();
  buf_setf(line, "LIST %d\r\n", n);
  sock_cat(s, line);
  sock_getline(s, line);
  if (bufleft(line, "+"))
    sscanf(line->data, "+%*s %d", &val);
  buf_free(line);
  return (val);
}

int pop3_dele(SOCKET s, int n)
{
  BUFFER *line;
  int ret = 0;

  line = buf_new();
  buf_setf(line, "DELE %d\r\n", n);
  sock_cat(s, line);
  sock_getline(s, line);
  if (!bufleft(line, "+"))
    ret = -1;
  buf_free(line);
  return (ret);
}

int pop3_retr(SOCKET s, int n, BUFFER *msg)
{
  BUFFER *line;
  int ret = -1;

  line = buf_new();
  buf_clear(msg);
  buf_setf(line, "RETR %d\r\n", n);
  sock_cat(s, line);
  sock_getline(s, line);
  if (bufleft(line, "+")) {
    for (;;) {
      if (sock_getline(s, line) == -1)
	break;
      if (bufeq(line, ".")) {
	ret = 0;
	break;
      } else if (bufleft(line, ".")) {
	buf_append(msg, line->data + 1, line->length - 1);
      } else
	buf_cat(msg, line);
      buf_nl(msg);
    }
  }
  buf_free(line);
  return (ret);
}

void pop3get(void)
{
  FILE *f;
  char cfg[LINELEN], user[LINELEN], host[LINELEN], pass[LINELEN], auth[5];
  SOCKET server;
  BUFFER *line, *msg;
  int i = 0, num = 0;

  line = buf_new();
  msg = buf_new();
  f = mix_openfile(POP3CONF, "r");
  if (f != NULL)
    while (fgets(cfg, sizeof(cfg), f) != NULL) {
      if (cfg[0] == '#')
	continue;
      if (strchr(cfg, '@'))
	strchr(cfg, '@')[0] = ' ';
      if (sscanf(cfg, "%127s %127s %127s %4s", user, host, pass, auth) < 3)
	continue;
      i = POP3_ANY;
      if (strileft(auth, "apop"))
	i = POP3_APOP;
      if (strileft(auth, "pass"))
	i = POP3_PASS;
      server = pop3_open(user, host, pass, i);
      if (server != INVALID_SOCKET) {
	num = pop3_stat(server);
	if (num < 0)
	  errlog(WARNING, "POP3 protocol error at %s.\n", host);
	else if (num == 0)
	  errlog(DEBUGINFO, "No mail at %s.\n", host);
	else
	  for (i = 1; i <= num; i++) {
	    if (POP3SIZELIMIT > 0 &&
		pop3_list(server, i) > POP3SIZELIMIT * 1024) {
	      errlog(WARNING, "Over size message on %s.", host);
	      if (POP3DEL == 1)
		pop3_dele(server, i);
	    } else {
	      if (pop3_retr(server, i, msg) == 0 &&
		  pool_add(msg, "inf") == 0)
		pop3_dele(server, i);
	      else {
		errlog(WARNING, "POP3 error while getting mail from %s.",
		       host);
		closesocket(server);
		goto end;
	      }
	    }
	  }
	pop3_close(server);
	closesocket(server);
      }
    }
 end:
  if (f != NULL)
    fclose(f);
  buf_free(line);
  buf_free(msg);
}
#endif /* USE_SOCK */
