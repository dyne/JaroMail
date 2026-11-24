#ifndef JARO_ABOOK_SQLITE_EDITOR_H
#define JARO_ABOOK_SQLITE_EDITOR_H
extern const char *jaro_editor_profile, *jaro_editor_list;
const char *jaro_editor_diagnostic(void);
int jaro_editor_load(void);
int jaro_editor_save(void);
#endif
