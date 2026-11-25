#ifndef JARO_ABOOK_SQLITE_EDITOR_H
#define JARO_ABOOK_SQLITE_EDITOR_H
#include "misc.h"
extern const char *jaro_editor_profile, *jaro_editor_list;
const char *jaro_editor_diagnostic(void);
int jaro_editor_load(void);
/* Returns one after commit, zero for no changes, or minus one on failure. */
int jaro_editor_save(void);
int jaro_editor_failed_item(void);
const char *jaro_editor_failed_field(void);
int jaro_editor_books(abook_list **books);
int jaro_editor_switch_book(const char *name);
int jaro_editor_matches(int item, const char *key);
#endif
