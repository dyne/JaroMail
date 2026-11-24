#define _POSIX_C_SOURCE 200809L
#include "snapshot.h"
#include "policy_internal.h"
#include "contact/migrate.h"
#include <sys/stat.h>
#include <sys/types.h>
#include <dirent.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

static char *join(const char *a, const char *b)
{
	size_t x=strlen(a),y=strlen(b);
	char *p;
	if (x>SIZE_MAX-y-2) return NULL;
	p=malloc(x+y+2);
	if (p) { memcpy(p,a,x); p[x]='/'; memcpy(p+x+1,b,y+1); }
	return p;
}

const struct jf_dependency *jf_snapshot_dependency(const struct jf_snapshot *s, const char *role, const char *path)
{
	size_t i;
	if (s) for (i=0;i<s->dependency_count;i++)
		if (!strcmp(s->dependencies[i].role,role) && !strcmp(s->dependencies[i].path,path)) return &s->dependencies[i];
	return NULL;
}

void jf_snapshot_free(struct jf_snapshot *s)
{
	size_t i;
	jf_policy_free(s->policy); jf_profile_free(&s->profile);
	for (i=0;i<s->source_count;i++) {
		free((void *)s->sources[i].identity); free((void *)s->sources[i].path); free((void *)s->sources[i].data);
	}
	for (i=0;i<s->dependency_count;i++) {
		free(s->dependencies[i].role); free(s->dependencies[i].path); jf_bytes_free(&s->dependencies[i].auxiliary);
	}
	free(s->sources); free(s->dependencies); free(s->profile_path); free(s->error_path); jf_bytes_free(&s->warnings); memset(s,0,sizeof *s);
}

static int diagnostic(struct jf_snapshot *s, const char *path)
{
	char *copy=strdup(path);
	if (!copy) return 3;
	free(s->error_path); s->error_path=copy; return 0;
}

static int dep(struct jf_snapshot *s, const char *role, const char *path, int present,
	const void *bytes, size_t n, const void *aux, size_t an)
{
	struct jf_dependency *v,*d;
	if (s->dependency_count==JF_DEPENDENCY_MAX) return 3;
	v=realloc(s->dependencies,(s->dependency_count+1)*sizeof *v); if (!v) return 3;
	s->dependencies=v; d=&v[s->dependency_count++]; memset(d,0,sizeof *d);
	d->role=strdup(role); d->path=strdup(path); d->present=present;
	if (!d->role || !d->path || jf_bytes_add(&d->auxiliary,aux,an)) return 3;
	if (present) jf_digest(bytes,n,d->digest);
	return 0;
}

static int config(struct jf_snapshot *s, const char *role, const char *value)
{
	return dep(s,role,"",1,value,strlen(value),value,strlen(value));
}

static int source(struct jf_snapshot *s, enum jf_source_kind kind, const char *identity,
	const char *path, struct jf_bytes *bytes)
{
	struct jf_source *v,*p;
	if (s->source_count==JF_RULE_MAX) return 3;
	v=realloc(s->sources,(s->source_count+1)*sizeof *v); if (!v) return 3;
	s->sources=v; p=&v[s->source_count++]; memset(p,0,sizeof *p);
	p->kind=kind; p->identity=strdup(identity); p->path=strdup(path); p->data=bytes->data; p->size=bytes->size;
	memset(bytes,0,sizeof *bytes);
	return !p->identity || !p->path ? 3 : 0;
}

static int readfd(int fd, struct jf_bytes *b)
{
	unsigned char buffer[16384];
	ssize_t n;
	while ((n=read(fd,buffer,sizeof buffer))!=0) {
		if (n<0) { if (errno==EINTR) continue; return 3; }
		if ((size_t)n>JF_INPUT_MAX-b->size || jf_bytes_add(b,buffer,(size_t)n)) return 3;
	}
	return 0;
}

