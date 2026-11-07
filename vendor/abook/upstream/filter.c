
/*
 * $Id$
 *
 * by JH <jheinonen@users.sourceforge.net>
 *
 * Copyright (C) Jaakko Heinonen
 */

#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <pwd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include "abook_curses.h"
#include "filter.h"
#include "abook.h"
#include "database.h"
#include "edit.h"
#include "gettext.h"
#include "list.h"
#include "misc.h"
#include "options.h"
#include "ui.h"
#include "xmalloc.h"
#include <assert.h>

#ifdef HAVE_VFORMAT
#include "vcard.h"
#endif

extern abook_field_list *fields_list;
extern int fields_count;
// see also enum field_types @database.h
extern abook_field standard_fields[];

/*
 * function declarations
 */

/*
 * import filter prototypes
 */

static int      ldif_parse_file(FILE *handle);
static int	mutt_parse_file(FILE *in);
static int	pine_parse_file(FILE *in);
static int 	csv_parse_file(FILE *in);
static int 	allcsv_parse_file(FILE *in);
static int 	palmcsv_parse_file(FILE *in);
static int 	vcard_parse_file(FILE *in);

/*
 * export filter prototypes
 */

static int      ldif_export_database(FILE *out, struct db_enumerator e);
static int	html_export_database(FILE *out, struct db_enumerator e);
static int	pine_export_database(FILE *out, struct db_enumerator e);
static int	csv_export_database(FILE *out, struct db_enumerator e);
static int	allcsv_export_database(FILE *out, struct db_enumerator e);
static int	palm_export_database(FILE *out, struct db_enumerator e);
static int	vcard_export_database(FILE *out, struct db_enumerator e);
static int	mutt_alias_export(FILE *out, struct db_enumerator e);
static int	mutt_query_export_database(FILE *out, struct db_enumerator e);
static int	elm_alias_export(FILE *out, struct db_enumerator e);
static int	text_export_database(FILE *out, struct db_enumerator e);
static int	spruce_export_database(FILE *out, struct db_enumerator e);
static int	wl_export_database(FILE *out, struct db_enumerator e);
static int	ical_export_database(FILE *out, struct db_enumerator e);
static int	bsdcal_export_database(FILE *out, struct db_enumerator e);
static int	custom_export_database(FILE *out, struct db_enumerator e);

/*
 * export filter item prototypes
 */

void vcard_export_item(FILE *out, int item);
void muttq_print_item(FILE *file, int item);
void mailto_print_item(FILE *file, int item);
void custom_print_item(FILE *out, int item);

/*
 * end of function declarations
 */

struct abook_input_filter i_filters[] = {
	{ "abook", N_("abook native format"), parse_database },
	{ "ldif", N_("ldif / Netscape addressbook"), ldif_parse_file },
	{ "mutt", N_("mutt alias"), mutt_parse_file },
	{ "pine", N_("pine addressbook"), pine_parse_file },
	{ "csv", N_("comma separated values"), csv_parse_file },
	{ "allcsv", N_("comma separated values (all fields)"), allcsv_parse_file },
	{ "palmcsv", N_("Palm comma separated values"), palmcsv_parse_file },
	{ "vcard", N_("vCard file"), vcard_parse_file },
	{ "\0", NULL, NULL }
};

struct abook_output_filter e_filters[] = {
	{ "abook", N_("abook native format"), write_database },
	{ "ldif", N_("ldif / Netscape addressbook (.4ld)"), ldif_export_database },
	{ "vcard", N_("vCard 2 file"), vcard_export_database },
	{ "mutt", N_("mutt alias"), mutt_alias_export },
	{ "muttq", N_("mutt query format (internal use)"), mutt_query_export_database },
	{ "html", N_("html document"), html_export_database },
	{ "pine", N_("pine addressbook"), pine_export_database },
	{ "csv", N_("comma separated values"), csv_export_database },
	{ "allcsv", N_("comma separated values (all fields)"), allcsv_export_database },
	{ "palmcsv", N_("Palm comma separated values"), palm_export_database},
	{ "elm", N_("elm alias"), elm_alias_export },
	{ "text", N_("plain text"), text_export_database },
	{ "wl", N_("Wanderlust address book"), wl_export_database },
	{ "spruce", N_("Spruce address book"), spruce_export_database },
	{ "ical", N_("iCalendar"), ical_export_database },
	{ "bsdcal", N_("BSD calendar"), bsdcal_export_database },
	{ "custom", N_("Custom format"), custom_export_database },
	{ "\0", NULL, NULL }
};

struct abook_output_item_filter u_filters[] = {
	{ "vcard", N_("vCard 2 file"), vcard_export_item },
	{ "muttq", N_("mutt alias"), muttq_print_item },
	{ "mailto", N_("mailto address"), mailto_print_item },
	{ "custom", N_("Custom format"), custom_print_item },
	{ "\0", NULL }
};

/*
 * common functions
 */

void
print_filters()
{
	int i;

	puts(_("input formats:"));
	for(i=0; *i_filters[i].filtname ; i++)
		printf("\t%s\t%s\n", i_filters[i].filtname,
			gettext(i_filters[i].desc));

	putchar('\n');

	puts(_("output formats:"));
	for(i=0; *e_filters[i].filtname ; i++)
		printf("\t%s\t%s\n", e_filters[i].filtname,
			gettext(e_filters[i].desc));

	putchar('\n');

	puts(_("query-compatible output formats:"));
	for(i=0; *u_filters[i].filtname ; i++)
		printf("\t%s\t%s\n", u_filters[i].filtname,
			gettext(u_filters[i].desc));

	putchar('\n');
}

static int
number_of_output_filters()
{
	int i;

	for(i=0; *e_filters[i].filtname ; i++)
		;

	return i;
}

static int
number_of_input_filters()
{
	int i;

	for(i=0; *i_filters[i].filtname ; i++)
		;

	return i;
}

static char *
get_real_name()
{
	char *username = getenv("USER");
	struct passwd *pwent;
	int rtn;
	char *tmp;

	//if USER env variable isn't defined, use nobody account to avoid SIGSEGV with getpwnam
	if (!username) {
          username = "nobody";
       }

	pwent = getpwnam(username);

	if((tmp = xstrdup(pwent->pw_gecos)) == NULL)
		return xstrdup(username);

	rtn = sscanf(pwent->pw_gecos, "%[^,]", tmp);
	if (rtn == EOF || rtn == 0) {
		free(tmp);
		return xstrdup(username);
	} else
		return tmp;
}

/*
 * import
 */

static int		i_read_file(char *filename, int (*func) (FILE *in));

static void
import_screen()
{
	int i;

	clear();

	refresh_statusline();
	headerline(_("import database"));

	mvaddstr(3, 1, _("please select a filter"));


	for(i=0; *i_filters[i].filtname ; i++)
		mvprintw(5 + i, 6, "%c -\t%s\t%s\n", 'a' + i,
			i_filters[i].filtname,
			gettext(i_filters[i].desc));

	mvprintw(6 + i, 6, _("x -\tcancel"));
}

int
import_database()
{
	int filter;
	char *filename;
	int tmp = db_n_items();

	import_screen();

	filter = getch() - 'a';
	if(filter == 'x' - 'a' ||
		filter >= number_of_input_filters() || filter < 0) {
		refresh_screen();
		return 1;
	}

	mvaddstr(5+filter, 2, "->");

	filename = ask_filename(_("Filename: "));
	if(!filename) {
		refresh_screen();
		return 2;
	}

	if(i_read_file(filename, i_filters[filter].func ))
		statusline_msg(_("Error occurred while opening the file"));
	else if(tmp == db_n_items())
		statusline_msg(_("File does not seem to be a valid addressbook"));

	refresh_screen();
	free(filename);

	return 0;
}



static int
i_read_file(char *filename, int (*func) (FILE *in))
{
	FILE *in;
	int ret = 0;

	if( (in = abook_fopen( filename, "r" )) == NULL )
		return 1;

	ret = (*func) (in);

	fclose(in);

	return ret;
}

int
import_file(char filtname[FILTNAME_LEN], char *filename)
{
	int i;
	int tmp = db_n_items();
	int ret = 0;

	for(i=0;; i++) {
		if(! strncasecmp(i_filters[i].filtname, filtname,
					FILTNAME_LEN) )
			break;
		if(! *i_filters[i].filtname) {
			i = -1;
			break;
		}
	}

	if(i < 0)
		return -1;

#ifdef HAVE_VFORMAT
	// this is a special case for
	// libvformat whose API expects a filename
	if(!strcmp(filtname, "vcard")) {
	  if(!strcmp(filename, "-"))
	    ret = vcard_parse_file_libvformat("/dev/stdin");
	  else
	    ret = vcard_parse_file_libvformat(filename);
	}
	else
#endif

	if(!strcmp(filename, "-")) {
		struct stat s;
		if((fstat(fileno(stdin), &s)) == -1 || S_ISDIR(s.st_mode))
			ret = 1;
		else
			ret = (*i_filters[i].func) (stdin);
	} else
		ret =  i_read_file(filename, i_filters[i].func);

	if(tmp == db_n_items())
		ret = 1;

	return ret;
}

/*
 * export
 */

