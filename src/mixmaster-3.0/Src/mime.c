/* Mixmaster version 3.0  --  (C) 1999 - 2006 Anonymizer Inc. and others.

   Mixmaster may be redistributed and modified under certain conditions.
   This software is distributed on an "AS IS" basis, WITHOUT WARRANTY OF
   ANY KIND, either express or implied. See the file COPYRIGHT for
   details.

   MIME functions
   $Id: mime.c 934 2006-06-24 13:40:39Z rabbi $ */


#include "mix3.h"
#include <ctype.h>
#include <string.h>

#define hex(i) (isdigit(i) ? (i) - '0' : tolower(i) - 'a' + 10)

#define hexdigit(i) ((byte)(i < 10 ? i + '0' : i - 10 + 'A'))

static void encode_word(BUFFER *in)
{
  BUFFER *out;
  int i;

  out = buf_new();
  for (i = 0; i < in->length; i++)
    if (in->data[i] < 32 || in->data[i] >= 127 || in->data[i] == '='
	|| in->data[i] == '?' || in->data[i] == '_') {
      buf_appendc(out, '=');
      buf_appendc(out, hexdigit(in->data[i] / 16));
      buf_appendc(out, hexdigit(in->data[i] % 16));
    } else if (in->data[i] == ' ')
      buf_appendc(out, '_');
    else
      buf_appendc(out, in->data[i]);
  buf_move(in, out);
  buf_free(out);
}

void body_encode(BUFFER *in, int transport, BUFFER *hdr)
{
  BUFFER *out;
  int c, l=0, encoding = 0;
  out = buf_new();

  buf_clear(hdr);

  l = in->ptr;
  while ((c = buf_getc(in)) != -1 && encoding != 2) {
    if (c >= 160)
      encoding = 1;
    else if (c == ' ') {
      if (buf_getc(in) == '\n')
	encoding = 1;
      buf_ungetc(in);
    } else if ((c < 32 && c != ' ' && c != '\n' && c != '\t') ||
	       (c >= 127 && c < 160)) {
      encoding = 2;
    }
  }
  in->ptr = l;

  if (encoding == 2) {
    buf_sets(hdr, "Content-Transfer-Encoding: base64\n");
    encode(in, 76);
  } else {

#if 0
    if (encoding == 0)
      buf_sets(hdr, "Content-Transfer-Encoding: 7bit\n");
#endif
    if (encoding != 0 && transport == MIME_8BIT)
      buf_sets(hdr, "Content-Transfer-Encoding: 8bit\n");
    if (encoding == 0 || transport == MIME_8BIT) {
      buf_rest(out, in); /* transparent */
      buf_move(in, out);
    } else {
      buf_sets(hdr, "Content-Transfer-Encoding: quoted-printable\n");
      l = 0;
      while ((c = buf_getc(in)) != -1) {
	if (c == '\n') {
	  buf_nl(out);
	  l = 0;
	}
	else if (c < 32 || c >= 127 || c == '=') {
	  if (l > 73) {
	    buf_appends(out, "=\n");
	    l = 0;
	  }
	  buf_appendc(out, '=');
	  buf_appendc(out, hexdigit(c / 16));
	  buf_appendc(out, hexdigit(c % 16));
	  l += 3;
	} else if (c == ' ') {
	  if (buf_getc(in) == '\n') {
	    buf_appendc(out, '=');
	    buf_appendc(out, hexdigit(c / 16));
	    buf_appendc(out, hexdigit(c % 16));
	    buf_nl(out);
	    l = 0;
	  } else {
	    buf_appendc(out, c);
	    buf_ungetc(in);
	    l++;
	  }
	} else {
	  buf_appendc(out, c);
	  l++;
	}
      }
      buf_move(in, out);
    }
  }
  buf_free(out);
}