static int link_dep(struct jf_snapshot *s, const char *role, const char *path)
{
	struct stat st;
	char target[JF_LINE_MAX+1], *name;
	ssize_t n;
	if (lstat(path,&st)) return errno==ENOENT ? 0 : 3;
	if (!S_ISLNK(st.st_mode)) return 0;
	n=readlink(path,target,sizeof target);
	if (n<0 || (size_t)n==sizeof target) return 3;
	name=join("link",role); if (!name) return 3;
	name[4]='.';
	n=dep(s,name,path,1,target,(size_t)n,target,(size_t)n); free(name); return (int)n;
}

static int input(struct jf_snapshot *s, const char *role, const char *path, int required, struct jf_bytes *bytes)
{
	int fd,rc;
	struct stat st,after;
	size_t i;
	if (diagnostic(s,path)) return 3;
	for (i=0;i<s->override_count;i++) if (!s->overrides[i].role && !strcmp(path,s->overrides[i].path)) {
		const struct jf_bytes *b=s->overrides[i].bytes;
		if (jf_bytes_add(bytes,b->data,b->size)) return 3;
		return dep(s,role,path,1,b->data,b->size,NULL,0);
	}
	if (link_dep(s,role,path)) return 3;
	fd=open(path,O_RDONLY|O_NONBLOCK);
	if (fd<0) {
		if (errno==ENOENT && !strcmp(role,"input.account")) { s->changed=1; return 6; }
		if (errno==ENOENT && !required && lstat(path,&st)<0 && errno==ENOENT)
			return dep(s,role,path,0,NULL,0,NULL,0);
		return 3;
	}
	rc=fstat(fd,&st) || !S_ISREG(st.st_mode) ? 3 : readfd(fd,bytes);
	if (!rc && (fstat(fd,&after) || st.st_size!=after.st_size ||
		st.st_mtime!=after.st_mtime || st.st_ctime!=after.st_ctime ||
#ifdef __APPLE__
		st.st_mtimespec.tv_nsec!=after.st_mtimespec.tv_nsec || st.st_ctimespec.tv_nsec!=after.st_ctimespec.tv_nsec)) {
#else
		st.st_mtim.tv_nsec!=after.st_mtim.tv_nsec || st.st_ctim.tv_nsec!=after.st_ctim.tv_nsec)) {
#endif
		s->changed=1; rc=6;
	}
	if (close(fd) && !rc) rc=3;
	if (!rc) rc=dep(s,role,path,1,bytes->data,bytes->size,NULL,0);
	return rc;
}

static int compare_names(const void *a, const void *b)
{
	return strcmp(*(const char *const *)a,*(const char *const *)b);
}

static int inventory_item(struct jf_bytes *b, const char *path, unsigned char kind)
{
	/* NUL framing is unambiguous for POSIX paths, including non-UTF8/newlines. */
	return jf_bytes_add(b,&kind,1) || jf_bytes_add(b,path,strlen(path)+1);
}