static int e_write_file(char *filename,
		int (*func) (FILE *in, struct db_enumerator e), int mode);

static void
export_screen()
{
	int i;

	clear();


	refresh_statusline();
	headerline(_("export database"));

	mvaddstr(3, 1, _("please select a filter"));


	for(i = 0; *e_filters[i].filtname ; i++)
		mvprintw(5 + i, 6, "%c -\t%s\t%s\n", 'a' + i,
			e_filters[i].filtname,
			gettext(e_filters[i].desc));

	mvprintw(6 + i, 6, _("x -\tcancel"));
}

int
export_database()
{
	int filter;
	int enum_mode = ENUM_ALL;
	char *filename;

	export_screen();

	filter = getch() - 'a';
	if(filter == 'x' - 'a' ||
		filter >= number_of_output_filters() || filter < 0) {
		refresh_screen();
		return 1;
	}

	mvaddstr(5 + filter, 2, "->");

	if(selected_items()) {
		switch(statusline_askchoice(
			_("Export <a>ll, export <s>elected, or <c>ancel?"),
			S_("keybindings:all/selected/cancel|asc"), 3)) {
			case 1:
				break;
			case 2:
				enum_mode = ENUM_SELECTED;
				break;
			case 0:
			case 3:
				refresh_screen();
				return 1;
		}
		clear_statusline();
	}

	filename = ask_filename(_("Filename: "));
	if(!filename) {
		refresh_screen();
		return 2;
	}

	if( e_write_file(filename, e_filters[filter].func, enum_mode))
		statusline_msg(_("Error occurred while exporting"));

	refresh_screen();
	free(filename);

	return 0;
}

struct abook_output_item_filter select_output_item_filter(char filtname[FILTNAME_LEN]) {
	int i;
	for(i=0;; i++) {
		if(!strncasecmp(u_filters[i].filtname, filtname, FILTNAME_LEN))
		  break;
		if(!*u_filters[i].filtname) {
		  i = -1;
		  break;
		}
	}
	return u_filters[i];
}

void
e_write_item(FILE *out, int item, void (*func) (FILE *in, int item))
{
  (*func) (out, item);
}

static int
e_write_file(char *filename, int (*func) (FILE *in, struct db_enumerator e),
		int mode)
{
	FILE *out;
	int ret = 0;
	struct db_enumerator enumerator = init_db_enumerator(mode);

	if((out = fopen(filename, "a")) == NULL)
		return 1;

	if(ftell(out)) {
		fclose(out);
		return 1;
	}

	ret = (*func) (out, enumerator);

	fclose(out);

	return ret;
}

int
fexport(char filtname[FILTNAME_LEN], FILE *handle, int enum_mode)
{
	int i;
	struct db_enumerator e = init_db_enumerator(enum_mode);

	for(i=0;; i++) {
		if(!strncasecmp(e_filters[i].filtname, filtname,
					FILTNAME_LEN))
			break;
		if(!*e_filters[i].filtname) {
			i = -1;
			break;
		}
	}

	return (e_filters[i].func) (handle, e);
}



int
export_file(char filtname[FILTNAME_LEN], char *filename)
{
	const int mode = ENUM_ALL;
	int i;
	int ret = 0;
	struct db_enumerator e = init_db_enumerator(mode);

	for(i=0;; i++) {
		if(!strncasecmp(e_filters[i].filtname, filtname,
					FILTNAME_LEN))
			break;
		if(!*e_filters[i].filtname) {
			i = -1;
			break;
		}
	}

	if(i < 0)
		return -1;

	if(!strcmp(filename, "-"))
		ret = (e_filters[i].func) (stdout, e);
	else
		ret =  e_write_file(filename, e_filters[i].func, mode);

	return ret;
}

/*
 * end of common functions
 */

/*
 * ldif import
 */

#include "ldif.h"

/* During LDIF import we need more fields than the
   ITEM_FIELDS of a *list_item. Eg: "objectclass"
   to test valid records, ...
   Here we extends the existing field_types enum
   to define new fields indexes usable during processing.
   Newly created LDIF attr names could be associated to
   them using ldif_conv_table[]. */
typedef enum {
	LDIF_OBJECTCLASS = ITEM_FIELDS + 1
} ldif_field_types;

#define	LDIF_ITEM_FIELDS	LDIF_OBJECTCLASS

typedef char *ldif_item[LDIF_ITEM_FIELDS];

/* LDIF field's names *must* respect the ordering
   defined by the field_types enum from database.h
   This is only used to define (for export only)
   abook standard field to LDIF attr name mappings */
static ldif_item ldif_field_names = {
	"cn",			// NAME
	"mail",			// EMAIL
	"streetaddress",	// ADDRESS
	"streetaddress2",	// ADDRESS2
	"locality",		// CITY
	"st",			// STATE
	"postalcode",		// ZIP
	"countryname",		// COUNTRY
	"homephone",		// PHONE
	"telephonenumber",	// WORKPHONE
	"facsimiletelephonenumber",	// FAX
	"cellphone",		// MOBILEPHONE
	"xmozillanickname",	// NICK
	"homeurl",		// URL
	"description",		// NOTES
	"anniversary",		// ANNIVERSARY
	"ou",			// GROUPS
};

/* Used to map LDIF attr names from input to
   the abook restricted set of standard fields. */
typedef struct {
	char *key;
	int  index;
} ldif_available_items;

static ldif_available_items ldif_conv_table[] = {
	/* initial field names respect the field_types enum
	   from database.h but this is only for readability.
	   This ldif_item struct allow use to define multiple
	   LDIF field names ("attribute names") for one abook field */

	{"cn",			NAME},		// 0
	{"mail",		EMAIL},
	{"streetaddress",	ADDRESS},
	{"streetaddress2",	ADDRESS2},
	{"locality",		CITY},
	{"st",			STATE},
	{"postalcode",		ZIP},
	{"countryname",		COUNTRY},
	{"homephone",		PHONE},
	{"telephonenumber",	WORKPHONE},	// workphone, according to Mozilla
	{"facsimiletelephonenumber",	FAX},
	{"cellphone",		MOBILEPHONE},
	{"mozillanickname",	NICK},
	{"homeurl",		URL},
	{"description",		NOTES},
	{"anniversary",		ANNIVERSARY},
	{"ou",			GROUPS},	// 16

	// here comes a couple of aliases
	{"mozillasecondemail",	EMAIL},
	{"homecity",		CITY},
	{"zip",			ZIP},
	{"tel",			PHONE},
	{"xmozillaanyphone",	WORKPHONE},	// ever used ?
	{"workphone",		WORKPHONE},
	{"fax",			FAX},
	{"telexnumber",		FAX},
	{"mobilephone",		MOBILEPHONE},
	{"mobile",		MOBILEPHONE},
	{"xmozillanickname",	NICK},
	{"labeledURI",		URL},
	{"notes",		NOTES},
	{"birthday",		ANNIVERSARY},
	{"category",		GROUPS},

	/* TODO:
	   "sn": append to lastname ?
	   "surname": append to lastname ?
	   "givenname": prepend to firstname ? */

	/* here starts dummy fields:

	   As long as additional indexes are created
	   (using the above ldif_field_types),
	   any other LDIF attr name can be added and
	   used during ldif entry processing.
	   But obviously fields > ITEM_FIELDS (database.h) won't be
	   copied into the final *list_item. */

	/* - to avoid mistake, don't use the special ITEM_FIELDS value.
	   - see also: http://mxr.mozilla.org/comm-central/source/mailnews/addrbook/src/nsAbLDIFService.cpp */

	// used to check valid LDIF records:
	{"objectclass",		LDIF_OBJECTCLASS}
};
const int LDIF_IMPORTABLE_ITEM_FIELDS = (int)sizeof(ldif_conv_table)/sizeof(*ldif_conv_table);

/*
  Handles multi-line strings.
  If a string starts with a space, it's the continuation
  of the previous line. Thus we need to always read ahead.
  But for this to work with stdin, we need to stores the next
  line for later use in case it's not a continuation of the
  first line.
 */
static char *
ldif_read_line(FILE *in, char **next_line)
{
	char *buf = NULL;
	char *ptr, *tmp;
	char *line;

	// buf filled with the first line
	if(!*next_line)
		buf = getaline(in);
	else {
		buf = xstrdup(*next_line);
		xfree(*next_line);
	}

	while(!feof(in)) {
		// if no line already read-ahead.
		line = getaline(in);
		if(!line) break;

		// this is not a continuation of what is already in buf
		// store it for the next round
		if(*line != ' ') {
			*next_line = line;
			break;
		}

		// starts with ' ': this is the continuation of buf
		ptr = line;
		while( *ptr == ' ')
			ptr++;

		tmp = buf;
		buf = strconcat(buf, ptr, NULL);
		free(tmp);
		free(line);
	}

	if(buf && *buf == '#' ) {
		free(buf);
		return NULL;
	}

	return buf;
}

