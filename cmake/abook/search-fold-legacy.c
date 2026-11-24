#include "search-fold.h"
#include "xmalloc.h"

#include <string.h>

char *
abook_search_key(const char *text)
{
	char *key = abook_search_key_alloc(text, strlen(text), xmalloc);
	if (!key)
		abort();
	return key;
}
