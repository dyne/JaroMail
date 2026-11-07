
/*
 * $Id$
 *
 * by JH <jheinonen@users.sourceforge.net>
 *
 * Copyright (C) Jaakko Heinonen
 *
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <assert.h>
#include "options.h"
#include "abook.h"
#include "gettext.h"
#include "misc.h"
#include "views.h"
#include "xmalloc.h"

#ifndef FALSE
#	define FALSE	0
#endif
#ifndef TRUE
#	define TRUE	1
#endif

#define UL	(unsigned long)

/*
 * option types
 */

enum opt_type {
	OT_BOOL,
	OT_STR,
	OT_INT
};

struct option {
	char *option;
	enum opt_type type;
	unsigned int data;
	unsigned long init;
};

static struct option abook_vars[] = {
	{ "autosave", OT_BOOL, BOOL_AUTOSAVE, TRUE },

	{ "show_all_emails", OT_BOOL, BOOL_SHOW_ALL_EMAILS, TRUE },
	{ "index_format", OT_STR, STR_INDEX_FORMAT, UL " {name:22} {email:40} {phone:12|workphone|mobile}" },
	{ "mutt_command", OT_STR, STR_MUTT_COMMAND, UL "mutt" },
	{ "mutt_return_all_emails", OT_BOOL, BOOL_MUTT_RETURN_ALL_EMAILS,
		TRUE },

	{ "print_command", OT_STR, STR_PRINT_COMMAND, UL "lpr" },

	{ "www_command", OT_STR, STR_WWW_COMMAND, UL "lynx" },

	{ "address_style", OT_STR, STR_ADDRESS_STYLE, UL "eu" },

	{ "use_ascii_only", OT_BOOL, BOOL_USE_ASCII_ONLY, FALSE },

	{ "add_email_prevent_duplicates", OT_BOOL, BOOL_ADD_EMAIL_PREVENT_DUPLICATES, FALSE },
	{ "preserve_fields", OT_STR, STR_PRESERVE_FIELDS, UL "standard" },
	{ "sort_field", OT_STR, STR_SORT_FIELD, UL "nick" },
	{ "show_cursor", OT_BOOL, BOOL_SHOW_CURSOR, FALSE },
	{ "use_mouse", OT_BOOL, BOOL_USE_MOUSE, FALSE },
	{ "scroll_speed", OT_INT, INT_SCROLL_SPEED, UL 2 },
	{ "use_colors", OT_BOOL, BOOL_USE_COLORS, FALSE },
	{ "color_header_fg", OT_STR, STR_COLOR_HEADER_FG, UL "blue" },
	{ "color_header_fg", OT_STR, STR_COLOR_HEADER_FG, UL "blue" },
	{ "color_header_bg", OT_STR, STR_COLOR_HEADER_BG, UL "red" },
	{ "color_footer_fg", OT_STR, STR_COLOR_FOOTER_FG, UL "red" },
	{ "color_footer_bg", OT_STR, STR_COLOR_FOOTER_BG, UL "default" },
	{ "color_list_even_fg", OT_STR, STR_COLOR_LIST_EVEN_FG, UL "yellow" },
	{ "color_list_even_bg", OT_STR, STR_COLOR_LIST_EVEN_BG, UL "default" },
	{ "color_list_odd_fg", OT_STR, STR_COLOR_LIST_ODD_FG, UL "default" },
	{ "color_list_odd_bg", OT_STR, STR_COLOR_LIST_ODD_BG, UL "default" },
	{ "color_list_header_fg", OT_STR, STR_COLOR_LIST_HEADER_FG, UL "white" },
	{ "color_list_header_bg", OT_STR, STR_COLOR_LIST_HEADER_BG, UL "blue" },
	{ "color_list_highlight_fg", OT_STR, STR_COLOR_LIST_HIGHLIGHT_FG, UL "black" },
	{ "color_list_highlight_bg", OT_STR, STR_COLOR_LIST_HIGHLIGHT_BG, UL "green" },
	{ "color_tab_border_fg", OT_STR, STR_COLOR_TAB_BORDER_FG, UL "cyan" },
	{ "color_tab_border_bg", OT_STR, STR_COLOR_TAB_BORDER_BG, UL "default" },
	{ "color_tab_label_fg", OT_STR, STR_COLOR_TAB_LABEL_FG, UL "magenta" },
	{ "color_tab_label_bg", OT_STR, STR_COLOR_TAB_LABEL_BG, UL "default" },
	{ "color_field_name_fg", OT_STR, STR_COLOR_FIELD_NAME_FG, UL "yellow" },
	{ "color_field_name_bg", OT_STR, STR_COLOR_FIELD_NAME_BG, UL "default" },
	{ "color_field_value_fg", OT_STR, STR_COLOR_FIELD_VALUE_FG, UL "green" },
	{ "color_field_value_bg", OT_STR, STR_COLOR_FIELD_VALUE_BG, UL "default" },
	{ NULL }
};

