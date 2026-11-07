/*
 * $Id$
 *
 * Common xmalloc memory allocation routines
 *
 * written by Jaakko Heinonen <jheinonen@users.sourceforge.net>
 */

/*
 * Copyright (c) 2005 Jaakko Heinonen
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer
 *    in this position and unchanged.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR(S) ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL THE AUTHOR(S) BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 * NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "gettext.h"
#include "xmalloc.h"

static void
xmalloc_default_error_handler(int err)
{
	fprintf(stderr, _("Memory allocation failure: %s\n"), strerror(err));
	exit(EXIT_FAILURE);
}

static void (*xmalloc_handle_error)(int err) = xmalloc_default_error_handler;

void
xmalloc_set_error_handler(void (*func)(int err))
{
	if(func)
		xmalloc_handle_error = func;
	else
		xmalloc_handle_error = xmalloc_default_error_handler;
}

void *
xmalloc(size_t size)
{
	void *p;

	if((p = malloc(size)) == NULL)
		(*xmalloc_handle_error)(errno);

	return p;
}

void *
xmalloc0(size_t size)
{
	void *p;

	p = xmalloc(size);
	if(p)
		memset(p, 0, size);

	return p;
}

static void *
_xmalloc_inc(size_t size, size_t inc, int zero)
{
	size_t total_size = size + inc;

	/*
	 * check if the calculation overflowed
	 */
	if(total_size < size) {
		(*xmalloc_handle_error)(EINVAL);
		return NULL;
	}

	return zero ? xmalloc0(total_size) : xmalloc(total_size);
}

void *
xmalloc_inc(size_t size, size_t inc)
{
	return _xmalloc_inc(size, inc, 0);
}

void *
xmalloc0_inc(size_t size, size_t inc)
{
	return _xmalloc_inc(size, inc, 1);
}

void *
xrealloc(void *ptr, size_t size)
{
	if((ptr = realloc(ptr, size)) == NULL)
		(*xmalloc_handle_error)(errno);

	return ptr;
}

void *
xrealloc_inc(void *ptr, size_t size, size_t inc)
{
	size_t total_size = size + inc;

	/*
	 * check if the calculation overflowed
	 */
	if(total_size < size) {
		(*xmalloc_handle_error)(EINVAL);
		return NULL;
	}

	if((ptr = realloc(ptr, total_size)) == NULL)
		(*xmalloc_handle_error)(errno);

	return ptr;
}

char *
xstrdup(const char *s)
{
	size_t len = strlen(s);
	void *new;

	new = xmalloc_inc(len, 1);
	if(new == NULL)
		return NULL;

	return (char *)memcpy(new, s, len + 1);
}

char *
xstrndup(const char *s, size_t len)
{
	char *new;
	size_t n = strlen(s);

	if(n > len)
		n = len;

	new = xmalloc_inc(n, 1);
	if(new == NULL)
		return NULL;

	memcpy(new, s, n);
	new[n] = '\0';

	return new;
}
