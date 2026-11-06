#define _POSIX_C_SOURCE 200809L
#include "generation.h"
#include "import.h"
#include <sys/stat.h>
#include <dirent.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int hook(const char *phase)
{
#ifdef JF_TESTING
	return jf_generation_test_hook(phase);
#else
	(void)phase; return 0;
#endif
}

static char *join(const char *a,const char *b)
{
	size_t n=strlen(a),m=strlen(b); char *s;
	if (n>SIZE_MAX-m-2) return NULL;
	s=malloc(n+m+2); if (s) { memcpy(s,a,n); s[n]='/'; memcpy(s+n+1,b,m+1); } return s;
}

static int valid_id(const char *s)
{
	size_t i;
	if (strlen(s)!=64) return 0;
	for (i=0;i<64;i++) if (!((s[i]>='0' && s[i]<='9') || (s[i]>='a' && s[i]<='f'))) return 0;
	return 1;
}

static int regular(int fd)
{
	struct stat st;
	return !fstat(fd,&st) && S_ISREG(st.st_mode) && (st.st_mode&0777)==0600 && st.st_nlink==1;
}

static int syncfd(int fd,const char *phase)
{
	return hook(phase) || fsync(fd);
}

static int load_mode(int dir,const char *name,struct jf_bytes *b,size_t limit,int private,mode_t *mode)
{
	unsigned char bytes[16384]; ssize_t n;
	struct stat st;
	int fd=openat(dir,name,O_RDONLY|O_NOFOLLOW|O_NONBLOCK),rc=3;
	if (fd<0) return errno==ENOENT || errno==ELOOP ? 5 : 3;
	if (fstat(fd,&st)) goto done;
	if (!S_ISREG(st.st_mode) || (private && ((st.st_mode&0777)!=0600 || st.st_nlink!=1))) { rc=5; goto done; }
	if (mode) *mode=st.st_mode&0777;
	while ((n=read(fd,bytes,sizeof bytes))) {
		if (n<0) { if (errno==EINTR) continue; goto done; }
		if (!jf_bytes_fits(b->size,(size_t)n,limit)) { rc=5; goto done; }
		if (jf_bytes_add_limit(b,bytes,(size_t)n,limit)) goto done;
	}
	rc=0;
done:
	if (close(fd) && !rc) rc=3;
	return rc;
}

static int load(int dir,const char *name,struct jf_bytes *b,size_t limit)
{
	return load_mode(dir,name,b,limit,1,NULL);
}

static int save(int dir,const char *name,const void *data,size_t size,const char *phase)
{
	size_t pos=0; int fd,rc=3;
	if (hook(phase)) return 3;
	fd=openat(dir,name,O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW,0600); if (fd<0) return 3;
	while (pos<size) {
		ssize_t n=write(fd,(const unsigned char *)data+pos,size-pos);
		if (n<0 && errno==EINTR) continue;
		if (n<=0) goto done;
		pos+=(size_t)n;
	}
	if (syncfd(fd,"fsync-file")) goto done;
	rc=0;
done:
	if (close(fd)) rc=3;
	return rc;
}

static int directory(int parent,const char *name,int create)
{
	int fd; struct stat st;
	if (create && mkdirat(parent,name,0700) && errno!=EEXIST) return -1;
	fd=openat(parent,name,O_RDONLY|O_DIRECTORY|O_NOFOLLOW);
	if (fd<0) return -1;
	if (fstat(fd,&st) || (st.st_mode&0777)!=0700) { close(fd); errno=EINVAL; return -1; }
	return fd;
}

struct location { int root,engine,generations,lock; };
static void location_close(struct location *l)
{
	if (l->lock>=0) close(l->lock);
	if (l->generations>=0) close(l->generations);
	if (l->engine>=0) close(l->engine);
	if (l->root>=0) close(l->root);
	l->root=l->engine=l->generations=l->lock=-1;
}

static int locate(const char *root,int create,struct location *l)
{
	int cache=-1;
	l->root=l->engine=l->generations=l->lock=-1;
	l->root=open(root,O_RDONLY|O_DIRECTORY); if (l->root<0) return 3;
	/* The legacy cache directory may already be 0755. Only native engine and
	 * generation directories have the new private-directory contract. */
	if (create && mkdirat(l->root,"cache",0700) && errno!=EEXIST) goto bad;
	cache=openat(l->root,"cache",O_RDONLY|O_DIRECTORY|O_NOFOLLOW); if (cache<0) goto bad;
	l->engine=directory(cache,"filter-engine",create);
	if (l->engine<0) goto bad;
	l->generations=directory(l->engine,"generations",create); if (l->generations<0) goto bad;
	l->lock=openat(l->engine,"lock",O_RDWR|O_NOFOLLOW|(create ? O_CREAT : 0),0600);
	if (l->lock<0 || !regular(l->lock)) goto bad;
	/* Persist children before their links in each immediate parent. Repeat
	 * even for existing directories: a prior invocation may have failed its
	 * parent fsync after mkdir succeeded. Readers must never initialize or
	 * flush this hierarchy. Keep legacy cache permissions unchanged. */
	if (create && (syncfd(l->generations,"fsync-hierarchy-generations") ||
		syncfd(l->engine,"fsync-hierarchy-engine") ||
		syncfd(cache,"fsync-hierarchy-cache") ||
		syncfd(l->root,"fsync-hierarchy-root") || hook("hierarchy-synced"))) goto bad;
	close(cache); cache=-1;
	return 0;
bad:
	{ int missing=errno==ENOENT; if (cache>=0) close(cache); location_close(l); return !create && missing ? 5 : 3; }
}

