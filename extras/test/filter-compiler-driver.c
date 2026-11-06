/* Test-only adapter: builds real compiler products into a caller-owned private
 * directory. It neither publishes a selector nor implements application CLI. */
#define _POSIX_C_SOURCE 200809L
#include "filter/snapshot.h"
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

static void escaped(const char *s)
{
	const unsigned char *p=(const unsigned char *)s;
	fputc('"',stderr);
	for (;*p;p++) {
		if (*p<' ' || *p>=127 || *p=='"' || *p=='\\') fprintf(stderr,"\\x%02x",*p);
		else fputc(*p,stderr);
	}
	fputc('"',stderr);
}

static int save(int dir, const char *name, const struct jf_bytes *b)
{
	int fd=openat(dir,name,O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW,0600), rc=3;
	size_t pos=0;
	if (fd<0) return 3;
	while (pos<b->size) {
		ssize_t n=write(fd,b->data+pos,b->size-pos);
		if (n<0 && errno==EINTR) continue;
		if (n<=0) goto done;
		pos+=(size_t)n;
	}
	rc=0;
done:
	if (close(fd)) rc=3;
	return rc;
}

static int load(const char *path, struct jf_bytes *b, size_t limit)
{
	FILE *f=fopen(path,"rb");
	unsigned char data[16384];
	size_t n;
	int rc=3;
	if (!f) return 3;
	while ((n=fread(data,1,sizeof data,f))) if (jf_bytes_add_limit(b,data,n,limit)) goto done;
	rc=ferror(f) ? 3 : 0;
done:
	if (fclose(f)) rc=3;
	return rc;
}

static int bounds(void)
{
	size_t total=0;
	struct jf_bytes payload={NULL,JF_CACHE_MAX,JF_CACHE_MAX};
	struct jf_bytes whole={NULL,JF_CACHE_FILE_MAX,JF_CACHE_FILE_MAX}, small={0};
	int failed=0;
	failed+=jf_cache_file_size(JF_CACHE_MAX,&total)!=0 || total!=JF_CACHE_MAX+88u;
	failed+=jf_cache_file_size(JF_CACHE_MAX+1u,&total)!=3;
	failed+=jf_cache_file_size(SIZE_MAX,&total)!=3;
	failed+=!jf_bytes_fits(JF_CACHE_MAX-1,1,JF_CACHE_MAX);
	failed+=jf_bytes_fits(JF_CACHE_MAX,1,JF_CACHE_MAX)!=0;
	failed+=!jf_bytes_fits(JF_CACHE_HEADER_SIZE,JF_CACHE_MAX,JF_CACHE_FILE_MAX);
	failed+=jf_bytes_fits(SIZE_MAX,1,SIZE_MAX)!=0;
	failed+=jf_bytes_fits(JF_INPUT_MAX,1,JF_INPUT_MAX)!=0;
	failed+=jf_bytes_add(&payload,"x",1)!=3;
	failed+=jf_bytes_add_limit(&whole,"x",1,JF_CACHE_FILE_MAX)!=3;
	failed+=jf_bytes_add_limit(&small,"12345678",8,12)!=0 || jf_bytes_add_limit(&small,"data",4,12)!=0 ||
		small.size!=12 || memcmp(small.data,"12345678data",12);
	failed+=jf_bytes_add_limit(&small,"x",1,12)!=3 || small.size!=12;
	jf_bytes_free(&small);
	if (failed) return 1;
	puts("PASS 12 product-boundary checks: 128 MiB payload plus 88-byte header; separate text/input caps; overflow rejected");
	return 0;
}

struct fault_allocator { size_t calls,fail,live; };
static void *fault_allocate(void *context,size_t size)
{
	struct fault_allocator *f=context;
	void *p;
	if (++f->calls==f->fail) return NULL;
	p=malloc(size); if (p) f->live++;
	return p;
}
static void fault_release(void *context,void *p)
{
	struct fault_allocator *f=context;
	if (p) { f->live--; free(p); }
}
static int restore(const char *path)
{
	struct jf_bytes cache={0}; struct jf_error error={0};
	struct jf_policy *policy=NULL;
	size_t fail;
	int rc=load(path,&cache,JF_CACHE_FILE_MAX);
	if (rc) return rc;
	for (fail=1;fail<10000;fail++) {
		struct fault_allocator fault={0,fail,0};
		struct jf_allocator allocator={&fault,fault_allocate,fault_release};
		const struct jf_rule *winner=NULL;
		struct jf_message empty={0};
		rc=jf_cache_policy(&cache,&allocator,&policy,&error);
		if (rc) {
			if (rc!=3 || policy || fault.live) { rc=1; break; }
			continue;
		}
		/* The returned policy owns all strings; no cache-backed views escape. */
		memset(cache.data,0,cache.size);
		rc=jf_policy_classify(policy,&empty,&winner,&error,NULL);
		if (rc || !winner || strcmp(winner->id,"builtin:unsorted")) rc=1;
		jf_policy_free(policy); policy=NULL;
		if (fault.live) rc=1;
		printf("cache reconstruction: %zu allocator failures; independent owned model\n",fail-1);
		break;
	}
	jf_bytes_free(&cache); return fail==10000 ? 1 : rc;
}

