#ifndef JARO_ABOOK_SEARCH_FOLD_H
#define JARO_ABOOK_SEARCH_FOLD_H

/* Return an allocated UTF-8 search key, discarding invalid input bytes. */
char *abook_search_key(const char *text);

#endif