static int lock(struct location *l,int exclusive)
{
	struct flock f;
	memset(&f,0,sizeof f); f.l_type=exclusive ? F_WRLCK : F_RDLCK; f.l_whence=SEEK_SET;
	if (!fcntl(l->lock,F_SETLK,&f)) return 0;
	return errno==EACCES || errno==EAGAIN ? 6 : 3;
}

static void unlock(struct location *l)
{
	struct flock f;
	memset(&f,0,sizeof f); f.l_type=F_UNLCK; f.l_whence=SEEK_SET; (void)fcntl(l->lock,F_SETLK,&f);
}

static int same_root(struct location *l,const char *path)
{
	struct stat opened,current;
	return !fstat(l->root,&opened) && !stat(path,&current) && opened.st_dev==current.st_dev && opened.st_ino==current.st_ino;
}

static int exists(int dir,const char *name)
{
	struct stat st;
	if (!fstatat(dir,name,&st,AT_SYMLINK_NOFOLLOW)) return 1;
	return errno==ENOENT ? 0 : -1;
}

static int selector(struct location *l,char id[65])
{
	char target[80]; ssize_t n=readlinkat(l->engine,"current",target,sizeof target-1);
	id[0]=0;
	if (n<0) return errno==ENOENT ? 0 : 5;
	target[n]=0;
	if (n!=76 || strncmp(target,"generations/",12) || !valid_id(target+12)) return 5;
	memcpy(id,target+12,65); return 0;
}

static int manifest(const struct jf_generation *g,struct jf_bytes *out)
{
	unsigned char digest[32]; char c[65],z[65],s[65],text[400];
	jf_digest(g->cache.data,g->cache.size,digest); jf_digest_hex(digest,c);
	jf_digest(g->zsh.data,g->zsh.size,digest); jf_digest_hex(digest,z);
	jf_digest(g->sieve.data,g->sieve.size,digest); jf_digest_hex(digest,s);
	snprintf(text,sizeof text,"{\"schema\":1,\"generation\":\"%s\",\"policy_sha256\":\"%s\",\"zsh_sha256\":\"%s\",\"sieve_sha256\":\"%s\"}\n",g->id,c,z,s);
	return jf_bytes_text(out,text);
}

static int validate_directory(int fd,const char *id,struct jf_generation *g,struct jf_error *error)
{
	struct jf_bytes actual={0},expected={0};
	DIR *dir=NULL; struct dirent *ent; unsigned found=0;
	int copy=-1,rc=5;
	memset(g,0,sizeof *g); g->directory=-1; strcpy(g->id,id);
	copy=dup(fd); if (copy<0 || !(dir=fdopendir(copy))) { rc=3; goto done; }
	copy=-1; errno=0;
	while ((ent=readdir(dir))) {
		unsigned bit=!strcmp(ent->d_name,"policy.bin") ? 1 : !strcmp(ent->d_name,"filters.zsh") ? 2 :
			!strcmp(ent->d_name,"Filters.sieve") ? 4 : !strcmp(ent->d_name,"manifest.json") ? 8 : 0;
		if (!strcmp(ent->d_name,".") || !strcmp(ent->d_name,"..")) continue;
		if (!bit || found&bit) goto done;
		found|=bit; errno=0;
	}
	if (errno) { rc=3; goto done; }
	if (found!=15) goto done;
	closedir(dir); dir=NULL;
	if ((rc=load(fd,"policy.bin",&g->cache,JF_CACHE_FILE_MAX)) || (rc=load(fd,"filters.zsh",&g->zsh,JF_CACHE_MAX)) ||
		(rc=load(fd,"Filters.sieve",&g->sieve,JF_CACHE_MAX)) || (rc=load(fd,"manifest.json",&actual,4096))) goto done;
	if ((rc=jf_cache_validate(g->cache.data,g->cache.size,&g->zsh,&g->sieve,NULL,error))) goto done;
	{ char hex[65]; jf_digest_hex(g->cache.data+24,hex); if (strcmp(hex,id)) { rc=5; goto done; } }
	if ((rc=manifest(g,&expected))) goto done;
	if (actual.size!=expected.size || memcmp(actual.data,expected.data,actual.size)) { rc=5; goto done; }
	g->directory=dup(fd); if (g->directory<0) { rc=3; goto done; }
	rc=0;
done:
	if (dir) closedir(dir);
	if (copy>=0) close(copy);
	jf_bytes_free(&actual); jf_bytes_free(&expected);
	if (rc) jf_generation_close(g);
	return rc;
}

void jf_generation_close(struct jf_generation *g)
{
	if (g->directory>=0) close(g->directory);
	jf_bytes_free(&g->cache); jf_bytes_free(&g->zsh); jf_bytes_free(&g->sieve);
	memset(g,0,sizeof *g); g->directory=-1;
}

int jf_generation_open(const struct jf_generation *g,const char *name)
{
	int fd;
	if (strcmp(name,"policy.bin") && strcmp(name,"filters.zsh") && strcmp(name,"Filters.sieve") && strcmp(name,"manifest.json")) { errno=EINVAL; return -1; }
	fd=openat(g->directory,name,O_RDONLY|O_NOFOLLOW|O_NONBLOCK);
	if (fd>=0 && !regular(fd)) { close(fd); fd=-1; errno=EINVAL; }
	return fd;
}

static int committed_journal(struct location *,char [65]);
static int recover_inputs(struct location *,int);