static int accounts_walk(struct jf_snapshot *s, const char *base, const char *relative,
	struct jf_bytes *inventory, unsigned depth, int external)
{
	char **names=NULL, *dirpath=*relative ? join(base,relative) : strdup(base);
	DIR *dir=NULL;
	struct dirent *ent;
	size_t count=0,i;
	int rc=3;
	if (!dirpath || depth>64) goto done;
	dir=opendir(dirpath);
	if (!dir) { if (errno==ENOENT) s->changed=1; goto done; }
	errno=0;
	while ((ent=readdir(dir))) {
		char **v;
		if (!strcmp(ent->d_name,".") || !strcmp(ent->d_name,"..")) continue;
		if (count==JF_RULE_MAX) goto done;
		v=realloc(names,(count+1)*sizeof *v); if (!v) goto done;
		names=v; names[count]=strdup(ent->d_name); if (!names[count]) goto done;
		count++; errno=0;
	}
	if (errno) goto done;
	if (closedir(dir)) { dir=NULL; goto done; } dir=NULL;
	if (count>1) qsort(names,count,sizeof *names,compare_names);
	for (i=0;i<count;i++) {
		char *rel=*relative ? join(relative,names[i]) : strdup(names[i]);
		char *path=rel ? join(base,rel) : NULL, *identity=rel ? join(external ? "accounts:" : "Accounts",rel) : NULL;
		struct stat lst,st;
		struct jf_bytes bytes={0};
		int er=3;
		if (!path || !identity) goto item;
		if (lstat(path,&lst)) { if (errno==ENOENT) s->changed=1; goto item; }
		/* External account roots retain the documented accounts: identity. */
		if (external) memmove(identity+9,identity+10,strlen(identity+10)+1);
		if (S_ISDIR(lst.st_mode)) {
			if (inventory_item(inventory,rel,'d')) goto item;
			er=accounts_walk(s,base,rel,inventory,depth+1,external);
		} else {
			if (stat(path,&st) || !S_ISREG(st.st_mode) || inventory_item(inventory,rel,S_ISLNK(lst.st_mode) ? 'l' : 'f')) goto item;
			er=input(s,"input.account",path,1,&bytes);
			if (!er) er=source(s,JF_ACCOUNT,identity,path,&bytes);
		}
	item:
		jf_bytes_free(&bytes); free(rel); free(path); free(identity);
		if (er) goto done;
	}
	rc=0;
done:
	if (dir) closedir(dir);
	for (i=0;i<count;i++) free(names[i]);
	free(names); free(dirpath); return rc;
}

const char *const jf_contact_roles[2]={"contacts.allow","contacts.block"};

static int append_key(struct jaro_contact_text key,void *data)
{
	struct jf_bytes *bytes=data;
	return jf_bytes_add_limit(bytes,key.data,key.size,JF_INPUT_MAX) ||
		jf_bytes_add_limit(bytes,"\n",1,JF_INPUT_MAX);
}

static int contact_sources(struct jf_snapshot *s,const char *root,const char *path,int preview,
	struct jaro_contact_store *borrowed,int (*snapshot_hook)(const char *),struct jf_error *error)
{
	static const struct jaro_contact_text names[]={{"allow",5},{"block",5}};
	struct jaro_contact_store *store=borrowed;
	struct jaro_contact_migration_error migration;
	struct jaro_contact_error detail={0};
	int status=0,rc=0;

	if (!store) {
		status=jaro_contact_profile_open(root,preview ? JARO_CONTACT_PROFILE_PREVIEW :
			JARO_CONTACT_PROFILE_READ,&store,&migration);
		if (status) detail.reason=migration.reason;
		if (!status) status=jaro_contact_begin_read(store,&detail);
	}
	for (size_t i=0;!status && !rc && i<2;i++) {
		struct jf_bytes keys={0};
		const struct jf_bytes *override=NULL;
		for (size_t j=0;j<s->override_count;j++)
			if (s->overrides[j].role && !strcmp(s->overrides[j].role,jf_contact_roles[i])) {
				override=s->overrides[j].bytes;
				break;
			}
		if (override)
			rc=jf_bytes_add(&keys,override->data,override->size);
		else
			status=jaro_contact_visit_keys(store,names[i],append_key,&keys,&detail);
		if (!status && !rc)
			rc=dep(s,jf_contact_roles[i],path,1,keys.data,keys.size,keys.data,keys.size) ||
				source(s,i ? JF_BLACKLIST : JF_KNOWN,names[i].data,path,&keys);
		jf_bytes_free(&keys);
		if (!i && !status && !rc && snapshot_hook && snapshot_hook("contacts-allow-read"))
			rc=3;
	}
	if (store && !borrowed) {
		if (status || rc) jaro_contact_rollback(store,NULL);
		else status=jaro_contact_commit(store,&detail);
		jaro_contact_close(store,NULL);
	}
	if (status) {
		jf_product_error(error,status==JARO_CONTACT_BUSY ? "E_CONTACT_BUSY" : "E_CONTACT_STORAGE",
			0,detail.reason);
		if (status==JARO_CONTACT_BUSY) s->changed=1;
		return status==JARO_CONTACT_BUSY ? 6 : 3;
	}
	return rc ? jf_product_error(error,"E_MEMORY",0,"contact membership exceeds snapshot limits") : 0;
}

