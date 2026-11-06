#define _GNU_SOURCE
#include "storage.h"
#include "sha256.h"
#include <sys/types.h>
#ifdef __linux__
#include <sys/syscall.h>
#endif
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define RETRIES 64u
#define COPY_CHUNK 65536u
struct owned_directory { int fd,parent,created; char name[80]; struct stat identity; };

static int hook(const char *phase)
{
#ifdef JF_TESTING
	return jf_generation_test_hook(phase);
#else
	(void)phase; return 0;
#endif
}

int jf_storage_same(const struct stat *a,const struct stat *b)
{
	return a->st_dev==b->st_dev && a->st_ino==b->st_ino && a->st_size==b->st_size &&
		a->st_mtim.tv_sec==b->st_mtim.tv_sec && a->st_mtim.tv_nsec==b->st_mtim.tv_nsec &&
		a->st_ctim.tv_sec==b->st_ctim.tv_sec && a->st_ctim.tv_nsec==b->st_ctim.tv_nsec;
}

static int inode(const struct stat *a,const struct stat *b)
{
	return a->st_dev==b->st_dev && a->st_ino==b->st_ino;
}

static int bytes_identity(const struct stat *a,const struct stat *b)
{
	return inode(a,b) && a->st_size==b->st_size &&
		a->st_mtim.tv_sec==b->st_mtim.tv_sec && a->st_mtim.tv_nsec==b->st_mtim.tv_nsec;
}