static int unfinished(struct location *l)
{
	char id[65];
	return (exists(l->engine,"journal")!=0 || exists(l->engine,"import.journal")!=0) && !committed_journal(l,id);
}

static int public_status(struct location *l,const struct jf_generation *g)
{
	struct jf_bytes public={0};
	int rc=load(l->root,"Filters.sieve",&public,JF_CACHE_MAX);
	if (!rc && (public.size!=g->sieve.size || memcmp(public.data,g->sieve.data,public.size))) rc=5;
	jf_bytes_free(&public); return rc;
}

static int pin_locked(struct location *l,const char *requested,struct jf_generation *g,struct jf_error *error)
{
	char id[65],committed[65]; int fd,rc,transaction=exists(l->engine,"journal");
	if ((transaction || exists(l->engine,"import.journal")) && !committed_journal(l,committed)) return 5;
	if (requested) { if (!valid_id(requested)) return 5; strcpy(id,requested); }
	else { if (selector(l,id) || !*id) return 5; }
	fd=directory(l->generations,id,0); if (fd<0) return 5;
	rc=validate_directory(fd,id,g,error); close(fd);
	if (!rc && transaction) {
		if (!strcmp(id,committed)) rc=public_status(l,g);
		else {
			struct jf_generation current={0}; current.directory=-1;
			fd=directory(l->generations,committed,0);
			rc=fd<0 ? 5 : validate_directory(fd,committed,&current,error);
			if (!rc) rc=public_status(l,&current);
			if (fd>=0) close(fd);
			jf_generation_close(&current);
		}
	}
	if (rc) jf_generation_close(g);
	return rc;
}

int jf_generation_pin(const char *root,const char *id,struct jf_generation *g,struct jf_error *error)
{
	struct location l; int rc;
	memset(g,0,sizeof *g); g->directory=-1;
	if ((rc=locate(root,0,&l))) return rc;
	if (!(rc=lock(&l,0))) { rc=pin_locked(&l,id,g,error); unlock(&l); }
	location_close(&l); return rc;
}

static int copy_file(int from,const char *name,int to,const char *output,const char *phase)
{
	struct jf_bytes bytes={0}; int rc=load(from,name,&bytes,JF_CACHE_MAX);
	if (!rc) rc=save(to,output,bytes.data,bytes.size,phase);
	jf_bytes_free(&bytes); return rc;
}

static int remove_file(int dir,const char *name)
{
	return unlinkat(dir,name,0) && errno!=ENOENT ? 3 : 0;
}

static int set_selector(struct location *l,const char *id,const char *temporary,const char *phase)
{
	char target[80];
	if (!*id) return remove_file(l->engine,"current");
	snprintf(target,sizeof target,"generations/%s",id);
	if (remove_file(l->engine,temporary) || symlinkat(target,l->engine,temporary) || hook(phase) ||
		renameat(l->engine,temporary,l->engine,"current")) return 3;
	return 0;
}

static int cleanup_transaction(struct location *l)
{
	/* Journal removal is the recovery fence. Backups survive until that removal
	 * has been fsynced, so rollback can be repeated after any interruption. */
	if (remove_file(l->engine,"journal") || fsync(l->engine)) return 3;
	if (remove_file(l->engine,"export.backup") || remove_file(l->engine,"export.next") ||
		remove_file(l->engine,"export.restore") || remove_file(l->engine,"journal.next") ||
		remove_file(l->engine,"current.next") || remove_file(l->engine,"current.rollback") ||
		remove_file(l->engine,"committed.next") || remove_file(l->engine,"committed") || fsync(l->engine)) return 3;
	return 0;
}

static int read_journal(struct location *l,char old[65],int *present,unsigned *mode,char next[65])
{
	struct jf_bytes journal={0}; char canonical[180]; int n=0,rc=3;
	if (load(l->engine,"journal",&journal,256) || jf_bytes_add(&journal,"",1)) goto done;
	if (sscanf((char *)journal.data,"JFT1 %64s %1d %3o %64s%n",old,present,mode,next,&n)!=4 ||
		(size_t)n+2!=journal.size || journal.data[n]!='\n' ||
		(strcmp(old,"-") && !valid_id(old)) || !valid_id(next) || *mode>0777 || (*present!=0 && *present!=1)) goto done;
	snprintf(canonical,sizeof canonical,"JFT1 %s %d %o %s\n",old,*present,*mode,next);
	if (strlen(canonical)+1!=journal.size || memcmp(canonical,journal.data,journal.size)) goto done;
	rc=0;
done:
	jf_bytes_free(&journal); return rc;
}

static int committed_journal(struct location *l,char next[65])
{
	char old[65],selected[65]; struct jf_bytes marker={0}; int present,valid=0; unsigned mode;
	if (read_journal(l,old,&present,&mode,next) || load(l->engine,"committed",&marker,64) ||
		marker.size!=64 || memcmp(marker.data,next,64) || selector(l,selected) || strcmp(selected,next)) goto done;
	valid=1;
done:
	jf_bytes_free(&marker); return valid;
}

static void input_name(char name[40],size_t i,const char *suffix)
{
	snprintf(name,40,"import-%zu.%s",i,suffix);
}