static unsigned char bool_opts[BOOL_MAX];
static int int_opts[INT_MAXIMUM];
static char *str_opts[STR_MAX];

static void
set_int(enum int_opts opt, int value)
{
	assert(opt >= 0 && opt < INT_MAXIMUM);

	int_opts[opt] = value;
}

static void
set_bool(enum bool_opts opt, bool value)
{
	assert(opt >= 0 && opt < BOOL_MAX);

	bool_opts[opt] = value;
}

static void
set_str(enum str_opts opt, char *value)
{
	assert(opt >= 0 && opt < STR_MAX);

	if(str_opts[opt])
		free(str_opts[opt]);

	str_opts[opt] = xstrdup(value);
}

int
opt_get_int(enum int_opts opt)
{
	assert(opt >= 0 && opt < INT_MAXIMUM);

	return int_opts[opt];
}

bool
opt_get_bool(enum bool_opts opt)
{
	assert(opt >= 0 && opt < BOOL_MAX);

	return bool_opts[opt];
}

char *
opt_get_str(enum str_opts opt)
{
	assert(opt >= 0 && opt < STR_MAX);

	return str_opts[opt];
}

static void
restore_default(struct option *p)
{
	switch(p -> type) {
		case OT_BOOL:
			set_bool(p -> data, (bool)p -> init);
			break;
		case OT_INT:
			set_int(p -> data, (int)p -> init);
			break;
		case OT_STR:
			if(p -> init)
				set_str(p -> data, (char *) p -> init);
			break;
		default:
			assert(0);
	}
}

void
init_opts()
{
	int i;

	for(i = 0; abook_vars[i].option; i++)
		restore_default(&abook_vars[i]);
}

void
free_opts()
{
	int i;

	/*
	 * only strings need to be freed
	 */
	for(i = 0; i < STR_MAX; i++) {
		free(str_opts[i]);
		str_opts[i] = NULL;
	}
}

/*
 * file parsing
 */

typedef struct {
	char *data, *ptr;
} buffer;

static void
opt_line_remove_comments(char *p)
{
	bool in_quote = FALSE;
	bool escape = FALSE;

	assert(p != NULL);

	for(; *p; p++) {
		switch(*p) {
			case '\"':
				if(!escape)
					in_quote = !in_quote;
				break;
			case '\\':
				escape = TRUE;
				break;
			case '#':
				if(!in_quote) {
					*p = 0;
					return;
				}
			default:
				escape = FALSE;
		}
	}
}

/* After calling,
 * - b->data points to the found token, or NULL is end of parsing
 * - b->ptr  points to the begining of next token
 *
 * If the TOKEN_ALLOC option is used, the original string is not mangled
 * and memory is allocated for the token.
 */