int mail_encode(BUFFER *in, int encoding)
{
  BUFFER *out, *line, *tmp;

  out = buf_new();
  line = buf_new();
  tmp = buf_new();

  while (buf_getline(in, line) == 0) {
    hdr_encode(line, 255);
    buf_cat(out, line);
    buf_nl(out);
  }
  if (in->ptr < in->length) {
    /* no newline if only encoding header lines */
    if (encoding == 0) {
      buf_nl(out);
      buf_rest(out, in);
    }
    else {
      body_encode(in, encoding, line);
      buf_cat(out, line);
      buf_nl(out);
      buf_cat(out, in);
    }
  }
  buf_move(in, out);
  buf_free(line);
  buf_free(tmp);
  buf_free(out);
  return (0);
}

int hdr_encode(BUFFER *in, int n)
{
  int i;
  int encodeword = 0, encode = 0;
  BUFFER *out, *word, *space;

  out = buf_new();
  word = buf_new();
  space = buf_new();
  for (i = 0; i <= in->length; i++) {
    if (isspace(in->data[i]) || in->data[i] == '\0') {
      if (word->length) {
	if (encodeword) {
	  if (encode == 0) {
	    buf_cat(out, space);
	    buf_clear(space);
	    buf_appends(out, "=?");
	    buf_appends(out, MIMECHARSET);
	    buf_appends(out, "?Q?");
	    encode = 1;
	  } else {
	    buf_cat(space, word);
	    buf_move(word, space);
	  }
	  encode_word(word);
	}
	if (encode && !encodeword) {
	  encode = 0;
	  buf_appends(out, "?=");
	}
	buf_cat(out, space);
	buf_cat(out, word);
	encodeword = 0;
	buf_clear(space);
	buf_clear(word);
      }
      buf_appendc(space, in->data[i]);
    } else {
      if (in->data[i] < 32 || in->data[i] >= 127)
	encodeword = 1;
      buf_appendc(word, in->data[i]);
    }
  }
  if (encode)
    buf_appends(out, "?=");

  buf_move(in, out);
  while (n > 0 && in->length - in->ptr > n) {
    for (i = 1; i < in->length - in->ptr; i++)
      if (isspace(in->data[in->length - i]))
	break;
    buf_get(in, out, in->length - i);
    buf_appends(out, "\n\t");
  }
  buf_rest(out, in);
  buf_move(in, out);
  buf_free(out);
  buf_free(space);
  buf_free(word);
  return (0);
}

void addprintable(BUFFER *out, int c)
{
  if (c == '\n')
    buf_appendc(out, (char) c);
  else if (c == '\t')
    buf_appends(out, "        ");
  else if (c == '\014')
    buf_appends(out, "^L");
  else if (c == '\r') ;
  else if (c <= 31 || (c >= 128 && c <= 128 + 31))
    buf_appendc(out, '?');
  else
    buf_appendc(out, (char) c);
}

void addprintablebuf(BUFFER *out, BUFFER *in)
{
  int c;

  while ((c = buf_getc(in)) != -1)
    addprintable(out, c);
}

int decode_line(BUFFER *line)
{
  BUFFER *out;
  unsigned int i;
  int c, softbreak = 0;

  out = buf_new();
  for (i = 0; line->data[i] != '\0'; i++) {
    if (line->data[i] == '=') {
      if (isxdigit(line->data[i + 1]) && isxdigit(line->data[i + 2])) {
	c = hex(line->data[i + 1]) * 16 + hex(line->data[i + 2]);
	i += 2;
	addprintable(out, c);
      } else if (line->data[i + 1] == '\0') {
	softbreak = 1;
	break;
      }
    } else
      addprintable(out, line->data[i]);
  }

  buf_move(line, out);
  buf_free(out);
  return (softbreak);
}