static void
ldif_add_item(ldif_item li)
{
	list_item item;
	int i;

	/* if there's no value for "objectclass"
	   it's probably a buggy record */
	if(!li[LDIF_OBJECTCLASS])
		goto bail_out;

	/* just copy from our extended ldif_item to a regular
	   list_item,
	   TODO: API could be changed so db_fput_byid() is usable */
	item = item_create();
	for(i=0; i < ITEM_FIELDS; i++) {
		if(li[i] && *li[i])
			item[i] = xstrdup(li[i]);
	}

	add_item2database(item);
	item_free(&item);

bail_out:
	for(i=0; i < LDIF_ITEM_FIELDS; i++)
		xfree(li[i]);
}

static void
ldif_convert(ldif_item item, char *type, char *value)
{
	/* this is the first (mandatory) attribute to expected
	   from a new valid LDIF record.
	   The previous record must be added to the database before
	   we can go further with the new one */
	if(!strcmp(type, "dn")) {
		ldif_add_item(item);
		return;
	}

	int i, index;

	for( i=0; i < LDIF_IMPORTABLE_ITEM_FIELDS; i++ ) {

		if( *value &&						// there's a value for the attr provided
		    ldif_conv_table[i].key &&				// there exists an ldif attr name...
		    !strcasecmp(ldif_conv_table[i].key, type)) {	// ...matching that provided at input

			assert((i >= 0) && (i < LDIF_ITEM_FIELDS));
			// which abook field this attribute's name maps to ?
			index = ldif_conv_table[i].index;
			assert((index >= 0) && (index < LDIF_ITEM_FIELDS));

			/* TODO: here must be handled multi-valued cases
			   (first or latest win, append or prepend values, ...)
			   Currently: emails are appended, for other fields the
			   first attribute found wins.
			   Eg: the value of "mobile" will be taken into
			   account if such a line comes before "cellphone". */

			/* Remember: LDIF_ITEM_FIELDS > ITEM_FIELDS,
			   lower (common) indexes of *ldif_item map well to *list_item.
			   We can use item_fput()... */
			if(index < ITEM_FIELDS) {
				// multiple email support, but two only will stay
				// in the final *list_item
				if(index == EMAIL && item_fget(item, EMAIL)) {
					item_fput(item,
					          EMAIL,
					          strconcat(item_fget(item, EMAIL), ",", value, 0));
				}
				else {
					/* Don't override already initialized fields:
					   This is the rule of the "first win" */
					if(! item_fget(item, index))
						item_fput(item, index, xstrdup(value));
				}
			}

			/* ... but if the ldif field's name index is higher
			   than what standards abook fields struct can hold,
			   these extra indexes must be managed manually.
			   This is the case of LDIF_OBJECTCLASS ("objectclass" attr) */
			else {
				item[index] = xstrdup(value);
			}

			// matching attr found and field filled:
			// no further attr search is needed for `type`
	                break;
		}
	}
}

static int
ldif_parse_file(FILE *handle)
{
	char *line = NULL;
	char *next_line = NULL;
	char *type, *value;
	int vlen;

	/* This is our extended fields holder to put the values from
	   successfully parsed LDIF attributes.
	   ldif_item item is temporary. When the end of an entry is reached,
	   values are copied into a regular *list_item struct, see ldif_add_item() */
	ldif_item item;

	memset(item, 0, sizeof(item));

	do {
		line = ldif_read_line(handle, &next_line);

		// EOF or empty lines: continue;
		if(!line || *line == '\0') continue;

		if(-1 == (str_parse_line(line, &type, &value, &vlen))) {
			xfree(line);
			continue; /* just skip the errors */
		}

		ldif_convert(item, type, value);

		xfree(line);
	} while ( !feof(handle) );

	// force registration (= ldif_add_item()) of the last LDIF entry
	ldif_convert(item, "dn", "");

	return 0;
}

/*
 * end of ldif import
 */

/*
 * mutt alias import filter
 */

#include "getname.h"

static int
mutt_read_line(FILE *in, char **groups, char **alias, char **rest)
{
	char *line, *ptr;
	char *start, *end;
	abook_list *glist = NULL;

	if( !(line = ptr = getaline(in)) )
		return 1; /* error / EOF */

	SKIPWS(ptr);

	if(strncmp("alias", ptr, 5)) {
		free(line);
		return 1;
	}

	ptr += 5;
	SKIPWS(ptr);

	/* If the group option is used, save the groups */
	*groups = NULL;
	start = ptr;
	int n_groups;
	for(n_groups = 0; 0 == strncmp("-group", ptr, 6); n_groups++) {
		ptr += 6;
		SKIPWS(ptr);
		start = ptr;
		SKIPNONWS(ptr);
		end = ptr;
		abook_list_append(&glist,xstrndup(start, end - start));
		SKIPWS(ptr);
	}

	if(n_groups && groups)
		*groups = abook_list_to_csv(glist);

	abook_list_free(&glist);	

	/* alias */
	start = ptr;
	SKIPNONWS(ptr);
	end = ptr;
	SKIPWS(ptr);
	if(alias)
		*alias = xstrndup(start, end - start);

	/* rest (email) */
	*rest = xstrdup(ptr);

	xfree(line);
	return 0;
}

static void
mutt_fix_quoting(char *p)
{
	char *escape = 0;

	for(; *p; p++) {
		switch(*p) {
			case '\"':
				if(escape)
					*escape = ' ';
				break;
			case '\\':
				escape = p;
				break;
			default:
				escape = 0;
		}
	}
}

static void
mutt_parse_email(list_item item)
{
	char *line = item_fget(item, NAME);
	char *name, *email;
#if 0
	char *start = line;
	int i = 0;
#endif

	mutt_fix_quoting(line);
	getname(line, &name, &email);

	if(name)
		item_fput(item, NAME, name);
	else
		return;

	if(email)
		item_fput(item, EMAIL, email);
	else
		return;

	/*
	 * this is completely broken
	 */
#if 0
	while( (start = strchr(start, ',')) && i++ < MAX_EMAILS - 1) {
		getname(++start, &name, &email);
		free(name);
		if(email) {
			if(*email) {
				tmp = strconcat(item[EMAIL], ",", email, NULL);
				free(item[EMAIL]);
				item[EMAIL] = tmp;
			} else {
				xfree(email);
			}
		}
	}
#endif
}

static int
mutt_parse_file(FILE *in)
{
	list_item item = item_create();

	for(;;) {
		memset(item, 0, fields_count * sizeof(char *));

		if(!mutt_read_line(in,
			(field_id(GROUPS) != -1) ? &item[field_id(GROUPS)] : NULL,
			(field_id(NICK) != -1) ? &item[field_id(NICK)] : NULL,
			&item[field_id(NAME)]) )
			mutt_parse_email(item);

		if(feof(in)) {
			item_empty(item);
			break;
		}

		add_item2database(item);
	}
	item_free(&item);

	return 0;
}

/*
 * end of mutt alias import filter
 */


/*
 * ldif export filter
 */

static void
ldif_fput_type_and_value(FILE *out,char *type, char *value )
{
	char *tmp;

	tmp = ldif_type_and_value(type, value, strlen(value));

	fputs(tmp, out);

	free(tmp);
}

static int
ldif_export_database(FILE *out, struct db_enumerator e)
{
	char email[MAX_EMAILSTR_LEN];
	abook_list *emails, *em;

	fprintf(out, "version: 1\n");

	db_enumerate_items(e) {
		char *tmp;
		int j;
		get_first_email(email, e.item);

		if(*email)
			tmp = strdup_printf("cn=%s,mail=%s",db_name_get(e.item),email);
		/* TODO: this may not be enough for a trully "Distinguished" name
		   needed by LDAP. Appending a random uuid could do the trick */
		else
			tmp = strdup_printf("cn=%s",db_name_get(e.item));

		ldif_fput_type_and_value(out, "dn", tmp);
		free(tmp);

		for(j = 0; j < ITEM_FIELDS; j++) {
			if(j == EMAIL) {
				if(*email) {
					tmp = db_email_get(e.item);
					emails = csv_to_abook_list(tmp);
					free(tmp);
					for(em = emails; em; em = em->next)
						ldif_fput_type_and_value(out,
						                         ldif_field_names[EMAIL],
						                         em->data);
				}
			}
			else if(db_fget(e.item,j)) {
				ldif_fput_type_and_value(out,
				                         ldif_field_names[j],
				                         db_fget(e.item, j));
			}
		}

		fprintf(out, "objectclass: top\n"
				"objectclass: person\n\n");
	}

	return 0;
}

/*
 * end of ldif export filter
 */

/*
 * html export filter
 */

static void            html_export_write_head(FILE *out);
static void            html_export_write_tail(FILE *out);

extern struct index_elem *index_elements;

static void
html_print_emails(FILE *out, struct list_field *f)
{
	abook_list *l = csv_to_abook_list(f->data);

	for(; l; l = l->next) {
		fprintf(out, "<a href=\"mailto:%s\">%s</a>", l->data, l->data);
		if(l->next)
			fprintf(out, ", ");
	}

	abook_list_free(&l);
}

