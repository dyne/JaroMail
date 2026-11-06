#define _POSIX_C_SOURCE 200809L
#include "contact.h"
#include "products.h"
#include "policy_internal.h"
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

static int valid_text(const char *s)
{
	const unsigned char *p=(const unsigned char *)s;
	if (strlen(s)>JF_LINE_MAX || !jf_valid_utf8(p,strlen(s))) return 0;
	for (;*p;p++) if (*p<32 || *p==127) return 0;
	return 1;
}

static int canonical(char *s)
{
	unsigned char *p=(unsigned char *)s;
	if (!jf_valid_utf8(p,strlen(s)) || !jf_normalize_mailbox(s)) return 0;
	for (;*p;p++) if (*p>='A' && *p<='Z') *p=(unsigned char)(*p+32);
	return 1;
}

static int same_email(const unsigned char *s,size_t n,const char *wanted)
{
	size_t start=0,i; int quote=0,escape=0;
	for (i=0;i<=n;i++) {
		if (i<n && escape) { escape=0; continue; }
		if (i<n && s[i]=='\\' && quote) { escape=1; continue; }
		if (i<n && s[i]=='"') { quote=!quote; continue; }
		if (i==n || (s[i]==',' && !quote)) {
			size_t end=i; char *copy; int same;
			while (start<end && (s[start]==' ' || s[start]=='\t')) start++;
			while (end>start && (s[end-1]==' ' || s[end-1]=='\t')) end--;
			copy=malloc(end-start+1); if (!copy) return -1;
			memcpy(copy,s+start,end-start); copy[end-start]=0;
			same=canonical(copy) && !strcmp(copy,wanted); free(copy);
			if (same) return 1;
			start=i+1;
		}
	}
	return 0;
}

static int sync_parent(const char *path)
{
	char *copy=strdup(path),*slash; int fd,rc;
	if (!copy) return -1;
	slash=strrchr(copy,'/');
	if (!slash) { free(copy); copy=strdup("."); if (!copy) return -1; }
	else if (slash==copy) slash[1]=0;
	else *slash=0;
	fd=open(copy,O_RDONLY|O_DIRECTORY|O_CLOEXEC); free(copy);
	if (fd<0) return -1;
	rc=fsync(fd); if (close(fd)) rc=-1; return rc;
}