int decode_header(BUFFER *in)
{
  int encoded = 0;
  int c;
  int err = 0;
  int last = 0;
  BUFFER *out;

  out = buf_new();
  for (in->ptr = 0; in->data[in->ptr] != '\0'; in->ptr++) {
    if (encoded == 'q' && in->data[in->ptr] == '=' &&
	isxdigit(in->data[in->ptr + 1])
	&& isxdigit(in->data[in->ptr + 2])) {
      c = hex(in->data[in->ptr + 1]) * 16 + hex(in->data[in->ptr + 2]);
      in->ptr += 2;
      addprintable(out, c);
    } else if (encoded == 'q' && in->data[in->ptr] == '_')
      buf_appendc(out, ' ');
    else if (in->data[in->ptr] == '=' && in->data[in->ptr + 1] == '?' &&
	     in->data[in->ptr + 2] != '\0') {
      if (last > 0 && out->length > last) {
	out->data[last] = '\0';
	out->length = last;
      }
      in->ptr++;
      while (in->data[++in->ptr] != '?')
	if (in->data[in->ptr] == 0) {
	  err = -1;
	  goto end;
	}
      if (in->data[in->ptr + 1] != '\0' && in->data[in->ptr + 2] == '?') {
	encoded = tolower(in->data[in->ptr + 1]);
	in->ptr += 2;
	if (encoded == 'b') {
	  BUFFER *tmp;

	  tmp = buf_new();
	  decode(in, tmp);
	  addprintablebuf(out, tmp);
	  last = out->length;
	  buf_free(tmp);
	} else if (encoded != 'q')
	  err = 1;
      } else {
	err = -1;
	goto end;
      }
    } else if (encoded && in->data[in->ptr] == '?' &&
	       in->data[in->ptr + 1] == '=') {
      in->ptr++;
      last = out->length;
      encoded = 0;
    } else {
      addprintable(out, in->data[in->ptr]);
      if (!encoded || !isspace(in->data[in->ptr]))
	last = out->length;
    }
  }
end:
  if (err == -1)
    buf_set(out, in);

  buf_move(in, out);
  buf_free(out);
  return (err);
}

#define delimclose 2

int boundary(BUFFER *line, BUFFER *boundary)
{
  int c;

  if (boundary->length == 0 || !bufleft(line, "--") ||
      !strleft(line->data + 2, boundary->data))
    return (0);
  line->ptr = boundary->length + 2;
  for (;;) {
    c = buf_getc(line);
    if (c == -1)
      return (1);
    if (c == '-' && buf_getc(line) == '-')
      return (delimclose);
    if (!isspace(c))
      return (0);
  }
}

#define pgpenc 1
#define pgpsig 2

/*
 * decodes non-multipart quoted printable message
 */
int qp_decode_message(BUFFER *msg)
{
  BUFFER *out, *line, *field, *content;
  out     = buf_new();
  line    = buf_new();
  field   = buf_new();
  content = buf_new();

  buf_rewind(msg);

  /* copy over headers without decoding */
  while (buf_getheader(msg, field, content) == 0) {
    if (bufieq(field, "content-transfer-encoding")
	&& bufieq(content, "quoted-printable")) {
      continue;                 /* no longer quoted-printable */
    } else {
      buf_appendheader(out, field, content);
    }
  }

  buf_nl(out);

  /* copy over body, quoted-printable decoding as we go */
  while (buf_getline(msg, line) != -1) {
    int softbreak;
    softbreak = decode_line(line);
    buf_cat(out, line);
    if (!softbreak)
      buf_nl(out);
  }
  buf_move(msg, out);
  buf_free(out);
  buf_free(line);
  buf_free(field);
  buf_free(content);
  return 0;
}


