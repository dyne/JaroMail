/* Test-only snapshot adapter. It is not installed or used by Jaromail. */
#define _POSIX_C_SOURCE 200809L
#include "filter/policy.h"
#include "filter/sha256.h"
#include "filter/headers.h"
#include "rfc822.h"
#include <fcntl.h>
#include <locale.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void field(const char *s)
{
	fwrite(s,1,strlen(s)+1,stdout);
}

static void number(size_t n)
{
	char value[32];
	snprintf(value,sizeof value,"%zu",n);
	field(value);
}

static void write_rule(const struct jf_rule *r)
{
	field(r->id); field(r->selector); field(r->predicate); field(r->action);
	field(r->logical); field(r->local); field(r->sieve); field(r->source); field(r->source_identity);
	number(r->line); number(r->occurrence); number(r->kind); number(r->stage); number(r->fields);
}

/* Test protocol: field-number NUL parsed-value NUL; empty field-number ends
 * each message. These are authored parsed values, never raw RFC822 headers.
 */
struct fault { size_t calls, fail, live; };
static void *fault_allocate(void *ctx, size_t n)
{
	struct fault *f=ctx;
	void *p;
	if (++f->calls==f->fail) return NULL;
	p=malloc(n);
	if (p) f->live++;
	return p;
}

static void fault_release(void *ctx, void *p)
{
	struct fault *f=ctx;
	if (p) { f->live--; free(p); }
}

