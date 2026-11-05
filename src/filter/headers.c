#define _POSIX_C_SOURCE 200809L
#include "headers.h"
#include "policy_internal.h"
#include "../rfc822.h"
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

struct owned_text { struct owned_text *next; unsigned char bytes[]; };
struct jf_headers {
	struct jf_allocator allocator;
	struct jf_message message;
	struct owned_text *texts;
	size_t address_capacity, sender_capacity, spam_capacity, wire_size;
};
struct header_parser {
	struct jf_headers *headers;
	struct jf_error *error;
	char *value, *scratch;
	size_t value_size, line, field_line;
	unsigned field;
	int active, callback_status;
};
struct input {
	const unsigned char *data;
	size_t size, position;
	int fd, streaming;
	unsigned char buffer[4096];
};

static void *heap_allocate(void *context, size_t size) { (void)context; return malloc(size); }
static void heap_release(void *context, void *p) { (void)context; free(p); }
static void *allocate(struct jf_headers *h, size_t size)
{
	return h->allocator.allocate(h->allocator.context,size);
}
static void release(struct jf_headers *h, void *p)
{
	if (p) h->allocator.release(h->allocator.context,p);
}
static int fail(struct header_parser *p, const char *code, const char *detail, int rc)
{
	if (p->error) {
		p->error->code=code; p->error->source=SIZE_MAX; p->error->line=p->field_line;
		snprintf(p->error->detail,sizeof p->error->detail,"%s",detail);
	}
	return rc;
}

void jf_headers_free(struct jf_headers *h)
{
	struct jf_allocator a;
	struct owned_text *text;
	if (!h) return;
	a=h->allocator;
	while ((text=h->texts)) { h->texts=text->next; release(h,text); }
	release(h,(void *)h->message.addresses); release(h,(void *)h->message.senders);
	release(h,(void *)h->message.spam_flags);
	a.release(a.context,h);
}
const struct jf_message *jf_headers_message(const struct jf_headers *h) { return h ? &h->message : NULL; }
size_t jf_headers_wire_size(const struct jf_headers *h) { return h ? h->wire_size : 0; }

static unsigned char *own(struct jf_headers *h, const char *s, size_t n)
{
	struct owned_text *text;
	if (n>SIZE_MAX-sizeof *text-1) return NULL;
	text=allocate(h,sizeof *text+n+1);
	if (!text) return NULL;
	memcpy(text->bytes,s,n); text->bytes[n]=0;
	text->next=h->texts; h->texts=text;
	return text->bytes;
}

static void *grow(struct jf_headers *h, const void *old, size_t count,
	size_t *capacity, size_t item_size)
{
	void *array;
	size_t cap;
	if (count<*capacity) return (void *)old;
	cap=*capacity ? *capacity*2 : 8;
	if (cap<*capacity || cap>SIZE_MAX/item_size) return NULL;
	array=allocate(h,cap*item_size);
	if (!array) return NULL;
	if (count) memcpy(array,old,count*item_size);
	release(h,(void *)old); *capacity=cap;
	return array;
}

static int emit_address(void *context, const char *mailbox, size_t length)
{
	struct header_parser *p=context;
	struct jf_headers *h=p->headers;
	struct jf_mailbox *array;
	unsigned char *text;
	if (!jf_valid_utf8((const unsigned char *)mailbox,length) || !jf_valid_mailbox(mailbox))
		return p->callback_status=fail(p,"E_MESSAGE_ADDRESS","unsupported/invalid mailbox domain or local part",7);
	array=grow(h,h->message.addresses,h->message.address_count,&h->address_capacity,sizeof *array);
	if (!array) return p->callback_status=fail(p,"E_MEMORY","address vector allocation failed",3);
	h->message.addresses=array;
	text=own(h,mailbox,length);
	if (!text) return p->callback_status=fail(p,"E_MEMORY","mailbox allocation failed",3);
	array[h->message.address_count].field=p->field;
	array[h->message.address_count].value.data=text;
	array[h->message.address_count++].value.size=length;
	return 0;
}