/* Simulate a valid prior-generation identity from another compiler recipe.
 * Test-only: no policy or runtime configuration hook is introduced. */
static int prior_identity(struct jf_snapshot *s, const char *role)
{
	size_t i;
	for (i=0;i<s->dependency_count;i++) if (!strcmp(s->dependencies[i].role,role)) {
		struct jf_dependency *d=&s->dependencies[i];
		if (!strncmp(role,"config.",7)) {
			if (jf_bytes_text(&d->auxiliary,"-prior")) return 3;
			jf_digest(d->auxiliary.data,d->auxiliary.size,d->digest);
		} else jf_digest("prior executable",16,d->digest);
		return 0;
	}
	return 2;
}

int main(int argc, char **argv)
{
	struct jf_snapshot snapshot={0},second={0};
	struct jf_products products={0};
	struct jf_error error={0};
	const char *path="";
	int dir=-1,rc=2;
	if (argc==2 && !strcmp(argv[1],"--bounds")) return bounds();
	if (argc==3 && !strcmp(argv[1],"--restore")) return restore(argv[2]);
	if (argc<3 || argc>6) return 2;
	if (!strcmp(argv[1],"--validate") && argc==5) {
		if (load(argv[2],&products.cache,JF_CACHE_FILE_MAX) || load(argv[3],&products.zsh,JF_CACHE_MAX) || load(argv[4],&products.sieve,JF_CACHE_MAX)) rc=3;
		else rc=jf_cache_validate(products.cache.data,products.cache.size,&products.zsh,&products.sieve,NULL,&error);
		goto done;
	}
	rc=jf_snapshot_read(argv[1],argc>4 && *argv[4] ? argv[4] : NULL,
		argc>3 && *argv[3] ? argv[3] : NULL,argv[0],NULL,&snapshot,&error);
	path=snapshot.error_path ? snapshot.error_path : "";
	if (rc) goto done;
	if (argc==6) {
		if (strcmp(argv[5],"reuse") && (rc=prior_identity(&snapshot,argv[5]))) goto done;
		rc=jf_snapshot_read(argv[1],*argv[4] ? argv[4] : NULL,*argv[3] ? argv[3] : NULL,argv[0],&snapshot,&second,&error);
		if (rc) { path=second.error_path ? second.error_path : ""; goto done; }
		jf_snapshot_free(&snapshot); snapshot=second; memset(&second,0,sizeof second);
		path=snapshot.error_path ? snapshot.error_path : "";
	}
	rc=jf_products_build(snapshot.policy,&snapshot.profile,snapshot.dependencies,snapshot.dependency_count,&products,&error);
	if (rc) {
		const struct jf_rule *rule=jf_policy_rule(snapshot.policy,error.source);
		path=rule ? rule->source : snapshot.profile_path; goto done;
	}
	dir=open(argv[2],O_RDONLY|O_DIRECTORY|O_NOFOLLOW);
	if (dir<0 || save(dir,"filters.zsh",&products.zsh) || save(dir,"Filters.sieve",&products.sieve) || save(dir,"policy.bin",&products.cache)) { rc=3; goto done; }
	fwrite(products.id,1,65,stdout);
	rc=ferror(stdout) ? 3 : 0;
done:
	if (rc) {
		fprintf(stderr,"%s ",error.code ? error.code : "E_IO"); escaped(path);
		fprintf(stderr,":%zu %s\n",error.line,error.detail);
	}
	if (dir>=0) close(dir);
	jf_products_free(&products); jf_snapshot_free(&snapshot); jf_snapshot_free(&second); return rc;
}
