#define _POSIX_C_SOURCE 200809L
#include "filter/generation.h"
#include "filter/headers.h"
#include "filter/storage.h"
#include "filter/pool.h"
#include <sys/stat.h>
#include <dirent.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PATH_COUNT_MAX 100000u
struct selected {
	char *path;
	struct stat identity;
	int snapshotted,attempted;
	struct stat parent_identity;
	const char *error;
	struct selected *first;
};
struct selection { struct selected *items; size_t count,capacity,bytes; };
static volatile sig_atomic_t interrupted;
static void on_signal(int signal) { interrupted=signal; }

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
	s=malloc(n+m+2);
	if (s) { memcpy(s,a,n); s[n]='/'; memcpy(s+n+1,b,m+1); }
	return s;
}

static char *absolute(const char *cwd,const char *path)
{
	return *path=='/' ? strdup(path) : join(cwd,path);
}

/* Walk directory components through descriptors: neither a selected Maildir
 * nor an explicit message path may redirect a read through a directory link. */
static int open_parent(const char *source,char **owned,char **name)
{
	char *slash;
	*owned=strdup(source); if (!*owned) return -1;
	slash=strrchr(*owned,'/');
	if (!slash) { free(*owned); *owned=NULL; return -1; }
	*slash=0; *name=slash+1;
	return jf_storage_directory(**owned ? *owned : "/");
}

static int compare_identities(const void *left,const void *right)
{
	const struct selected *a=*(const struct selected *const *)left,*b=*(const struct selected *const *)right;
	if (a->identity.st_dev!=b->identity.st_dev) return a->identity.st_dev<b->identity.st_dev ? -1 : 1;
	if (a->identity.st_ino!=b->identity.st_ino) return a->identity.st_ino<b->identity.st_ino ? -1 : 1;
	return a<b ? -1 : a>b;
}

static int snapshot_paths(const char *cwd,struct selection *s)
{
	struct selected **references;
	size_t i,n=0;
	for (i=0;i<s->count;i++) {
		struct selected *item=&s->items[i];
		char *path,*owned=NULL,*name=NULL; int fd;
		if (item->snapshotted) continue;
		path=absolute(cwd,item->path); if (!path) return 3;
		fd=open_parent(path,&owned,&name); free(path);
		if (fd<0) {
			free(owned); if (errno==ENOMEM) return 3;
			item->error="E_MESSAGE_OPEN"; continue;
		}
		if (fstatat(fd,name,&item->identity,AT_SYMLINK_NOFOLLOW)) item->error="E_MESSAGE_OPEN";
		else if (!S_ISREG(item->identity.st_mode)) item->error="E_MESSAGE_TYPE";
		else item->snapshotted=1;
		close(fd); free(owned);
	}
	references=malloc(s->count*sizeof *references);
	if (!references) return 3;
	for (i=0;i<s->count;i++) if (s->items[i].snapshotted) references[n++]=&s->items[i];
	if (n) qsort(references,n,sizeof *references,compare_identities);
	for (i=0;i<n;i++) {
		if (i && references[i]->identity.st_dev==references[i-1]->identity.st_dev &&
			references[i]->identity.st_ino==references[i-1]->identity.st_ino)
			references[i]->first=references[i-1]->first;
		else references[i]->first=references[i];
	}
	free(references);
	return 0;
}

static void selection_free(struct selection *s)
{
	size_t i;
	for (i=0;i<s->count;i++) free(s->items[i].path);
	free(s->items);
}

static int select_path(struct selection *s,const char *path,const struct stat *identity)
{
	struct selected *item;
	size_t n=strlen(path);
	if (!n || s->count==PATH_COUNT_MAX || n>=JF_INPUT_MAX-s->bytes) return 2;
	if (s->count==s->capacity) {
		size_t cap=s->capacity ? s->capacity*2 : 64;
		struct selected *v=realloc(s->items,cap*sizeof *v);
		if (!v) return 3;
		s->items=v; s->capacity=cap;
	}
	item=&s->items[s->count]; memset(item,0,sizeof *item);
	item->path=strdup(path); if (!item->path) return 3;
	if (identity) { item->identity=*identity; item->snapshotted=1; }
	s->count++; s->bytes+=n+1;
	return 0;
}

