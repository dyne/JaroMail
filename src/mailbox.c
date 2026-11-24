#include "mailbox.h"

#include <stdint.h>
#include <string.h>

/* Reject overlong UTF8, surrogates, isolated continuations and > U+10FFFF. */
int jf_valid_utf8(const unsigned char *s, size_t n)
{
	size_t i=0;
	while (i<n) {
		uint32_t c=s[i++], min;
		unsigned need;
		if (!c) return 0;
		if (c<128) continue;
		if (c>=0xc2 && c<=0xdf) { need=1; min=0x80; c&=31; }
		else if (c>=0xe0 && c<=0xef) { need=2; min=0x800; c&=15; }
		else if (c>=0xf0 && c<=0xf4) { need=3; min=0x10000; c&=7; }
		else return 0;
		if (n-i<need) return 0;
		while (need--) {
			if ((s[i]&0xc0)!=0x80) return 0;
			c=(c<<6)|(s[i++]&63);
		}
		if (c<min || c>0x10ffff || (c>=0xd800 && c<=0xdfff)) return 0;
	}
	return 1;
}

static int alnum_ascii(unsigned c)
{
	return (c>='a'&&c<='z') || (c>='A'&&c<='Z') || (c>='0'&&c<='9');
}

int jf_valid_domain(const char *s)
{
	size_t n=strlen(s), label=0, i;
	if (!n || n>253) return 0;
	for (i=0;i<n;i++) {
		unsigned c=(unsigned char)s[i];
		if (c=='.') {
			if (!label || s[i-1]=='-') return 0;
			label=0;
		} else {
			if ((!alnum_ascii(c) && c!='-') || (!label && c=='-') || ++label>63) return 0;
		}
	}
	return label && s[n-1]!='-';
}

/* Policy inputs are single mailboxes, not display names/address lists.
 * Domains are ASCII/A-label. Normalization below minimizes local quoting.
 */
int jf_valid_mailbox(const char *s)
{
	const unsigned char *p=(const unsigned char *)s;
	if (*p=='"') {
		p++;
		while (*p && *p!='"') {
			if (*p=='\\') { p++; if (!*p) return 0; }
			if (*p<32 || *p==127) return 0;
			p++;
		}
		if (*p++!='"' || *p++!='@') return 0;
	} else {
		int component=0;
		while (*p && *p!='@') {
			if (*p=='.') { if (!component) return 0; component=0; }
			else {
				if (*p<128 && !alnum_ascii(*p) && !strchr("!#$%&'*+-/=?^_`{|}~",*p)) return 0;
				component=1;
			}
			p++;
		}
		if (!component || *p++!='@') return 0;
	}
	return jf_valid_domain((const char *)p);
}

int jf_normalize_mailbox(char *s)
{
	size_t original=strlen(s),read=1,n=0,i,out,escapes=0,tail;
	int quoted=0,component=0;
	if (original>JARO_MAILBOX_MAX || !jf_valid_mailbox(s)) return 0;
	if (*s!='"') return 1;
	/* Decode a quoted local part, retaining every decoded byte and its case. */
	while (s[read]!='"') {
		if (s[read]=='\\') read++;
		s[n++]=s[read++];
	}
	if (!n) return 0;
	tail=original-read; /* @domain plus terminating NUL */
	memmove(s+n,s+read+1,tail);
	for (i=0;i<n;i++) {
		unsigned char c=(unsigned char)s[i];
		if (c=='.') { if (!component) quoted=1; component=0; }
		else {
			if (c<128 && !alnum_ascii(c) && !strchr("!#$%&'*+-/=?^_`{|}~",c)) quoted=1;
			component=1;
		}
		if (c=='"' || c=='\\') escapes++;
	}
	if (!component) quoted=1;
	if (!quoted) return 1;
	/* The original quoted representation already had all necessary escapes,
 * so this cannot exceed its capacity. Serialize backwards to avoid overlap. */
	out=n+escapes+2;
	if (out+tail>original+1) return 0;
	memmove(s+out,s+n,tail);
	s[--out]='"';
	for (i=n;i;) {
		unsigned char c=(unsigned char)s[--i];
		s[--out]=(char)c;
		if (c=='"' || c=='\\') s[--out]='\\';
	}
	s[0]='"';
	return 1;
}