static int
html_export_database(FILE *out, struct db_enumerator e)
{
	struct list_field f;
	struct index_elem *cur;

	if(list_is_empty())
		return 2;

	init_index();

	html_export_write_head(out);

	db_enumerate_items(e) {
		fprintf(out, " <tr>\n");
		for(cur = index_elements; cur; cur = cur->next) {
			if(cur->type != INDEX_FIELD)
				continue;
			
			get_list_field(e.item, cur, &f);

			fprintf(out, "  <td>");

			if(f.type == FIELD_EMAILS) {
				html_print_emails(out, &f);
			} else {
				if (strcmp(safe_str(f.data), "") == 0)
					fprintf(out, "&nbsp;");
				else
					fprintf(out, "%s", safe_str(f.data));
			}
			fprintf(out, "</td>\n");
		}
		fprintf(out, " </tr>\n");
	}

	html_export_write_tail(out);

	return 0;
}

static void
html_export_write_head(FILE *out)
{
	char *realname = get_real_name(), *str;
	struct index_elem *cur;

	fprintf(out, "<!DOCTYPE html>\n");
	fprintf(out, "<html>\n");
	fprintf(out, "<head>\n");
	fprintf(out, " <meta charset=\"utf-8\" />\n");
	fprintf(out, " <title>");
	fprintf(out, _("%s's addressbook"), realname );
	fprintf(out, "</title>\n");
	fprintf(out, " <style type=\"text/css\">\n");
	fprintf(out, "  table {border-collapse: collapse ; border: 1px solid #000;}\n");
	fprintf(out, "  table th, table td {text-align: left; border: 1px solid #000; padding: 0.33em;}\n");
	fprintf(out, "  table th {border-bottom: 3px double #000; background-color: #ccc;}\n");
	fprintf(out, " </style>\n");
	fprintf(out, "</head>\n");
	fprintf(out, "<body>\n");
	fprintf(out, "<h1>");
	fprintf(out, _("%s's addressbook"), realname);
	fprintf(out, "</h1>\n");

	fprintf(out, "<table>\n");
	fprintf(out, "<thead>\n");
	fprintf(out, " <tr>\n");
	for(cur = index_elements; cur; cur = cur->next) {
		if(cur->type != INDEX_FIELD)
			continue;

		get_field_info(cur->d.field.id, NULL, &str, NULL);

		fprintf(out, "  <th>");

		if (strcmp(str, "") == 0)
			fprintf(out, "&nbsp;");
		else
			fprintf(out, "%s", str);

		fprintf(out, "</th>\n");
	}
	fprintf(out, " </tr>\n");
	fprintf(out, "<thead>\n");
	fprintf(out, "<tbody>\n");

	free(realname);
}

static void
html_export_write_tail(FILE *out)
{
	fprintf(out, "</table>\n");
	fprintf(out, "</body>\n");
	fprintf(out, "</html>");
}

/*
 * end of html export filter
 */


/*
 * pine addressbook import filter
 */

#define PINE_BUF_SIZE 2048

static void
pine_fixbuf(char *buf)
{
	int i,j;

	for(i = 0,j = 0; j < (int)strlen(buf); i++, j++)
		buf[i] = buf[j] == '\n' ? buf[++j] : buf[j];
}

static void
pine_convert_emails(char *s)
{
	int i;
	char *tmp;

	if(s == NULL || *s != '(')
		return;

	for(i = 0; s[i]; i++)
		s[i] = s[i + 1];

	if( ( tmp = strchr(s,')')) )
		*tmp = '\0';

	for(i = 1; ( tmp = strchr(s, ',') ) != NULL ; i++, s = tmp + 1)
		if(i > MAX_LIST_ITEMS - 1) {
			*tmp = '\0';
			break;
		}

}

static void
pine_parse_buf(char *buf)
{
	list_item item;
	char *start = buf;
	char *end;
	char tmp[PINE_BUF_SIZE];
	int i, len, last;
	int pine_conv_table[]= {NICK, NAME, EMAIL, -1, NOTES};

	item = item_create();

	for(i=0, last=0; !last ; i++) {
		if( !(end = strchr(start, '\t')) )
			last=1;

		len = last ? strlen(start) : (int) (end-start);
		len = min(len, PINE_BUF_SIZE - 1);

		if(i < (int)(sizeof(pine_conv_table) / sizeof(*pine_conv_table))
				&& pine_conv_table[i] >= 0) {
			strncpy(tmp, start, len);
			tmp[len] = 0;
			if(*tmp)
				item_fput(item, pine_conv_table[i],
						xstrdup(tmp));
		}
		start = end + 1;
	}

	pine_convert_emails(item_fget(item, EMAIL));
	add_item2database(item);
	item_free(&item);
}


#define LINESIZE	1024

static int
pine_parse_file(FILE *in)
{
	char line[LINESIZE];
	char *buf = NULL;
	char *ptr;
	int i;

	fgets(line, LINESIZE, in);

	while(!feof(in)) {
		for(i = 2;;i++) {
			buf = xrealloc(buf, i*LINESIZE);
			if(i == 2)
				strcpy(buf, line);
			fgets(line, LINESIZE, in);
			ptr=(char *)&line;
			if(*ptr != ' ' || feof(in))
				break;
			else
				while(*ptr == ' ')
					ptr++;

			strcat(buf, ptr);
		}
		if(*buf == '#') {
			xfree(buf);
			continue;
		}
		pine_fixbuf(buf);

		pine_parse_buf(buf);

		xfree(buf);
	}

	return 0;
}

/*
 * end of pine addressbook import filter
 */


/*
 * pine addressbook export filter
 *
 *  filter doesn't wrap the lines as it should but Pine seems to handle
 *  created files without problems - JH
 */

static int
pine_export_database(FILE *out, struct db_enumerator e)
{
	char *emails;

	db_enumerate_items(e) {
		emails = db_email_get(e.item);
		fprintf(out, strchr(emails, ',') /* multiple addresses? */ ?
				"%s\t%s\t(%s)\t\t%s\n" : "%s\t%s\t%s\t\t%s\n",
				safe_str(db_fget(e.item, NICK)),
				safe_str(db_name_get(e.item)),
				emails,
				safe_str(db_fget(e.item, NOTES))
				);
		free(emails);
	}

	return 0;
}

/*
 * end of pine addressbook export filter
 */


/*
 * csv import filter
 */

/* This is used by both allcsv_export_database() and csv_export_common()
   to distinguish between standard and defined fields.
   To avoid confusions this should stay > ITEM_FIELDS  */
#define CUSTOM_FIELD_START_INDEX       (ITEM_FIELDS + 10)

/* FIXME
 * these files should be parsed according to a certain
 * lay out, or the default if layout is not given, at
 * the moment only default is done...
 */

#define CSV_COMMENT_CHAR	'#'
#define CSV_DUPLICATE_SEPARATOR	" "
#define CSV_TABLE_SIZE(t)	(sizeof (t) / sizeof *(t))

static int csv_conv_table[] = {
	NAME,
	EMAIL,
	PHONE,
	NOTES,
	NICK
};

static int allcsv_conv_table[] = {
	NAME,
	EMAIL,
	ADDRESS,
	ADDRESS2,
	CITY,
	STATE,
	ZIP,
	COUNTRY,
	PHONE,
	WORKPHONE,
	FAX,
	MOBILEPHONE,
	NICK,
	URL,
	NOTES,
	ANNIVERSARY
};

static int palmcsv_conv_table[] = {
	NAME,		/* Last name */
	NAME,		/* First name */
	NOTES,		/* Title */
	NICK,		/* Company */
	WORKPHONE,
	PHONE,
	FAX,
	MOBILEPHONE,
	EMAIL,
	ADDRESS,
	CITY,
	STATE,
	ZIP,
	COUNTRY,
	ANNIVERSARY,
};

static void
csv_convert_emails(char *s)
{
	int i;
	char *tmp;

	if(s == NULL)
		return;

	for(i = 1; ( tmp = strchr(s, ',') ) != NULL ; i++, s = tmp + 1)
		if(i > MAX_LIST_ITEMS - 1) {
			*tmp = 0;
			break;
		}

}

static char *
csv_remove_quotes(char *s)
{
	char *copy, *trimmed;
	int len;

	copy = trimmed = xstrdup(s);
	strtrim(trimmed);

	len = strlen(trimmed);
	if(trimmed[len - 1] == '\"' && *trimmed == '\"') {
		if(len < 3) {
			xfree(copy);
			return NULL;
		}
		trimmed[len - 1] = 0;
		trimmed++;
		trimmed = xstrdup(trimmed);
		free(copy);
		return trimmed;
	}

	xfree(copy);
	return xstrdup(s);
}

static int
csv_field_to_item(int *table_base, size_t table_size, int field)
{
	if(field < table_size)
		return field_id(table_base[field]);

	return -1;
}

static void
csv_store_item(list_item item, int i, char *s)
{
	char *newstr = NULL;

	if(!s || !*s)
		return;

	if( !(newstr = csv_remove_quotes(s)) )
		return;

	if(i >= 0) {
		if (item[i] != NULL) {
			char *oldstr = item[i];

			item[i] = strconcat(newstr, CSV_DUPLICATE_SEPARATOR,
				oldstr, NULL);
			xfree(newstr);
			xfree(oldstr);
		} else {
			item[i] = newstr;
		}
	} else {
		xfree(newstr);
	}
}

static int
csv_is_valid_quote_end(char *p)
{
	if(*p != '\"')
		return FALSE;

	for(p++; *p; p++) {
		if(*p == ',')
			return TRUE;
		else if(!ISSPACE(*p))
			return FALSE;
	}

	return TRUE;
}