static int store_value(struct header_parser *p, int spam)
{
	struct jf_headers *h=p->headers;
	size_t *count=spam ? &h->message.spam_count : &h->message.sender_count;
	size_t *capacity=spam ? &h->spam_capacity : &h->sender_capacity;
	const struct jf_text *old=spam ? h->message.spam_flags : h->message.senders;
	struct jf_text *array=grow(h,old,*count,capacity,sizeof *array);
	unsigned char *text;
	if (!array) return fail(p,"E_MEMORY","header vector allocation failed",3);
	if (spam) h->message.spam_flags=array; else h->message.senders=array;
	text=own(h,p->value,p->value_size);
	if (!text) return fail(p,"E_MEMORY","header value allocation failed",3);
	array[*count].data=text; array[(*count)++].size=p->value_size;
	return 0;
}

static int finish_field(struct header_parser *p)
{
	int rc;
	if (!p->active || !p->field) return 0;
	p->value[p->value_size]=0;
	if (p->field==JF_SPAM) return store_value(p,1);
	if (!jf_valid_utf8((const unsigned char *)p->value,p->value_size))
		return fail(p,"E_MESSAGE_ADDRESS","invalid UTF8 in address field",7);
	p->callback_status=0;
	rc=rfc822_parse_mailboxes(p->value,p->value_size,p->scratch,JF_LINE_MAX+1,emit_address,p);
	if (rc==RFC822_BOUNDED_EMIT) return p->callback_status;
	if (rc) return fail(p,"E_MESSAGE_ADDRESS","malformed/unsupported address list",7);
	return p->field==JF_SENDER ? store_value(p,0) : 0;
}

static int name_is(const char *name, size_t n, const char *expected)
{
	size_t i;
	if (strlen(expected)!=n) return 0;
	for (i=0;i<n;i++) {
		unsigned char c=(unsigned char)name[i];
		if (c>='A' && c<='Z') c=(unsigned char)(c+'a'-'A');
		if (c!=(unsigned char)expected[i]) return 0;
	}
	return 1;
}

static int line(struct header_parser *p, const char *s, size_t n)
{
	size_t i, start;
	int rc;
	p->line++;
	if (s[0]==' ' || s[0]=='\t') {
		if (!p->active) { p->field_line=p->line; return fail(p,"E_MESSAGE_HEADER","orphan continuation",7); }
		start=0;
	} else {
		if ((rc=finish_field(p))) return rc;
		p->field_line=p->line; p->active=1; p->value_size=0; p->field=0;
		for (i=0;i<n && s[i]!=':';i++)
			if ((unsigned char)s[i]<33 || (unsigned char)s[i]>126)
				return fail(p,"E_MESSAGE_HEADER","invalid header field name",7);
		if (!i || i==n) return fail(p,"E_MESSAGE_HEADER","missing header colon/name",7);
		if (name_is(s,i,"from")) p->field=JF_FROM;
		else if (name_is(s,i,"sender")) p->field=JF_SENDER;
		else if (name_is(s,i,"to")) p->field=JF_TO;
		else if (name_is(s,i,"cc")) p->field=JF_CC;
		else if (name_is(s,i,"x-spam-flag")) p->field=JF_SPAM;
		start=i+1;
	}
	if (n-start>JF_LINE_MAX-p->value_size) return fail(p,"E_MESSAGE_LIMIT","unfolded field exceeds 64 KiB",7);
	if (p->field) memcpy(p->value+p->value_size,s+start,n-start);
	p->value_size+=n-start;
	return 0;
}

