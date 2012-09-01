/* Mixmaster version 3.0  --  (C) 1999 - 2006 Anonymizer Inc. and others.

   Mixmaster may be redistributed and modified under certain conditions.
   This software is distributed on an "AS IS" basis, WITHOUT WARRANTY OF
   ANY KIND, either express or implied. See the file COPYRIGHT for
   details.


   Mixmaster Library API
   =====================

The Mixmaster library consists of a set of high-level functions that
generate or process remailer messages, lower-level functions that
manipulate data in various ways, and a number of functions that
provide an interface to the underlying cryptographic library.
Generally, a return value of 0 indicates success, and -1 an error.


Initialization
==============

int mix_init(char mixdir[]);

  This function initializes internal data of the Mixmaster library,
  such as the random number generator. This should be the first call
  to the Mixmaster library. It returns 0 on success. If the random
  number generator cannot be initialized, mix_init() terminates.

  The variable mixdir determines where the Mixmaster configuration
  files and the message pool are located. If mixdir is NULL, the
  library will use the directory specified in the environment variable
  $MIXPATH, the directory given at compile time if it exists, and the
  directory ~/Mix otherwise.


void mix_exit(void);

  A program must call mix_exit before exiting. This function writes back
  the state of the random number generator.


Using the Mixmaster DLL
=======================

In textmode applications, mix_init() can be used as described above.
In graphical applications, these functions are not needed. Instead,
the function rnd_mouse() should be called whenever the program gets
WM_MOUSEMOVE or other messages:

int rnd_mouse(UINT i, WPARAM w, LPARAM l);

  All events that a window gets may be passed to this function. It
  will extract the inherent randomness in user interaction, especially
  in mouse movements. It returns 100 if it has accumulated enough
  randomness to perform cryptographic operations, and a number between
  0 and 99 otherwise. This number can be used to provide graphical
  feedback on the progress of initializing the random number generator
  while asking the user to move the mouse. A runtime error will occur
  if any cryptographic functions are used before rnd_mouse() has
  signaled success.


Message I/O
===========

The library uses dynamically allocated buffers for messages and other
data. Functions for buffer manipulation are described in section
"Buffers" below.


BUFFER *buf_new(void);

  Buffers must be initialized before they can be used. buf_new() returns
  a pointer to a newly initialized buffer.


int buf_free(BUFFER *buf);

  When a buffer is no longer needed, it should be freed. This function
  returns the memory used for the buffer to the operating system.


int buf_read(BUFFER *message, FILE *infile);

  This function reads data from a stream and appends them to the buffer.

  Return values:
   0 on success,
   1 if the file is too large to store it in a buffer,
  -1 if no data could be read.


int buf_write(BUFFER *message, FILE *outfile);

  This function writes the entire buffer to the output stream.

  Return values:
   0 if the buffer could be written completely,
  -1 otherwise.

int buf_write_sync(BUFFER *message, FILE *outfile);

  This function does the same as buf_write but also does
  checks for return values of fflush, fsync and ***fclose***.

  Return values:
   0 if the buffer could be written, synced and closed completely,
  -1 otherwise.

Remailer Messages
=================

int mix_encrypt(int type, BUFFER *message, char *chain, int numcopies,
	BUFFER *feedback);

  This function creates a Mixmaster message and stores it the Mixmaster
  message pool.

  The type is one of the following:

   MSG_MAIL  electronic mail message
   MSG_POST  Usenet news article
   MSG_NULL  dummy message, will be discarded

  *chain is a string consisting of a comma-separated list of remailer
  names that the message will be sent through. '*' means that a remailer
  will be chosen at random. If *chain is NULL, mix_encrypt() will use the
  default chain.

  numcopies is a number between 1 and 10 that indicates how many
  (redundant) copies of the message should be sent. If numcopies is 0,
  the default value will be used. The default values for *chain and
  numcopies are read from the configuration file.

  If *feedback is not NULL, mix_encrypt() will write the chain(s) that
  have been selected as newline-separated strings, or a textual error
  message to *feedback. This text can be presented to the user as
  feedback.

  Return values:
   0 on success,
  -1 if the message could not be created.


int mix_decrypt(BUFFER *message);

  This is the remailer function, which reads Mixmaster and Cypherpunk
  remailer messages as well as help file and key requests. Remailer
  messages are decrypted and stored in the message pool. Replies to
  information requests are sent immediately.

  Return values:
   0  if the message has been processed successfully,
   1  if the message is of an unknown type,
  -1  if the message could not be processed.


int mix_send(void);

  This function causes the messages in the pool to be sent. Depending on
  the configuration, mix_send() may send only a certain fraction of the
  messages in the pool.

  Return value: The size of the pool after the messages have been sent.


int mix_regular(int force);

  This function is responsible for regular actions of the remailer such
  as sending messages from the pool, getting mail from POP3 servers and
  expiring log files.


Nymserver Client Functions
==========================

The nymserver functions use user_pass() to get the passphrase for
opening the nym database.

int nym_config(int mode, char *nym, char *nymserver, BUFFER *pseudonym,
	       char *sendchain, int sendnumcopies, BUFFER *chains,
	       BUFFER *options);

  Create, modify or delete a nym. mode is one of NYM_CREATE, NYM_MODIFY and
  NYM_DELETE.

  nym is the pseudonymous address or its local part. In the latter case,
  nymserver must contain a string that selects a nymserver.

  pseudonym is a text string or NULL.

  sendchain and sendnumcopies are the chain and number of copies of
  the Mixmaster message sent to the nymserver.

  chains contains a list of reply blocks, consisting of "To:",
  "Newsgroups:", "Null:", "Latency:", "Chain:" and arbitrary header lines
  such as "Subject:". The "Chain:" line contains a remailer selection
  string for type 1 remailers. The reply blocks are separated by empty
  lines.

  options contains nymserver options (any of "acksend", "signsend",
  "fixedsize", "disable", "fingerkey" with a "+" or "-" prefix) or is NULL.


int nym_encrypt(BUFFER *msg, char *nym, int type);

  Prepare the message msg of type MSG_MAIL or MSG_POST to be sent using
  the nym. After successful encryption, msg contains a message of type
  MSG_MAIL addressed to the nymserver.


int nym_decrypt(BUFFER *msg, char *nym, BUFFER *log);

  Decrypt nymserver replies and PGP messages. If msg contains a nymserver
  reply, the the recipient nym is stored in nym (unless nym is NULL), and
  msg is replaced with the plaintext message in the Unix mail folder
  format.

  If log is not NULL, nym_decrypt will compute a unique ID for each
  message and append it to log. If the ID already is contained in log,
  it will return an empty msg buffer.


Lower-Level Remailer Functions
==============================

t1_decrypt(BUFFER *in);

  Decrypts and processes a Cypherpunk remailer message.


t2_decrypt(BUFFER *in);

  Decrypts and processes a Mixmaster remailer message.


int mix_pool(BUFFER *msg, int type, long latent);

  Adds the message msg of type MSG_MAIL or MSG_POST to the pool.
  latent is 0 or the message latency in seconds.


OpenPGP encryption
==================

int pgp_encrypt(int mode, BUFFER *message, BUFFER *encr,
               BUFFER *sigid, BUFFER *pass, char *pubring,
	       char *secring);

  This function encrypts and signs a message according to OpenPGP (RFC 2440).

  mode is the bitwise or of one of PGP_ENCRYPT, PGP_CONVENTIONAL and PGP_SIGN,
  and any of PGP_TEXT, PGP_REMAIL and PGP_NOARMOR.

  PGP_CONVENTIONAL: the message is encrypted conventionally, using
            the passphrase encr. If PGP_NCONVENTIONAL is used instead,
            the new OpenPGP format is used.
  PGP_ENCRYPT: public key encryption is used. The message is encrypted to
            the first public key on the keyring a User ID of which contains
            the substring encr. encr may contain several lines with one
            address substring each.
  PGP_SIGN: the message is signed with the first key from the secret
            key ring whose user ID contains sigid as a substring, or the
            first key if sigid is NULL.
  PGP_TEXT: message is treated as text, without PGP_TEXT as binary.
  PGP_DETACHEDSIG: signature will not include the signed message.
  PGP_REMAIL: a random offset is subtracted from signature dates, and the
            ASCII armor is made to mimic PGP.
  PGP_NOARMOR: message armor is not applied.

  If none of PGP_SIGN, PGP_CONVENTIONAL and PGP_ENCRYPT is set, the
  message is only compressed and armored.

  pubring and secring can be NULL or specify the name of a key ring.

  Return values:
   0       on success,
  -1       no matching key found,
  PGP_PASS bad signature passphrase.


int pgp_mailenc(int mode, BUFFER *message, char *sigid,
		BUFFER *pass, char *pubring, char *secring);

  This function encrypts and signs an RFC 822 e-mail message according to
  RFC 2015 (OpenPGP/MIME). Signatures without encryption on non-MIME messages
  are "cleartext" signatures.


int pgp_decrypt(BUFFER *message, BUFFER *pass, BUFFER *sig, char *pubring,
               char *secring);

  This function decrypts the OpenPGP message and verifies its signature.
  pass must contain the passphrase if message is conventionally encrypted
  or the secret key is protected by a passphrase. Otherwise it can be
  NULL.

  If message is a detached signature, sig must contain the signed data.
  It sig is NULL, the message will be decrypted without signature
  verification.

  pgp_getmsg() writes a string containing the signing time and
  signer's user ID or the key ID of the unknown signature key to sig.

  pubring and secring can be NULL or specify the name of a key ring.

  Return values:
  PGP_OK      on success,
  PGP_ERR     the message can't be read,
  PGP_PASS    bad passphrase,
  PGP_NOMSG   message is not an OpenPGP message,
  PGP_SIGOK   success, and signature has been verified,
  PGP_SIGNKEY can't verify signature,
  PGP_SIGBAD  bad signature,
  PGP_NODATA  OpenPGP message does not contain user data.


int pgp_keygen(int algo, int bits, BUFFER *userid, BUFFER *pass, char *pubring,
               char *secring, int remail);

  Generate a new key pair with given userid, encrypt the secret key with
  pass if not NULL.  Use a fake date if remail is not zero. Assume an
  encrypted secring if remail == 2.  algo is PGP_ES_RSA or PGP_E_ELG.


Buffers
=======

Buffers contain binary data of arbitrary length. You can append data
to buffers, clear buffers, and read data from buffers sequentially.
As data are appended to a buffer, memory is allocated dynamically.

typedef unsigned char byte;

typedef struct
{
    byte *data;
    long length;
    long ptr;
    long size;
    byte sensitive;
} BUFFER;

For a buffer *b, b->data is a pointer to at least b->length+1 bytes of
memory. b->data[b->length] is guaranteed to contain a null byte, so that
string functions can be used directly on buffers that contain text.

ptr is a counter for reading data from the buffer. b->data[b->ptr] is
the first data byte that has not been read (0 <= ptr <= length).

If sensitive is 1, the buffer contents will be overwritten before the
memory is released.


int buf_reset(BUFFER *buf);

  This function empties the buffer and returns the memory it has used to
  the operating system. It does not free the buffer itself.


int buf_clear(BUFFER *buf);

  buf_clear() empties the buffer but does not free the memory it uses.
  This function should be used if data of a similar size will be stored
  to the buffer later.


int buf_eq(BUFFER *buf1, BUFFER *buf2);

  Return values:
   1 if the buffers contain identical data,
   0 otherwise.


int buf_append(BUFFER *buf, byte *msg, int len);

  This is the most basic function for appending data to a buffer. It is
  called by all other functions that write to buffers. buf_append()
  appends len bytes pointed to by msg to buf. New memory will be
  allocated for the buffer if necessary.

  If msg is NULL, the buffer is increased by len bytes, but no
  guarantee is made about the contents of the appended bytes.

  Return value:
   0 on success,
   does not return if allocation of memory fails.


int buf_appendc(BUFFER *buf, byte b);
  appends the byte b to buf.


int buf_appends(BUFFER *buf, char *s);
  appends the null-terminated string s to buf.


int buf_appendf(BUFFER *buf, char *fmt, ...);
  appends formatted output to buf.


int buf_sets(BUFFER *buf, char *s);
  sets buf to contain the null-terminated string s.


int buf_setf(BUFFER *buf, char *fmt, ...);
  sets buf to contain the formatted output.


int buf_nl(BUFFER *buf);
  appends a newline character to buf.


int buf_cat(BUFFER *buf, BUFFER *f);
  appends the entire contents of f to buf.


int buf_rest(BUFFER *buf, BUFFER *f);
  appends the unread data from f to buf.


int buf_set(BUFFER *buf, BUFFER *f);
  sets buf to a copy of the contents of f.


int buf_move(BUFFER *buf, BUFFER *f);
  sets buf to the contents of f, and resets f. This is equivalent to
  buf_set(buf, f); buf_reset(f); but more efficient.


int buf_appendrnd(BUFFER *buf, int n);
  appends n cryptographically strong pseudo-random bytes to buf.


int buf_setrnd(BUFFER *buf, int n);
  places n cryptographically strong pseudo-random bytes in buf.


int buf_appendzero(BUFFER *buf, int n);
  appends n null bytes to buf.


int buf_pad(BUFFER *buf, int size);
  pads the buffer with cryptographically strong pseudo-random data to
  length size. Aborts if size < buf->length.


int buf_appendi(BUFFER *b, int i);
  appends the two bytes representing i in big-endian byte order to buf.


int buf_appendi_lo(BUFFER *b, int i);
  appends the two bytes representing i in little-endian byte order to buf.


int buf_appendl(BUFFER *buf, long l);
  appends the four bytes representing l in big-endian byte order to buf.


int buf_appendl_lo(BUFFER *buf, long l);
  appends the four bytes representing l in little-endian byte order to buf.


int buf_prepare(BUFFER *buf, int size);
  sets buf to contain size bytes of arbitrary data.


int buf_get(BUFFER *buf, BUFFER *t, int n);

  This function sets buffer t to contain n bytes read from buf.

  Return values:
   0 on success,
  -1 if buf does not contain n unread bytes.


int buf_getc(BUFFER *buf);
  reads one byte from buf. Returns -1 if buf contains no unread data,
  the byte otherwise.


int buf_geti(BUFFER *buf);
  reads two bytes from buf. Returns -1 if buf buf does not contain two
  unread bytes, the integer represented by the bytes in big-endian
  byte order otherwise.


int buf_geti_lo(BUFFER *buf);
  reads two bytes from buf. Returns -1 if buf buf does not contain two
  unread bytes, the integer represented by the bytes in little-endian
  byte order otherwise.


long buf_getl(BUFFER *buf);
  reads four bytes from buf. Returns -1 if buf buf does not contain four
  unread bytes, the integer represented by the bytes in big-endian
  byte order otherwise.


long buf_getl_lo(BUFFER *buf);
  reads four bytes from buf. Returns -1 if buf buf does not contain four
  unread bytes, the integer represented by the bytes in little-endian
  byte order otherwise.


void buf_ungetc(BUFFER *buf);
  restores one character for reading.


int buf_appendb(BUFFER *buf, BUFFER *p);
  appends p (with length information) to buf.


int buf_getb(BUFFER *buf, BUFFER *p);
  gets length information, then p from buf.


int buf_getline(BUFFER *buf, BUFFER *line);

  This function reads one line of text from buf, and stores it (without
  the trailing newline) in the buffer line.

  Return values:
   0 if a line of text has been read,
   1 if the line read is empty,
  -1 if buf contains no unread data.


int buf_lookahead(BUFFER *buf, BUFFER *line);

  This function reads one line of text from buf, and stores it (without
  the trailing newline) in the buffer line, without increasing the read
  counter.

  Return values:
   0 if a line of text has been read,
   1 if the line read is empty,
  -1 if buf contains no unread data.


int buf_chop(BUFFER *buf);

  buf is assumed to contain one line of text. A trailing newline and any
  other lines of text buf may contain are removed.


int buf_isheader(BUFFER *buf);

  This function checks whether the first line of buf is a RFC 822 header line.

  Returns:
   0 if it is not a header line.
   1 if it is a header line.

int buf_getheader(BUFFER *buf, BUFFER *field, BUFFER *content);

  This function reads a RFC 822 header line from buf. The field name of
  the header line without the colon is stored in field, the line's
  contents in content.

  Returns:
   0 on success,
   1 at end of header,
  -1 if buf contains no unread data.


int buf_appendheader(BUFFER *buffer, BUFFER *field, BUFFER *content);

  This function appends the RFC 822 header consisting of field and content
  to buffer.


int buf_rewind(BUFFER *buf);

  This function sets the read counter of buf to the start of the buffer
  (equivalent to buf->ptr = 0).


Randomness
==========

byte rnd_byte(void);
  returns a random byte.


int rnd_number(int n);
  returns a random number in 0 .. n-1.


int rnd_bytes(byte *b, int n);
  stores n random bytes at b.


Interface to the crypto library PRNG
====================================

int rnd_init(void);

  initializes the PRNG from the random seed file. Called from mix_init().
  Return values:
   0 on success,
  -1 on error.


int rnd_final(void);

  writes the random seed file and ends the PRNG. Called from mix_exit().
  Return values:
   0 on success,
  -1 on error.


int rnd_seed(void);
  seeds the PRNG, using console input if necessary.


void rnd_update(byte *b, int n);
  adds n bytes from b to the PRNG, unless b == NULL, and adds randomness
  from the system environment.


extern int rnd_state;
  An application may set rnd_state = RND_WILLSEED before executing
  mix_init() to indicate that it will seed the PRNG later by making calls
  to rnd_update() and then to rnd_initialized(). In that case,
  rnd_seed() will not ask for user input. [This is what the DLL startup code
  does internally.]


String comparison
=================

These functions operate on null-terminated strings. They return truth
values.


int streq(const char *s1, const char *s2);

  Return values:
   1 if the strings s1 and s2 are equal,
   0 otherwise.


int strieq(const char *s1, const char *s2);

  Return values:
   1 if the strings s1 and s2 are equal except for case,
   0 otherwise.


int strleft(const char *s, const char *keyword);

  Return values:
   1 if keyword is the left part of s,
   0 otherwise.


int strileft(const char *s, const char *keyword);

  Return values:
   1 if keyword is the left part of s, except for case,
   0 otherwise.


int strfind(const char *s, const char *keyword);

  Return values:
   1 if keyword is contained in s,
   0 otherwise.


int strifind(const char *s, const char *keyword);

  Return values:
   1 if keyword is contained in s, except for case,
   0 otherwise.


RFC 822 Addresses
=================

void rfc822_addr(BUFFER *destination, BUFFER *list);
  stores a list of RFC 822 addresses from destination in list, separated
  by newlines.

void rfc822_name(BUFFER *line, BUFFER *name);
  stores the name given in the RFC 822 address in line in name.


Files and Pipes
===============

int mixfile(char path[PATHMAX], const char *name);
  stores the path to a given file in the Mixmaster directory in path[].


FILE *mix_openfile(const char *name, const char *a);
  opens a file in the Mixmaster directory.


LOCK *lockfile(char *filename);
  creates and locks a lockfile associated with filename.


int unlockfile(LOCK *lock);
  releases the lock and deletes the lockfile.


int lock(FILE *f);
  sets a lock on a file.


int unlock(FILE *f);
  releases a lock on a file.


FILE *openpipe(const char *prog);
  opens a pipe.


int closepipe(FILE *p);
  closes a pipe.


int sendmail(BUFFER *message, BUFFER *address, const char *from);

  This function sends a mail message. The From: line and the destination
  address may be contained in the message; in that case address and from
  must be NULL. address is checked against the destination block list.

int sendmail_loop(BUFFER *message, BUFFER *address, const char *from);

  Identical to sendmail() but adds an X-Loop: header line.


Printable Encoding
==================

int encode(BUFFER *buf, int linelen);

  buf is encoded in base 64 encoding [RFC 1421]. If linelen > 0, the
  resulting text is broken into lines of linelen characters.

  Return value: 0.


int decode(BUFFER *in, BUFFER *out);

  This function reads the unread data from in, as long as it is valid
  base 64 encoded text, and stores the decoded data in out.

  Return values:
   0 if the in could be decoded to the end,
  -1 otherwise.


int hdr_encode(BUFFER *in, int n);

  Encodes a header line according to the MIME standard. The header is
  broken into lines of at most n characters.


int mail_encode(BUFFER *in, int encoding);

  Encodes the mail headers of a message, and encodes the body according
  to encoding MIME_7BIT or MIME_8BIT.


void id_encode(byte id[16], byte *s);
  stores the hexadecimal representation of id in s.


void id_decode(byte *s, byte id[16]);
  sets id to the value of the hexadecimal string s.


Compression
===========

int buf_zip(BUFFER *buf, BUFFER *f, int b);

  compresses buffer f using GZIP with b bits (or a default value, if
  b == 0), and appends the result to buf.

  Return values:
   0 on success,
  -1 on error.


int buf_unzip(BUFFER *buf, int type);

  uncompresses a GZIP [RFC 1952] compressed buffer. If type == 1, uncompress
  a ZLIB [RFC 1950] compressed buffer.

  Return values:
   0 on success,
  -1 on error.


**************************************************************************/

