#ifdef HAVE_CONFIG_H
#	include "config.h"
#endif

#include <string.h>
#include "gettext.h"

const char *
sgettext(const char *msgid)
{
	const char *msgval = gettext(msgid);

	if(msgval == msgid)
		msgval = strrchr(msgid, '|') + 1;

	return msgval;
}

