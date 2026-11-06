#include "products.h"
#include "regex_qualification.h"
#include <string.h>

const char *jf_regex_catalogue(void) { return JF_REGEX_CATALOGUE; }
const char *jf_regex_qualification(void) { return JF_REGEX_QUALIFICATION; }

/* A concatenation language: no repetition, alternatives, captures, backrefs,
 * locale classes or engine-specific escapes. POSIX longest-match and PCRE2
 * first-match therefore have the same existence result. Preserve every byte. */
int jf_regex_supported(const char *pattern)
{
	const unsigned char *p=(const unsigned char *)pattern;
	if (*p=='^') p++;
	if (!*p || (*p=='$' && !p[1])) return 0;
	for (;*p;p++) {
		if (*p<32 || *p>126) return 0;
		if (*p=='$' && !p[1]) return 1;
		if (*p=='\\') {
			if (!p[1] || !strchr(".\\+?*(){}|^$[",p[1])) return 0;
			p++; continue;
		}
		if (*p=='[' && p[1]=='.' && p[2]==']') { p+=2; continue; }
		if (strchr("^$+?*(){}|[]",*p)) return 0;
	}
	return 1;
}
