#include "contact.h"
#include "contact/migrate.h"
#include "products.h"

#include <stdio.h>
#include <string.h>

static struct jaro_contact_text text(const char *s)
{
	return (struct jaro_contact_text){s, strlen(s)};
}

static int contact_error(struct jf_error *error, int status, const char *reason, const char *field)
{
	const char *code=status==JARO_CONTACT_BUSY ? "E_CONTACT_BUSY" :
		status==JARO_CONTACT_INVALID ? "E_CONTACT_FIELD" : "E_CONTACT_STORAGE";
	char diagnostic[192];
	snprintf(diagnostic,sizeof diagnostic,"%s%s%s",field ? field : "",field ? ": " : "",reason);
	jf_product_error(error,code,0,diagnostic);
	return status==JARO_CONTACT_BUSY ? 6 : status==JARO_CONTACT_INVALID ? 4 : 3;
}

struct display {
	int vcard, all_addresses, wrote;
	const char *heading;
};

static int vcard_text(struct jaro_contact_text value)
{
	for (size_t i=0;i<value.size;i++) {
		unsigned char c=value.data[i];
		if (c=='\r') {
			if (i+1<value.size && value.data[i+1]=='\n') i++;
			c='\n';
		}
		if (c=='\n') {
			if (fputs("\\n",stdout)<0) return 1;
		} else {
			if ((c=='\\' || c==';' || c==',') && putchar('\\')==EOF) return 1;
			if (putchar(c)==EOF) return 1;
		}
	}
	return fputs("\r\n",stdout)<0;
}

static int display_contact(const struct jaro_contact *contact,void *data)
{
	struct display *display=data;
	if (display->vcard) {
		if (fputs("BEGIN:VCARD\r\nVERSION:3.0\r\nFN:",stdout)<0 || vcard_text(contact->name) ||
			fputs("N:",stdout)<0 || vcard_text(contact->name)) return 1;
		if (contact->nickname.size && (fputs("NICKNAME:",stdout)<0 || vcard_text(contact->nickname))) return 1;
		if (contact->notes.size && (fputs("NOTE:",stdout)<0 || vcard_text(contact->notes))) return 1;
		for (size_t i=0;i<contact->address_count;i++)
			if (fputs("EMAIL:",stdout)<0 || vcard_text(contact->addresses[i])) return 1;
		if (fputs("END:VCARD\r\n",stdout)<0) return 1;
		display->wrote=1;
		return 0;
	}
	if (!contact->address_count) return 0;
	if (!display->wrote && puts(display->heading)==EOF) return 1;
	display->wrote=1;
	size_t count=display->all_addresses ? contact->address_count : 1;
	for (size_t i=0;i<count;i++)
		if (jaro_contact_write_mutt_row(stdout,contact->addresses[i],contact->name,contact->notes)) return 1;
	return 0;
}

int jf_contact_command(int argc,char **argv,struct jf_error *error)
{
	const char *command=argc>1 ? argv[1] : "";
	int add=!strcmp(command,"--add-contact"), has=!strcmp(command,"--has-contact");
	int query=!strcmp(command,"--query-contacts"), recipients=!strcmp(command,"--recipient-contacts");
	int export=!strcmp(command,"--export-contacts");
	if (!add && !has && !query && !recipients && !export) return -1;
	if ((add && !(argc==6 || (argc==7 && !strcmp(argv[6],"--check")))) ||
		(has && argc!=5) || (query && argc!=5) || (recipients && argc!=4) || (export && argc!=4))
		return 2;

	struct jaro_contact_store *store;
	struct jaro_contact_migration_error migration;
	struct jaro_contact_error detail={0};
	int preview=add && argc==7;
	int status=jaro_contact_profile_open(argv[2],preview ? JARO_CONTACT_PROFILE_PREVIEW :
		add ? JARO_CONTACT_PROFILE_WRITE : JARO_CONTACT_PROFILE_READ,&store,&migration);
	if (status) return contact_error(error,status,migration.reason,migration.field);

	int result,found=0;
	int64_t book_id=0;
	struct jaro_contact_text book=recipients ? (struct jaro_contact_text){0} : text(argv[3]);
	if (add) {
		status=jaro_contact_learn(store,book,text(argv[4]),text(argv[5]),preview,&found,&detail);
		result=!found;
	} else {
		status=jaro_contact_begin_read(store,&detail);
		if (!status && !recipients) {
			status=jaro_contact_book(store,book,0,&book_id,&detail);
			if (status==JARO_CONTACT_MISSING) status=JARO_CONTACT_OK;
		}
		if (!status && has && book_id)
			status=jaro_contact_has_mailbox(store,book_id,text(argv[4]),&found,&detail);
		if (!status && !has && (recipients || book_id)) {
			const char *needle=export ? "" : argv[recipients ? 3 : 4];
			int all=!strcmp(needle,"all");
			struct display display={.vcard=export,.all_addresses=!recipients,.heading=all ? "All items" : ""};
			status=jaro_contact_visit(store,book_id,text(all ? "" : needle),display_contact,&display,&detail);
			found=display.wrote;
		}
		status=jaro_contact_finish(store,status,&detail);
		result=!found && !export;
	}
	jaro_contact_close(store,NULL);
	if (status) return contact_error(error,status,detail.reason,detail.field);
	if (fflush(stdout) || ferror(stdout)) return contact_error(error,JARO_CONTACT_STORAGE,"cannot write contact output",NULL);
	return result;
}