static int recover_inputs(struct location *l,int committed)
{
	struct jf_bytes journal={0}; size_t i; int rc=3;
	int changed[4]={0},present[4]={0}; unsigned modes[4]={0};
	if (exists(l->engine,"import.journal")) {
		char canonical[160]; size_t offset=0; int pos=0,n;
		if (load(l->engine,"import.journal",&journal,160) || jf_bytes_add(&journal,"",1)) goto done;
		for (i=0;i<4;i++) {
			if (sscanf((char *)journal.data+offset,"%1d %1d %3o%n",&changed[i],&present[i],&modes[i],&pos)!=3 ||
				changed[i]>1 || present[i]>1 || modes[i]>0777 || journal.data[offset+(size_t)pos]!='\n') goto done;
			n=snprintf(canonical,sizeof canonical,"%d %d %o\n",changed[i],present[i],modes[i]);
			if (n!=pos+1 || offset+(size_t)n>=journal.size || memcmp(canonical,journal.data+offset,(size_t)n)) goto done;
			offset+=(size_t)n;
		}
		if (offset+1!=journal.size) goto done;
		if (!committed) for (i=0;i<4;i++) if (changed[i]) {
			char backup[40],restore[40]; int fd;
			input_name(backup,i,"backup"); input_name(restore,i,"restore");
			if (present[i]) {
				if (remove_file(l->engine,restore) || copy_file(l->engine,backup,l->engine,restore,"import-restore")) goto done;
				fd=openat(l->engine,restore,O_RDONLY|O_NOFOLLOW); if (fd<0) goto done;
				if (fchmod(fd,(mode_t)modes[i]) || fsync(fd)) { close(fd); goto done; } close(fd);
				if (renameat(l->engine,restore,l->root,jf_import_names[i])) goto done;
			} else if (remove_file(l->root,jf_import_names[i])) goto done;
		}
		if (fsync(l->root) || fsync(l->engine) || remove_file(l->engine,"import.journal") || fsync(l->engine)) goto done;
	}
	for (i=0;i<4;i++) {
		char name[40]; size_t j; const char *suffixes[]={"backup","next","restore"};
		for (j=0;j<3;j++) { input_name(name,i,suffixes[j]); if (remove_file(l->engine,name)) goto done; }
	}
	if (remove_file(l->engine,"import.journal.next") || fsync(l->engine)) goto done;
	rc=0;
done:
	jf_bytes_free(&journal); return rc;
}

static int prepare_inputs(struct location *l,const struct jf_bytes *original,const struct jf_bytes *output,
	const int *present,const mode_t *modes,const int *changed)
{
	size_t i; struct jf_bytes journal={0}; int rc=3;
	for (i=0;i<4;i++) {
		char name[40],line[40];
		if (changed[i]) {
			input_name(name,i,"backup");
			if (present[i] && save(l->engine,name,original[i].data,original[i].size,"import-backup")) goto done;
			input_name(name,i,"next");
			if (save(l->engine,name,output[i].data,output[i].size,"import-stage")) goto done;
		}
		snprintf(line,sizeof line,"%d %d %o\n",changed[i],present[i],(unsigned)modes[i]);
		if (jf_bytes_text(&journal,line)) goto done;
	}
	if (save(l->engine,"import.journal.next",journal.data,journal.size,"import-journal") ||
		renameat(l->engine,"import.journal.next",l->engine,"import.journal") || syncfd(l->engine,"import-fsync-journal") ||
		hook("import-prepared")) goto done;
	for (i=0;i<4;i++) if (changed[i]) {
		char name[40]; int fd; input_name(name,i,"next");
		fd=openat(l->engine,name,O_RDONLY|O_NOFOLLOW); if (fd<0) goto done;
		if (fchmod(fd,modes[i]) || fsync(fd)) { close(fd); goto done; } close(fd);
		if (hook("import-rename-input") || renameat(l->engine,name,l->root,jf_import_names[i]) || hook("import-input-renamed")) goto done;
	}
	if (syncfd(l->root,"import-fsync-root") || hook("import-inputs-published")) goto done;
	rc=0;
done:
	jf_bytes_free(&journal); return rc;
}

static int recover(struct location *l)
{
	struct jf_bytes marker={0};
	char old[65],next[65]; int public_present,rc=3; unsigned mode;
	if (exists(l->engine,"journal")==0) {
		if (recover_inputs(l,0)) return 3;
		return cleanup_transaction(l);
	}
	if (read_journal(l,old,&public_present,&mode,next)) goto done;
	if (exists(l->engine,"committed")) {
		struct jf_generation committed={0}; committed.directory=-1;
		if (load(l->engine,"committed",&marker,64) || marker.size!=64 || memcmp(marker.data,next,64)) goto done;
		rc=pin_locked(l,NULL,&committed,NULL);
		if (rc) { jf_generation_close(&committed); goto done; }
		jf_generation_close(&committed);
	} else {
		if (public_present) {
			int fd;
			if (remove_file(l->engine,"export.restore") || copy_file(l->engine,"export.backup",l->engine,"export.restore","recover-copy")) goto done;
			fd=openat(l->engine,"export.restore",O_RDONLY|O_NOFOLLOW);
			if (fd<0) goto done;
			if (fchmod(fd,(mode_t)mode) || fsync(fd)) { close(fd); goto done; }
			close(fd);
			if (renameat(l->engine,"export.restore",l->root,"Filters.sieve")) goto done;
		} else if (remove_file(l->root,"Filters.sieve")) goto done;
		if (set_selector(l,!strcmp(old,"-") ? "" : old,"current.rollback","recover-selector") || fsync(l->root) || fsync(l->engine)) goto done;
	}
	/* The same commit marker covers inputs, public export and selector. Keep
	 * the main journal until input recovery/cleanup is durable and repeatable. */
	if ((rc=recover_inputs(l,marker.size==64))) goto done;
	rc=cleanup_transaction(l);
done:
	jf_bytes_free(&marker); return rc;
}