static int list_paths(const char *path,struct selection *s)
{
	struct jf_bytes item={0};
	unsigned char buffer[4096]; size_t i,total=0;
	int fd=!strcmp(path,"-") ? STDIN_FILENO : open(path,O_RDONLY),rc=0;
	if (fd<0) return 3;
	for (;;) {
		ssize_t n=read(fd,buffer,sizeof buffer);
		if (n<0 && errno==EINTR) continue;
		if (n<0) { rc=3; break; }
		if (!n) { if (item.size) rc=2; break; }
		if ((size_t)n>JF_INPUT_MAX-total) { rc=2; break; }
		total+=(size_t)n;
		for (i=0;i<(size_t)n;i++) {
			if (jf_bytes_add_limit(&item,buffer+i,1,JF_INPUT_MAX)) { rc=3; goto done; }
			if (!buffer[i]) {
				rc=select_path(s,(char *)item.data,NULL);
				if (rc) goto done;
				item.size=0;
			}
		}
	}
done:
	jf_bytes_free(&item);
	if (fd!=STDIN_FILENO && close(fd) && !rc) rc=3;
	return rc;
}

static int compare_paths(const void *a,const void *b)
{
	return strcmp(((const struct selected *)a)->path,((const struct selected *)b)->path);
}

static int maildir_paths(const char *cwd,const char *path,struct selection *s)
{
	const char *names[]={"cur","new"};
	char *base=absolute(cwd,path);
	int dir=-1,fd=-1,rc=3; size_t i;
	DIR *stream=NULL;
	if (!base) goto done;
	dir=jf_storage_directory(base); if (dir<0) goto done;
	for (i=0;i<2;i++) {
		struct dirent *ent;
		char *sub=join(base,names[i]);
		if (!sub) goto done;
		fd=openat(dir,names[i],O_RDONLY|O_DIRECTORY|O_NOFOLLOW);
		if (fd<0) { free(sub); goto done; }
		stream=fdopendir(fd);
		if (!stream) { free(sub); goto done; }
		fd=-1;
		for (;;) {
			struct stat st; char *full;
			errno=0; ent=readdir(stream);
			if (!ent) { if (errno) { free(sub); goto done; } break; }
			if (!strcmp(ent->d_name,".") || !strcmp(ent->d_name,"..")) continue;
			if (fstatat(dirfd(stream),ent->d_name,&st,AT_SYMLINK_NOFOLLOW)) {
				/* A vanished entry is outside the completed name snapshot. */
				if (errno==ENOENT) continue;
				free(sub); goto done;
			}
			if (!S_ISREG(st.st_mode)) continue;
			full=join(sub,ent->d_name);
			if (!full) { free(sub); goto done; }
			rc=select_path(s,full,&st); free(full);
			if (rc) { free(sub); goto done; }
			rc=3;
		}
		free(sub);
		if (closedir(stream)) { stream=NULL; goto done; }
		stream=NULL;
	}
	if (s->count) qsort(s->items,s->count,sizeof *s->items,compare_paths);
	rc=0;
done:
	if (stream) closedir(stream);
	if (fd>=0) close(fd);
	if (dir>=0) close(dir);
	free(base); return rc;
}

static int already_there(int parent,const char *destination)
{
	const char *names[]={"cur","new"};
	struct stat a,b;
	int directory=jf_storage_directory(destination),same=0;
	size_t i;
	if (directory<0) return errno==ENOMEM ? -1 : 0;
	if (!fstat(parent,&a)) for (i=0;i<2 && !same;i++) {
		int child=openat(directory,names[i],O_RDONLY|O_DIRECTORY|O_NOFOLLOW);
		if (child>=0) {
			same=!fstat(child,&b) && a.st_dev==b.st_dev && a.st_ino==b.st_ino;
			close(child);
		}
	}
	close(directory); return same;
}

static int field(const char *s)
{
	return fwrite(s,1,strlen(s)+1,stdout)!=strlen(s)+1;
}

static int record(size_t index,const char *path,const struct jf_rule *r,
	const char *local,const char *status,const char *detail)
{
	char number[32],line[32]; int error=!strcmp(status,"error");
	snprintf(number,sizeof number,"%zu",index); snprintf(line,sizeof line,"%zu",r ? r->line : 0);
	return field(number) || field(path) || field(r ? r->id : "") ||
		field(r && !error && strcmp(status,"noop") ? "move" : "none") || field(r && !error ? r->logical : "") ||
		field(local && !error ? local : "") || field(r && !error ? r->sieve : "") || field(status) ||
		field(r ? r->source : "") || field(line) || field(detail ? detail : "");
}