#ifndef _MIXLIB_H
#define _MIXLIB_H

#include <stdio.h>
#include <time.h>
#ifdef WIN32
#include <windows.h>
#endif /* WIN32 */

typedef unsigned char byte;

typedef struct {
  byte *data;
  long length;
  long ptr;
  long size;
  byte sensitive;
} BUFFER;

int mix_init(char *);
void mix_exit(void);
void rnd_update(byte *b, int n);
void rnd_initialized(void);
#ifdef WIN32
int rnd_mouse(UINT i, WPARAM w, LPARAM l);
#endif /* WIN32 */

BUFFER *buf_new(void);
int buf_free(BUFFER *buf);
int buf_read(BUFFER *message, FILE *infile);
int buf_write(BUFFER *message, FILE *outfile);
int buf_write_sync(BUFFER *message, FILE *outfile);

#define MSG_MAIL 1
#define MSG_POST 2
#define MSG_NULL 0

extern char MIXDIR[];

int mix_encrypt(int type, BUFFER *message, char *chain, int numcopies,
		BUFFER *feedback);
int mix_decrypt(BUFFER *message);
int mix_send(void);

#define FORCE_POOL 1
#define FORCE_POP3 2
#define FORCE_DAILY 4
#define FORCE_MAILIN 8
#define FORCE_STATS 16
void mix_check_timeskew(void);
int mix_regular(int force);
int mix_daemon(void);
int process_mailin(void);