static int publish(struct location *l,const char *old,const struct jf_products *p)
{
	char text[180]; int public_present=exists(l->root,"Filters.sieve"),rc=3,prepared=0;
	mode_t mode=0600;
	if (public_present<0) return 3;
	if (public_present) {
		struct jf_bytes backup={0}; int failed=load_mode(l->root,"Filters.sieve",&backup,JF_CACHE_MAX,0,&mode);
		if (!failed) failed=save(l->engine,"export.backup",backup.data,backup.size,"write-backup");
		jf_bytes_free(&backup); if (failed) goto done;
	}
	if (save(l->engine,"export.next",p->sieve.data,p->sieve.size,"write-export")) goto done;
	snprintf(text,sizeof text,"JFT1 %s %d %o %s\n",*old ? old : "-",public_present,(unsigned)mode,p->id);
	if (save(l->engine,"journal.next",text,strlen(text),"write-journal") ||
		renameat(l->engine,"journal.next",l->engine,"journal") || syncfd(l->engine,"fsync-journal")) goto done;
	prepared=1;
	if (hook("prepared") || hook("rename-export") || renameat(l->engine,"export.next",l->root,"Filters.sieve") ||
		hook("export-renamed") || set_selector(l,p->id,"current.next","rename-current") || hook("selector-renamed") ||
		syncfd(l->root,"fsync-root") || syncfd(l->engine,"fsync-engine") ||
		save(l->engine,"committed.next",p->id,64,"write-commit") || hook("commit-written") || hook("rename-commit") ||
		renameat(l->engine,"committed.next",l->engine,"committed") || syncfd(l->engine,"fsync-commit")) goto done;
	if (hook("committed")) goto done;
	/* Commit is durable. Leave cleanup to the next exclusive invocation; this
	 * avoids a fallible cleanup operation after reporting a committed outcome.
	 * Readers accept only a fully verified committed journal. */
	return 0;
done:
	/* An ordinary reported failure rolls back, even if a commit marker was
	 * just created. A crash instead leaves the durable marker for recovery. */
	if (remove_file(l->engine,"committed")) return 3;
	if (prepared || exists(l->engine,"journal")==1) (void)recover(l);
	else (void)cleanup_transaction(l);
	return rc;
}

static void discard_stage(int engine,const char *name)
{
	static const char *files[]={"policy.bin","filters.zsh","Filters.sieve","manifest.json"};
	size_t i; int fd=directory(engine,name,0);
	if (fd<0) return;
	for (i=0;i<4;i++) (void)unlinkat(fd,files[i],0);
	close(fd); (void)unlinkat(engine,name,AT_REMOVEDIR);
}

static int stage(struct location *l,const char *engine_path,const struct jf_products *p,char **name,struct jf_error *error)
{
	char *path=join(engine_path,".building-XXXXXX"); struct jf_generation g={0},verified={0};
	struct jf_bytes info={0}; int fd=-1,rc=3,created=0;
	verified.directory=-1; *name=NULL;
	if (!path || !mkdtemp(path)) goto done;
	created=1;
	if (hook("stage-name")) goto done;
	*name=strdup(strrchr(path,'/')+1); if (!*name) goto done;
	fd=directory(l->engine,*name,0); if (fd<0) goto done;
	strcpy(g.id,p->id); g.cache=p->cache; g.zsh=p->zsh; g.sieve=p->sieve;
	if (manifest(&g,&info) || save(fd,"policy.bin",p->cache.data,p->cache.size,"write-product") ||
		save(fd,"filters.zsh",p->zsh.data,p->zsh.size,"write-zsh") ||
		save(fd,"Filters.sieve",p->sieve.data,p->sieve.size,"write-sieve") ||
		save(fd,"manifest.json",info.data,info.size,"write-manifest") || syncfd(fd,"fsync-stage") ||
		hook("validate") || validate_directory(fd,p->id,&verified,error)) goto done;
	rc=0;
done:
	if (fd>=0) close(fd);
	if (created && !*name) (void)unlinkat(l->engine,strrchr(path,'/')+1,AT_REMOVEDIR);
	free(path); jf_bytes_free(&info); jf_generation_close(&verified);
	return rc;
}

void jf_compile_result_free(struct jf_compile_result *r)
{
	free(r->directory); jf_bytes_free(&r->warnings); memset(r,0,sizeof *r);
}

int jf_generation_legacy_run(const char *root,char *const argv[])
{
	struct location l={-1,-1,-1,-1}; int rc,held;
	if ((rc=locate(root,1,&l)) || (rc=lock(&l,1)) || (rc=recover(&l))) goto done;
	/* POSIX locks survive exec, but not fork. Replace this process while keeping
	 * the non-CLOEXEC lock descriptor open: no supervising parent can die and
	 * release the lock while an orphan continues to publish a legacy export. */
	held=l.lock; l.lock=-1; location_close(&l);
	execvp(argv[0],argv);
	rc=errno==ENOENT ? 127 : 126;
	close(held);
done:
	location_close(&l); return rc;
}