static int
csv_is_valid_quote_start(char *p)
{
	for(; *p; p++) {
		if(*p == '\"')
			return TRUE;
		else if(!ISSPACE(*p))
			return FALSE;
	}

	return FALSE;
}

static void
csv_parse_line(char *line, int *table_base, size_t table_size)
{
	char *p, *start;
	int field;
	bool in_quote = FALSE;
	list_item item;

	item = item_create();

	for(p = start = line, field = 0; *p; p++) {
		if(in_quote) {
			if(csv_is_valid_quote_end(p))
				in_quote = FALSE;
		} else {
			if ( (((p - start) / sizeof (char)) < 2 ) &&
				csv_is_valid_quote_start(p) )
				in_quote = TRUE;
		}

		if(*p == ',' && !in_quote) {
			*p = 0;
			csv_store_item(item,
				csv_field_to_item(table_base,table_size,field),
				start);
			field++;
			start = p + 1;
		}
	}
	/*
	 * store last field
	 */
	csv_store_item(item, csv_field_to_item(table_base, table_size, field),
		start);

	csv_convert_emails(item_fget(item, EMAIL));
	add_item2database(item);
	item_free(&item);
}

static int
csv_parse_file_common(FILE *in, int *conv_table, size_t table_size)
{
	char *line = NULL;

	while(!feof(in)) {
		line = getaline(in);

		if(line && *line && *line != CSV_COMMENT_CHAR)
			csv_parse_line(line, conv_table, table_size);

		xfree(line);
	}

	return 0;
}

static int
csv_parse_file(FILE *in)
{
	return csv_parse_file_common(in, csv_conv_table,
		CSV_TABLE_SIZE(csv_conv_table));
}

static int
allcsv_parse_file(FILE *in)
{
	return csv_parse_file_common(in, allcsv_conv_table,
		CSV_TABLE_SIZE(allcsv_conv_table));
}

static int
palmcsv_parse_file(FILE *in)
{
	return csv_parse_file_common(in, palmcsv_conv_table,
		CSV_TABLE_SIZE(palmcsv_conv_table));
}

/*
 * end of csv import filter
 */

/*
 * vCard import filter
 */

static char *vcard_fields[] = {
	"FN",			/* FORMATTED NAME */
	"EMAIL",		/* EMAIL */
	"ADR",			/* ADDRESS */
	"ADR",			/* ADDRESS2 */
	"ADR",			/* CITY */
	"ADR",			/* STATE */
	"ADR",			/* ZIP */
	"ADR",			/* COUNTRY */
	"TEL",			/* PHONE */
	"TEL",			/* WORKPHONE */
	"TEL",			/* FAX */
	"TEL",			/* MOBILEPHONE */
	"NICKNAME",		/* NICK */
	"URL",			/* URL */
	"NOTE",			/* NOTES */
	"BDAY",			/* ANNIVERSARY */
	"N",			/* NAME: special case/mapping in vcard_parse_line() */
	NULL			/* ITEM_FIELDS */
};

enum {
	VCARD_KEY = 0,
	VCARD_KEY_ATTRIBUTE,
	VCARD_VALUE,
};

static char *
vcard_get_line_element(char *line, int element)
{
	int i;
	char *line_copy = 0;
	char *result = 0;
	char *key = 0;
	char *key_attr = 0;
	char *value = 0;

	line_copy = xstrdup(line);

	/* change newline characters, if present, to end of string */
	for(i=0; line_copy[i]; i++) {
		if(line_copy[i] == '\r' || line_copy[i] == '\n') {
			line_copy[i] = '\0';
			break;
		}
	}

	/* separate key from value */
	for(i=0; line_copy[i]; i++) {
		if(line_copy[i] == ':') {
			line_copy[i] = '\0';
			key = line_copy;
			value = &line_copy[i+1];
			break;
		}
	}

	/* separate key from key attributes */
	/* works for vCard 2 as well (automagically) */
	if (key) {
		for(i=0; key[i]; i++) {
			if(key[i] == ';') {
				key[i] = '\0';
				key_attr = &key[i+1];
				break;
			}
		}
	}

	switch(element) {
	case VCARD_KEY:
		if(key)
			result = xstrdup(key);
		break;
	case VCARD_KEY_ATTRIBUTE:
		if(key_attr)
			result = xstrdup(key_attr);
		break;
	case VCARD_VALUE:
		if(value)
			result = xstrdup(value);
		break;
	}

	xfree(line_copy);
	return result;
}

static void
vcard_parse_email(list_item item, char *line)
{
	char *email;

	email = vcard_get_line_element(line, VCARD_VALUE);

	if(item[1]) {
		item[1] = strconcat(item[1], ",", email, 0);
		xfree(email);
	}
	else {
		item[1] = email;
	}
}


/*
 * mappings between vCard ADR field and abook's ADDRESS
 * see rfc2426 section 3.2.1
 */
static void
vcard_parse_address(list_item item, char *line)
{
	char *value;

	value = vcard_get_line_element(line, VCARD_VALUE);
	if(!value)
		return;

	// vCard(post office box) - not used
	strsep(&value, ";");
	if(!value) return;

	// vCard(the extended address)
	item_fput(item, ADDRESS2, xstrdup(strsep(&value, ";")));
	if(!value) return;

	// vCard(the street address)
	item_fput(item, ADDRESS, xstrdup(strsep(&value, ";")));
	if(!value) return;

	// vCard(the locality)
	item_fput(item, CITY, xstrdup(strsep(&value, ";")));
	if(!value) return;

	// vCard(the region)
	item_fput(item, STATE, xstrdup(strsep(&value, ";")));
	if(!value) return;

	// vCard(the postal code)
	item_fput(item, ZIP, xstrdup(strsep(&value, ";")));
	if(!value) return;

	// vCard(the country name)
	item_fput(item, COUNTRY, xstrdup(strsep(&value, ";")));

	// support of optional trailing ";" to the ADR field
	if(value && *value) xfree(value);
}

static void
vcard_parse_name(list_item item, char *line)
{
	// store the "N" field into "NAME" *if* no "FN:"
	// value has already been stored here
	if(item[0]) return;

	int i = -1;
	item[0] = vcard_get_line_element(line, VCARD_VALUE);
	// "N:" can be multivalued => replace ';' separators by ' '
	while(item[0][++i]) if(item[0][i] == ';') item[0][i] = ' ';

	// http://www.daniweb.com/software-development/c/code/216919
	char *original = item[0], *p = original;
	int trimmed = 0;
	do {
	  if (*original != ' ' || trimmed) {
	    trimmed = 1; *p++ = *original;
	  }
	} while(*original++);
}

static void
vcard_parse_phone(list_item item, char *line)
{
	char *type = vcard_get_line_element(line, VCARD_KEY_ATTRIBUTE);
	char *value = vcard_get_line_element(line, VCARD_VALUE);

	/* set the standard number */
	if (!type) item_fput(item, PHONE, value);

	/*
	 * see rfc2426 section 3.3.1
	 * Note: we probably support both vCard 2 and 3
	 */
	else {
		if (strcasestr(type, "home") != NULL)
			item_fput(item, PHONE, xstrdup(value));
		else if (strcasestr(type, "work") != NULL)
			item_fput(item, WORKPHONE, xstrdup(value));
		else if (strcasestr(type, "fax") != NULL)
			item_fput(item, FAX, xstrdup(value));
		else if (strcasestr(type, "cell") != NULL)
			item_fput(item, MOBILEPHONE, xstrdup(value));

		xfree(type);
		xfree(value);
	}
}

static void
vcard_parse_line(list_item item, char *line)
{
	int i;
	char *key;

	for(i=0; vcard_fields[i]; i++) {
		key = vcard_fields[i];

		if(0 == strncmp(key, line, strlen(key))) {
			if(0 == strcmp(key, "EMAIL"))
				vcard_parse_email(item, line);
			else if(i == 2)
				vcard_parse_address(item, line);
			else if(0 == strcmp(key, "TEL"))
				vcard_parse_phone(item, line);
			else if(0 == strcmp(key, "N"))
				vcard_parse_name(item, line);
			else
				item[i] = vcard_get_line_element(line, VCARD_VALUE);
			return;
		}
	}
}

static void
vcard_parse_item(FILE *in)
{
	char *line = NULL;
	list_item item = item_create();

	while(!feof(in)) {
		line = getaline(in);

		if(line && !strncmp("END:VCARD", line, 9)) {
			xfree(line);
			break;
		}
		else if(line) {
			vcard_parse_line(item, line);
			xfree(line);
		}
	}

	add_item2database(item);
	item_free(&item);
}

static int
vcard_parse_file(FILE *in)
{
	char *line = NULL;

	while(!feof(in)) {
		line = getaline(in);

		if(line && !strncmp("BEGIN:VCARD", line, 11)) {
			xfree(line);
			vcard_parse_item(in);
		}
		else if(line) {
			xfree(line);
		}
	}

	return 0;
}

/*
 * end of vCard import filter
 */

/*
 * csv addressbook export filters
 */