static void recovery_diagnostic(const char *path)
{
	const unsigned char *p=(const unsigned char *)path;
	fputs("E_RETAINED_PATH \"",stderr);
	for (;*p;p++) if (*p<32 || *p>=127 || *p=='"' || *p=='\\') fprintf(stderr,"\\x%02x",*p); else fputc(*p,stderr);
	fputs("\"\n",stderr);
}

/* Preparation and output are coordinator-only. Workers borrow the pinned
 * source/parent descriptors and run only the unchanged singleton move. */
struct decision {
	const struct selected *item;
	const struct jf_rule *winner;
	struct stat opened;
	char *source,*local,*parent,*name;
	const char *detail;
	char digest[65];
	size_t header_size,index;
	int fd,parentfd,rc,noop,quarantine;
	struct jf_storage_recovery recovery;
};

static void prepare(struct decision *d,const char *cwd,const char *root,
	const struct selected *item,const struct jf_policy *policy,struct jf_storage *storage,
	size_t index)
{
	struct jf_headers *headers=NULL;
	struct jf_error error={0};
	struct stat before,after,current;
	memset(d,0,sizeof *d); d->fd=d->parentfd=-1; d->rc=7; d->item=item; d->index=index;
	d->source=absolute(cwd,item->path);
	if (!d->source) { d->rc=3; d->detail="E_MEMORY"; return; }
	if (item->error) { d->detail=item->error; return; }
	if (storage && item->first) {
		if (item->first->attempted) { d->detail="E_MESSAGE_CHANGED"; return; }
		item->first->attempted=1;
	}
	d->parentfd=open_parent(d->source,&d->parent,&d->name);
	if (d->parentfd<0) { d->rc=errno==ENOMEM ? 3 : 7; d->detail=d->rc==3 ? "E_MEMORY" : "E_MESSAGE_OPEN"; return; }
	if (fstatat(d->parentfd,d->name,&before,AT_SYMLINK_NOFOLLOW)) { d->detail="E_MESSAGE_OPEN"; return; }
	if (!S_ISREG(before.st_mode)) { d->detail="E_MESSAGE_TYPE"; return; }
	if (storage && before.st_dev==storage->lock_identity.st_dev && before.st_ino==storage->lock_identity.st_ino) {
		d->detail="E_MESSAGE_TYPE"; return;
	}
	if (item->snapshotted && !jf_storage_same(&before,&item->identity)) { d->detail="E_MESSAGE_CHANGED"; return; }
	if (hook("message-before-open")) { d->rc=3; return; }
	d->fd=openat(d->parentfd,d->name,O_RDONLY|O_NOFOLLOW|O_NONBLOCK);
	if (d->fd<0) { d->detail="E_MESSAGE_OPEN"; return; }
	/* The inode can change between the name check and open. Closing any alias
	 * of the lock drops POSIX process locks, so defer uncertain/lock FDs. */
	if (fstat(d->fd,&d->opened)) { d->quarantine=storage!=NULL; d->detail="E_MESSAGE_CHANGED"; return; }
	if (storage && d->opened.st_dev==storage->lock_identity.st_dev && d->opened.st_ino==storage->lock_identity.st_ino) {
		d->quarantine=1; d->detail="E_MESSAGE_CHANGED"; (void)hook("message-lock-alias"); return;
	}
	if (!S_ISREG(d->opened.st_mode) || !jf_storage_same(&before,&d->opened)) { d->detail="E_MESSAGE_CHANGED"; return; }
	if (hook("message-opened")) { d->rc=3; return; }
	d->rc=jf_headers_read(d->fd,NULL,&headers,&error);
	if (d->rc) {
		d->detail=error.code;
		if (d->rc!=3 || (error.code && !strcmp(error.code,"E_MESSAGE_IO"))) d->rc=7;
		goto done;
	}
	if (hook("headers-read")) { d->rc=3; goto done; }
	if (fstat(d->fd,&after) || fstatat(d->parentfd,d->name,&current,AT_SYMLINK_NOFOLLOW) ||
		!jf_storage_same(&d->opened,&after) || !jf_storage_same(&d->opened,&current)) {
		d->detail="E_MESSAGE_CHANGED"; d->rc=7; goto done;
	}
	d->rc=jf_policy_classify(policy,jf_headers_message(headers),&d->winner,&error,NULL);
	if (d->rc) { d->detail=error.code; goto done; }
	d->local=join(root,d->winner->local);
	if (!d->local) { d->rc=3; goto done; }
	d->header_size=jf_headers_wire_size(headers);
	memcpy(d->digest,jf_headers_digest(headers),sizeof d->digest);
done:
	jf_headers_free(headers);
}

