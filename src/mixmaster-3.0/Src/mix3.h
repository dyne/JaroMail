/* Mixmaster version 3.0  --  (C) 1999 - 2006 Anonymizer Inc. and others.

   Mixmaster may be redistributed and modified under certain conditions.
   This software is distributed on an "AS IS" basis, WITHOUT WARRANTY OF
   ANY KIND, either express or implied. See the file COPYRIGHT for
   details.

   Function prototypes
   $Id: mix3.h 934 2006-06-24 13:40:39Z rabbi $ */


#ifndef _MIX3_H
#define _MIX3_H
#define COPYRIGHT "Copyright Anonymizer Inc. et al."

#include "config.h"
#include "mix.h"

#ifdef WIN32
#ifndef USE_SOCK
#define _WINSOCKAPI_		/* don't include winsock */
#endif /* not USE_SOCK */
#include <windows.h>
#ifdef _MSC
#define snprintf _snprintf
#endif /* _MSC */
#define DIRSEP '\\'
#define DIRSEPSTR "\\"
#else /* end of WIN32 */
#define DIRSEP '/'
#define DIRSEPSTR "/"
#endif /* else if not WIN32 */

#define NOT_IMPLEMENTED {printf("Function not implemented.\n");return -1;}
#define SECONDSPERDAY 86400

#include <time.h>

/* Dynamically allocated buffers */

int buf_reset(BUFFER *buffer);
int buf_clear(BUFFER *buffer);
int buf_append(BUFFER *buffer, byte *mess, int len);
int buf_cat(BUFFER *to, BUFFER *from);
int buf_set(BUFFER *to, BUFFER *from);
int buf_rest(BUFFER *to, BUFFER *from);
int buf_appendrnd(BUFFER *to, int n);
int buf_appendzero(BUFFER *to, int n);
int buf_setc(BUFFER *buf, byte c);
int buf_appendc(BUFFER *to, byte b);
int buf_setrnd(BUFFER *b, int n);
int buf_setf(BUFFER *buffer, char *fmt, ...);
int buf_appendf(BUFFER *buffer, char *fmt, ...);
int buf_sets(BUFFER *buf, char *s);
int buf_appends(BUFFER *buffer, char *s);
int buf_nl(BUFFER *buffer);
int buf_pad(BUFFER *buffer, int size);
int buf_prepare(BUFFER *buffer, int size);
int buf_rewind(BUFFER *buffer);
int buf_getc(BUFFER *buffer);
void buf_ungetc(BUFFER *buffer);
int buf_get(BUFFER *buffer, BUFFER *to, int n);
int buf_getline(BUFFER *buffer, BUFFER *line);
int buf_chop(BUFFER *b);
void buf_move(BUFFER *dest, BUFFER *src);
byte *buf_data(BUFFER *buffer);
int buf_isheader(BUFFER *buffer);
int buf_getheader(BUFFER *buffer, BUFFER *field, BUFFER *content);
int buf_appendheader(BUFFER *buffer, BUFFER *field, BUFFER *contents);
int buf_lookahead(BUFFER *buffer, BUFFER *line);
int buf_eq(BUFFER *b1, BUFFER *b2);
int buf_ieq(BUFFER *b1, BUFFER *b2);
void buf_cut_out(BUFFER *buffer, BUFFER *cut_out, BUFFER *rest,
		 int from, int len);

int buf_appendl(BUFFER *b, long l);
int buf_appendl_lo(BUFFER *b, long l);
long buf_getl(BUFFER *b);
long buf_getl_lo(BUFFER *b);
int buf_appendi(BUFFER *b, int i);
int buf_appendi_lo(BUFFER *b, int i);
int buf_geti(BUFFER *b);
int buf_geti_lo(BUFFER *b);

/* String comparison */
int strieq(const char *s1, const char *s2);
int strileft(const char *string, const char *keyword);
int striright(const char *string, const char *keyword);
int strifind(const char *string, const char *keyword);

int streq(const char *s1, const char *s2);
int strfind(const char *string, const char *keyword);
int strleft(const char *string, const char *keyword);

void strcatn(char *dest, const char *src, int n);

int bufleft(BUFFER *b, char *k);
int buffind(BUFFER *b, char *k);
int bufeq(BUFFER *b, char *k);