int jf_compile_import(const char *root,const char *accounts,const char *profile,const char *compiler,
	const char *script,int legacy,int check,struct jf_compile_result *result,struct jf_error *error,char **error_path)
{
	struct location l={-1,-1,-1,-1}; struct jf_snapshot before={0},after={0},recheck={0};
	struct jf_policy *imported=NULL; struct jf_products products={0}; struct jf_bytes input={0};
	struct jf_bytes original[4]={{0}},output[4]={{0}};
	struct jf_input_override overrides[4]; char *paths[4]={0},*temporary=NULL,*engine=join(root,"cache/filter-engine");
	int present[4]={0},changed[4]={0},rc=3,fd=-1,prepared=0,stage_ready=0; mode_t modes[4]={0600,0600,0600,0600};
	unsigned char fingerprint[32],again[32]; char old[65]; size_t i,count=0;
	struct stat st;
	memset(result,0,sizeof *result); *error_path=NULL; if (error) memset(error,0,sizeof *error);
	if (!engine) goto done;
	/* Snapshot the source descriptor; never modify or remove the import file. */
	fd=open(script,O_RDONLY|O_NONBLOCK);
	if (fd<0 || fstat(fd,&st) || !S_ISREG(st.st_mode)) goto script_error;
	for (;;) {
		unsigned char bytes[16384]; ssize_t n=read(fd,bytes,sizeof bytes);
		if (n<0 && errno==EINTR) continue;
		if (n<0) goto script_error;
		if (!n) break;
		if (!jf_bytes_fits(input.size,(size_t)n,JF_INPUT_MAX)) { rc=4; jf_product_error(error,"E_SIEVE_IMPORT",0,"script exceeds 64 MiB"); goto script_error; }
		if (jf_bytes_add(&input,bytes,(size_t)n)) goto script_error;
	}
	if (close(fd)) { fd=-1; goto script_error; } fd=-1;
	rc=locate(root,!check,&l);
	if (rc && !(check && rc==5)) goto done;
	if (!rc && ((rc=lock(&l,!check)) || (check ? unfinished(&l) ? (rc=5) : 0 : (rc=recover(&l))))) goto done;
	if (l.root>=0 && !same_root(&l,root)) { rc=6; goto done; }
	if ((rc=jf_snapshot_read(root,accounts,profile,compiler,NULL,&before,error))) goto source_error;
	if ((rc=jf_sieve_import(&input,legacy,&before.profile,&imported,error))) goto script_error;
	if (legacy) fputs("W_LEGACY_SIEVE recovered generated header-contains rules; policy-v1 parsed-address/ERE semantics replace legacy substring behavior\n",stderr);
	/* Read complete originals for byte-preserving merge and rollback. Import
	 * deliberately refuses to replace input symlinks or nonregular objects. */
	for (i=0;i<4;i++) {
		int rootfd=l.root;
		paths[i]=join(root,jf_import_names[i]); if (!paths[i]) { rc=3; goto done; }
		if (rootfd<0) { rootfd=open(root,O_RDONLY|O_DIRECTORY); if (rootfd<0) { rc=3; goto done; } }
		present[i]=exists(rootfd,jf_import_names[i]);
		rc=present[i]<0 ? 3 : present[i] ? load_mode(rootfd,jf_import_names[i],&original[i],JF_INPUT_MAX,0,&modes[i]) : 0;
		if (l.root<0) close(rootfd);
		if (rc) { jf_product_error(error,"E_IMPORT_INPUT",0,"import inputs must be regular files, never replacement symlinks"); goto done; }
	}
	if ((rc=jf_import_merge(before.policy,imported,original,output,error))) goto done;
	for (i=0;i<4;i++) {
		changed[i]=original[i].size!=output[i].size || (original[i].size && memcmp(original[i].data,output[i].data,original[i].size));
		if (changed[i]) { overrides[count].path=paths[i]; overrides[count++].bytes=&output[i]; }
	}
	if ((rc=jf_snapshot_overlay(root,accounts,profile,compiler,overrides,count,&after,error)) ||
		(rc=jf_snapshot_parse(&after,error)) ||
		(rc=jf_products_build(after.policy,&after.profile,after.dependencies,after.dependency_count,&products,error))) goto source_error;
	if ((rc=jf_dependencies_fingerprint(before.dependencies,before.dependency_count,fingerprint)) ||
		(rc=jf_snapshot_collect(root,accounts,profile,compiler,&before,&recheck,error)) ||
		(rc=jf_dependencies_fingerprint(recheck.dependencies,recheck.dependency_count,again))) goto source_error;
	if (memcmp(fingerprint,again,32)) { rc=6; goto done; }
	if (check) { strcpy(result->id,products.id); rc=0; goto done; }
	if ((rc=selector(&l,old)) || (rc=stage(&l,engine,&products,&temporary,error))) goto done;
	stage_ready=1;
	if (!same_root(&l,root)) { rc=6; goto done; }
	if (exists(l.generations,products.id)==0) {
		if (hook("rename-generation") || renameat(l.engine,temporary,l.generations,products.id)) { rc=3; goto done; }
		stage_ready=0;
	} else {
		struct jf_generation existing={0}; int directory_fd=directory(l.generations,products.id,0); existing.directory=-1;
		rc=directory_fd<0 ? 5 : validate_directory(directory_fd,products.id,&existing,error);
		if (directory_fd>=0) close(directory_fd);
		if (!rc && (existing.cache.size!=products.cache.size || memcmp(existing.cache.data,products.cache.data,products.cache.size))) rc=5;
		jf_generation_close(&existing); if (rc) goto done;
	}
	if (syncfd(l.generations,"fsync-generations")) { rc=3; goto done; }
	/* Staging/exporter work may take time. Recheck the complete original
	 * inventory immediately before preparing any input publication. */
	jf_snapshot_free(&recheck);
	if ((rc=jf_snapshot_collect(root,accounts,profile,compiler,&before,&recheck,error)) ||
		(rc=jf_dependencies_fingerprint(recheck.dependencies,recheck.dependency_count,again))) goto source_error;
	if (memcmp(fingerprint,again,32)) { rc=6; goto done; }
	{
		char *generations=join(engine,"generations");
		result->directory=generations ? join(generations,products.id) : NULL; free(generations);
		if (!result->directory) { rc=3; goto done; }
	}
	if (count) {
		prepared=1;
		if ((rc=prepare_inputs(&l,original,output,present,modes,changed))) goto done;
	}
	if (!strcmp(old,products.id) && !count) {
		struct jf_generation same={0}; same.sieve=products.sieve;
		rc=public_status(&l,&same);
		if (rc==5) { rc=publish(&l,old,&products); if (!rc) result->changed=1; }
	} else { rc=publish(&l,old,&products); if (!rc) result->changed=1; }
	if (!rc) strcpy(result->id,products.id);
	goto done;
script_error:
	*error_path=strdup(script); goto done;
source_error:
	{
		const struct jf_rule *r=error && error->code && !strcmp(error->code,"E_REGEX_EXPORT") ? jf_policy_rule(after.policy,error->source) : NULL;
		*error_path=strdup(r ? r->source : after.error_path ? after.error_path : before.error_path ? before.error_path : root);
	}
done:
	if (rc && prepared && l.engine>=0) { (void)remove_file(l.engine,"committed"); (void)recover(&l); }
	if (!rc) { result->warnings=after.warnings; memset(&after.warnings,0,sizeof after.warnings); }
	if (temporary && (stage_ready || exists(l.engine,temporary)==1)) discard_stage(l.engine,temporary);
	if (rc && error && !error->code) jf_product_error(error,rc==6 ? "E_BUSY_OR_CHANGED" : "E_IMPORT_IO",0,"import failed; no partial configuration is committed");
	if (fd>=0) close(fd);
	for (i=0;i<4;i++) { free(paths[i]); jf_bytes_free(&original[i]); jf_bytes_free(&output[i]); }
	jf_bytes_free(&input); jf_policy_free(imported); jf_snapshot_free(&before); jf_snapshot_free(&after); jf_snapshot_free(&recheck);
	jf_products_free(&products); free(temporary); free(engine); location_close(&l); return rc;
}