static char *
get_token(buffer *b, int options)
{
	char quote = 0, c;
	char *end = NULL;

	assert(b);

	SKIPWS(b->ptr);
	if(*b->ptr && strchr("\"'", *b->ptr))
		quote = *(b->ptr++);
	b->data = b->ptr;

	while(1) {
		if(!(c = *b->ptr)) {
			end = b->ptr;
			break;
		}

		if(!quote && (
				ISSPACE(c) ||
				((options & TOKEN_EQUAL) && (c == '=')) ||
				((options & TOKEN_COMMA) && (c == ',')))
				) {
			end = b->ptr;
			break;
		} else if(c == quote) {
			quote = 0;
			end = b->ptr++;
			break;
		}

		b->ptr++;
	}

	if(quote)
		return _("quote mismatch");

	if(options & (TOKEN_EQUAL | TOKEN_COMMA))
		SKIPWS(b->ptr); /* whitespaces can precede the sign */

	if((options & TOKEN_EQUAL) && (*b->ptr != '='))
		return _("no assignment character found");

	if((options & TOKEN_COMMA) && *b->ptr && (*b->ptr != ','))
		return _("error in comma separated list");

	if(b->ptr == b->data) {
		b->data = NULL;
		return NULL; /* no error, just end of parsing */
	}

	if(options & TOKEN_ALLOC) /* freeing is the caller's responsibility */
		b->data = xstrndup(b->data, end - b->data);
	else
		*end = 0;

	b->ptr++; /* advance to next token */
	SKIPWS(b->ptr);

	return NULL;
}

static const char *
opt_set_set_option(char *p, struct option *opt)
{
	int len;

	assert(p);

	strtrim(p);
	len = strlen(p);

	if(*p == '\"' && p[len - 1] == '\"') {
		if(len < 3)
			return _("invalid value");
		p[len - 1] = 0;
		p++;
	}

	switch(opt -> type) {
		case OT_STR:
			set_str(opt -> data, p);
			break;
		case OT_INT:
			set_int(opt -> data, safe_atoi(p));
			break;
		case OT_BOOL:
			if(!strcasecmp(p, "true") || !strcasecmp(p, "on"))
				set_bool(opt -> data, TRUE);
			else if(!strcasecmp(p, "false") ||
					!strcasecmp(p, "off"))
				set_bool(opt -> data, FALSE);
			else
				return _("invalid value");
			break;
		default:
			assert(0);
	}

	return NULL;
}

static const char *
opt_set_option(char *var, char *p)
{
	int i;

	assert(var);
	assert(p);

	for(i = 0; abook_vars[i].option; i++)
		if(!strcmp(abook_vars[i].option, var))
			return opt_set_set_option(p, &abook_vars[i]);

	return _("unknown option");
}

static int
check_options()
{
	char *str;
	int err = 0;

	str = opt_get_str(STR_PRESERVE_FIELDS);
	if(strcasecmp(str, "all") && strcasecmp(str, "none") &&
			strcasecmp(str, "standard")) {
		fprintf(stderr, _("valid values for the 'preserve_fields' "
					"option are 'all', 'standard' "
					"(default), and 'none'\n"));
		restore_default(&abook_vars[STR_PRESERVE_FIELDS]);
		err++;
	}
	str = opt_get_str(STR_ADDRESS_STYLE);
	if(strcasecmp(str, "eu") && strcasecmp(str, "uk") &&
			strcasecmp(str, "us")) {
		fprintf(stderr, _("valid values for the 'address_style' "
					"option are 'eu' (default), 'uk', "
					"and 'us'\n"));
		restore_default(&abook_vars[STR_ADDRESS_STYLE]);
		err++;
	}

	return err;
}

/*
 * syntax: set <option> = <value>
 */
static const char *
opt_parse_set(buffer *b)
{
	char *var, *err;

	if((err = get_token(b, TOKEN_EQUAL)))
		return err;

	if((var = b->data) == NULL)
		return _("invalid value assignment");

	return opt_set_option(var, b->ptr);
}