static void move_decision(void *context,struct jf_storage *storage)
{
	struct decision *d=context;
#ifdef JF_TESTING
	jf_generation_test_job(d->index);
#endif
	if (!d->rc && hook("store-job-start")) { d->rc=3; d->detail="E_MEMORY"; }
	if (!d->rc) {
		if (storage->should_stop && storage->should_stop(storage->context)) {
			d->rc=7; d->detail="E_INTERRUPTED";
		} else {
			d->rc=jf_storage_move(storage,d->parentfd,d->name,d->fd,&d->opened,
				d->header_size,d->digest,d->winner->local,d->source,&d->recovery,&d->detail);
			if (d->rc==1) { d->rc=0; d->noop=1; }
		}
	}
#ifdef JF_TESTING
	/* Complete deterministic phase gates only after publishing global stop. */
#endif
	if (d->rc==3 && storage->borrowed) jf_pool_stop(storage->context);
#ifdef JF_TESTING
	jf_generation_test_done(d->index);
#endif
}

static void close_decision(struct decision *d,struct jf_storage *storage)
{
	if (d->fd>=0 && !d->quarantine) {
		if (close(d->fd) && !d->rc) { d->rc=7; d->detail="E_MESSAGE_IO"; }
		d->fd=-1;
	}
	if (d->parentfd>=0) { close(d->parentfd); d->parentfd=-1; }
	if (storage && d->noop && !d->rc && d->item->first) d->item->first->attempted=0;
}

static int emit_decision(struct decision *d,struct jf_storage *storage,struct jf_error *global)
{
	int rc=d->rc;
	if (rc!=3 && record(d->index,d->item->path,d->winner,rc ? NULL : d->local,
		rc ? "error" : d->noop ? "noop" : storage ? "moved" : "classified",rc ? d->detail : NULL)) {
		rc=3; d->detail="E_OUTPUT";
	}
	if (rc==3) global->code=d->detail ? d->detail : "E_MEMORY";
	if (d->recovery.source) recovery_diagnostic(d->recovery.source);
	if (d->recovery.destination) recovery_diagnostic(d->recovery.destination);
	if (d->recovery.temporary) recovery_diagnostic(d->recovery.temporary);
	return rc;
}

static void free_decision(struct decision *d)
{
	/* Quarantined source FDs are released only after all pool threads join. */
	if (d->fd>=0) close(d->fd);
	free(d->source); free(d->local); free(d->parent);
	free(d->recovery.source); free(d->recovery.destination); free(d->recovery.temporary); free(d->recovery.published);
}

static int classify(const char *cwd,const char *root,const struct selected *item,
	const struct jf_policy *policy,struct jf_storage *storage,size_t index,struct jf_error *global)
{
	struct decision d; int rc;
	prepare(&d,cwd,root,item,policy,storage,index);
	if (!d.rc) {
		if (storage) move_decision(&d,storage);
		else if ((d.noop=already_there(d.parentfd,d.local))<0) d.rc=3;
	}
	close_decision(&d,storage); rc=emit_decision(&d,storage,global);
	/* In the singleton path a post-open lock alias stops admission before its
	 * deferred close, preserving lock ownership for every prior operation. */
	if (d.quarantine) { global->code="E_BATCH_LOCK_CHANGED"; rc=3; }
	free_decision(&d); return rc;
}