int jf_storage_directory(const char *path)
{
	char *copy=strdup(path),*component,*next;
	int fd=-1;
	if (!copy) return -1;
	fd=open(*path=='/' ? "/" : ".",O_RDONLY|O_DIRECTORY|O_CLOEXEC);
	for (component=copy;fd>=0 && *component;component=next) {
		int child;
		while (*component=='/') component++;
		if (!*component) break;
		next=strchr(component,'/');
		if (next) *next++=0; else next=component+strlen(component);
		child=openat(fd,component,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
		close(fd); fd=child;
	}
	free(copy); return fd;
}

static int syncfd(int fd,const char *phase)
{
	return hook(phase) || fsync(fd);
}

static int stopped(const struct jf_storage *s)
{
	return s->interrupted && *s->interrupted;
}

void jf_storage_close(struct jf_storage *s)
{
	if (s->random>=0) close(s->random);
	if (s->lock>=0) close(s->lock);
	if (s->engine>=0) close(s->engine);
	if (s->root>=0) close(s->root);
	s->root=s->engine=s->lock=s->random=-1;
}

int jf_storage_begin(const char *root,const struct jf_generation *generation,
	volatile sig_atomic_t *interrupted,struct jf_storage *s,const char **detail)
{
	struct flock lock={0}; struct stat actual,expected;
	int cache=-1,generations=-1,selected=-1,rc=3;
	size_t used=0;
	memset(s,0,sizeof *s); s->root=s->engine=s->lock=s->random=-1; s->interrupted=interrupted;
	s->root_path=root;
	*detail="E_BATCH_LOCK";
	s->root=jf_storage_directory(root); if (s->root<0 || fstat(s->root,&s->root_identity)) goto done;
	cache=openat(s->root,"cache",O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC); if (cache<0) goto done;
	s->engine=openat(cache,"filter-engine",O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC); if (s->engine<0) goto done;
	generations=openat(s->engine,"generations",O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC); if (generations<0) goto done;
	selected=openat(generations,generation->id,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
	if (selected<0 || fstat(selected,&actual) || fstat(generation->directory,&expected) || !inode(&actual,&expected)) {
		rc=5; *detail="E_GENERATION_CHANGED"; goto done;
	}
	s->random=open("/dev/urandom",O_RDONLY|O_CLOEXEC); if (s->random<0) goto done;
	while (used<sizeof s->nonce) {
		ssize_t n=read(s->random,s->nonce+used,sizeof s->nonce-used);
		if (n<0 && errno==EINTR && !stopped(s)) continue;
		if (n<=0) goto done;
		used+=(size_t)n;
	}
	s->lock=openat(s->engine,"batch.lock",O_RDWR|O_CREAT|O_NOFOLLOW|O_NONBLOCK|O_CLOEXEC,0600);
	if (s->lock<0 || fstat(s->lock,&s->lock_identity) || !S_ISREG(s->lock_identity.st_mode) ||
		s->lock_identity.st_nlink!=1 || (s->lock_identity.st_mode&0777)!=0600 ||
		s->lock_identity.st_uid!=geteuid()) goto done;
	lock.l_type=F_WRLCK; lock.l_whence=SEEK_SET;
	if (fcntl(s->lock,F_SETLK,&lock)) { if (errno==EACCES || errno==EAGAIN) rc=6; goto done; }
	if (fstatat(s->engine,"batch.lock",&actual,AT_SYMLINK_NOFOLLOW) || !inode(&actual,&s->lock_identity)) {
		rc=6; *detail="E_BATCH_LOCK_CHANGED"; goto done;
	}
	if (syncfd(s->lock,"store-sync-lock-file") || syncfd(s->engine,"store-sync-lock")) goto done;
	rc=0;
done:
	if (selected>=0) close(selected);
	if (generations>=0) close(generations);
	if (cache>=0) close(cache);
	if (rc) jf_storage_close(s);
	return rc;
}

static int directory_at(int parent,const char *name)
{
	int fd;
	if (mkdirat(parent,name,0700) && errno!=EEXIST) return -1;
	fd=openat(parent,name,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
	/* Also sync existing entries: a prior invocation could have failed its
	 * immediate parent fsync after successfully creating the directory. */
	if (fd>=0 && syncfd(parent,"store-sync-hierarchy")) { close(fd); return -1; }
	return fd;
}

static int destination(struct jf_storage *s,const char *relative,int *cur,int *new,int *tmp)
{
	char *copy=strdup(relative),*component,*next;
	int dir=dup(s->root),rc=-1;
	if (!copy || dir<0) goto done;
	for (component=copy;*component;component=next) {
		int child;
		next=strchr(component,'/'); if (next) *next++=0; else next=component+strlen(component);
		/* These were cache-validated. Retain the boundary here too. */
		if (!*component || !strcmp(component,".") || !strcmp(component,"..")) goto done;
		child=directory_at(dir,component); if (child<0) goto done;
		close(dir); dir=child;
	}
	*cur=directory_at(dir,"cur"); *new=directory_at(dir,"new"); *tmp=directory_at(dir,"tmp");
	if (*cur>=0 && *new>=0 && *tmp>=0) rc=0;
done:
	if (dir>=0) close(dir);
	free(copy); return rc;
}

static int destination_current(struct jf_storage *s,const char *relative,int cur,int tmp)
{
	char *copy=strdup(relative),*component,*next;
	struct stat opened,current;
	int dir=dup(s->root),a=-1,b=-1,ok=0;
	if (!copy || dir<0) goto done;
	for (component=copy;*component;component=next) {
		int child;
		next=strchr(component,'/'); if (next) *next++=0; else next=component+strlen(component);
		child=openat(dir,component,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
		if (child<0) goto done;
		close(dir); dir=child;
	}
	a=openat(dir,"cur",O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
	b=openat(dir,"tmp",O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
	ok=a>=0 && b>=0 && !fstat(a,&current) && !fstat(cur,&opened) && inode(&opened,&current) &&
		!fstat(b,&current) && !fstat(tmp,&opened) && inode(&opened,&current);
done:
	if (a>=0) close(a);
	if (b>=0) close(b);
	if (dir>=0) close(dir);
	free(copy); return ok;
}

static int create_owned(struct jf_storage *s,int parent,struct owned_directory *d)
{
	unsigned attempt,i; char hex[33];
	d->fd=-1; d->parent=parent; d->created=0; d->name[0]=0;
	for (i=0;i<16;i++) snprintf(hex+i*2,3,"%02x",s->nonce[i]);
	for (attempt=0;attempt<RETRIES;attempt++) {
		snprintf(d->name,sizeof d->name,".jaro-filter.%s.%zx",hex,++s->sequence);
		if (mkdirat(parent,d->name,0700)) { if (errno==EEXIST) continue; return -1; }
		d->created=1;
		if (fstatat(parent,d->name,&d->identity,AT_SYMLINK_NOFOLLOW)) return -1;
		d->fd=openat(parent,d->name,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
		if (d->fd<0) return -1;
		{ struct stat actual; if (fstat(d->fd,&actual) || !inode(&actual,&d->identity)) return -1; }
		return 0;
	}
	errno=EEXIST; return -1;
}

static int remove_owned(struct owned_directory *d)
{
	struct stat current;
	if (!d->created) return 0;
	if (fstatat(d->parent,d->name,&current,AT_SYMLINK_NOFOLLOW) || !inode(&current,&d->identity)) return -1;
	if (unlinkat(d->parent,d->name,AT_REMOVEDIR)) return -1;
	d->created=0;
	return syncfd(d->parent,"store-sync-cleanup-directory");
}

static int source_current(int parent,const char *name,int fd,const struct stat *expected)
{
	struct stat a,b;
	return !fstat(fd,&a) && !fstatat(parent,name,&b,AT_SYMLINK_NOFOLLOW) &&
		S_ISREG(b.st_mode) && jf_storage_same(&a,expected) && jf_storage_same(&b,expected);
}

static int lock_current(struct jf_storage *s)
{
	struct stat a,b;
	struct flock lock={0};
	if (fstat(s->lock,&a) || a.st_nlink!=1 || fstatat(s->engine,"batch.lock",&b,AT_SYMLINK_NOFOLLOW) ||
		!inode(&a,&s->lock_identity) || !inode(&a,&b)) return 0;
	/* Reassert ownership even if an unexpected input alias was closed. POSIX
	 * locks belong to the process and any close on that inode can release them. */
	lock.l_type=F_WRLCK; lock.l_whence=SEEK_SET;
	return !fcntl(s->lock,F_SETLK,&lock);
}

static int copy_message(struct jf_storage *s,int source,int stage,const struct stat *expected,
	size_t header_size,const char *header_digest,char copied[65],const char **detail)
{
	unsigned char bytes[COPY_CHUNK]; char first[65],second[65];
	struct jf_sha256 hash,headers; off_t position=0;
	jf_sha256_init(&hash);
	jf_sha256_init(&headers);
	while (position<expected->st_size) {
		ssize_t n; size_t used=0,want=expected->st_size-position>(off_t)sizeof bytes ? sizeof bytes : (size_t)(expected->st_size-position);
		if (stopped(s)) { *detail="E_INTERRUPTED"; return -1; }
		if (hook("store-read")) { *detail="E_COPY_READ"; return -1; }
		n=pread(source,bytes,want,position);
		if (n<0 && errno==EINTR) continue;
		if (n<=0) { *detail=n<0 ? "E_COPY_READ" : "E_SOURCE_CHANGED"; return -1; }
		jf_sha256_update(&hash,bytes,(size_t)n);
		if ((uintmax_t)position<header_size) {
			size_t left=header_size-(size_t)position;
			jf_sha256_update(&headers,bytes,left<(size_t)n ? left : (size_t)n);
		}
		while (used<(size_t)n) {
			ssize_t written;
			if (stopped(s)) { *detail="E_INTERRUPTED"; return -1; }
			if (hook("store-write")) { *detail="E_COPY_WRITE"; return -1; }
			written=write(stage,bytes+used,(size_t)n-used);
			if (written<0 && errno==EINTR) continue;
			if (written<=0) { *detail="E_COPY_WRITE"; return -1; }
			used+=(size_t)written;
		}
		position+=n;
	}
	jf_sha256_hex(&hash,first);
	jf_sha256_hex(&headers,second);
	if (strcmp(second,header_digest)) { *detail="E_SOURCE_CHANGED"; return -1; }
	memcpy(copied,first,65);
	if (syncfd(stage,"store-sync-file")) { *detail="E_DESTINATION_FSYNC"; return -1; }
	if (hook("store-before-verify")) { *detail="E_COPY_VERIFY"; return -1; }
	jf_sha256_init(&hash); position=0;
	for (;;) {
		ssize_t n;
		if (stopped(s)) { *detail="E_INTERRUPTED"; return -1; }
		n=pread(stage,bytes,sizeof bytes,position);
		if (n<0 && errno==EINTR) continue;
		if (n<0) { *detail="E_COPY_VERIFY"; return -1; }
		if (!n) break;
		if (n>expected->st_size-position) { *detail="E_COPY_VERIFY"; return -1; }
		position+=n; jf_sha256_update(&hash,bytes,(size_t)n);
	}
	jf_sha256_hex(&hash,second);
	if (position!=expected->st_size || strcmp(first,second)) { *detail="E_COPY_VERIFY"; return -1; }
	return 0;
}

static int source_digest(struct jf_storage *s,int fd,off_t size,const char expected[65])
{
	unsigned char bytes[COPY_CHUNK]; char actual[65];
	struct jf_sha256 hash; off_t position=0;
	jf_sha256_init(&hash);
	for (;;) {
		ssize_t n;
		if (stopped(s)) return 0;
		n=pread(fd,bytes,sizeof bytes,position);
		if (n<0 && errno==EINTR) continue;
		if (n<0) return 0;
		if (!n) break;
		if (n>size-position) return 0;
		position+=n; jf_sha256_update(&hash,bytes,(size_t)n);
	}
	jf_sha256_hex(&hash,actual);
	return position==size && !strcmp(actual,expected);
}

static char *published_name(struct jf_storage *s,const char *source,unsigned attempt,int cur)
{
	const char *flags=strstr(source,":2,");
	char prefix[80]; size_t n,available; long maximum;
	if (!attempt) return strdup(source);
	if (!flags) flags="";
	errno=0; maximum=fpathconf(cur,_PC_NAME_MAX);
	if (maximum<0) { if (errno) return NULL; maximum=255; }
	n=strlen(flags); if ((size_t)maximum<=n) { errno=EEXIST; return NULL; }
	available=(size_t)maximum-n;
	snprintf(prefix,sizeof prefix,"jaro.%02x%02x%02x%02x.%zx.%u",s->nonce[0],s->nonce[1],s->nonce[2],s->nonce[3],s->sequence,attempt);
	if (strlen(prefix)>available) {
		static const char alphabet[]="0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";
		size_t i,used=available<sizeof prefix-1 ? available : sizeof prefix-1;
		for (i=0;i<used;i++) prefix[i]=alphabet[(s->nonce[i%16]+s->sequence+attempt+i)%62];
		prefix[used]=0;
	}
	n=strlen(prefix);
	{ char *out=malloc(n+strlen(flags)+1); if (out) { memcpy(out,prefix,n); strcpy(out+n,flags); } return out; }
}

/* Rollback must never overwrite a name another Maildir client created. Linux
 * supports atomic restoration for every entry type. The portable link fallback
 * restores nondirectories; an unexpected directory remains in owned recovery. */
static int restore(int claim,const char *name,int parent)
{
#if defined(__linux__) && defined(SYS_renameat2)
	if (!syscall(SYS_renameat2,claim,name,parent,name,1u)) return 0;
	if (errno!=ENOSYS && errno!=EINVAL && errno!=EOPNOTSUPP) return -1;
#endif
	if (linkat(claim,name,parent,name,0)) return -1;
	if (syncfd(parent,"store-sync-restore")) return -1;
	return unlinkat(claim,name,0);
}

static char *recovery_path(const char *source,const char *directory,const char *name)
{
	const char *slash=strrchr(source,'/');
	size_t parent=slash ? (size_t)(slash-source) : 0,n=strlen(directory),m=strlen(name);
	char *out;
	if (parent>SIZE_MAX-n-m-3) return NULL;
	out=malloc(parent+n+m+3);
	if (out) { memcpy(out,source,parent); out[parent]='/'; memcpy(out+parent+1,directory,n); out[parent+n+1]='/'; memcpy(out+parent+n+2,name,m+1); }
	return out;
}

static char *destination_path(struct jf_storage *s,const char *relative,const char *sub,const char *name)
{
	size_t a=strlen(s->root_path),b=strlen(relative),c=strlen(sub),d=strlen(name);
	char *out;
	if (a>SIZE_MAX-b-c-d-4) return NULL;
	out=malloc(a+b+c+d+4);
	if (out) snprintf(out,a+b+c+d+4,"%s/%s/%s/%s",s->root_path,relative,sub,name);
	return out;
}

int jf_storage_move(struct jf_storage *s,int parent,const char *name,int source,
	const struct stat *expected,size_t header_size,const char *header_digest,
	const char *relative,const char *source_path,struct jf_storage_recovery *recovery,const char **detail)
{
	struct owned_directory stage={-1,-1,0,"",{0}},claim={-1,-1,0,"",{0}};
	struct stat a,b,claimed={0},stage_identity={0};
	int cur=-1,new=-1,tmp=-1,file=-1,copyfd=-1,stage_message=0,claimed_message=0,rc=7,published=0;
	char *final_name=NULL,*claim_path=NULL,*final_path=NULL,*stage_path=NULL;
	char copied[65];
	unsigned attempt;
	memset(recovery,0,sizeof *recovery); *detail="E_DESTINATION_IO";
	if (!lock_current(s)) { *detail="E_BATCH_LOCK_CHANGED"; goto done; }
	if (!source_current(parent,name,source,expected)) { *detail="E_SOURCE_CHANGED"; goto done; }
	if (destination(s,relative,&cur,&new,&tmp)) goto done;
	if (fstat(parent,&a) || fstat(cur,&b)) goto done;
	if (inode(&a,&b)) { rc=1; goto done; }
	if (fstat(new,&b)) goto done;
	if (inode(&a,&b)) { rc=1; goto done; }
	if (stopped(s)) { *detail="E_INTERRUPTED"; goto done; }
	if (create_owned(s,tmp,&stage)) goto done;
	stage_path=destination_path(s,relative,"tmp",stage.name);
	if (stage_path) {
		size_t n=strlen(stage_path); char *p=realloc(stage_path,n+9);
		if (!p) { *detail="E_MEMORY"; goto done; }
		stage_path=p; memcpy(stage_path+n,"/message",9);
	} else { *detail="E_MEMORY"; goto done; }
	file=openat(stage.fd,"message",O_RDWR|O_CREAT|O_EXCL|O_NOFOLLOW|O_CLOEXEC,0600); if (file<0) goto done;
	stage_message=1;
	if (hook("store-staging-created")) goto done;
	if (copy_message(s,source,file,expected,header_size,header_digest,copied,detail)) goto done;
	if (fstat(file,&stage_identity)) goto done;
	copyfd=dup(file); if (copyfd<0) goto done;
	if (close(file)) { file=-1; *detail="E_DESTINATION_IO"; goto done; }
	file=-1;
	if (!source_current(parent,name,source,expected)) { *detail="E_SOURCE_CHANGED"; goto done; }
	if (hook("store-before-publish")) { *detail="E_DESTINATION_PUBLISH"; goto done; }
	if (stopped(s)) { *detail="E_INTERRUPTED"; goto done; }
	if (!destination_current(s,relative,cur,tmp)) { *detail="E_DESTINATION_CHANGED"; goto done; }
	if (fstatat(stage.fd,"message",&a,AT_SYMLINK_NOFOLLOW) || !jf_storage_same(&a,&stage_identity)) { *detail="E_COPY_VERIFY"; goto done; }
	for (attempt=0;attempt<RETRIES;attempt++) {
		char *candidate=published_name(s,name,attempt,cur);
		char *path;
		int result;
		if (!candidate) { *detail=errno==EEXIST ? "E_DESTINATION_COLLISION" : "E_MEMORY"; goto done; }
		path=destination_path(s,relative,"cur",candidate);
		if (!path) { free(candidate); *detail="E_MEMORY"; goto done; }
		if (hook("store-publish-attempt")) { free(candidate); free(path); *detail="E_DESTINATION_PUBLISH"; goto done; }
		result=linkat(stage.fd,"message",cur,candidate,0);
		if (!result) { final_name=candidate; final_path=path; published=1; break; }
		free(candidate); free(path);
		if (errno!=EEXIST) { *detail="E_DESTINATION_PUBLISH"; goto done; }
	}
	if (!published) { *detail="E_DESTINATION_COLLISION"; goto done; }
	if (hook("store-published") || syncfd(copyfd,"store-sync-published-file") || syncfd(cur,"store-sync-published")) {
		*detail="E_DESTINATION_FSYNC"; goto done;
	}
	/* A failure never removes a published name: it may already have been
	 * renamed by an independent Maildir client. Source retention is recovery. */
	if (hook("store-cleanup-stage") || unlinkat(stage.fd,"message",0)) { *detail="E_STAGE_CLEANUP"; goto done; }
	stage_message=0;
	if (syncfd(stage.fd,"store-sync-stage") || remove_owned(&stage)) { *detail="E_STAGE_CLEANUP"; goto done; }
	close(stage.fd); stage.fd=-1;
	if (!source_current(parent,name,source,expected)) { *detail="E_SOURCE_CHANGED"; goto done; }
	if (stopped(s)) { *detail="E_INTERRUPTED"; goto done; }
	if (create_owned(s,parent,&claim)) { *detail="E_SOURCE_CLAIM"; goto done; }
	claim_path=recovery_path(source_path,claim.name,name);
	if (!claim_path) { *detail="E_MEMORY"; goto done; }
	if (hook("store-before-claim")) { *detail="E_SOURCE_CLAIM"; goto done; }
	if (stopped(s)) { *detail="E_INTERRUPTED"; goto done; }
	if (!lock_current(s)) { *detail="E_BATCH_LOCK_CHANGED"; goto done; }
	if (!destination_current(s,relative,cur,tmp) || fstatat(cur,final_name,&a,AT_SYMLINK_NOFOLLOW) || !bytes_identity(&a,&stage_identity)) {
		*detail="E_DESTINATION_CHANGED"; goto done;
	}
	if (!source_digest(s,copyfd,expected->st_size,copied)) { *detail=stopped(s) ? "E_INTERRUPTED" : "E_DESTINATION_CHANGED"; goto done; }
	if (renameat(parent,name,claim.fd,name)) { *detail="E_SOURCE_CHANGED"; goto done; }
	claimed_message=1;
	if (fstatat(claim.fd,name,&a,AT_SYMLINK_NOFOLLOW) || fstat(source,&claimed) ||
		!S_ISREG(a.st_mode) || !inode(&a,expected) || !bytes_identity(&claimed,expected) || !jf_storage_same(&a,&claimed)) {
		*detail="E_SOURCE_CHANGED"; goto done;
	}
	if (hook("store-source-claimed") || syncfd(claim.fd,"store-sync-claim") || syncfd(parent,"store-sync-source-claim")) {
		*detail="E_SOURCE_FSYNC"; goto done;
	}
	if (hook("store-before-unlink")) { *detail="E_SOURCE_UNLINK"; goto done; }
	if (stopped(s)) { *detail="E_INTERRUPTED"; goto done; }
	if (!lock_current(s)) { *detail="E_BATCH_LOCK_CHANGED"; goto done; }
	if (!destination_current(s,relative,cur,tmp) || fstatat(cur,final_name,&a,AT_SYMLINK_NOFOLLOW) || !bytes_identity(&a,&stage_identity)) {
		*detail="E_DESTINATION_CHANGED"; goto done;
	}
	if (!source_digest(s,copyfd,expected->st_size,copied)) { *detail=stopped(s) ? "E_INTERRUPTED" : "E_DESTINATION_CHANGED"; goto done; }
	if (fstat(source,&a) || fstatat(claim.fd,name,&b,AT_SYMLINK_NOFOLLOW) ||
		!jf_storage_same(&a,&claimed) || !jf_storage_same(&b,&claimed)) { *detail="E_SOURCE_CHANGED"; goto done; }
	if (!source_digest(s,source,expected->st_size,copied)) { *detail=stopped(s) ? "E_INTERRUPTED" : "E_SOURCE_CHANGED"; goto done; }
	if (fstat(source,&a) || !jf_storage_same(&a,&claimed)) { *detail="E_SOURCE_CHANGED"; goto done; }
	/* Only this process writes the newly created private claim namespace.
	 * The original mutable Maildir name is never passed to unlinkat. */
	if (hook("store-unlink") || unlinkat(claim.fd,name,0)) { *detail="E_SOURCE_UNLINK"; goto done; }
	claimed_message=0;
	if (syncfd(claim.fd,"store-sync-source-unlinked") || remove_owned(&claim)) { *detail="E_SOURCE_FSYNC"; goto done; }
	close(claim.fd); claim.fd=-1;
	rc=0;
done:
	if (file>=0 && close(file) && rc==0) { rc=7; *detail="E_DESTINATION_IO"; }
	if (copyfd>=0 && close(copyfd) && rc==0) { rc=7; *detail="E_DESTINATION_IO"; }
	if (claimed_message) {
		if (!hook("store-before-restore") && !restore(claim.fd,name,parent)) {
			claimed_message=0;
			if (syncfd(claim.fd,"store-sync-restore-claim") || syncfd(parent,"store-sync-restore")) *detail="E_SOURCE_FSYNC";
		} else {
			recovery->source=claim_path; claim_path=NULL;
			*detail="E_SOURCE_RECOVERY";
			/* Persist the retained claim even when it captured a replacement
			 * and therefore never reached the normal source-claim sync path. */
			(void)syncfd(claim.fd,"store-sync-retained-claim");
			(void)syncfd(parent,"store-sync-retained-parent");
		}
	}
	if (stage_message) {
		if (unlinkat(stage.fd,"message",0)) { if (!*detail) *detail="E_STAGE_CLEANUP"; }
		else stage_message=0;
		if (stage_message) {
			(void)syncfd(stage.fd,"store-sync-retained-stage");
			(void)syncfd(tmp,"store-sync-retained-tmp");
		}
	}
	if (stage.created && !stage_message && remove_owned(&stage) && rc==0) { rc=7; *detail="E_STAGE_CLEANUP"; }
	if (claim.created && !claimed_message && remove_owned(&claim) && rc==0) { rc=7; *detail="E_SOURCE_FSYNC"; }
	if (stage.created && stage_path) {
		if (!stage_message) { char *slash=strrchr(stage_path,'/'); if (slash) *slash=0; }
		recovery->temporary=stage_path; stage_path=NULL;
	}
	if (claim.created && claim_path) {
		if (!claimed_message) { char *slash=strrchr(claim_path,'/'); if (slash) *slash=0; }
		recovery->source=claim_path; claim_path=NULL;
	}
	if (rc==7 && published) { recovery->destination=final_path; final_path=NULL; }
	if (stage.fd>=0) close(stage.fd);
	if (claim.fd>=0) close(claim.fd);
	if (cur>=0) close(cur);
	if (new>=0) close(new);
	if (tmp>=0) close(tmp);
	free(final_name); free(claim_path); free(final_path); free(stage_path);
	return rc==7 && !strcmp(*detail,"E_MEMORY") ? 3 : rc;
}