int entity_decode(BUFFER *msg, int message, int mptype, BUFFER *data)
{
  BUFFER *out, *line, *field, *content, *type, *subtype, *disposition,
      *mboundary, *part, *sigdata;
  int ret = 0, ptype = 0, partno = 0;
  int p, encoded = 0;

  out = buf_new();
  line = buf_new();
  field = buf_new();
  content = buf_new();
  type = buf_new();
  subtype = buf_new();
  disposition = buf_new();
  mboundary = buf_new();
  part = buf_new();
  sigdata = buf_new();

  if (message && bufileft(msg, "From ")) {
    buf_getline(msg, out); /* envelope from */
    buf_nl(out);
  }

  while (buf_getheader(msg, field, content) == 0) {
    if (bufieq(field, "content-transfer-encoding") &&
	bufieq(content, "quoted-printable"))
      encoded = 'q';
    if (bufieq(field, "content-type")) {
      get_type(content, type, subtype);
      if (bufieq(type, "multipart"))
	get_parameter(content, "boundary", mboundary);
      if (bufieq(type, "multipart") && bufieq(subtype, "encrypted")) {
	get_parameter(content, "protocol", line);
	if (bufieq(line, "application/pgp-encrypted"))
	  ptype = pgpenc;
      }
      if (bufieq(type, "multipart") && bufieq(subtype, "signed")) {
	get_parameter(content, "protocol", line);
	if (bufieq(line, "application/pgp-signature"))
	  ptype = pgpsig;
      }
    }
    if (bufieq(field, "content-disposition"))
      buf_set(disposition, content);
    if (message) {
      decode_header(content);
      buf_appendheader(out, field, content);
    }
  }

  if (message)
    buf_nl(out);

  if (bufifind(disposition, "attachment")) {
    buf_appendf(out, "[-- %b attachment", type);
    get_parameter(disposition, "filename", line);
    if (line->length)
       buf_appendf(out, " (%b)", line);
    buf_appends(out, " --]\n");
  }

  if (mboundary->length) {
  /* multipart */
    while (buf_getline(msg, line) > -1 && !boundary(line, mboundary))
      ; /* ignore preamble */
    while (buf_getline(msg, line) != -1) {
      p = boundary(line, mboundary);
      if (p) {
	if (part->data[part->length - 1] == '\n')
	  part->data[--(part->length)] = '\0';
	partno++;
	if (ptype == pgpsig && partno == 1)
	  buf_set(sigdata, part);
	ret += entity_decode(part, 0, ptype, sigdata);
	buf_cat(out, part);
	buf_clear(part);
	if (p == delimclose)
	  break;
	if (bufieq(subtype, "alternative") && ret > 0)
	  break;
	if (bufieq(subtype, "mixed"))
	  buf_appends(out,
		      "[-------------------------------------------------------------------------]\n");
      } else {
	buf_cat(part, line);
	buf_nl(part);
      }
    }
  } else if (mptype == pgpenc && bufieq(type, "application") &&
	     bufieq(subtype, "pgp-encrypted")) {
    /* application/pgp-encrypted part of multipart/encrypted */
    ; /* skip */
  } else if (mptype == pgpenc && bufieq(type, "application") &&
	     bufieq(subtype, "octet-stream")) {
    /* application/octet-stream part of multipart/encrypted */
    int ok = 0;
    buf_getline(msg, line);
    if (bufleft(line, info_beginpgp)) {
      if (buffind(line, "(SIGNED)")) {
	  buf_getline(msg, line);
	  buf_appends(out, "[-- OpenPGP message with signature --]\n");
	  if (bufleft(line, info_pgpsig))
	    buf_appendf(out, "[%s]\n",
			line->data + sizeof(info_pgpsig) - 1);
	  else
	    buf_appends(out, "[Signature invalid]\n");
      } else
	buf_appends(out, "[-- OpenPGP message --]\n");
      while (buf_getline(msg, line) != -1) {
	if (bufleft(line, info_endpgp)) {
	  ret += entity_decode(part, 0, 0, NULL);
	  buf_cat(out, part);
	  buf_appends(out, "[-- End OpenPGP message --]\n");
	  ok = 1, ret++;
	  break;
	}
	buf_cat(part, line);
	buf_nl(part);
      }
    }
    if (!ok) {
      buf_appends(out, "[-- Bad OpenPGP message --]\n");
      buf_cat(out, msg);
    }
  } else if (mptype == pgpsig && bufeq(type, "application") &&
	     bufieq(subtype, "pgp-signature")) {
    buf_rest(part, msg);
#ifdef USE_PGP
    if (pgp_decrypt(part, NULL, data, PGPPUBRING, NULL) == PGP_SIGOK)
      buf_appendf(out, "[-- OpenPGP signature from:\n    %b --]\n", data);
    else
      buf_appends(out, "[-- Invalid OpenPGP signature --]\n");
#else /* USE_PGP */
    buf_appends(out, "[-- No OpenPGP support --]\n");
#endif /* !USE_PGP */
  } else if (type->length == 0 || bufieq(type, "text")) {
    while (buf_getline(msg, line) != -1) {
      int softbreak;
      softbreak = encoded ? decode_line(line) : 0;
      buf_cat(out, line);
      if (!softbreak)
	buf_nl(out);
    }
    ret++;
  } else {
    buf_appendf(out, "[-- %b/%b message part --]\n", type, subtype);
    buf_cat(out, msg);
  }

  buf_move(msg, out);
  buf_free(line);
  buf_free(out);
  buf_free(field);
  buf_free(content);
  buf_free(type);
  buf_free(subtype);
  buf_free(disposition);
  buf_free(mboundary);
  buf_free(part);
  buf_free(sigdata);
  return (0);
}