struct dependencies { struct stat *parents; size_t count; };
static int compare_inode(const void *left,const void *right)
{
	const struct stat *a=left,*b=right;
	if (a->st_dev!=b->st_dev) return a->st_dev<b->st_dev ? -1 : 1;
	return a->st_ino<b->st_ino ? -1 : a->st_ino>b->st_ino;
}
static int compare_strings(const void *a,const void *b)
{
	return strcmp(*(const char *const *)a,*(const char *const *)b);
}
static int name_capacity(int fd)
{
	long maximum;
	errno=0; maximum=fpathconf(fd,_PC_NAME_MAX);
	return maximum>=255;
}
static int ancestor_capacity(char *path,size_t root_length)
{
	int fd;
	for (;;) {
		char *slash;
		fd=jf_storage_directory(path);
		if (fd>=0) { int ok=name_capacity(fd); close(fd); return ok; }
		if (errno!=ENOENT) return 0;
		slash=strrchr(path,'/'); if (!slash || (size_t)(slash-path)<root_length) return 0;
		*slash=0;
	}
}
static int independent_destination(void *context,int cur,int newfd)
{
	struct dependencies *deps=context; struct stat st;
	/* The reserved jaro. namespace proof requires room for the full prefix.
	 * A smaller/unknown limit retains singleton collision semantics. */
	if (!name_capacity(cur) || !name_capacity(newfd)) return 0;
	if (fstat(cur,&st) || bsearch(&st,deps->parents,deps->count,sizeof st,compare_inode)) return 0;
	if (fstat(newfd,&st) || bsearch(&st,deps->parents,deps->count,sizeof st,compare_inode)) return 0;
	return 1;
}

/* Generic conservative fallback: never overlap inode aliases, original-name
 * collision winners, noops, or any policy destination in a selected source
 * directory. Public alternative names begin jaro.; a long flag suffix can
 * truncate that prefix, so those inputs also retain singleton ordering. */
static int independent(const char *cwd,const char *root,struct selection *s,
	const struct jf_policy *policy,struct dependencies *deps)
{
	const char **names=NULL,**locals=NULL; size_t i,n=jf_policy_count(policy); int safe=0;
	deps->parents=calloc(s->count,sizeof *deps->parents);
	names=malloc(s->count*sizeof *names); locals=malloc(n*sizeof *locals);
	if (!deps->parents || !names || !locals) goto done;
	for (i=0;i<s->count;i++) {
		struct selected *item=&s->items[i]; char *source,*parent=NULL,*name=NULL; int fd;
		const char *base=strrchr(item->path,'/'),*flags;
		base=base ? base+1 : item->path; names[i]=base; flags=strstr(base,":2,");
		if (strlen(item->path)>32768 || !strncmp(base,"jaro.",5) || (flags && strlen(flags)>175) ||
			(item->first && item->first!=item)) goto done;
		source=absolute(cwd,item->path); if (!source) goto done;
		fd=open_parent(source,&parent,&name); free(source); free(parent);
		if (fd<0) goto done;
		if (fstat(fd,&item->parent_identity)) { close(fd); goto done; }
		close(fd); deps->parents[i]=item->parent_identity;
	}
	qsort(names,s->count,sizeof *names,compare_strings);
	for (i=1;i<s->count;i++) if (!strcmp(names[i-1],names[i])) goto done;
	qsort(deps->parents,s->count,sizeof *deps->parents,compare_inode); deps->count=s->count;
	for (i=0;i<n;i++) locals[i]=jf_policy_rule(policy,i)->local;
	qsort(locals,n,sizeof *locals,compare_strings);
	for (i=0;i<n;i++) {
		char *path; int fd,cur,newfd,unknown=0;
		if (i && !strcmp(locals[i],locals[i-1])) continue;
		if (strlen(locals[i])>32768) goto done;
		path=join(root,locals[i]); if (!path) goto done;
		fd=jf_storage_directory(path);
		if (fd<0) {
			int missing=errno==ENOENT,capacity=missing && ancestor_capacity(path,strlen(root));
			free(path); if (capacity) continue; goto done;
		}
		free(path);
		if (!name_capacity(fd)) { close(fd); goto done; }
		cur=openat(fd,"cur",O_RDONLY|O_DIRECTORY|O_NOFOLLOW); if (cur<0 && errno!=ENOENT) unknown=1;
		newfd=openat(fd,"new",O_RDONLY|O_DIRECTORY|O_NOFOLLOW); if (newfd<0 && errno!=ENOENT) unknown=1;
		if (unknown) { if (cur>=0) close(cur); if (newfd>=0) close(newfd); close(fd); goto done; }
		if ((cur>=0 && !independent_destination(deps,cur,cur)) || (newfd>=0 && !independent_destination(deps,newfd,newfd))) {
			if (cur>=0) close(cur);
			if (newfd>=0) close(newfd);
			close(fd); goto done;
		}
		if (cur>=0) close(cur);
		if (newfd>=0) close(newfd);
		close(fd);
	}
	safe=1;
done:
	free(names); free(locals);
	if (!safe) { free(deps->parents); memset(deps,0,sizeof *deps); }
	return safe;
}