int bufileft(BUFFER *b, char *k);
int bufifind(BUFFER *b, char *k);
int bufiright(BUFFER *b, char *k);
int bufieq(BUFFER *b, char *k);

/* Utility functions */
void whoami(char *addr, char *defaultname);
int sendinfofile(char *name, char *log, BUFFER *address, BUFFER *subject);
int stats(BUFFER *out);
int conf(BUFFER *out);
void conf_premail(BUFFER *out);

void rfc822_addr(BUFFER *line, BUFFER *list);
void rfc822_name(BUFFER *line, BUFFER *name);
void sendmail_begin(void);	/* begin mail sending session */
void sendmail_end(void);	/* end mail sending session */
int sendmail_loop(BUFFER *message, char *from, BUFFER *address);
int sendmail(BUFFER *message, char *from, BUFFER *address);
int mixfile(char *path, const char *name);
int file_to_out(const char *name);
FILE *mix_openfile(const char *name, const char *a);
FILE *openpipe(const char *prog);
int closepipe(FILE *fp);
int maildirWrite(char *maildir, BUFFER *message, int create);
int write_pidfile(char *pidfile);
int clear_pidfile(char *pidfile);
time_t parse_yearmonthday(char* str);

int url_download(char* url, char* dest);
int download_stats(char *sourcename);

typedef struct {
  char *name;
  FILE *f;
} LOCK;

int lock(FILE *f);
int unlock(FILE *f);
LOCK *lockfile(char *filename);
int unlockfile(LOCK *lock);

int filtermsg(BUFFER *msg);
BUFFER *readdestblk( );
int doblock(BUFFER *line, BUFFER *filter, int logandreset);
int doallow(BUFFER *line, BUFFER *filter);
int allowmessage(BUFFER *in);

void errlog(int type, char *format,...);
void clienterr(BUFFER *msgbuf, char *err);
void logmail(char *mailbox, BUFFER *message);

void mix_status(char *fmt,...);
void mix_genericerror(void);

#define ERRORMSG 1
#define WARNING 2
#define NOTICE 3
#define LOG 4
#define DEBUGINFO 5

int decode(BUFFER *in, BUFFER *out);
int encode(BUFFER *b, int linelen);
void id_encode(byte id[], byte *s);
void id_decode(byte *s, byte id[]);

int decode_header(BUFFER *content);
int boundary(BUFFER *line, BUFFER *mboundary);
void get_parameter(BUFFER *content, char *attribute, BUFFER *value);
int get_type(BUFFER *content, BUFFER *type, BUFFER *subtype);
int mail_encode(BUFFER *in, int encoding);
int hdr_encode(BUFFER *in, int n);
int attachfile(BUFFER *message, BUFFER *filename);
int pgpmime_sign(BUFFER *message, BUFFER *uid, BUFFER *pass, char *secring);
int mime_attach(BUFFER *message, BUFFER *attachment, BUFFER *type);
void mimedecode(BUFFER *msg);
int qp_decode_message(BUFFER *msg);

#define MIME_8BIT 1   /* transport is 8bit */
#define MIME_7BIT 2   /* transport is 7bit */

/* randomness */
int rnd_bytes(byte *b, int n);
byte rnd_byte(void);
int rnd_number(int n);
int rnd_add(byte *b, int l);
int rnd_seed(void);
void rnd_time(void);

int rnd_init(void);
int rnd_final(void);
void rnd_error(void);

#define RND_QUERY 0
#define RND_NOTSEEDED -1
#define RND_SEEDED 1
#define RND_WILLSEED 2
extern int rnd_state; /* flag for PRNG status */

/* compression */
int buf_compress(BUFFER *b);
int buf_zip(BUFFER *out, BUFFER *in, int bits);
int buf_uncompress(BUFFER *b);
int buf_unzip(BUFFER *b, int type);

/* crypto functions */
int digest_md5(BUFFER *b, BUFFER *md);
int isdigest_md5(BUFFER *b, BUFFER *md);
int digestmem_md5(byte *b, int n, BUFFER *md);
int digest_sha1(BUFFER *b, BUFFER *md);
int digest_rmd160(BUFFER *b, BUFFER *md);

#define KEY_ID_LEN 32
int keymgt(int force);
int key(BUFFER *b);
int adminkey(BUFFER *b);

#define ENCRYPT 1
#define DECRYPT 0
int buf_crypt(BUFFER *b, BUFFER *key, BUFFER *iv, int enc);