void mimedecode(BUFFER *msg)
{
  entity_decode(msg, 1, 0, NULL);
}

int attachfile(BUFFER *message, BUFFER *filename)
{
  BUFFER *type, *attachment;
  FILE *f;
  int ret = -1;

  type = buf_new();
  attachment = buf_new();

  if ((bufiright(filename, ".txt") || !bufifind(filename, ".")) &&(strlen(DEFLTENTITY) != 0))
    buf_sets(type, DEFLTENTITY);
  else if (bufiright(filename, ".htm") || bufiright(filename, ".html"))
    buf_sets(type, "text/html");
  else if (bufiright(filename, ".jpeg"))
    buf_sets(type, "image/jpeg");
  else if (bufiright(filename, ".gif"))
    buf_sets(type, "image/gif");
  else if (bufiright(filename, ".pcm"))
    buf_sets(type, "audio/basic");
  else if (bufiright(filename, ".mpg") || bufiright(filename, ".mpeg"))
    buf_sets(type, "video/mpeg");
  else if (bufiright(filename, ".ps"))
    buf_sets(type, "application/postscript");
  else
    buf_sets(type, "application/octet-stream");

  f = fopen(filename->data, "r");
  if (f) {
    buf_read(attachment, f);
    fclose(f);
    ret = mime_attach(message, attachment, type);
  }

  buf_free(attachment);
  buf_free(type);
  return(ret);
}

int mime_attach(BUFFER *message, BUFFER *attachment, BUFFER *attachtype)
{
  BUFFER *out, *part, *line, *type, *subtype, *mboundary, *field, *content;
  int mimeheader = 0, multipart = 0, versionheader = 0;

  out = buf_new();
  line = buf_new();
  part = buf_new();
  type = buf_new();
  subtype = buf_new();
  mboundary = buf_new();
  field = buf_new();
  content = buf_new();

  buf_rewind(message);
  while (buf_getheader(message, field, content) == 0) {
    if (bufieq(field, "mime-version"))
      versionheader = 1;
    if (bufieq(field, "content-type")) {
      get_type(content, type, subtype);
      if (bufieq(type, "multipart") && bufieq(subtype, "mixed")) {
	multipart = 1;
	get_parameter(content, "boundary", mboundary);
      }
    }
    if (bufileft(field, "content-"))
      mimeheader = 1;
  }

  if (mimeheader && !multipart) {
    buf_rewind(message);
    while (buf_getheader(message, field, content) == 0) {
      if (bufileft(field, "content-"))
	buf_appendheader(part, field, content);
      else
	buf_appendheader(out, field, content);
    }
  } else {
    buf_ungetc(message);
    buf_append(out, message->data, message->ptr);
    buf_getc(message);
  }

  if (!versionheader)
    buf_appends(out, "MIME-Version: 1.0\n");

  if (!multipart) {
    buf_setrnd(mboundary, 18);
    encode(mboundary, 0);
    buf_appendf(out, "Content-Type: multipart/mixed; boundary=\"%b\"\n",
		mboundary);
  }
  buf_nl(out);

  if (multipart) {
    while (buf_getline(message, line) != -1) {
      if (boundary(line, mboundary) == delimclose)
	break;
      buf_cat(out, line);
      buf_nl(out);
    }
  } else {
    buf_appendf(out, "--%b\n", mboundary);
    if (part->length) {
      buf_cat(out, part); /* body part header */
    }
    else {
      if (strlen(DEFLTENTITY))
	buf_appendf(out, "Content-Type: %s\n", DEFLTENTITY);
    }

    buf_nl(out);
    buf_cat(out, message);
    buf_nl(out);
  }

  buf_appendf(out, "--%b\n", mboundary);
  buf_appendf(out, "Content-Type: %b\n", attachtype);

  body_encode(attachment, MIME_8BIT, line);
  buf_cat(out, line);
  buf_nl(out);
  buf_cat(out, attachment);
  buf_appendf(out, "\n--%b--\n", mboundary);

  buf_move(message, out);

  buf_free(out);
  buf_free(line);
  buf_free(part);
  buf_free(type);
  buf_free(subtype);
  buf_free(mboundary);
  buf_free(field);
  buf_free(content);
  return (1);
}

