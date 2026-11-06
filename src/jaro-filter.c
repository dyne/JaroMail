#define _POSIX_C_SOURCE 200809L
#include "filter/generation.h"
#include "filter/headers.h"
#include "filter/storage.h"
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

static int classify(const char *cwd,const char *root,const struct selected *item,
	const struct jf_policy *policy,struct jf_storage *storage,size_t index,struct jf_error *global)
{
	struct jf_headers *headers=NULL;
	struct jf_error error={0};
	const struct jf_rule *winner=NULL;
	const char *detail=NULL;
	struct stat before,opened,after,current;
	char *source=absolute(cwd,item->path),*local=NULL,*parent=NULL,*name=NULL;
	struct jf_storage_recovery recovery={0};
	int fd=-1,parentfd=-1,rc=7,noop=0;
	if (!source) { global->code="E_MEMORY"; return 3; }
	if (item->error) { detail=item->error; goto done; }
	/* A selected inode gets one apply attempt. Do not trust a later name merely
	 * because an inode number/timestamp was recycled after the first attempt. */
	if (storage && item->first) {
		if (item->first->attempted) { detail="E_MESSAGE_CHANGED"; goto done; }
		item->first->attempted=1;
	}
	parentfd=open_parent(source,&parent,&name);
	if (parentfd<0) { if (errno==ENOMEM) { rc=3; detail="E_MEMORY"; } else detail="E_MESSAGE_OPEN"; goto done; }
	if (fstatat(parentfd,name,&before,AT_SYMLINK_NOFOLLOW)) { detail="E_MESSAGE_OPEN"; goto done; }
	if (!S_ISREG(before.st_mode)) { detail="E_MESSAGE_TYPE"; goto done; }
	/* Closing any descriptor for a POSIX record-lock inode releases this
	 * process's locks. Explicit input must never open our batch lock as mail. */
	if (storage && before.st_dev==storage->lock_identity.st_dev && before.st_ino==storage->lock_identity.st_ino) {
		detail="E_MESSAGE_TYPE"; goto done;
	}
	if (item->snapshotted && !jf_storage_same(&before,&item->identity)) { detail="E_MESSAGE_CHANGED"; goto done; }
	fd=openat(parentfd,name,O_RDONLY|O_NOFOLLOW|O_NONBLOCK);
	if (fd<0) { detail="E_MESSAGE_OPEN"; goto done; }
	if (fstat(fd,&opened) || !S_ISREG(opened.st_mode) || !jf_storage_same(&before,&opened)) {
		detail="E_MESSAGE_CHANGED"; goto done;
	}
	if (hook("message-opened")) { rc=3; goto done; }
	rc=jf_headers_read(fd,NULL,&headers,&error);
	if (rc) {
		if (rc==3 && (!error.code || strcmp(error.code,"E_MESSAGE_IO"))) goto done;
		detail=error.code; rc=7; goto done;
	}
	if (hook("headers-read")) { rc=3; goto done; }
	if (fstat(fd,&after) || fstatat(parentfd,name,&current,AT_SYMLINK_NOFOLLOW) || !jf_storage_same(&opened,&after) || !jf_storage_same(&opened,&current)) {
		detail="E_MESSAGE_CHANGED"; rc=7; goto done;
	}
	rc=jf_policy_classify(policy,jf_headers_message(headers),&winner,&error,NULL);
	if (rc) { detail=error.code; goto done; }
	local=join(root,winner->local);
	if (!local) { rc=3; goto done; }
	if (storage) {
		rc=jf_storage_move(storage,parentfd,name,fd,&opened,jf_headers_wire_size(headers),
			jf_headers_digest(headers),winner->local,source,&recovery,&detail);
		if (rc==1) { rc=0; noop=1; }
	} else if ((noop=already_there(parentfd,local))<0) rc=3;
done:
	if (fd>=0 && close(fd) && !rc) { rc=7; detail="E_MESSAGE_IO"; }
	if (parentfd>=0) close(parentfd);
	if (storage && noop && !rc && item->first) item->first->attempted=0;
	if (rc!=3 && record(index,item->path,winner,rc ? NULL : local,
		rc ? "error" : noop ? "noop" : storage ? "moved" : "classified",rc ? detail : NULL)) { rc=3; detail="E_OUTPUT"; }
	if (rc==3) global->code=detail ? detail : error.code ? error.code : "E_MEMORY";
	if (recovery.source) recovery_diagnostic(recovery.source);
	if (recovery.destination) recovery_diagnostic(recovery.destination);
	if (recovery.temporary) recovery_diagnostic(recovery.temporary);
	jf_headers_free(headers); free(source); free(local); free(parent);
	free(recovery.source); free(recovery.destination); free(recovery.temporary); return rc;
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
		if ((rc=jf_storage_begin(resolved,&pinned,&interrupted,&storage,&error.code))) goto done;
		apply=&storage;
		if (hook("batch-locked")) { rc=3; goto done; }
	}
	if (field("JFD1") || field(pinned.id)) { rc=3; goto done; }
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
	if (interrupted) { rc=128+interrupted; error.code="E_INTERRUPTED"; }
	if (rc && rc!=7) fprintf(stderr,"%s native batch classification failed\n",error.code ? error.code :
		rc==5 ? "E_GENERATION" : rc==6 ? "E_BUSY" : "E_IO");
	jf_storage_close(&storage); jf_policy_free(policy); jf_generation_close(&pinned); selection_free(&selected);
	free(resolved); free(cwd); return rc;
}
