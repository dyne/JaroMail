/*
 * $Id$
 *
 * by Imran Haider <imran@tangledwire.org>
 *
 * Copyright (C) Imran Haider
 *
 */

#include "remap.h"
#include "gettext.h"
#include <stdio.h>

static char remap_table[127];

char *add_key_remap(char *replacement, char *original)
{
	char old_key = *original;
	int new_key = *replacement;

	if (old_key > 0 && new_key > 0) {
		remap_table[new_key] = old_key;
		return NULL;
	}

	return _("remap key out of range");
}

const char *get_remap_table()
{
	return remap_table;
}

void init_remap_table()
{
	int i;

	for (i=0; i<127; ++i)
		remap_table[i] = i;
}