#ifdef USE_IDEA
int buf_ideacrypt(BUFFER *b, BUFFER *key, BUFFER *iv, int enc);
#endif /* USE_IDEA */
int buf_bfcrypt(BUFFER *b, BUFFER *key, BUFFER *iv, int enc);
int buf_3descrypt(BUFFER *b, BUFFER *key, BUFFER *iv, int enc);
int buf_castcrypt(BUFFER *b, BUFFER *key, BUFFER *iv, int enc);
#ifdef USE_AES
int buf_aescrypt(BUFFER *b, BUFFER *key, BUFFER *iv, int enc);
#endif /* USE_AES */

int db_getseckey(byte keyid[], BUFFER *key);
int db_getpubkey(byte keyid[], BUFFER *key);
int pk_decrypt(BUFFER *encrypted, BUFFER *privkey);
int pk_encrypt(BUFFER *plaintext, BUFFER *privkey);
int check_seckey(BUFFER *buf, const byte id[]);
int check_pubkey(BUFFER *buf, const byte id[]);
int v2createkey(void);
int getv2seckey(byte keyid[], BUFFER *key);
int seckeytopub(BUFFER *pub, BUFFER *sec, byte keyid[]);

/* configuration, general remailer functions */
int mix_configline(char *line);
int mix_config(void);
int mix_initialized(void);
int mix_daily(void);

/* message pool */
#define INTERMEDIATE 0
int pool_send(void);
int pool_read(BUFFER *pool);
int pool_add(BUFFER *msg, char *type);
FILE *pool_new(char *type, char *tmpname, char *path);
int mix_pool(BUFFER *msg, int type, long latent);
int pool_packetfile(char *fname, BUFFER *mid, int packetnum);
void pool_packetexp(void);
int idexp(void);
int pgpmaxexp(void);
void pop3get(void);

typedef struct {  /* added for binary id.log change */
  char id[16];
  long time;
} idlog_t;

/* statistics */
int stats_log(int);
int stats_out(int);

/* OpenPGP */
#define PGP_ARMOR_NORMAL        0
#define PGP_ARMOR_REM           1
#define PGP_ARMOR_KEY           2
#define PGP_ARMOR_NYMKEY        3
#define PGP_ARMOR_NYMSIG        4
#define PGP_ARMOR_SECKEY        5

#define PGP_TYPE_UNDEFINED	0
#define PGP_TYPE_PRIVATE	1
#define PGP_TYPE_PUBLIC		2

int pgp_keymgt(int force);
int pgp_latestkeys(BUFFER* outtxt, int algo);
int pgp_armor(BUFFER *buf, int mode);
int pgp_dearmor(BUFFER *buf, BUFFER *out);
int pgp_pubkeycert(BUFFER *userid, char *keyring, BUFFER *pass,
		   BUFFER *out, int remail);
int pgp_signtxt(BUFFER *msg, BUFFER *uid, BUFFER *pass,
		char *secring, int remail);
int pgp_isconventional(BUFFER *buf);
int pgp_mailenc(int mode, BUFFER *msg, char *sigid,
		BUFFER *pass, char *pubring, char *secring);
int pgp_signhashalgo(BUFFER *algo, BUFFER *userid, char *secring,
		     BUFFER *pass);

/* menu */
int menu_initialized;
void menu_main(void);
void menu_folder(char command, char *name);
int menu_getuserpass(BUFFER *p, int mode);

int user_pass(BUFFER *b);
int user_confirmpass(BUFFER *b);
void user_delpass(void);

/* remailer */
typedef struct {
  char name[20];
  int version;
  char addr[128];
  byte keyid[16];
  struct {
    unsigned int mix:1;
    unsigned int compress:1;

    unsigned int cpunk:1;
    unsigned int pgp:1;
    unsigned int pgponly:1;
    unsigned int latent:1;
    unsigned int hash:1;
    unsigned int ek:1;
    unsigned int esub:1;

    unsigned int nym:1;
    unsigned int newnym:1;

    unsigned int post:1;
    unsigned int middle:1;

    unsigned int star_ex:1;
  } flags;
  struct rinfo {
    int reliability;
    int latency;
    char history[13];
  } info[2];
} REMAILER;

