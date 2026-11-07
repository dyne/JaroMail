#ifndef _LDIF_H
#define _LDIF_H


/*
 * prototypes
 */

int		str_parse_line(char        *line, char        **type,
		char        **value, int         *vlen);
char		*str_getline( char **next );
void		put_type_and_value( char **out, char *t, char *val, int vlen );
char		*ldif_type_and_value( char *type, char *val, int vlen );


#endif