/* 1 byte, 0 EOF, -1 IO failure. The buffer never grows with the body. */
static int next(struct input *in, unsigned char *byte)
{
	if (in->position==in->size) {
		ssize_t n;
		if (!in->streaming) return 0;
		do { n=read(in->fd,in->buffer,sizeof in->buffer); } while (n<0 && errno==EINTR);
		if (n<=0) return n<0 ? -1 : 0;
		in->data=in->buffer; in->size=(size_t)n; in->position=0;
	}
	*byte=in->data[in->position++];
	return 1;
}

static int parse(struct input *in, const struct jf_allocator *allocator,
	struct jf_headers **out, struct jf_error *error)
{
	struct jf_allocator a={NULL,heap_allocate,heap_release};
	struct header_parser p={0};
	struct jf_headers *h;
	char *physical=NULL;
	size_t used=0;
	unsigned char byte;
	int rc=0, got;
	p.error=error;
	if (error) memset(error,0,sizeof *error);
	if (!out) return fail(&p,"E_MESSAGE_ARGUMENT","missing result pointer",7);
	*out=NULL;
	if (allocator) a=*allocator;
	if (!a.allocate || !a.release) return fail(&p,"E_MESSAGE_ARGUMENT","invalid allocator",7);
	h=a.allocate(a.context,sizeof *h);
	if (!h) return fail(&p,"E_MEMORY","message allocation failed",3);
	memset(h,0,sizeof *h); h->allocator=a; p.headers=h;
	physical=allocate(h,JF_LINE_MAX+2); p.value=allocate(h,JF_LINE_MAX+1); p.scratch=allocate(h,JF_LINE_MAX+1);
	if (!physical || !p.value || !p.scratch) { rc=fail(&p,"E_MEMORY","header scratch allocation failed",3); goto done; }
	for (;;) {
		got=next(in,&byte);
		if (got<=0) { p.field_line=p.line+1; rc=fail(&p,got<0 ? "E_MESSAGE_IO" : "E_MESSAGE_TRUNCATED",
			got<0 ? "message read failed" : "missing header/body separator",got<0 ? 3 : 7); break; }
		if (++h->wire_size>JF_HEADERS_MAX) { p.field_line=p.line+1; rc=fail(&p,"E_MESSAGE_LIMIT","headers exceed 1 MiB",7); break; }
		if (byte=='\n') {
			if (used && physical[used-1]=='\r') used--;
			if (used>JF_LINE_MAX) { p.field_line=p.line+1; rc=fail(&p,"E_MESSAGE_LIMIT","physical header line exceeds 64 KiB",7); break; }
			if (!used) { rc=finish_field(&p); break; }
			if ((rc=line(&p,physical,used))) break;
			used=0;
		} else {
			if ((byte<32 && byte!='\t' && byte!='\r') || byte==127 || (used && physical[used-1]=='\r')) {
				p.field_line=p.line+1; rc=fail(&p,"E_MESSAGE_TEXT","NUL/control byte or bare CR in header",7); break;
			}
			if (used==JF_LINE_MAX+1) { p.field_line=p.line+1; rc=fail(&p,"E_MESSAGE_LIMIT","physical header line exceeds 64 KiB",7); break; }
			physical[used++]=(char)byte;
		}
	}
done:
	release(h,physical); release(h,p.value); release(h,p.scratch);
	if (rc) jf_headers_free(h); else *out=h;
	return rc;
}

int jf_headers_parse(const unsigned char *data, size_t size, const struct jf_allocator *allocator,
	struct jf_headers **out, struct jf_error *error)
{
	struct input in={0};
	if (size && !data) { struct header_parser p={0}; p.error=error; if (out) *out=NULL; return fail(&p,"E_MESSAGE_ARGUMENT","missing message bytes",7); }
	in.data=data; in.size=size;
	return parse(&in,allocator,out,error);
}

int jf_headers_read(int fd, const struct jf_allocator *allocator, struct jf_headers **out, struct jf_error *error)
{
	struct input in={0}; in.fd=fd; in.streaming=1;
	return parse(&in,allocator,out,error);
}
