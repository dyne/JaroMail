#ifndef JARO_ABOOK_SEARCH_FOLD_H
#define JARO_ABOOK_SEARCH_FOLD_H

#include <stddef.h>

/* Return an allocated UTF-8 search key, discarding invalid input bytes. */
char *abook_search_key(const char *text);
/* Length-aware variant. The returned key is allocated or NULL on failure. */
char *abook_search_key_bytes(const char *text, size_t length);
char *abook_search_key_alloc(const char *text, size_t length, void *(*allocate)(size_t));

#endif