#define CSV_LAST		(-1)
#define CSV_UNDEFINED		(-2)
#define CSV_SPECIAL(X)		(-3 - (X))
#define CSV_IS_SPECIAL(X)	((X) <= -3)

static int
csv_export_common(FILE *out, struct db_enumerator e,
		int fields[], void (*special_func)(FILE *, int, int))
{
	int i;

	db_enumerate_items(e) {
		for(i = 0; fields[i] != CSV_LAST; i++) {
			if(fields[i] == CSV_UNDEFINED)
				fprintf(out, "\"\"");
			else if(CSV_IS_SPECIAL(fields[i])) {
				if(special_func)
					(*special_func)(out, e.item, fields[i]);
			}
			else if(fields[i] >= CUSTOM_FIELD_START_INDEX) {
				fprintf(out, "\"%s\"",
					safe_str(db_fget_byid(e.item, fields[i] - CUSTOM_FIELD_START_INDEX)));
			}
			else
				/*fprintf(out,(
			strchr(safe_str(database[e.item][field_idx(fields[i])]), ',') ||
			strchr(safe_str(database[e.item][field_idx(fields[i])]), '\"')) ?
				"\"%s\"" : "%s",
				safe_str(database[e.item][field_idx(fields[i])])
				);*/
				fprintf(out, "\"%s\"",
					safe_str(db_fget(e.item,fields[i])));

			if(fields[i + 1] != CSV_LAST)
				fputc(',', out);
		}
		fputc('\n', out);
	}

	return 0;
}

static int
csv_export_database(FILE *out, struct db_enumerator e)
{
	int csv_export_fields[] = {
		NAME,
		EMAIL,
		PHONE,
		NOTES,
		NICK,
		CSV_LAST
	};

	csv_export_common(out, e, csv_export_fields, NULL);

	return 0;
}

static int
allcsv_export_database(FILE *out, struct db_enumerator e)
{
	/*
	 * TODO: Should get these atomatically from abook_fileds
	 *  - JH
	 */
	int allcsv_export_fields[ITEM_FIELDS + 6] = { // only the 5 custom fields are allowed so far
		NAME,
		EMAIL,
		ADDRESS,
		ADDRESS2,
		CITY,
		STATE,
		ZIP,
		COUNTRY,
		PHONE,
		WORKPHONE,
		FAX,
		MOBILEPHONE, // spelt "mobile" in standard_fields
		NICK,
		URL,
		NOTES,
		ANNIVERSARY,
		GROUPS,
		CSV_LAST
	};

	fprintf(out, "#");
	int i = 0;
	while(allcsv_export_fields[i+1] != CSV_LAST) {
		fprintf(out, "\"%s\",", standard_fields[i++].key);
	}
	fprintf(out, "\"%s\"", standard_fields[i].key);

	/*
	  Custom fields handling:
	  This loop appends custom fields' id at the end of allcsv_export_fields and shift
	  the CSV_LAST sentinel value each time one is found.
	  CUSTOM_FIELD_START_INDEX is added to these index values so csv_export_common()
	  can later recognize them and call db_fget_byid() instead of the traditional db_fget()
	*/

	// pointer to the end of the field list
	int append_field = ITEM_FIELDS;
	// index used by custom_field_key
	int field_no;
	// name of the defined field <field_no> as chosen by the user
	char *custom_field_name = NULL;

	abook_field_list *cur, *custom_fieldsl;
	for(cur = list_custom_fields(); cur; cur = cur->next) {
		if(find_declared_field(cur->field->key)) {
			find_field_number(cur->field->key, &field_no);
			get_field_info(field_no, NULL, &custom_field_name, NULL);
			// append the field to the list
			allcsv_export_fields[append_field] = field_no + CUSTOM_FIELD_START_INDEX;
			allcsv_export_fields[++append_field] = CSV_LAST;
			// print column name
			fprintf(out, ",\"%s\"", custom_field_name);
		}
	}
	if (custom_field_name) free(custom_field_name);

	fprintf(out, "\n");

	csv_export_common(out, e, allcsv_export_fields, NULL);

	return 0;
}

/*
 * palm csv
 */

#define PALM_CSV_NAME	CSV_SPECIAL(0)
#define PALM_CSV_END	CSV_SPECIAL(1)
#define PALM_CSV_CAT	CSV_SPECIAL(2)

static void
palm_split_and_write_name(FILE *out, char *name)
{
	char *p;

	assert(name);

	if ( (p = strchr(name, ' ')) ) {
		/*
		 * last name first
		 */
		fprintf(out, "\"%s\",\"" , p + 1);
		fwrite((void *)name, p - name, sizeof(char), out);
		fputc('\"', out);
	} else {
		fprintf(out, "\"%s\"", safe_str(name));
	}
}

static void
palm_csv_handle_specials(FILE *out, int item, int field)
{
	switch(field) {
		case PALM_CSV_NAME:
			palm_split_and_write_name(out, db_name_get(item));
			break;
		case PALM_CSV_CAT:
			fprintf(out, "\"abook\"");
			break;
		case PALM_CSV_END:
			fprintf(out, "\"0\"");
			break;
		default:
			assert(0);
	}
}

static int
palm_export_database(FILE *out, struct db_enumerator e)
{
	int palm_export_fields[] = {
		PALM_CSV_NAME,		/* LASTNAME, FIRSTNAME 	*/
		CSV_UNDEFINED,		/* TITLE   		*/
		CSV_UNDEFINED, 		/* COMPANY 		*/
		WORKPHONE,		/* WORK PHONE 		*/
		PHONE,			/* HOME PHONE		*/
		FAX,			/* FAX			*/
		MOBILEPHONE, 		/* OTHER 		*/
		EMAIL,			/* EMAIL		*/
		ADDRESS,		/* ADDRESS		*/
		CITY,			/* CITY			*/
		STATE,			/* STATE		*/
		ZIP,			/* ZIP			*/
		COUNTRY,		/* COUNTRY		*/
        	NICK, 			/* DEFINED 1		*/
        	URL, 			/* DEFINED 2 		*/
		CSV_UNDEFINED,		/* DEFINED 3		*/
		CSV_UNDEFINED,		/* DEFINED 4 		*/
		NOTES,			/* NOTE			*/
		PALM_CSV_END,		/* "0" 			*/
		PALM_CSV_CAT,		/* CATEGORY 		*/
		CSV_LAST
	};

	csv_export_common(out, e, palm_export_fields, palm_csv_handle_specials);

	return 0;
}

/*
 * end of csv export filters
 */

/*
 * vCard 2 addressbook export filter
 */

static int
vcard_export_database(FILE *out, struct db_enumerator e)
{
  db_enumerate_items(e)
    vcard_export_item(out, e.item);
  return 0;
}

void
vcard_export_item(FILE *out, int item)
{
	int j, email_no;
	char *name, *tmp;
	abook_list *emails, *em;
	fprintf(out, "BEGIN:VCARD\r\nFN:%s\r\n",
		safe_str(db_name_get(item)));

	name = get_surname(db_name_get(item));
	for( j = strlen(db_name_get(item)) - 1; j >= 0; j-- ) {
	  if((db_name_get(item))[j] == ' ')
	    break;
	}
	fprintf(out, "N:%s;%.*s\r\n",
		safe_str(name),
		j,
		safe_str(db_name_get(item))
		);

	free(name);

	if(db_fget(item, NICK))
	  fprintf(out, "NICKNAME:%s\r\n",
		  safe_str(db_fget(item, NICK)));
	if(db_fget(item, ANNIVERSARY))
	  fprintf(out, "BDAY:%s\r\n",
		  safe_str(db_fget(item, ANNIVERSARY)));

	// see rfc6350 section 6.3.1
	if(db_fget(item, ADDRESS)) {
		fprintf(out, "ADR:;%s;%s;%s;%s;%s;%s\r\n",
		        // pobox (unsupported)
		        safe_str(db_fget(item, ADDRESS2)), // ext (n°, ...)
		        safe_str(db_fget(item, ADDRESS)), // street
		        safe_str(db_fget(item, CITY)), // locality
		        safe_str(db_fget(item, STATE)), // region
		        safe_str(db_fget(item, ZIP)), // code (postal)
		        safe_str(db_fget(item, COUNTRY)) // country
		        );
	}

	if(db_fget(item, PHONE))
	  fprintf(out, "TEL;HOME:%s\r\n",
		  db_fget(item, PHONE));
	if(db_fget(item, WORKPHONE))
	  fprintf(out, "TEL;WORK:%s\r\n",
		  db_fget(item, WORKPHONE));
	if(db_fget(item, FAX))
	  fprintf(out, "TEL;FAX:%s\r\n",
		  db_fget(item, FAX));
	if(db_fget(item, MOBILEPHONE))
	  fprintf(out, "TEL;CELL:%s\r\n",
		  db_fget(item, MOBILEPHONE));

	tmp = db_email_get(item);
	if(*tmp) {
	  emails = csv_to_abook_list(tmp);
	  fprintf(out, "EMAIL;PREF;INTERNET:%s\r\n", emails->data);
	  email_no = 1;
	  for(em = emails->next; em; em = em->next, email_no++ )
		  fprintf(out, "EMAIL;%d;INTERNET:%s\r\n", email_no, em->data);

	  abook_list_free(&emails);
	}
	free(tmp);

	if(db_fget(item, NOTES))
	  fprintf(out, "NOTE:%s\r\n",
		  db_fget(item, NOTES));
	if(db_fget(item, URL))
	  fprintf(out, "URL:%s\r\n",
		  db_fget(item, URL));

	fprintf(out, "END:VCARD\r\n\r\n");

}

