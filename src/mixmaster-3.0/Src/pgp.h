/* Mixmaster version 3.0  --  (C) 1999 - 2006 Anonymizer Inc. and others.

   Mixmaster may be redistributed and modified under certain conditions.
   This software is distributed on an "AS IS" basis, WITHOUT WARRANTY OF
   ANY KIND, either express or implied. See the file COPYRIGHT for
   details.

   OpenPGP messages
   $Id: pgp.h 934 2006-06-24 13:40:39Z rabbi $ */


#ifdef USE_PGP
#ifndef _PGP_H
#include "mix3.h"
#ifdef USE_OPENSSL
#include <openssl/opensslv.h>
#endif /* USE_OPENSSL */

/* in the PGP Version header, list the same information as all other
   versions of Mixmaster to prevent anonymity set division. */
#define CLOAK

/* try to make the messages look similar to PGP 2.6.3i output
   (compression is not always the same though). */
#define MIMIC

/* packet types */
#define PGP_SESKEY 1
#define PGP_SIG 2
#define PGP_SYMSESKEY 3
#define PGP_OSIG 4
#define PGP_SECKEY 5
#define PGP_PUBKEY 6
#define PGP_SECSUBKEY 7
#define PGP_COMPRESSED 8
#define PGP_ENCRYPTED 9
#define PGP_MARKER 10
#define PGP_LITERAL 11
#define PGP_TRUST 12
#define PGP_USERID 13
#define PGP_PUBSUBKEY 14
#define PGP_ENCRYPTEDMDC 18
#define PGP_MDC 19

/* symmetric algorithms */
#define PGP_K_ANY 0
#define PGP_K_IDEA 1
#define PGP_K_3DES 2
#define PGP_K_CAST5 3
#define PGP_K_BF 4
#define PGP_K_AES128 7
#define PGP_K_AES192 8
#define PGP_K_AES256 9

/* hash algorithms */
#define PGP_H_MD5 1
#define PGP_H_SHA1 2
#define PGP_H_RIPEMD 3

/* signature types */
#define PGP_SIG_BINARY 0
#define PGP_SIG_CANONIC 1
#define PGP_SIG_CERT 0x10
#define PGP_SIG_CERT1 0x11
#define PGP_SIG_CERT2 0x12
#define PGP_SIG_CERT3 0x13
#define isPGP_SIG_CERT(x) (x >= PGP_SIG_CERT && x <= PGP_SIG_CERT3)
#define PGP_SIG_BINDSUBKEY 0x18
#define PGP_SIG_KEYREVOKE 0x20
#define PGP_SIG_SUBKEYREVOKE 0x28
#define PGP_SIG_CERTREVOKE 0x30

/* signature subpacket types */
#define PGP_SUB_CREATIME 2
#define PGP_SUB_CERTEXPIRETIME 3
#define PGP_SUB_KEYEXPIRETIME 9
#define PGP_SUB_PSYMMETRIC 11
#define PGP_SUB_ISSUER 16
#define PGP_SUB_PRIMARY 25
#define PGP_SUB_FEATURES 30

#define ARMORED 1

/* publick key algorithm operation modes */

#define PK_ENCRYPT 1
#define PK_DECRYPT 2
#define PK_SIGN 3
#define PK_VERIFY 4

#define MD5PREFIX "\x30\x20\x30\x0C\x06\x08\x2A\x86\x48\x86\xF7\x0D\x02\x05\x05\x00\x04\x10"
#define SHA1PREFIX "\x30\x21\x30\x09\x06\x05\x2b\x0E\x03\x02\x1A\x05\x00\x04\x14"

typedef struct {
  int ok;
  BUFFER *userid;
  byte sigtype;
  long sigtime;
  byte hash[16];
} pgpsig;

/* internal error codes */
#define PGP_SIGVRFY 99		/* valid signature packet to be verified */

/* pgpdata.c */
int pgp_getsk(BUFFER *p, BUFFER *pass, BUFFER *key);
int pgp_makesk(BUFFER *out, BUFFER *key, int sym, int type, int hash,
	       BUFFER *pass);