static int collect(const char *root, const char *accounts_arg, const char *profile_arg,
	const char *compiler, const char *runtime, int preview, struct jaro_contact_store *contacts,
	int (*snapshot_hook)(const char *),const struct jf_input_override *overrides,
	size_t override_count, struct jf_snapshot *out, struct jf_error *error)
{
	struct jf_snapshot s={0};
	struct jf_bytes bytes={0},inventory={0},execbytes={0};
	char *accounts=accounts_arg ? strdup(accounts_arg) : join(root,"Accounts");
	char *path=NULL;
	struct stat st;
	size_t i;
	int rc=3;
	s.overrides=overrides; s.override_count=override_count;
	memset(out,0,sizeof *out); if (error) memset(error,0,sizeof *error);
	if (!accounts) goto memory;
	if (diagnostic(&s,root) || link_dep(&s,"root",root)) goto io;
	if (config(&s,"config.compiler-build",JF_COMPILER_ID) || config(&s,"config.cache-version","1") ||
		config(&s,"config.policy-version","2") || config(&s,"config.regex-dialect","posix-ere-c-icase-v1") ||
		config(&s,"config.locale","C") || config(&s,"config.parser","bounded-rfc822-v2") ||
		config(&s,"config.address-normalization",JF_ADDRESS_NORMALIZATION) ||
		config(&s,"config.contact-membership","sqlite-sorted-mailbox-keys-v1") ||
		config(&s,"config.address-case","ascii-mailbox-casemap-v1") || config(&s,"config.sieve-comparator","i;ascii-casemap") ||
		config(&s,"config.regex-catalogue",jf_regex_catalogue()) ||
		config(&s,"config.regex-qualification",jf_regex_qualification())) goto memory;
	/* Executable bytes, not version text or mtime, bind compiler identity. */
	if (!compiler || input(&s,"input.compiler",compiler,1,&execbytes)) goto io;
	jf_bytes_free(&execbytes);
	if (runtime) {
		path=join(runtime,"SHA256SUMS");
		if (!path || input(&s,"input.sieve-runtime",path,1,&bytes)) goto io;
		jf_bytes_free(&bytes); free(path); path=NULL;
	}
	path=join(root,"Filters.txt"); if (!path) goto memory;
	if (input(&s,"input.filters",path,1,&bytes)) goto io;
	if (source(&s,JF_FILTERS,"Filters.txt",path,&bytes)) goto memory;
	free(path); path=join(root,"Aliases.txt"); if (!path) goto memory;
	if (input(&s,"input.aliases",path,0,&bytes)) goto io;
	if (source(&s,JF_ALIASES,"Aliases.txt",path,&bytes)) goto memory;
	free(path); path=NULL;
	if (diagnostic(&s,accounts)) goto memory;
	if (link_dep(&s,"inventory.accounts",accounts)) goto io;
	if (stat(accounts,&st)) {
		if (errno!=ENOENT || dep(&s,"inventory.accounts",accounts,0,NULL,0,NULL,0)) goto io;
	} else {
		if (!S_ISDIR(st.st_mode) || accounts_walk(&s,accounts,"",&inventory,0,accounts_arg!=NULL)) goto io;
		if (dep(&s,"inventory.accounts",accounts,1,inventory.data,inventory.size,inventory.data,inventory.size)) goto memory;
	}
	s.profile_path=profile_arg ? strdup(profile_arg) : join(root,"FilterProfile.txt");
	if (!s.profile_path) goto memory;
	if (input(&s,"input.profile",s.profile_path,profile_arg!=NULL,&bytes)) goto io;
	if ((rc=jf_profile_parse(bytes.data,bytes.size,&s.profile,error))) goto done;
	if (dep(&s,"config.profile","",1,bytes.data,bytes.size,bytes.data,bytes.size)) goto memory;
	if (config(&s,"config.profile-id",s.profile.id) || config(&s,"config.namespace-prefix",s.profile.prefix) ||
		config(&s,"config.namespace-separator",s.profile.separator) ||
		config(&s,"config.create-mailboxes",s.profile.create ? "yes" : "no") ||
		config(&s,"config.capabilities",s.profile.regex ? (s.profile.mailbox ? "fileinto,mailbox,regex" : "fileinto,regex") :
			s.profile.mailbox ? "fileinto,mailbox" : "fileinto")) goto memory;
	{
		struct jf_bytes mappings={0};
		/* NUL separates UTF8 mapping keys/values. Include builtin defaults too;
		 * explicit mappings follow and override their named logical keys. */
		static const char defaults[]="known\0INBOX\0spam\0Spam\0";
		int failure=jf_bytes_add(&mappings,defaults,sizeof defaults-1);
		for (i=0;!failure && i<s.profile.count;i++) {
			failure=jf_bytes_add(&mappings,s.profile.mappings[i].logical,strlen(s.profile.mappings[i].logical)+1) ||
				jf_bytes_add(&mappings,s.profile.mappings[i].mailbox,strlen(s.profile.mappings[i].mailbox)+1);
		}
		if (!failure) failure=dep(&s,"config.mailbox-mappings","",1,mappings.data,mappings.size,mappings.data,mappings.size);
		jf_bytes_free(&mappings); if (failure) goto memory;
	}
	jf_bytes_free(&bytes);
	path=join(root,JARO_CONTACT_DATABASE); if (!path || diagnostic(&s,path)) goto memory;
	rc=contact_sources(&s,root,path,preview,contacts,snapshot_hook,error);
	if (rc) goto done;
	rc=0;
	goto done;
memory:
	rc=jf_product_error(error,"E_MEMORY",0,"snapshot allocation/size limit"); goto done;
io:
	if (s.changed) { jf_product_error(error,"E_SNAPSHOT_CHANGED",0,"input changed while reading"); rc=6; }
	else rc=jf_product_error(error,"E_IO",0,"cannot snapshot filter input");
done:
	free(accounts); free(path); jf_bytes_free(&bytes); jf_bytes_free(&execbytes); jf_bytes_free(&inventory);
	s.overrides=NULL; s.override_count=0; *out=s; return rc;
}