static int entity_encode(BUFFER *message, BUFFER *out, BUFFER *messagehdr,
			 int encoding)
{
  BUFFER *field, *content, *mboundary, *part, *line, *line2, *tmp;

  field = buf_new();
  content = buf_new();
  mboundary = buf_new();
  part = buf_new();
  line = buf_new();
  line2 = buf_new();
  tmp = buf_new();

  buf_rewind(message);
  buf_clear(out);
  buf_clear(messagehdr);

  while (buf_getheader(message, field, content) == 0) {
    if (bufileft(field, "content-"))
      buf_appendheader(out, field, content);
    else if (messagehdr)
      buf_appendheader(messagehdr, field, content);

    if (bufieq(field, "content-type")) {
      get_type(content, line, tmp);
      if (bufieq(line, "multipart"))
	get_parameter(content, "boundary", mboundary);
    }
  }

  buf_nl(out);
  if (mboundary->length) {
      while (buf_getline(message, line) != -1) {
	buf_cat(out, line);
	buf_nl(out);
	if (boundary(line, mboundary))
	  break;
      }
      while (buf_getline(message, line) != -1) {
	  if (boundary(line, mboundary)) {
	    entity_encode(part, tmp, line2, encoding);
	    buf_cat(out, line2);
	    buf_cat(out, tmp);
	    buf_cat(out, line);
	    buf_nl(out);
	    buf_clear(part);
	    if (boundary(line, mboundary) == delimclose)
	      break;
	  } else {
	    buf_cat(part, line);
	    buf_nl(part);
	  }
      }
  } else
    buf_rest(out, message);
  buf_rewind(out);
  mail_encode(out, encoding);

  buf_free(field);
  buf_free(content);
  buf_free(mboundary);
  buf_free(part);
  buf_free(line);
  buf_free(line2);
  buf_free(tmp);
  return (1);
}

int pgpmime_sign(BUFFER *message, BUFFER *uid, BUFFER *pass, char *secring)
{
#ifndef USE_PGP
  return (-1)
#else /* end of not USE_PGP */
  BUFFER *out, *body, *mboundary, *algo;
  int err;

  out = buf_new();
  body = buf_new();
  mboundary = buf_new();
  algo = buf_new();

  pgp_signhashalgo(algo, uid, secring, pass);

  entity_encode(message, body, out, MIME_7BIT);

  buf_setrnd(mboundary, 18);
  encode(mboundary, 0);
  buf_appendf(out, "Content-Type: multipart/signed; boundary=\"%b\";\n",
	      mboundary);
  buf_appendf(out,
	      "\tmicalg=pgp-%b; protocol=\"application/pgp-signature\"\n",
	      algo);
  buf_nl(out);

  buf_appendf(out, "--%b\n", mboundary);
  buf_cat(out, body);
  buf_nl(out);
  buf_appendf(out, "--%b\n", mboundary);

  err = pgp_encrypt(PGP_SIGN | PGP_TEXT | PGP_DETACHEDSIG, body, NULL,
		    uid, pass, NULL, secring);

  buf_appends(out, "Content-Type: application/pgp-signature\n");
  buf_nl(out);
  buf_cat(out, body);
  buf_nl(out);
  buf_appendf(out, "--%b--\n", mboundary);
  if (err == 0)
    buf_move(message, out);

  buf_free(out);
  buf_free(body);
  buf_free(mboundary);
  buf_free(algo);
  return (err);
#endif /* else if USE_PGP */
}