/*
 * end of vCard export filter
 */


/*
 * mutt alias export filter
 */

static char *
mutt_alias_genalias(int i)
{
	char *tmp, *pos;

	if(db_fget(i, NICK))
		return xstrdup(db_fget(i, NICK));

	tmp = xstrdup(db_name_get(i));

	if( ( pos = strchr(tmp, ' ') ) )
		*pos = 0;

	strlower(tmp);

	return tmp;
}

/*
 * This function is a variant of abook_list_to_csv
 * */
static char *
mutt_alias_gengroups(int i)
{
	char *groups, *res = NULL;
	char groupstr[7] = "-group ";
	abook_list *list, *tmp;

	groups = db_fget(i, GROUPS);

	if(!groups)
		return NULL;

	list = csv_to_abook_list(groups);
	for(tmp = list; tmp; tmp = tmp->next) {
		if(tmp == list) {
			res = xmalloc(strlen(groupstr)+strlen(tmp->data)+1);
			res = strcpy(res, groupstr);
		} else {
			res = xrealloc(res, strlen(res)+1+strlen(groupstr)+strlen(tmp->data)+1);
			strcat(res, " ");
			strcat(res, groupstr);
		}
		strcat(res, tmp->data);
	}
	abook_list_free(&list);

	return res;
}

static int
mutt_alias_export(FILE *out, struct db_enumerator e)
{
	char email[MAX_EMAIL_LEN];
	char *alias = NULL;
	char *groups = NULL;
	int email_addresses;
	char *ptr;

	db_enumerate_items(e) {
		alias = (field_id(NICK) != -1) ? mutt_alias_genalias(e.item) : NULL;
		groups = (field_id(GROUPS) != -1) ?  mutt_alias_gengroups(e.item) : NULL;
		get_first_email(email, e.item);

		/* do not output contacts without email address */
		/* cause this does not make sense in mutt aliases */
		if (*email) {

			/* output first email address */
			fprintf(out,"alias ");
			if(groups)
				fprintf(out, "%s ", groups);
			if(alias)
				fprintf(out, "%s ", alias);
			fprintf(out, "%s <%s>\n",
					db_name_get(e.item),
					email);

			/* number of email addresses */
			email_addresses = 1;
			ptr = db_email_get(e.item);
			while (*ptr != '\0') {
				if (*ptr == ',') {
					email_addresses++;
				}
				ptr++;
			}

			/* output other email addresses */
			while (email_addresses-- > 1) {
				roll_emails(e.item, ROTATE_RIGHT);
				get_first_email(email, e.item);
				fprintf(out,"alias ");
				if( groups )
					fprintf(out, "%s ", groups);
				if(alias)
					fprintf(out, "%s__%s ", alias, email);
				else
					fprintf(out, "%s__%s ", db_name_get(e.item), email);
				fprintf(out, "%s <%s>\n",
						db_name_get(e.item),
						email);
			}
			roll_emails(e.item, ROTATE_RIGHT);
			xfree(alias);
			xfree(groups);
		}
	}

	return 0;
}

void muttq_print_item(FILE *file, int item)
{
	abook_list *emails, *e;
	char *tmp = db_email_get(item);

	emails = csv_to_abook_list(tmp);
	free(tmp);

	for(e = emails; e; e = e->next) {
		fprintf(file, "%s\t%s\t%s\n", e->data, db_name_get(item),
				!db_fget(item, NOTES) ?" " :db_fget(item, NOTES)
				);
		if(!opt_get_bool(BOOL_MUTT_RETURN_ALL_EMAILS))
			break;
	}
	abook_list_free(&emails);
}

static int
mutt_query_export_database(FILE *out, struct db_enumerator e)
{
  fprintf(out, "All items\n");
  db_enumerate_items(e)
    muttq_print_item(out, e.item);
  return 0;
}

/*
 * end of mutt alias export filter
 */


void mailto_print_item(FILE *file, int item)
{
	abook_list *emails, *e;
	char *tmp = db_email_get(item);

	emails = csv_to_abook_list(tmp);
	free(tmp);

	for(e = emails; e; e = e->next) {
		fprintf(file, "%s <%s>\n", db_name_get(item), e->data);
		if(!opt_get_bool(BOOL_MUTT_RETURN_ALL_EMAILS))
			break;
	}
	abook_list_free(&emails);
}


/*
 * printable export filter
 */


static void
text_write_address_us(FILE *out, int i) {
	fprintf(out, "\n%s", db_fget(i, ADDRESS));

   	if(db_fget(i, ADDRESS2))
		fprintf(out, "\n%s", db_fget(i, ADDRESS2));

	if(db_fget(i, CITY))
		fprintf(out, "\n%s", db_fget(i, CITY));

	if(db_fget(i, STATE) || db_fget(i, ZIP)) {
		fputc('\n', out);

		if(db_fget(i, STATE)) {
			fprintf(out, "%s", db_fget(i, STATE));
			if(db_fget(i, ZIP))
				fputc(' ', out);
		}

		if(db_fget(i, ZIP))
			fprintf(out, "%s", db_fget(i, ZIP));
	}

	if(db_fget(i, COUNTRY))
		fprintf(out, "\n%s", db_fget(i, COUNTRY));
}


static void
text_write_address_uk(FILE *out, int i) {
	int j;

	for(j = ADDRESS; j <= COUNTRY; j++)
		if(db_fget(i, j))
			fprintf(out, "\n%s", db_fget(i, j));
}

static void
text_write_address_eu(FILE *out, int i) {
	fprintf(out, "\n%s", db_fget(i, ADDRESS));

   	if(db_fget(i, ADDRESS2))
		fprintf(out, "\n%s", db_fget(i, ADDRESS2));

	if(db_fget(i, ZIP) || db_fget(i, CITY)) {
		fputc('\n', out);

		if(db_fget(i, ZIP)) {
			fprintf(out, "%s", db_fget(i, ZIP));
			if(db_fget(i, CITY))
				fputc(' ', out);
		}

		fprintf(out, "%s", safe_str(db_fget(i, CITY)));
	}

	if(db_fget(i, STATE))
		fprintf(out, "\n%s", db_fget(i, STATE));

	if(db_fget(i, COUNTRY))
		fprintf(out, "\n%s", db_fget(i, COUNTRY));
}

static int
text_export_database(FILE * out, struct db_enumerator e)
{
	abook_list *emails, *em;
	int j;
	char *realname = get_real_name(), *str = NULL, *tmp;
	char *style = opt_get_str(STR_ADDRESS_STYLE);

	fprintf(out,
		"-----------------------------------------\n%s's address book\n"
		"-----------------------------------------\n\n\n",
		realname);
	free(realname);

	db_enumerate_items(e) {
		fprintf(out,
			"-----------------------------------------\n\n");
		fprintf(out, "%s", db_name_get(e.item));
		if(db_fget(e.item, NICK) && *db_fget(e.item, NICK))
			fprintf(out, "\n(%s)", db_fget(e.item, NICK));
		fprintf(out, "\n");

		tmp = db_email_get(e.item);
		if(*tmp) {
			emails = csv_to_abook_list(tmp);

			fprintf(out, "\n");
			for(em = emails; em; em = em->next)
				fprintf(out, "%s\n", em->data);

			abook_list_free(&emails);
		}
		free(tmp);
		/* Print address */
		if(db_fget(e.item, ADDRESS)) {
			if(!safe_strcmp(style, "us"))	/* US like */
				text_write_address_us(out, e.item);
			else if(!safe_strcmp(style, "uk"))	/* UK like */
				text_write_address_uk(out, e.item);
			else	/* EU like */
				text_write_address_eu(out, e.item);

			fprintf(out, "\n");
		}

		if((db_fget(e.item, PHONE)) ||
			(db_fget(e.item, WORKPHONE)) ||
			(db_fget(e.item, FAX)) ||
			(db_fget(e.item, MOBILEPHONE))) {
			fprintf(out, "\n");
			for(j = PHONE; j <= MOBILEPHONE; j++)
				if(db_fget(e.item, j)) {
					get_field_info(field_id(j),
							NULL, &str, NULL);
					fprintf(out, "%s: %s\n", str,
						db_fget(e.item, j));
				}
		}

		if(db_fget(e.item, URL))
			fprintf(out, "\n%s\n", db_fget(e.item, URL));
		if(db_fget(e.item, NOTES))
			fprintf(out, "\n%s\n", db_fget(e.item, NOTES));

		fprintf(out, "\n");
	}

	fprintf(out, "-----------------------------------------\n");

	return 0;
}

/*
 * end of printable export filter
 */

/*
 * elm alias export filter
 */

static int
elm_alias_export(FILE *out, struct db_enumerator e)
{
	char email[MAX_EMAIL_LEN];
	char *alias = NULL;

	db_enumerate_items(e) {
		alias = mutt_alias_genalias(e.item);
		get_first_email(email, e.item);
		fprintf(out, "%s = %s = %s\n",alias,db_name_get(e.item),email);
		xfree(alias);
	}

	return 0;
}