int jf_compile(const char *root,const char *accounts,const char *profile,const char *compiler,int check,
	struct jf_compile_result *result,struct jf_error *error,char **error_path)
{
	struct location l={-1,-1,-1,-1}; struct jf_generation previous={0}; struct jf_snapshot prior={0};
	char *cache_path=join(root,"cache/filter-engine"),old[65]; int rc,attempt;
	memset(result,0,sizeof *result); *error_path=NULL; previous.directory=-1;
	if (error) memset(error,0,sizeof *error);
	if (!cache_path) { rc=3; goto final; }
	rc=locate(root,!check,&l);
	if (rc && !(check && rc==5)) goto final;
	if (!rc) {
		if (check) {
			if ((rc=lock(&l,0))) goto done;
			if (unfinished(&l)) { rc=5; goto done; }
		} else {
			if ((rc=lock(&l,1)) || (rc=recover(&l))) goto done;
		}
		if ((rc=selector(&l,old))) goto done;
		if (*old) {
			if ((rc=pin_locked(&l,NULL,&previous,error)) ||
				(rc=jf_cache_dependencies(&previous.cache,&prior.dependencies,&prior.dependency_count))) goto done;
		}
		unlock(&l);
	}
	for (attempt=0;attempt<3;attempt++) {
		struct jf_snapshot snapshot={0},recheck={0}; struct jf_products products={0};
		unsigned char fingerprint[32],again[32]; char id[65],*temporary=NULL;
		int stage_ready=0,retry=0,collected=0,rechecked=0;
		if (l.engine>=0 && (rc=lock(&l,0))) goto iteration;
		if (l.engine>=0 && !same_root(&l,root)) { rc=6; retry=1; goto iteration; }
		if (l.engine>=0 && unfinished(&l)) { rc=5; goto iteration; }
		rc=jf_snapshot_collect(root,accounts,profile,compiler,&prior,&snapshot,error);
		if (!rc) collected=1;
		if (rc==6) retry=1;
		if (l.engine>=0) unlock(&l);
		if (rc) goto iteration;
		if (hook("snapshot")) { rc=3; goto iteration; }
		if ((rc=jf_dependencies_fingerprint(snapshot.dependencies,snapshot.dependency_count,fingerprint))) goto iteration;
		jf_digest_hex(fingerprint,id);
		if (!check && previous.directory>=0 && !strcmp(id,previous.id)) {
			struct jf_generation warm={0}; warm.directory=-1;
			int repair=0;
			if ((rc=lock(&l,0))) goto iteration;
			rc=jf_snapshot_collect(root,accounts,profile,compiler,&snapshot,&recheck,error);
			if (!rc) rechecked=1;
			if (!rc) rc=jf_dependencies_fingerprint(recheck.dependencies,recheck.dependency_count,again);
			if (!rc && memcmp(again,fingerprint,32)) { rc=6; retry=1; }
			if (!rc) rc=pin_locked(&l,NULL,&warm,error);
			if (!rc && strcmp(warm.id,id)) { rc=6; retry=1; }
			if (!rc) { rc=public_status(&l,&warm); if (rc==5) repair=1; }
			jf_generation_close(&warm);
			unlock(&l);
			if (!repair) {
				if (!rc) { strcpy(result->id,id); result->directory=join(cache_path,"generations");
					if (result->directory) { char *full=join(result->directory,id); free(result->directory); result->directory=full; }
					if (!result->directory) rc=3;
				}
				goto iteration;
			}
			/* A serialized legacy export can differ from the retained native
			 * generation. Revalidate and transactionally repair that public view. */
			rc=0; jf_snapshot_free(&recheck); rechecked=0;
		}
		if (hook("parse")) { rc=3; goto iteration; }
		if ((rc=jf_snapshot_parse(&snapshot,error)) ||
			(rc=jf_products_build(snapshot.policy,&snapshot.profile,snapshot.dependencies,snapshot.dependency_count,&products,error))) goto iteration;
		if (check) {
			if (l.engine>=0 && (rc=lock(&l,0))) goto iteration;
			rc=jf_snapshot_collect(root,accounts,profile,compiler,&snapshot,&recheck,error);
			if (!rc) rechecked=1;
			if (!rc) rc=jf_dependencies_fingerprint(recheck.dependencies,recheck.dependency_count,again);
			if (!rc && memcmp(again,fingerprint,32)) { rc=6; retry=1; }
			if (!rc) strcpy(result->id,products.id);
			goto iteration;
		}
		if ((rc=stage(&l,cache_path,&products,&temporary,error))) goto iteration;
		stage_ready=1;
		if ((rc=lock(&l,1)) || (rc=recover(&l))) goto iteration;
		if (!same_root(&l,root)) { rc=6; retry=1; goto iteration; }
		rc=jf_snapshot_collect(root,accounts,profile,compiler,&snapshot,&recheck,error);
		if (!rc) rechecked=1;
		if (rc || (rc=jf_dependencies_fingerprint(recheck.dependencies,recheck.dependency_count,again))) { if (recheck.changed) retry=1; goto iteration; }
		if (memcmp(fingerprint,again,32)) { rc=6; retry=1; goto iteration; }
		if ((rc=selector(&l,old))) goto iteration;
		if (exists(l.generations,products.id)==0) {
			if (hook("rename-generation") || renameat(l.engine,temporary,l.generations,products.id) || syncfd(l.generations,"fsync-generations")) { rc=3; goto iteration; }
			stage_ready=0;
		} else {
			struct jf_generation existing={0}; int fd=directory(l.generations,products.id,0); existing.directory=-1;
			rc=fd<0 ? 5 : validate_directory(fd,products.id,&existing,error);
			if (!rc && (existing.cache.size!=products.cache.size || existing.zsh.size!=products.zsh.size ||
				existing.sieve.size!=products.sieve.size || memcmp(existing.cache.data,products.cache.data,products.cache.size) ||
				memcmp(existing.zsh.data,products.zsh.data,products.zsh.size) ||
				memcmp(existing.sieve.data,products.sieve.data,products.sieve.size))) rc=5;
			if (fd>=0) close(fd);
			jf_generation_close(&existing); if (rc) goto iteration;
			/* A prior failed attempt may have renamed this otherwise complete
			 * directory but failed its parent fsync. Make reuse durable too. */
			if (syncfd(l.generations,"fsync-generations")) { rc=3; goto iteration; }
		}
		{
			char *generations=join(cache_path,"generations");
			result->directory=generations ? join(generations,products.id) : NULL; free(generations);
			if (!result->directory) { rc=3; goto iteration; }
		}
		if (!strcmp(old,products.id)) {
			struct jf_generation same={0}; same.sieve=products.sieve;
			rc=public_status(&l,&same);
			if (rc==5) { rc=publish(&l,old,&products); if (!rc) result->changed=1; }
		} else {
			rc=publish(&l,old,&products);
			if (!rc) result->changed=1;
		}
		if (!rc) strcpy(result->id,products.id);
	iteration:
		if (rc && !*error_path) {
			const struct jf_rule *r=error && error->code && !strcmp(error->code,"E_REGEX_EXPORT") ? jf_policy_rule(snapshot.policy,error->source) : NULL;
			const char *where=error && error->code && snapshot.error_path ? snapshot.error_path : root;
			if (error && error->code && !strcmp(error->code,"E_MAILBOX_MAPPING")) where=snapshot.profile_path;
			if (recheck.changed) where=recheck.error_path;
			*error_path=strdup(r ? r->source : where ? where : root);
		}
		if (l.engine>=0) unlock(&l);
		if (temporary && (stage_ready || exists(l.engine,temporary)==1)) discard_stage(l.engine,temporary);
		if (rc==6 && retry && attempt<2 && (collected || rechecked)) {
			struct jf_snapshot *latest=rechecked ? &recheck : &snapshot;
			jf_snapshot_free(&prior);
			prior.dependencies=latest->dependencies; prior.dependency_count=latest->dependency_count;
			latest->dependencies=NULL; latest->dependency_count=0;
		}
		if (!rc) { result->warnings=snapshot.warnings; memset(&snapshot.warnings,0,sizeof snapshot.warnings); }
		free(temporary); jf_snapshot_free(&snapshot); jf_snapshot_free(&recheck); jf_products_free(&products);
		if (rc!=6 || !retry || attempt==2) break;
		free(*error_path); *error_path=NULL;
	}
done:
	location_close(&l);
final:
	if (rc && error && !error->code) jf_product_error(error,rc==6 ? "E_BUSY_OR_CHANGED" : rc==5 ? "E_GENERATION" : "E_IO",0,"native generation unavailable or publication failed");
	jf_generation_close(&previous); jf_snapshot_free(&prior); free(cache_path);
	return rc;
}
