#define _POSIX_C_SOURCE 200809L
#include "filter/generation.h"
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int jf_generation_test_hook(const char *phase)
{
	static int failed=0;
	const char *hold=getenv("JF_TEST_HOLD"),*fail=getenv("JF_TEST_FAIL"),*crash=getenv("JF_TEST_CRASH");
	if (getenv("JF_TEST_TRACE")) fprintf(stderr,"TRACE:%s\n",phase);
	if (hold && !strcmp(hold,phase)) {
		const char *a=getenv("JF_TEST_NOTIFY_FD"),*b=getenv("JF_TEST_RELEASE_FD"); char byte='1'; ssize_t n;
		if (!a || !b) return -1;
		do { n=write(atoi(a),&byte,1); } while (n<0 && errno==EINTR);
		if (n!=1) return -1;
		do { n=read(atoi(b),&byte,1); } while (n<0 && errno==EINTR);
		if (n!=1) return -1;
	}
	if (crash && !strcmp(crash,phase)) raise(SIGKILL);
	if (!failed && fail && !strcmp(fail,phase)) {
		fprintf(stderr,"INJECTED:%s\n",phase);
		failed=1; errno=EIO; return -1;
	}
	return 0;
}

#ifdef JF_PIN_DRIVER
int main(int argc,char **argv)
{
	struct jf_generation g={0}; struct jf_error error={0}; int rc,fd; char buffer[4096]; ssize_t n;
	g.directory=-1;
	if (argc<2 || argc>3) return 2;
	rc=jf_generation_pin(argv[1],argc==3 ? argv[2] : NULL,&g,&error); if (rc) return rc;
	if (jf_generation_test_hook("pinned")) { jf_generation_close(&g); return 3; }
	fwrite(g.id,1,65,stdout);
	fd=jf_generation_open(&g,"filters.zsh"); if (fd<0) { jf_generation_close(&g); return 3; }
	while ((n=read(fd,buffer,sizeof buffer))>0) if (fwrite(buffer,1,(size_t)n,stdout)!=(size_t)n) break;
	rc=n<0 || ferror(stdout) ? 3 : 0;
	close(fd); jf_generation_close(&g); return rc;
}
#endif
