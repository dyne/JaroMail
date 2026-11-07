#ifndef _XMALLOC_H
#define _XMALLOC_H

#include <stdlib.h> /* for size_t */

/*
 * avoid possible collision with readline xmalloc functions
 */

#define xmalloc		_xmalloc_xmalloc
#define xrealloc	_xmalloc_xrealloc

void		xmalloc_set_error_handler(void (*)(int));
void *		xmalloc(size_t);
void *		xmalloc0(size_t);
void *		xmalloc_inc(size_t, size_t);
void *		xmalloc0_inc(size_t, size_t);
void *		xrealloc(void *, size_t);
void *		xrealloc_inc(void *, size_t, size_t);
char *		xstrdup(const char *s);
char *		xstrndup(const char *s, size_t);

#define xfree(ptr)	do { free(ptr); ptr = NULL; } while(0)

#endif
