#ifndef _GETTEXT_H
#define _GETTEXT_H

const char *sgettext(const char *msgid); /* Strip context prefix */

# if ENABLE_NLS
#  include <libintl.h>
# else
#  define gettext(Msgid) ((const char *) (Msgid))
#  define dcgettext(Domainname, Msgid, Category) ((const char *) (Msgid))
#  define textdomain(Domainname) do {} while(0)
#  define bindtextdomain(Domainname, Dirname) do {} while(0)
# endif /* ENABLE_NLS */

# define _(String) gettext (String)
# define gettext_noop(String) String
# define N_(String) gettext_noop (String)
# define S_(String) sgettext (String)

#endif /* _GETTEXT_H */
