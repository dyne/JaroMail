#ifndef _HELP_H
#define _HELP_H

#ifdef HAVE_CONFIG_H
#	include "config.h"
#endif

/* TODO gettext: handle key and description separately? */
static char *mainhelp[] = {

N_("	?		help\n"),
N_("	q		quit\n"),
N_("	Q		quit without saving\n"),
N_("	P		quit and output selected item(s) to stderr\n"),
N_("	^L		refresh screen\n"),
"\n",
N_("	arrows / j,k	scroll list\n"),
N_("	enter		view/edit item\n"),
N_("	a		add item\n"),
N_("	r / del		remove selected items\n"),
N_("	M		merge selected items (into top one)\n"),
N_("	D		duplicate item\n"),
N_("	U		remove duplicates\n"),
"\n",
N_("	space		select item\n"),
N_("	+		select all\n"),
N_("	-		unselect all\n"),
N_("	*		invert selection\n"),
"\n",
N_("	w		write database to disk\n"),
N_("	l		read database from disk\n"),
N_("	C		clear whole database\n"),
N_("	i		import database\n"),
N_("	e		export database\n"),
N_("	p		print database\n"),
N_("	o		open database\n"),
"\n",
N_("	s		sort database\n"),
N_("	S		\"surname sort\"\n"),
N_("	F		sort by field (defined in configuration file)\n"),
"\n",
N_("	/		search\n"),
N_("	\\		search next occurrence\n"),
"\n",
N_("	A		move current item up\n"),
N_("	Z		move current item down\n"),
"\n",
N_("	m		send mail with mutt\n"),
N_("	v		view URL with web browser\n"),
NULL

};

static char *editorhelp[] = {

"\n",
N_("	arrows/h,l		change tab\n"),
"\n",
N_("	q			quit to main screen\n"),
"\n",
N_("	1 - 5 A - Z		edit fields\n"),
"\n",
N_("	k or <			previous item\n"),
N_("	j or >			next item\n"),
"\n",
N_("	r			roll e-mail addresses up\n"),
N_("	ESC-r			roll e-mail addresses down\n"),
"\n",
N_("	u			undo\n"),
"\n",
N_("	m			send mail with mutt\n"),
N_("	v			view url with web browser\n"),
"\n",
NULL

};

#endif