static int parallel(const char *cwd,const char *root,struct selection *selected,
	const struct jf_policy *policy,struct jf_storage *storage,struct jf_pool **pool,
	struct jf_error *error)
{
	struct decision decisions[JF_POOL_WINDOW]; void *jobs[JF_POOL_WINDOW];
	size_t start=0,i,count=0; int rc=0,aggregate=0,quarantine=0;
#ifdef JF_TESTING
	jf_generation_test_parallel(1);
#endif
	while (start<selected->count && !jf_pool_stopped(*pool)) {
		count=0;
		while (count<JF_POOL_WINDOW && start+count<selected->count && !jf_pool_stopped(*pool)) {
			struct decision *d=&decisions[count]; struct stat parent;
			prepare(d,cwd,root,&selected->items[start+count],policy,storage,start+count+1);
			if (!d->rc && (fstat(d->parentfd,&parent) || compare_inode(&parent,&d->item->parent_identity))) {
				d->rc=7; d->detail="E_MESSAGE_CHANGED";
			}
			jobs[count]=d; count++;
			if (d->rc==3 || d->quarantine) { quarantine=d->quarantine; jf_pool_stop(*pool); break; }
		}
		jf_pool_window(*pool,jobs,count,move_decision);
		for (i=0;i<count;i++) close_decision(&decisions[i],storage);
		for (i=0;i<count;i++) {
			rc=emit_decision(&decisions[i],storage,error);
			if (rc==3) { jf_pool_stop(*pool); break; }
			if (rc) aggregate=7;
		}
		if (fflush(stdout) || ferror(stdout)) { error->code="E_OUTPUT"; rc=3; jf_pool_stop(*pool); }
		/* All operations in this bounded window are settled before output. On
		 * output failure diagnostics conservatively name every durable outcome,
		 * including rows that may already have reached the consumer. */
		if (rc==3) for (i=0;i<count;i++) {
			struct jf_storage_recovery *r=&decisions[i].recovery;
			if (r->published) recovery_diagnostic(r->published);
			if (r->source) recovery_diagnostic(r->source);
			if (r->destination) recovery_diagnostic(r->destination);
			if (r->temporary) recovery_diagnostic(r->temporary);
		}
		if (rc==3 || quarantine || jf_pool_stopped(*pool)) {
			int signal=jf_pool_finish(*pool); *pool=NULL;
			if (signal) { interrupted=signal; error->code="E_INTERRUPTED"; }
			if (quarantine) { rc=3; error->code="E_BATCH_LOCK_CHANGED"; }
			for (i=0;i<count;i++) free_decision(&decisions[i]);
			break;
		}
		for (i=0;i<count;i++) free_decision(&decisions[i]);
		start+=count;
	}
#ifdef JF_TESTING
	jf_generation_test_parallel(0);
#endif
	return rc==3 ? 3 : aggregate;
}