static const char *
opt_parse_customfield(buffer *b)
{
	return _("customfield: obsolete command - please use the "
			"'field' and 'view' commands instead");
}

#include "views.h" /* needed for add_field_to_view */

/*
 * syntax: view <tab name> = <field1> [ , <field2>, ... ]
 */
static const char *
opt_parse_view(buffer *b)
{
	char *err, *view;

	if((err = get_token(b, TOKEN_EQUAL)))
		return err;

	if((view = b->data) == NULL)
		return _("no view name provided");

	while(1) {
		if((err = get_token(b, TOKEN_COMMA)))
			return err;

		if(b->data == NULL)
			break;

		if((err = add_field_to_view(view, b->data)))
			return err;
	}

	return NULL;
}

#include "database.h" /* needed for declare_new_field */

/*
 * syntax: field <identifier> = <human readable name> [ , <type> ]
 */
static const char *
opt_parse_field(buffer *b)
{
	char *err, *field, *name;

	if((err = get_token(b, TOKEN_EQUAL)))
		return err;

	if((field = b->data) == NULL)
		return _("no field identifier provided");

	if((err = get_token(b, TOKEN_COMMA)))
		return err;

	if((name = b->data) == NULL)
		return _("no field name provided");

	if((err = declare_new_field(field,
					name,
					b->ptr,
					0 /* reject "standard" fields */)))
		return err;

	return NULL;
}

#include "remap.h" /* needed for add_key_remap */

/*
 * syntax: remap <replacement> = <original>
 */
static const char *
opt_parse_mapping(buffer *b)
{
	char *err, *replacement;

	if((err = get_token(b, TOKEN_EQUAL)))
		return err;

	if((replacement = b->data) == NULL)
		return _("no replacement provided");

	return add_key_remap(replacement, b->ptr);
}

static struct {
	char *token;
	const char * (*func) (buffer *line);
} opt_parsers[] = {
	{ "set", opt_parse_set },
	{ "customfield", opt_parse_customfield }, /* obsolete */
	{ "view", opt_parse_view },
	{ "field", opt_parse_field },
	{ "remap", opt_parse_mapping },
	{ NULL }
};

static bool
opt_parse_line(char *line, int n, char *fn)
{
	int i;
	const char *err = NULL;
	char *token;
	buffer b;

	assert(line && fn);

	b.ptr = line;

	if((err = get_token(&b, 0))) {
		fprintf(stderr, "%s\n", err);
		return FALSE;
	}

	if(b.data == NULL)
		return FALSE;

	strtrim(b.data);
	strtrim(b.ptr);

	token = b.data;
	b.data = b.ptr = b.ptr;

	for(i = 0; opt_parsers[i].token; i++)
		if(!strcmp(opt_parsers[i].token, token)) {
			if(!(err = opt_parsers[i].func(&b)))
				return FALSE;
			break;
		}

	fprintf(stderr, _("%s: parse error at line %d: "), fn, n);
	if(err)
		fprintf(stderr, "%s\n", err);
	else
		fprintf(stderr, _("unknown token %s\n"), token);

	return TRUE;
}

int
load_opts(char *filename)
{
	FILE *in;
	char *line = NULL;
	int n;
	int err = 0;

	if((in = fopen(filename, "r")) == NULL)
		return -1;

	for(n = 1;!feof(in); n++) {
		line = getaline(in);

		if(feof(in))
			break;

		if(line && *line) {
			opt_line_remove_comments(line);
			if(*line)
				err += opt_parse_line(line, n, filename) ? 1:0;
		}

		free(line);
		line = NULL;
	}

	free(line);

	/* post-initialization */
	err += check_options();
	if(!strcasecmp(opt_get_str(STR_PRESERVE_FIELDS), "standard"))
		init_standard_fields();

	return err;
}