int jf_contact_add(const char *path,const char *email,const char *name,int check,struct jf_error *error)
{
	struct jf_bytes raw={0},record={0}; struct stat before,after,named;
	struct flock lock={0}; char *address=NULL; unsigned char buffer[4096];
	size_t pos=0,next=0,records=0; int format=0,section=0,fd=-1,rc=3,created=0;
	char number[64]; ssize_t n;
	if (error) memset(error,0,sizeof *error);
	if (!valid_text(email) || !valid_text(name) || strchr(email,','))
		return jf_product_error(error,"E_CONTACT",0,"contact fields contain unsupported bytes or exceed their limit");
	address=strdup(email); if (!address) goto done;
	if (!canonical(address)) { rc=jf_product_error(error,"E_ADDRESS",0,"invalid contact mailbox"); goto done; }
	if (!check) {
		fd=open(path,O_RDWR|O_APPEND|O_NONBLOCK|O_CLOEXEC|O_CREAT|O_EXCL,0600);
		if (fd>=0) created=1;
		else if (errno!=EEXIST) goto done;
	}
	if (fd<0) fd=open(path,(check ? O_RDONLY : O_RDWR|O_APPEND)|O_NONBLOCK|O_CLOEXEC);
	if (fd<0 && check && errno==ENOENT && lstat(path,&named)<0 && errno==ENOENT) { rc=0; goto done; }
	if (fd<0 || fstat(fd,&before) || !S_ISREG(before.st_mode)) goto done;
	lock.l_type=check ? F_RDLCK : F_WRLCK; lock.l_whence=SEEK_SET;
	if (fcntl(fd,F_SETLK,&lock)) {
		if (errno==EAGAIN || errno==EACCES) { rc=6; jf_product_error(error,"E_BOOK_BUSY",0,"addressbook is locked; retry after its writer finishes"); }
		goto done;
	}
	while ((n=read(fd,buffer,sizeof buffer))) {
		if (n<0) { if (errno==EINTR) continue; goto done; }
		if (jf_bytes_add_limit(&raw,buffer,(size_t)n,JF_INPUT_MAX)) goto done;
	}
	/* Numeric section labels are chosen independently of contact contents.
	 * Preserve every original byte, including custom fields and invalid old
	 * addresses. Never send contact fields through abook's header importer. */
	while (pos<raw.size) {
		size_t start=pos,end,key,value,i,id=0;
		while (pos<raw.size && raw.data[pos]!='\n') pos++;
		end=pos; if (pos<raw.size) pos++;
		while (start<end && (raw.data[start]==' ' || raw.data[start]=='\t')) start++;
		while (end>start && (raw.data[end-1]==' ' || raw.data[end-1]=='\t' || raw.data[end-1]=='\r')) end--;
		if (end-start>JF_LINE_MAX || memchr(raw.data+start,0,end-start)) goto invalid;
		if (start==end || raw.data[start]=='#') continue;
		if (raw.data[start]=='[') {
			if (end-start==8 && !memcmp(raw.data+start,"[format]",8)) {
				if (format || records) goto invalid;
				format=1; section=0; continue;
			}
			if (!format || end-start<3 || raw.data[end-1]!=']') goto invalid;
			for (i=start+1;i<end-1;i++) {
				unsigned char c=raw.data[i];
				if (c<'0' || c>'9' || id>(SIZE_MAX-(size_t)(c-'0'))/10) goto invalid;
				id=id*10+(size_t)(c-'0');
			}
			if (id==SIZE_MAX || ++records>JF_RULE_MAX) goto invalid;
			if (id>=next) next=id+1;
			section=1; continue;
		}
		key=start; while (key<end && raw.data[key]!='=') key++;
		if (!format || key==end) goto invalid;
		value=key+1; while (key>start && (raw.data[key-1]==' ' || raw.data[key-1]=='\t')) key--;
		if (section && key-start==5 && !memcmp(raw.data+start,"email",5)) {
			int same=same_email(raw.data+value,end-value,address);
			if (same<0) goto done;
			if (same) { rc=1; goto done; }
		}
	}
	if (check) { rc=0; goto done; }
	if (records==JF_RULE_MAX) goto invalid;
	if (!format && jf_bytes_text(&record,"\n[format]\nprogram=abook\nversion=0.6.1\n")) goto done;
	snprintf(number,sizeof number,"\n[%zu]\nname=",next);
	if (jf_bytes_text(&record,number) || jf_bytes_text(&record,*name ? name : email) ||
		jf_bytes_text(&record,"\nemail=") || jf_bytes_text(&record,email) || jf_bytes_text(&record,"\n")) goto done;
	if (!jf_bytes_fits(raw.size,record.size,JF_INPUT_MAX)) goto invalid;
	if (fstat(fd,&after) || stat(path,&named) || before.st_dev!=after.st_dev ||
		before.st_ino!=after.st_ino || before.st_size!=after.st_size ||
		before.st_mtime!=after.st_mtime || before.st_ctime!=after.st_ctime ||
		named.st_dev!=after.st_dev || named.st_ino!=after.st_ino) {
		rc=6; jf_product_error(error,"E_BOOK_CHANGED",0,"addressbook changed while preparing the contact; retry"); goto done;
	}
	/* O_APPEND leaves all old records intact. Check the complete append and
	 * fsync; on a short/failed write retain the bytes for inspection, never
	 * truncate a file which may also have an uncooperative external writer. */
	do { n=write(fd,record.data,record.size); } while (n<0 && errno==EINTR);
	if (n<0 || (size_t)n!=record.size || fsync(fd) || (created && sync_parent(path))) goto done;
	rc=0; goto done;
invalid:
	rc=jf_product_error(error,"E_ADDRESSBOOK",0,"unsupported addressbook structure or contact limit; existing records unchanged");
done:
	if (rc==3 && error && !error->code)
		jf_product_error(error,"E_IO",0,"contact append failed; original records retained; inspect the book before retrying");
	if (fd>=0) close(fd);
	free(address); jf_bytes_free(&raw); jf_bytes_free(&record); return rc;
}
