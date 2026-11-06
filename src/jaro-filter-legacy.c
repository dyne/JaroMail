/* Private workflow guard/lock bridge. Never an alternative native engine. */
#define _POSIX_C_SOURCE 200809L
#include "filter/generation.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void escaped(const char *s)
{
	const unsigned char *p=(const unsigned char *)s;
	fputc('"',stderr);
	for (;*p;p++) if (*p<32 || *p>=127 || *p=='"' || *p=='\\') fprintf(stderr,"\\x%02x",*p); else fputc(*p,stderr);
	fputc('"',stderr);
}

int main(int argc,char **argv)
{
	const char *root; char *path; FILE *file; unsigned char block[16384]; size_t n,i;
	struct jf_bytes bytes={0}; struct jf_source source={0};
	struct jf_policy *policy=NULL; struct jf_error error={0}; int rc=3,check;
	if (argc<4 || strcmp(argv[1],"--root") || !*argv[2]) return 2;
	root=argv[2]; check=argc==4 && !strcmp(argv[3],"--check");
	if (!check && (argc<5 || strcmp(argv[3],"--"))) return 2;
	path=malloc(strlen(root)+13); if (!path) return 3;
	sprintf(path,"%s/Filters.txt",root);
	file=fopen(path,"rb"); if (!file) goto done;
	while ((n=fread(block,1,sizeof block,file))) if (jf_bytes_add_limit(&bytes,block,n,JF_INPUT_MAX)) break;
	if (n || ferror(file)) { fclose(file); goto done; }
	if (fclose(file)) goto done;
	source.kind=JF_FILTERS; source.identity="Filters.txt"; source.path=path; source.data=bytes.data; source.size=bytes.size;
	rc=jf_policy_parse(&source,1,NULL,&policy,&error); if (rc) goto done;
	for (i=0;i<jf_policy_count(policy);i++) {
		const struct jf_rule *r=jf_policy_rule(policy,i);
		if (r->stage!=2 && r->stage!=6 && r->stage!=8 && r->kind!=JF_ERE) {
			rc=jf_product_error(&error,"E_LEGACY_POLICY",r->line,"typed/domain records require JARO_FILTER_ENGINE=native; legacy would discard them"); goto done;
		}
	}
	rc=check ? 0 : jf_generation_legacy_run(root,argv+4);
done:
	if (rc) { fprintf(stderr,"%s ",error.code ? error.code : "E_LEGACY_IO"); escaped(path);
		fprintf(stderr,":%zu %s\n",error.line,error.detail); }
	free(path); jf_bytes_free(&bytes); jf_policy_free(policy); return rc;
}