int main(int argc,char **argv)
{
	const char *root=NULL,*generation=NULL,*mode=NULL,*maildir=NULL,*list=NULL;
	struct selection selected={0};
	struct jf_generation pinned={0};
	struct jf_policy *policy=NULL;
	struct jf_error error={0};
	struct jf_storage storage={0};
	struct jf_storage *apply=NULL;
	struct jf_pool *pool=NULL;
	struct dependencies dependencies={0};
	char *cwd=NULL,*resolved=NULL;
	int i,selection=0,rc=2,aggregate=0;
	size_t n;
	pinned.directory=-1;
	storage.root=storage.engine=storage.lock=storage.random=-1;
	for (i=1;i<argc;i++) {
		const char **slot=NULL;
		if (argc==2 && !strcmp(argv[i],"--help")) {
			puts("jaro-filter --root ROOT [--generation ID] [--mode classify|apply] (--maildir DIR | --files0-from FILE | -- PATH...)"); return 0;
		}
		if (argc==2 && !strcmp(argv[i],"--version")) { puts(JF_COMPILER_ID); return 0; }
		if (!strcmp(argv[i],"--root")) slot=&root;
		else if (!strcmp(argv[i],"--generation")) slot=&generation;
		else if (!strcmp(argv[i],"--mode")) slot=&mode;
		else if (!strcmp(argv[i],"--maildir")) { if (selection) goto usage; selection=1; slot=&maildir; }
		else if (!strcmp(argv[i],"--files0-from")) { if (selection) goto usage; selection=2; slot=&list; }
		else if (!strcmp(argv[i],"--")) {
			if (selection || i+1==argc) goto usage;
			selection=3;
			while (++i<argc) { rc=select_path(&selected,argv[i],NULL); if (rc) goto usage; }
			break;
		} else goto usage;
		if (*slot || i+1==argc || !*argv[i+1]) goto usage;
		*slot=argv[++i];
	}
	if (!root || !selection || (mode && strcmp(mode,"classify") && strcmp(mode,"apply"))) goto usage;
	cwd=getcwd(NULL,0); if (!cwd || !(resolved=absolute(cwd,root))) { rc=3; goto done; }
	if (selection==1) rc=maildir_paths(cwd,maildir,&selected);
	else if (selection==2) rc=list_paths(list,&selected);
	else rc=0;
	if (rc) { error.code=rc==2 ? "E_PATH_LIST" : "E_SELECTION_IO"; goto done; }
	if (hook("batch-selected")) { rc=3; goto done; }
	if ((rc=jf_generation_pin(resolved,generation,&pinned,&error))) goto done;
	if ((rc=jf_cache_policy(&pinned.cache,NULL,&policy,&error))) goto done;
	if (hook("batch-pinned")) { rc=3; goto done; }
	if (mode && !strcmp(mode,"apply") && selected.count) {
		struct sigaction action;
		memset(&action,0,sizeof action); action.sa_handler=on_signal; sigemptyset(&action.sa_mask);
		if (sigaction(SIGINT,&action,NULL) || sigaction(SIGTERM,&action,NULL) || sigaction(SIGHUP,&action,NULL) || sigaction(SIGPIPE,&action,NULL)) { rc=3; goto done; }
		if ((rc=snapshot_paths(cwd,&selected))) goto done;
		if (hook("batch-preflight")) { rc=3; goto done; }
		if (independent(cwd,resolved,&selected,policy,&dependencies)) pool=jf_pool_start();
		if ((rc=jf_storage_begin(resolved,&pinned,&interrupted,&storage,&error.code))) goto done;
		if (pool) { storage.destination_allowed=independent_destination; storage.context=&dependencies; jf_pool_bind(pool,&storage); }
		apply=&storage;
		if (hook("batch-locked")) { rc=3; goto done; }
	}
	if (field("JFD1") || field(pinned.id) || (apply && fflush(stdout))) { error.code="E_OUTPUT"; rc=3; goto done; }
	if (pool) { rc=parallel(cwd,resolved,&selected,policy,apply,&pool,&error); goto done; }
	for (n=0;n<selected.count;n++) {
		if (interrupted) break;
		rc=classify(cwd,resolved,&selected.items[n],policy,apply,n+1,&error);
		if (rc==3) goto done;
		if (rc) aggregate=7;
	}
	rc=aggregate;
	if (fflush(stdout) || ferror(stdout)) rc=3;
	goto done;
usage:
	if (rc!=3) rc=2;
	error.code=rc==2 ? "E_USAGE" : "E_MEMORY";
done:
	if (pool) { int signal=jf_pool_finish(pool); if (signal) interrupted=signal; }
	free(dependencies.parents);
	if (interrupted) { rc=128+interrupted; error.code="E_INTERRUPTED"; }
	if (rc && rc!=7) fprintf(stderr,"%s native batch classification failed\n",error.code ? error.code :
		rc==5 ? "E_GENERATION" : rc==6 ? "E_BUSY" : "E_IO");
	jf_storage_close(&storage); jf_policy_free(policy); jf_generation_close(&pinned); selection_free(&selected);
	free(resolved); free(cwd); return rc;
}
