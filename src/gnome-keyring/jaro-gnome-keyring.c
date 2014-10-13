/* Jaro Mail gnome keyring handler

   Originally from some Gnome example code, slightly modified

   Copyright (C) 2012 Denis Roio <jaromil@dyne.org>

   * This source code is free software; you can redistribute it and/or
   * modify it under the terms of the GNU Public License as published 
   * by the Free Software Foundation; either version 3 of the License,
   * or (at your option) any later version.
   *
   * This source code is distributed in the hope that it will be useful,
   * but WITHOUT ANY WARRANTY; without even the implied warranty of
   * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
   * Please refer to the GNU Public License for more details.
   *
   * You should have received a copy of the GNU Public License along with
   * this source code; if not, write to:
   * Free Software Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
   
*/


#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <glib.h>
#include <glib/gprintf.h>
#include <gnome-keyring.h>

static GnomeKeyringPasswordSchema jaro_schema = {
    GNOME_KEYRING_ITEM_GENERIC_SECRET,
    {
        { "protocol", GNOME_KEYRING_ATTRIBUTE_TYPE_STRING },
        { "host", GNOME_KEYRING_ATTRIBUTE_TYPE_STRING },
        { "path", GNOME_KEYRING_ATTRIBUTE_TYPE_STRING },
        { "username", GNOME_KEYRING_ATTRIBUTE_TYPE_STRING },
        { NULL, 0 },
    }
};

typedef struct jaro_credential {
    gchar *protocol;
    gchar *host;
    gchar *path;
    gchar *username;
    gchar *password;
} jaro_credential_t;

static void
error(const char *err, ...)
{
    char msg[4096];
    va_list params;

    va_start(params, err);
    vsnprintf(msg, sizeof(msg), err, params);
    fprintf(stderr, "%s\n", msg);
    va_end(params);
}

static int
get_password(jaro_credential_t *cred)
{
    GnomeKeyringResult keyres;
    gchar *pass = NULL;
    
    keyres = gnome_keyring_find_password_sync(&jaro_schema,
					      &pass,
					      "protocol", cred->protocol,
					      "host", cred->host,
					      "path", cred->path,
					      "username", cred->username,
					      NULL);
    if (keyres != GNOME_KEYRING_RESULT_OK) {
	error("failed to get password: %s", gnome_keyring_result_to_message(keyres));
	return 1;
    }
    g_printf("%s\n", pass);
    gnome_keyring_free_password(pass);
    return 0;
}

static int
check_password(jaro_credential_t *cred)
{
    GnomeKeyringResult keyres;
    gchar *pass = NULL;
    
    keyres = gnome_keyring_find_password_sync(&jaro_schema,
					      &pass,
					      "protocol", cred->protocol,
					      "host", cred->host,
					      "path", cred->path,
					      "username", cred->username,
					      NULL);
    if (keyres != GNOME_KEYRING_RESULT_OK) {
	error("failed to check password: %s", gnome_keyring_result_to_message(keyres));
	return 1;
    }
    gnome_keyring_free_password(pass);
    return 0;
}

static int
store_password(jaro_credential_t *cred)
{
    gchar desc[1024];
    GnomeKeyringResult keyres;

    /* Only store complete credentials */
    if (!cred->protocol || !cred->host ||
       	!cred->username || !cred->password)
      return 1;

    g_snprintf(desc, sizeof(desc), "%s %s", cred->protocol, cred->host);
    keyres = gnome_keyring_store_password_sync(&jaro_schema,
					       GNOME_KEYRING_DEFAULT,
					       desc,
					       cred->password,
					       "protocol", cred->protocol,
					       "host", cred->host,
					       "path", cred->path,
					       "username", cred->username,
					       NULL);
    if (keyres != GNOME_KEYRING_RESULT_OK) {
	error("failed to store password: %s", gnome_keyring_result_to_message(keyres));
	return 1;
    }
    return 0;
}

static int
erase_password(jaro_credential_t *cred)
{
    GnomeKeyringResult keyres;

    keyres = gnome_keyring_delete_password_sync(&jaro_schema,
						"protocol", cred->protocol,
						"host", cred->host,
						"path", cred->path,
						"username", cred->username,
						NULL);
    if (keyres != GNOME_KEYRING_RESULT_OK) {
	error("failed to erase password: %s", gnome_keyring_result_to_message(keyres));
	return 1;
    }
    return 0;
}

static int
read_credential(jaro_credential_t *cred)
{
    char buf[1024];

    while (fgets(buf, sizeof(buf), stdin)) {
	char *v;

	if (strcmp(buf, "\n") == 0)
	    break;

	buf[strlen(buf) - 1] = '\0';
	v = strchr(buf, '=');
	if (!v) {
	    error("bad input: %s", buf);
	    return -1;
	}
	*v++ = '\0';

#define SET_CRED_ATTR(name) do { \
    if (strcmp(buf, #name) == 0) { \
	cred->name = g_strdup(v); \
    } \
} while (0)
	SET_CRED_ATTR(protocol);
	SET_CRED_ATTR(host);
	SET_CRED_ATTR(path);
	SET_CRED_ATTR(username);
	SET_CRED_ATTR(password);
    }
    return 0;
}

static void
clear_credential(jaro_credential_t *cred)
{
    if (cred->protocol) g_free(cred->protocol);
    if (cred->host) g_free(cred->host);
    if (cred->path) g_free(cred->path);
    if (cred->username) g_free(cred->username);
    if (cred->password) gnome_keyring_free_password(cred->password);
}

int
main(int argc, const char **argv)
{
    jaro_credential_t cred = {0};
    int res = 0;

    if (argc < 2) {
	error("Usage: jaro-gnome-keyring <get|check|store|erase>");
	error("input from stdin: newline separated parameter=value tuples");
	error("i.e: protocol, path, username, host, password (password on store)");
	return 1;
    }

    if (read_credential(&cred)) {
	clear_credential(&cred);
	return 1;
    }

    if (strcmp(argv[1], "get") == 0) {
      res = get_password(&cred);
    }
    if (strcmp(argv[1], "check") == 0) {
      res = check_password(&cred);
    }
    else if (strcmp(argv[1], "store") == 0) {
      res = store_password(&cred);
    }
    else if (strcmp(argv[1], "erase") == 0) {
      res = erase_password(&cred);
    }
    clear_credential(&cred);

    return res;
}