#define CHAINMAX 421
#define MAXREM 100
int prepare_type2list(BUFFER *out);
int mix2_rlist(REMAILER remailer[], int badchains[MAXREM][MAXREM]);
int t1_rlist(REMAILER remailer[], int badchains[MAXREM][MAXREM]);
int pgp_rlist(REMAILER remailer[], int n);
int pgp_rkeylist(REMAILER remailer[], int keyid[], int n);
void parse_badchains(int badchains[MAXREM][MAXREM], char *file, char *startindicator, REMAILER *remailer, int maxrem);
int chain_select(int hop[], char *chainstr, int maxrem, REMAILER *remailer,
		 int type, BUFFER *feedback);
int chain_rand(REMAILER *remailer, int badchains[MAXREM][MAXREM], int maxrem,
	       int thischain[], int chainlen, int t, int ignore_constraints_if_necessary);
int chain_randfinal(int type, REMAILER *remailer, int badchains[MAXREM][MAXREM],
	       int maxrem, int rtype, int chain[], int chainlen, int ignore_constraints_if_necessary);

float chain_reliability(char *chain, int chaintype,
			char *reliability_string);
int redirect_message(BUFFER *sendmsg, char *chain, int numcopies, BUFFER *chainlist);
int mix2_encrypt(int type, BUFFER *message, char *chainstr, int numcopies,
		int ignore_constraints_if_necessary, BUFFER *feedback);
int t1_encrypt(int type, BUFFER *message, char *chainstr, int latency,
	       BUFFER *ek, BUFFER *feedback);

int t1_getreply(BUFFER *msg, BUFFER *ek, int len);

int t1_decrypt(BUFFER *in);
int t2_decrypt(BUFFER *in);

int mix2_decrypt(BUFFER *m);
int v2body(BUFFER *body);
int v2body_setlen(BUFFER *body);
int v2partial(BUFFER *body, BUFFER *mid, int packet, int numpackets);
int v2_merge(BUFFER *mid);
int mix_armor(BUFFER *in);
int mix_dearmor(BUFFER *armored, BUFFER *bin);

/* type 1 */
#define HDRMARK "::"
#define EKMARK "**"
#define HASHMARK "##"
int isline(BUFFER *line, char *text);

/* nym database */

#define NYM_WAITING 0
#define NYM_OK 1
#define NYM_DELETED 2
#define NYM_ANY -1

int nymlist_read(BUFFER *n);
int nymlist_write(BUFFER *list);
int nymlist_get(BUFFER *list, char *nym, BUFFER *config, BUFFER *ek,
		BUFFER *options, BUFFER *name, BUFFER *rblocks, int *status);
int nymlist_append(BUFFER *list, char *nym, BUFFER *config, BUFFER *options,
		   BUFFER *name, BUFFER *chains, BUFFER *eklist, int status);
int nymlist_del(BUFFER *list, char *nym);
int nymlist_getnym(char *nym, BUFFER *config, BUFFER *ek, BUFFER *opt,
		   BUFFER *name, BUFFER *rblocks);
int nymlist_getstatus(char *nym);

/* Visual C lacks dirent */
#ifdef _MSC
typedef HANDLE DIR;

struct dirent {
  char d_name[PATHMAX];
};

DIR *opendir(const char *name);
struct dirent *readdir(DIR *dir);
int closedir(DIR *dir);
#endif /* _MSC */

/* sockets */
#if defined(WIN32) && defined(USE_SOCK)
#include <winsock.h>
int sock_init(void);
void sock_exit(void);

#else /* end of defined(WIN32) && defined(USE_SOCK) */
typedef int SOCKET;

#define INVALID_SOCKET -1
SOCKET opensocket(char *hostname, int port);
int closesocket(SOCKET s);

#endif /* else if not defined(WIN32) && defined(USE_SOCK) */

#ifdef WIN32
int is_nt_service(void);
void set_nt_exit_event();
#endif /* WIN32 */

/* check for memory leaks */
#ifdef DEBUG
#define malloc mix3_malloc
#define free mix3_free
BUFFER *mix3_bufnew(char *, int, char*);
#if __GNUC__ >= 2
# define buf_new() mix3_bufnew(__FILE__, __LINE__, __PRETTY_FUNCTION__)
#else /* end of __GNUC__ >= 2 */
# define buf_new() mix3_bufnew(__FILE__, __LINE__, "file")
#endif /* else if not __GNUC__ >= 2 */
#endif /* DEBUG */

#endif /* not _MIX3_H */