void pgp_iteratedsk(BUFFER *salted, BUFFER *salt, BUFFER *pass, byte c);
int pgp_expandsk(BUFFER *key, int skalgo, int hashalgo, BUFFER *data);
int skcrypt(BUFFER *data, int skalgo, BUFFER *key, BUFFER *iv, int enc);
int mpi_get(BUFFER *buf, BUFFER *mpi);
int mpi_put(BUFFER *buf, BUFFER *mpi);
int pgp_rsa(BUFFER *buf, BUFFER *key, int mode);
void pgp_sigcanonic(BUFFER *msg);
int pgp_makepubkey(BUFFER *seckey, BUFFER *outtxt, BUFFER *pubkey,
		   BUFFER *pass, int keyalgo);
int pgp_makekeyheader(int type, BUFFER *keypacket, BUFFER *outtxt,
                   BUFFER *pass, int keyalgo);
int pgp_getkey(int mode, int algo, int *sym, int *mdc, long *expires, BUFFER *keypacket, BUFFER *key,
	       BUFFER *keyid, BUFFER *userid, BUFFER *pass);
int pgp_rsakeygen(int bits, BUFFER *userid, BUFFER *pass, char *pubring,
		  char *secring, int remail);
int pgp_dhkeygen(int bits, BUFFER *userid, BUFFER *pass, char *pubring,
		 char *secring, int remail);
int pgp_dosign(int algo, BUFFER *data, BUFFER *key);
int pgp_elgencrypt(BUFFER *b, BUFFER *key);
int pgp_elgdecrypt(BUFFER *b, BUFFER *key);
int pgp_keyid(BUFFER *key, BUFFER *id);
int pgp_keylen(int symalgo);
int pgp_blocklen(int symalgo);

/* pgpget.c */
int pgp_getmsg(BUFFER *in, BUFFER *key, BUFFER *sig, char *pubring,
	       char *secring);
int pgp_ispacket(BUFFER *buf);
int pgp_isconventional(BUFFER *buf);
int pgp_packettype(BUFFER *buf, long *len, int *partial);
int pgp_packetpartial(BUFFER *buf, long *len, int *partial);
int pgp_getpacket(BUFFER *buf, BUFFER *p);
int pgp_getsig(BUFFER *p, pgpsig *sig, char *pubring);
void pgp_verify(BUFFER *msg, BUFFER *detached, pgpsig *sig);
int pgp_getsymmetric(BUFFER *buf, BUFFER *key, int algo, int type);
int pgp_getliteral(BUFFER *buf);
int pgp_uncompress(BUFFER *buf);
int pgp_getsessionkey(BUFFER *buf, BUFFER *pass, char *secring);
int pgp_getsymsessionkey(BUFFER *buf, BUFFER *pass);

/* pgpcreat.c */
int pgp_packet(BUFFER *buf, int type);
int pgp_packet3(BUFFER *buf, int type);
int pgp_symmetric(BUFFER *buf, BUFFER *key, int mdc);
int pgp_literal(BUFFER *buf, char *filename, int text);
int pgp_compress(BUFFER *buf);
int pgp_sessionkey(BUFFER *buf, BUFFER *user, BUFFER *keyid, BUFFER *seskey,
		   char *pubring);
void pgp_marker(BUFFER *buf);
int pgp_symsessionkey(BUFFER *buf, BUFFER *seskey, BUFFER *pass);
int pgp_sign(BUFFER *msg, BUFFER *msg2, BUFFER *sig, BUFFER *userid,
	     BUFFER *pass, int type, int self, long now, int remail,
	     BUFFER *seckey, char *secring);
int pgp_digest(int hashalgo, BUFFER *in, BUFFER *d);

/* pgpdb.c */

int pgpdb_getkey(int mode, int algo, int *sym, int *mdc, long *expires, BUFFER *key, BUFFER *user,
		 BUFFER *founduid, BUFFER *keyid, char *keyring, BUFFER *pass);

typedef struct {
  int filetype;
  BUFFER *db;
  LOCK *lock;
  int modified;
  int type; /* undefined, public, private */
  char filename[LINELEN];
  BUFFER *encryptkey;
#ifndef NDEBUG
  int writer;
#endif
} KEYRING;

KEYRING *pgpdb_new(char *keyring, int filetype, BUFFER *encryptkey, int type);
KEYRING *pgpdb_open(char *keyring, BUFFER *encryptkey, int writer, int type);
int pgpdb_append(KEYRING *keydb, BUFFER *p);
int pgpdb_getnext(KEYRING *keydb, BUFFER *p, BUFFER *keyid, BUFFER *userid);
int pgpdb_close(KEYRING *keydb);

#endif /* not _PGP_H */
#endif /* USE_PGP */