/*
 * end of elm alias export filter
 */


/*
 * Spruce export filter
 */

static int
spruce_export_database (FILE *out, struct db_enumerator e)
{
	char email[MAX_EMAIL_LEN];

	fprintf(out, "# This is a generated file made by abook for the Spruce e-mail client.\n\n");

	db_enumerate_items(e) {
		get_first_email(email, e.item);
		if(strcmp(email, "")) {
			fprintf(out, "# Address %d\nName: %s\nEmail: %s\nMemo: %s\n\n",
					e.item,
					db_name_get(e.item),
					email,
					safe_str(db_fget(e.item, NOTES))
					);
		}
	}

	fprintf (out, "# End of address book file.\n");

	return 0;
}

/*
 * end of Spruce export filter
 */

/*
 * wanderlust addressbook export filter
 */

static int
wl_export_database(FILE *out, struct db_enumerator e)
{
	char email[MAX_EMAIL_LEN];

	fprintf(out, "# Wanderlust address book written by %s\n\n", PACKAGE);
	db_enumerate_items(e) {
		get_first_email(email, e.item);
		if(*email) {
			fprintf(out,
				"%s\t\"%s\"\t\"%s\"\n",
				email,
				safe_str(db_fget(e.item, NICK)),
				safe_str(db_name_get(e.item))
			);
		}
	}

	fprintf (out, "\n# End of address book file.\n");

	return 0;
}

/*
 * end of wanderlust addressbook export filter
 */

/*
 * iCalendar export filter
 */

static int
ical_export_database(FILE *out, struct db_enumerator e)
{
	fprintf(out, "BEGIN:VCALENDAR\n");
	fprintf(out, "PRODID:-//%s//NONSGML %s//EN\n", PACKAGE, VERSION);
	fprintf(out, "VERSION:2.0\n");

	db_enumerate_items(e) {
		int year, month = 0, day = 0;
		char *anniversary = db_fget(e.item, ANNIVERSARY);
		char *summary;

		if (anniversary) {
			if(!parse_date_string(anniversary, &day, &month, &year))
				continue;

			/*
			 * If there is no year, or an unrepresentable one,
			 * use the lowest leap year that can fit on a system
			 * using a 32-bit time_t.
			 *
			 * Unfortunately we cannot use the --MMDD reduced form
			 * which was allowed in ISO 8601:2000, as that is not
			 * allowed anymore in ISO 8601:2004, which is what
			 * iCalendar is based on.
			 */
			if(year < 1904)
				year = 1904;

			summary = strdup_printf(_("Anniversary of %s"),
				safe_str(db_name_get(e.item))
			);

			fprintf(out, "BEGIN:VEVENT\n");
			fprintf(out, "DTSTART:%d%02d%02d\n", year, month, day);
			fprintf(out, "RRULE:FREQ=YEARLY\n");
			fprintf(out, "SUMMARY:%s\n", summary);
			fprintf(out, "END:VEVENT\n");

			free(summary);
		}
	}

	fprintf(out, "END:VCALENDAR\n");

	return 0;
}

/*
 * end of iCalendar export filter
 */

/*
 * BSD calendar export filter
 */

static int
bsdcal_export_database(FILE *out, struct db_enumerator e)
{
	db_enumerate_items(e) {
		int year, month = 0, day = 0;
		char *anniversary = db_fget(e.item, ANNIVERSARY);

		if(anniversary) {
			if(!parse_date_string(anniversary, &day, &month, &year))
				continue;

			fprintf(out,
				_("%02d/%02d\tAnniversary of %s\n"),
				month,
				day,
				safe_str(db_name_get(e.item))
			);
		}
	}

	return 0;
}

/*
 * end of BSD calendar export filter
 */

/*
 * custom export filter
 */

static int
find_field_enum(char *s) {
	int i = -1;
	while(standard_fields[++i].key)
		if(!strcmp(standard_fields[i].key, s))
			return i;
	return -1;
}

/* Convert a string with named placeholders to
   a *printf() compatible string.
   Stores the abook field values into ft. */
void
parse_custom_format(char *s, char *fmt_string, int *ft)
{
	if(! fmt_string || ! ft) {
		fprintf(stderr, _("parse_custom_format: fmt_string or ft not allocated\n"));
		exit(EXIT_FAILURE);
	}

	char tmp[1] = { 0 };
	char *p, *start, *field_name = NULL;
	abook_field *custom_field = NULL;
	int field_num;
	p = start = s;

	while(*p) {
		if(*p == '{') {
		  start = ++p;

		  if(! *start) goto cannotparse;
		  p = strchr(start, '}');
		  if(! p) goto cannotparse;
		  strcat(fmt_string, "%s");
		  field_name = strndup(start, (size_t)(p-start));
		  *ft = find_field_enum(field_name);
		  if(*ft == -1) {
			  // It's either invalid, either a custom field
			  custom_field = find_declared_field(field_name);
			  if (! custom_field) {
				  fprintf(stderr, _("parse_custom_format: invalid placeholder: {%s}\n"), field_name);
				  exit(EXIT_FAILURE);
			  } else {
				  find_field_number(field_name, &field_num);
				  *ft = CUSTOM_FIELD_START_INDEX + field_num;
			  }
		  }

		  ft++;
		  start = ++p;
		}

		else if(*p == '\\') {
			++p;
			if(! *p) tmp[0] = '\\'; // last char is a '\' ?
			else if(*p == 'n') *tmp = '\n';
			else if(*p == 't') *tmp = '\t';
			else if(*p == 'r') *tmp = '\r';
			else if(*p == 'v') *tmp = '\v';
			else if(*p == 'b') *tmp = '\b';
			else if(*p == 'a') *tmp = '\a';
			else *tmp = *p;
			strncat(fmt_string, tmp, 1);
			start = ++p;
		}

		// if no '\' following: quick mode using strchr/strncat
		else if(! strchr(start, '\\')) {
		  p = strchr(start, '{');
		  if(p) { // copy until the next placeholder
		    strncat(fmt_string, start, (size_t)(p-start));
		    start = p;
		  }
		  else { // copy till the end
		    strncat( fmt_string,
			     start,
			     FORMAT_STRING_LEN - strlen(fmt_string) - 1 );
		    break;
		  }
		}

		// otherwise character by character
		else {
			strncat(fmt_string, p, 1);
			start = ++p;
		}
	}

	*ft = ITEM_FIELDS;
	return;

 cannotparse:
	fprintf(stderr, _("%s: invalid format, index %ld\n"), __FUNCTION__, (start - s));
	free(ft);
	exit(EXIT_FAILURE);
}

static int
custom_export_item(FILE *out, int item, char *s, int *ft);


// stores the format string generated from --outformatstr {custom_format}
// (when "custom" output format is requested)
// overrides default value of custom_format set by from abook.c
extern char custom_format[FORMAT_STRING_LEN];
char parsed_custom_format[FORMAT_STRING_LEN];
int custom_format_fields[FORMAT_STRING_MAX_FIELDS + 1] = {0};

/* wrapper for custom_export_item:
   1) avoid messing with extern pointer
   2) adds \n
   3) follow the prototype needed for an abook_output_item_filter entry */
void
custom_print_item(FILE *out, int item)
{

  if(custom_export_item(out, item, parsed_custom_format, custom_format_fields) == 0)
    fprintf(out, "\n");
}

static int
custom_export_item(FILE *out, int item, char *fmt, int *ft)
{
  char *p, *q = 0;

  // if the first character is '!':
  // we first check that all fields exist before continuing
  if(*fmt == '!') {
    int *ftp = ft;
    while(*ft != ITEM_FIELDS) {
      if(! db_fget(item, *ft) )
	return 1;
      ft++;
    }
    ft = ftp;
    fmt++;
  }

  while (*fmt) {
    if(!strncmp(fmt, "%s", 2)) {
       if(*ft >= CUSTOM_FIELD_START_INDEX) {
	       fprintf(out, "%s", safe_str(db_fget_byid(item, *ft - CUSTOM_FIELD_START_INDEX)));
       } else {
	       fprintf(out, "%s", safe_str(db_fget(item, *ft)));
       }

      ft++;
      fmt+=2;
    } else if (*ft == ITEM_FIELDS) {
      fprintf(out, "%s", fmt);
      return 0;
    } else {
      p = strchr(fmt, '%');
      if(*p) {
	q = strndup(fmt, (size_t)(p-fmt));
	fprintf(out, "%s", q);
	free(q);
	fmt = p;
      }
      else {
	fprintf(out, "%s", fmt);
	return 0;
      }
    }
  }

  return 0;
}

static int
custom_export_database(FILE *out, struct db_enumerator e)
{
	int ft[FORMAT_STRING_MAX_FIELDS + 1];

	parse_custom_format(custom_format, (char*)&parsed_custom_format, ft);
	db_enumerate_items(e) {
	  if(custom_export_item(out, e.item, (char*)&parsed_custom_format, ft) == 0)
	    fprintf(out, "\n");
	}
	return 0;
}

/*
 * end of custom export filter
 */
