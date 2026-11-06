#define _POSIX_C_SOURCE 200809L
#include "filter/generation.h"
#include "filter/contact.h"
#include "filter/policy_internal.h"
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char *absolute(const char *path)
{
	char *cwd,*out; size_t n,m;
	if (*path=='/') return strdup(path);
	cwd=getcwd(NULL,0); if (!cwd) return NULL;
	n=strlen(cwd); m=strlen(path); out=malloc(n+m+2);
	if (out) { memcpy(out,cwd,n); out[n]='/'; memcpy(out+n+1,path,m+1); }
	free(cwd); return out;
}

#ifndef __linux__
static char *executable(const char *name)
{
	const char *path,*end;
	if (strchr(name,'/')) return absolute(name);
	path=getenv("PATH"); if (!path) path="/bin:/usr/bin";
	for (;;) {
		size_t n; char *candidate,*resolved;
		end=strchr(path,':'); n=end ? (size_t)(end-path) : strlen(path);
		candidate=malloc(n+strlen(name)+2); if (!candidate) return NULL;
		if (n) { memcpy(candidate,path,n); candidate[n]='/'; strcpy(candidate+n+1,name); }
		else strcpy(candidate,name);
		if (!access(candidate,X_OK)) { resolved=absolute(candidate); free(candidate); return resolved; }
		free(candidate); if (!end) return NULL;
		path=end+1;
	}
}
#endif

static void escaped(const char *s)
{
	const unsigned char *p=(const unsigned char *)s;
	fputc('"',stderr);
	for (;*p;p++) if (*p<32 || *p>=127 || *p=='"' || *p=='\\') fprintf(stderr,"\\x%02x",*p); else fputc(*p,stderr);
	fputc('"',stderr);
}

int main(int argc,char **argv)
{
	const char *root=NULL,*accounts=NULL,*profile=NULL,*import=NULL;
	int check=0,report=0,legacy=0,i,rc=2;
	char *resolved=NULL,*compiler=NULL,*error_path=NULL;
	struct jf_compile_result result={0}; struct jf_error error={0};
	if (argc==3 && !strcmp(argv[1],"--check-address")) {
		char *copy=strdup(argv[2]); int valid;
		if (!copy) return 3;
		valid=jf_valid_utf8((const unsigned char *)copy,strlen(copy)) && jf_normalize_mailbox(copy);
		free(copy); return valid ? 0 : 4;
	}
	if ((argc==5 || (argc==6 && !strcmp(argv[5],"--check"))) && !strcmp(argv[1],"--add-contact")) {
		rc=jf_contact_add(argv[2],argv[3],argv[4],argc==6,&error);
		if (rc!=0 && rc!=1) {
			fprintf(stderr,"%s ",error.code ? error.code : "E_IO"); escaped(argv[2]);
			fprintf(stderr," %s\n",error.detail);
		}
		return rc;
	}
	for (i=1;i<argc;i++) {
		const char **slot=NULL;
		if (!strcmp(argv[i],"--help") && argc==2) {
			puts("jaro-filter-compile --root ROOT [--accounts-dir DIR] [--profile FILE] [--import-sieve FILE [--legacy-generated]] [--check] [--report0]");
			puts("jaro-filter-compile --add-contact BOOK ADDRESS NAME [--check]\njaro-filter-compile --check-address ADDRESS"); return 0;
		}
		if (!strcmp(argv[i],"--version") && argc==2) { puts(JF_COMPILER_ID); return 0; }
		if (!strcmp(argv[i],"--root")) slot=&root;
		else if (!strcmp(argv[i],"--accounts-dir")) slot=&accounts;
		else if (!strcmp(argv[i],"--profile")) slot=&profile;
		else if (!strcmp(argv[i],"--import-sieve")) slot=&import;
		else if (!strcmp(argv[i],"--check") && !check) check=1;
		else if (!strcmp(argv[i],"--report0") && !report) report=1;
		else if (!strcmp(argv[i],"--legacy-generated") && !legacy) legacy=1;
		else if (!strcmp(argv[i],"--") && i+1==argc) break;
		else goto usage;
		if (slot) { if (*slot || i+1==argc || !*argv[i+1]) goto usage; *slot=argv[++i]; }
	}
	if (!root || !*root || (legacy && !import)) goto usage;
	resolved=absolute(root);
#ifdef __linux__
	compiler=strdup("/proc/self/exe");
#else
	compiler=executable(argv[0]);
#endif
	if (!resolved || !compiler) { rc=3; goto done; }
	rc=import ? jf_compile_import(resolved,accounts,profile,compiler,import,legacy,check,&result,&error,&error_path) :
		jf_compile(resolved,accounts,profile,compiler,check,&result,&error,&error_path);
	if (result.warnings.size && (fwrite(result.warnings.data,1,result.warnings.size,stderr)!=result.warnings.size || fflush(stderr))) {
		rc=jf_product_error(&error,"E_IO",0,"cannot write addressbook warnings");
	}
	if (!rc && report) {
		fwrite("JFC1",1,5,stdout); fwrite(result.id,1,65,stdout);
		fwrite(result.changed ? "1" : "0",1,2,stdout);
		fwrite(result.directory ? result.directory : "",1,result.directory ? strlen(result.directory)+1 : 1,stdout);
		if (fflush(stdout) || ferror(stdout)) rc=3;
	}
done:
	if (rc) {
		fprintf(stderr,"%s ",error.code ? error.code : "E_IO"); escaped(error_path ? error_path : root);
		fprintf(stderr,":%zu %s\n",error.line,error.detail);
	}
	free(resolved); free(compiler); free(error_path); jf_compile_result_free(&result); return rc;
usage:
	fputs("E_USAGE invalid compiler invocation\n",stderr); return 2;
}