#define CHECK(x) do { if (!(x)) { fprintf(stderr,"selftest line %d: %s\n",__LINE__,#x); return 1; } } while (0)
static int selftest(void)
{
	static const unsigned char rules[]="from ^List\\+@example[.]org$ save lists\n"
		"to-address TEAM@EXAMPLE.ORG move team\nknown-domain *.example.org\n"
		"from-address x@example.org move old\nfrom-address X@example.org move new\n";
	static const unsigned char invalid[]="from [ move bad\n";
	struct jf_source src={JF_FILTERS,"Filters.txt","/root/Filters.txt",rules,sizeof rules-1};
	struct jf_policy *policy=NULL;
	struct jf_error error;
	struct fault f={0,0,0};
	struct jf_allocator a={&f,fault_allocate,fault_release};
	struct jf_sha256 hash;
	char digest[65];
	char many[4096];
	char header_bytes[8192], saved_headers[8192];
	struct jf_headers *headers=NULL;
	ADDRESS *legacy;
	int pipe_fds[2];
	size_t header_faults, invalid_header_faults;
	size_t count, valid_faults, i;
	locale_t thread_locale=uselocale((locale_t)0);
	jf_sha256_init(&hash); jf_sha256_hex(&hash,digest);
	CHECK(!strcmp(digest,"e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"));
	jf_sha256_init(&hash); jf_sha256_update(&hash,"abc",3); jf_sha256_hex(&hash,digest);
	CHECK(!strcmp(digest,"ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
	jf_sha256_init(&hash);
	for (i=0;i<1000000;i++) jf_sha256_update(&hash,"a",1);
	jf_sha256_hex(&hash,digest);
	CHECK(!strcmp(digest,"cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0"));
	strcpy(many,(const char *)rules);
	for (i=0;i<40;i++) strcat(many,"known-domain resize.example\n");
	src.data=(const unsigned char *)many; src.size=strlen(many);
	CHECK(jf_policy_parse(&src,1,&a,&policy,&error)==0);
	CHECK(policy && jf_policy_count(policy)==8);
	CHECK(jf_policy_rule(policy,99)==NULL);
	CHECK(uselocale((locale_t)0)==thread_locale);
	jf_policy_free(policy); CHECK(f.live==0); count=f.calls; valid_faults=count;
	for (i=1;i<=count;i++) {
		f.calls=0; f.fail=i; policy=(void *)(uintptr_t)1;
		CHECK(jf_policy_parse(&src,1,&a,&policy,&error)==3);
		CHECK(!policy && !strcmp(error.code,"E_MEMORY") && f.live==0);
	}
	f.fail=0; f.calls=0; src.data=invalid; src.size=sizeof invalid-1;
	CHECK(jf_policy_parse(&src,1,&a,&policy,&error)==4 && !policy && f.live==0);
	CHECK(!strcmp(error.code,"E_REGEX") && error.line==1 && error.source==0);
	count=f.calls;
	for (i=1;i<=count;i++) {
		f.calls=0; f.fail=i;
		CHECK(jf_policy_parse(&src,1,&a,&policy,&error)==3 && !policy && f.live==0);
	}
	src.size=(size_t)JF_INPUT_MAX+1; f.fail=0;
	CHECK(jf_policy_parse(&src,1,&a,&policy,&error)==4 && !policy && f.live==0);
	CHECK(!strcmp(error.code,"E_INPUT_LIMIT"));
	CHECK(jf_policy_parse(NULL,0,NULL,&policy,&error)==0 && jf_policy_count(policy)==3);
	jf_policy_free(policy);
	CHECK(jf_policy_parse(NULL,1,NULL,&policy,&error)==4 && !policy);
	a.release=NULL;
	CHECK(jf_policy_parse(NULL,0,&a,&policy,&error)==4 && !policy);
	a.release=fault_release; f.fail=0; f.calls=0;
	strcpy(header_bytes,"From: \"Name\" <\"A B\"@Example.org>\r\nSender: list@example.org (mailman-bounce)\r\n");
	for (i=0;i<40;i++) strcat(header_bytes,"To: Group: team@example.org;\r\n");
	strcat(header_bytes,"X-Spam-Flag: \tYES\r\n\t \r\n\r\nBody"); strcpy(saved_headers,header_bytes);
	CHECK(jf_headers_parse((unsigned char *)header_bytes,strlen(header_bytes),&a,&headers,&error)==0);
	CHECK(jf_headers_message(headers)->address_count==42 && jf_headers_message(headers)->sender_count==1);
	memset(header_bytes,'?',strlen(header_bytes));
	CHECK(!strcmp((const char *)jf_headers_message(headers)->addresses[0].value.data,"\"A B\"@Example.org"));
	jf_headers_free(headers); headers=NULL; CHECK(f.live==0); header_faults=f.calls;
	strcpy(header_bytes,saved_headers);
	for (i=1;i<=header_faults;i++) {
		f.calls=0; f.fail=i;
		CHECK(jf_headers_parse((unsigned char *)header_bytes,strlen(header_bytes),&a,&headers,&error)==3);
		CHECK(!headers && f.live==0 && !strcmp(error.code,"E_MEMORY"));
	}
	f.calls=0; f.fail=0;
	strcpy(header_bytes,"From: good@example.org\nTo: good@example.org, broken <\n\n");
	CHECK(jf_headers_parse((unsigned char *)header_bytes,strlen(header_bytes),&a,&headers,&error)==7 && !headers && f.live==0);
	CHECK(!strcmp(error.code,"E_MESSAGE_ADDRESS"));
	invalid_header_faults=f.calls;
	for (i=1;i<=invalid_header_faults;i++) {
		f.calls=0; f.fail=i;
		CHECK(jf_headers_parse((unsigned char *)header_bytes,strlen(header_bytes),&a,&headers,&error)==3 && !headers && f.live==0);
	}
	f.fail=0;
	CHECK(jf_headers_read(-1,NULL,&headers,&error)==3 && !headers);
	CHECK(jf_headers_parse(NULL,1,NULL,&headers,&error)==7 && !headers);
	CHECK(pipe(pipe_fds)==0);
	CHECK(write(pipe_fds[1],"Subject: test\n\n\0binary",22)==22);
	CHECK(jf_headers_read(pipe_fds[0],NULL,&headers,&error)==0);
	CHECK(jf_headers_wire_size(headers)==15);
	jf_headers_free(headers); headers=NULL;
	CHECK(close(pipe_fds[0])==0 && close(pipe_fds[1])==0);
	legacy=rfc822_parse_adrlist(NULL,"\"a\\\"b\"@example.org");
	CHECK(legacy && !strcmp(legacy->mailbox,"\"a\\\"b\"@example.org")); rfc822_free_address(&legacy);
	legacy=rfc822_parse_adrlist(NULL,"(comment);"); CHECK(!legacy && RFC822Error);
	legacy=rfc822_parse_adrlist(NULL,"x@example.org ");
	CHECK(legacy && !strcmp(legacy->mailbox,"x@example.org")); rfc822_free_address(&legacy);
	printf("PASS native selftest: SHA256 vectors; %zu valid/resize and %zu invalid-input allocation faults; %zu valid and %zu invalid header allocation faults; bounds; owned snapshots; legacy parser regressions; locale preservation\n",valid_faults,count,header_faults,invalid_header_faults);
	return 0;
}

int main(int argc, char **argv)
{
	struct jf_source *sources;
	struct jf_policy *policy=NULL;
	struct jf_error error;
	size_t count, i;
	int rc=3, first=1;
	if (argc==2 && !strcmp(argv[1],"--selftest")) return selftest();
	if ((argc-first)%4) return 2;
	count=(size_t)(argc-first)/4;
	sources=calloc(count ? count : 1,sizeof *sources);
	if (!sources) return 3;
	for (i=0;i<count;i++) {
		FILE *f;
		long n;
		int j=first+(int)(i*4);
		static const char *roles[]={"filters","account","aliases","known","blacklist"};
		size_t role;
		for (role=0;role<5 && strcmp(argv[j],roles[role]);role++) {}
		if (role==5) { rc=2; goto done; }
		sources[i].kind=(enum jf_source_kind)role;
		sources[i].identity=argv[j+1]; sources[i].path=argv[j+2];
		f=fopen(argv[j+3],"rb");
		if (!f) goto done;
		if (fseek(f,0,SEEK_END) || (n=ftell(f))<0 || fseek(f,0,SEEK_SET)) { fclose(f); goto done; }
		sources[i].size=(size_t)n;
		if (sources[i].size<=JF_INPUT_MAX) {
			unsigned char *data=malloc(sources[i].size ? sources[i].size : 1);
			if (!data) { fclose(f); goto done; }
			sources[i].data=data;
			if (fread(data,1,sources[i].size,f)!=sources[i].size) { fclose(f); goto done; }
		} else sources[i].data=(const unsigned char *)"";
		if (fclose(f)) goto done;
	}
	rc=jf_policy_parse(sources,count,NULL,&policy,&error);
	if (rc) fprintf(stderr,"%s source=%zu line=%zu %s\n",error.code,error.source,error.line,error.detail);
	else for (i=0;i<jf_policy_count(policy);i++) write_rule(jf_policy_rule(policy,i));
done:
	jf_policy_free(policy);
	for (i=0;i<count;i++) if (sources[i].size<=JF_INPUT_MAX) free((void *)sources[i].data);
	free(sources);
	return rc;
}
