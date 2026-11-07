#ifndef _MISC_H
#define _MISC_H

typedef struct abook_list_t {
	char *data;
	struct abook_list_t *next;
} abook_list;

enum rotate_dir {
	ROTATE_LEFT,
	ROTATE_RIGHT
};

char		*revstr(char *str);
char		*strupper(char *str);
char		*strlower(char *str);
char		*strtrim(char *);

int		is_number(char *s);

char		*strdup_printf(const char *format, ... );
char		*strconcat(const char *str, ...);

int		safe_strcmp(const char *s1, const char *s2);
int		safe_strcoll(const char *s1, const char *s2);

char		*my_getcwd();

char		*getaline(FILE *f);

int		strwidth(const char *s);
int		bytes2width(const char *s, int width);


void		abook_list_append(abook_list **list, char *str);
void		abook_list_free(abook_list **list);
char		*abook_list_to_csv(abook_list *list);
abook_list	*csv_to_abook_list(char *str);
void		abook_list_rotate(abook_list **list, enum rotate_dir dir);
void		abook_list_replace(abook_list **list, int index, char *str);
abook_list	*abook_list_get(abook_list *list, int index);
#define	abook_list_delete(list, index) abook_list_replace(list, index, NULL)


#ifdef HAVE_CONFIG_H
#	include "config.h"
#endif

#include <stdarg.h>

#ifndef HAVE_SNPRINTF
int snprintf (char *str, size_t count, const char *fmt, ...);
#endif
#ifndef HAVE_VSNPRINTF
int vsnprintf (char *str, size_t count, const char *fmt, va_list arg);
#endif
#ifndef HAVE_STRCASESTR
char *strcasestr(const char *, const char *);
#endif

#endif