#ifdef USE_PGP

#define NYM_CREATE 0
#define NYM_MODIFY 1
#define NYM_DELETE 2

int nym_config(int mode, char *nym, char *nymserver, BUFFER *pseudonym,
	       char *sendchain, int sendnumcopies, BUFFER *chains,
	       BUFFER *options);
int nym_encrypt(BUFFER *msg, char *nym, int type);
int nym_decrypt(BUFFER *msg, char *nym, BUFFER *log);

#define PGP_SIGN 1
#define PGP_ENCRYPT 2
#define PGP_CONVENTIONAL 4
#define PGP_REMAIL 8
#define PGP_TEXT 16
#define PGP_NOARMOR 32
#define PGP_DETACHEDSIG 64
#define PGP_NCONVENTIONAL 128
#define PGP_CONV3DES 256
#define PGP_CONVCAST 512

/* error codes */
#define PGP_OK 0		/* valid message, not signed */
#define PGP_SIGOK 1		/* valid signature */
#define PGP_NOMSG 2		/* is not an OpenPGP message */
#define PGP_NODATA 3		/* OpenPGP packet does not contain user data */
#define PGP_SIGNKEY 4		/* can't verify signature */
#define PGP_ERR -1		/* can't read message, no matching key found */
#define PGP_PASS -2		/* bad passphrase */
#define PGP_SIGBAD -3		/* bad signature */


/* algorithms */
#define PGP_ANY 0
#define PGP_ES_RSA 1
#define PGP_E_ELG 16
#define PGP_S_DSA 17

int pgp_encrypt(int mode, BUFFER *message, BUFFER *encr, BUFFER *sigid,
		BUFFER *pass, char *pubring, char *secring);
int pgp_decrypt(BUFFER *message, BUFFER *pass, BUFFER *sig, char *pubring,
		char *secring);
int pgp_keygen(int algo, int bits, BUFFER *userid, BUFFER *pass,
		  char *pubring, char *secring, int remail);
#endif /* USE_PGP */


/* parsedate */
time_t parsedate(char *p);



#ifdef WIN32

#define sleep(x) Sleep((x)*1000)
#define strcasecmp stricmp

#endif /* WIN32 */

#endif /* not _MIXLIB_H */