int jf_snapshot_collect(const char *root, const char *accounts, const char *profile,
	const char *compiler, const char *runtime, int preview, struct jaro_contact_store *contacts,
	int (*snapshot_hook)(const char *),struct jf_snapshot *out, struct jf_error *error)
{
	return collect(root,accounts,profile,compiler,runtime,preview,contacts,snapshot_hook,NULL,0,out,error);
}

int jf_snapshot_overlay(const char *root, const char *accounts, const char *profile,
	const char *compiler, const char *runtime, int preview, const struct jf_input_override *overrides, size_t count,
	struct jf_snapshot *out, struct jf_error *error)
{
	return collect(root,accounts,profile,compiler,runtime,preview,NULL,NULL,overrides,count,out,error);
}

int jf_snapshot_parse(struct jf_snapshot *s, struct jf_error *error)
{
	int rc=jf_policy_parse(s->sources,s->source_count,NULL,&s->policy,error);
	if (rc && error && error->source<s->source_count && diagnostic(s,s->sources[error->source].path))
		return jf_product_error(error,"E_MEMORY",0,"diagnostic allocation failed");
	return rc;
}

int jf_snapshot_read(const char *root, const char *accounts, const char *profile,
	const char *compiler, const struct jf_snapshot *prior, struct jf_snapshot *out, struct jf_error *error)
{
	(void)prior;
	int rc=jf_snapshot_collect(root,accounts,profile,compiler,NULL,1,NULL,NULL,out,error);
	return rc ? rc : jf_snapshot_parse(out,error);
}
