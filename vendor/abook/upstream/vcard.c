
/*
 * Copyright 2012, Raphaël Droz <raphael.droz+floss@gmail.com>
 *
 * abook's wrapper for libvformat:
 * fits a vcard parsed by libvformat into a usable abook item list
 *
 * see:
 * libvformat's vf_iface.h
 * http://www.imc.org/pdi/vcard-21.txt
 * rfc 2426
 * rfc 2739
 */

#include <stdio.h>
#include <string.h>

#include "database.h"
#include "options.h" // bool
#include "misc.h" // abook_list_to_csv
#include "xmalloc.h"

#include "vcard.h"

int vcard_parse_file_libvformat(char *filename) {
  VF_OBJECT_T*  vfobj;
  if (!vf_read_file(&vfobj, filename)) {
    fprintf(stderr, "Could not read VCF file %s\n", filename);
    return 1;
  }

  // a libvformat property
  VF_PROP_T* prop;
  // property number (used for multivalued properties)
  int props = 0;
  // temporary values
  abook_list *multivalues = NULL;
  char *propval = 0;
  bool phone_found;

  do {
    list_item item = item_create();
    phone_found = false;
    /* Note: libvformat use va_args, we *must* cast the last
       NULL argument to (char*) for arch where
       sizeof(int) != sizeof(char *) */

    // fullname [ or struct-name [ or name ] ]
    if (vf_get_property(&prop, vfobj, VFGP_FIND, NULL, "FN", (char*)0))
      if ((propval = vf_get_prop_value_string(prop, 0)))
	item_fput(item, NAME, xstrdup(propval));

    if (!propval && vf_get_property(&prop, vfobj, VFGP_FIND, NULL, "N", (char*)0)) {
      // TODO: GIVENNAME, FAMILYNAME
      propval = vf_get_prop_value_string(prop, 0);
      if(propval)
	item_fput(item, NAME, xstrdup(propval));
    }

    if (!propval && vf_get_property(&prop, vfobj, VFGP_FIND, NULL, "NAME", (char*)0)) {
      propval = vf_get_prop_value_string(prop, 0);
      if(propval)
	item_fput(item, NAME, xstrdup(propval));
    }

    // email(s). (TODO: EMAIL;PREF: should be abook's first)
    if (vf_get_property(&prop, vfobj, VFGP_FIND, NULL, "EMAIL", (char*)0)) {
	    do {
		    props = 0;
		    while ((propval = vf_get_prop_value_string(prop, props++))) {
			    abook_list_append(&multivalues, propval);
		    }
	    } while (vf_get_next_property(&prop));
	    item_fput(item, EMAIL, abook_list_to_csv(multivalues));
	    abook_list_free(&multivalues);
    }

    // format for ADR:
    // PO Box, Extended Addr, Street, Locality, Region, Postal Code, Country
    if (vf_get_property(&prop, vfobj, VFGP_FIND, NULL, "ADR", (char*)0)) {
      props = 0;
      // PO Box: abook ignores
      vf_get_prop_value_string(prop, props++);

      // ext-address
      propval = vf_get_prop_value_string(prop, props++);
      if(propval) item_fput(item, ADDRESS2, xstrdup(propval));
      // address (street)
      propval = vf_get_prop_value_string(prop, props++);
      if(propval) item_fput(item, ADDRESS, xstrdup(propval));
      // locality (city)
      propval = vf_get_prop_value_string(prop, props++);
      if(propval) item_fput(item, CITY, xstrdup(propval));
      // region (state)
      propval = vf_get_prop_value_string(prop, props++);
      if(propval) item_fput(item, STATE, xstrdup(propval));
      // postal-code (zip)
      propval = vf_get_prop_value_string(prop, props++);
      if(propval) item_fput(item, ZIP, xstrdup(propval));
      // country
      propval = vf_get_prop_value_string(prop, props++);
      if(propval) item_fput(item, COUNTRY, xstrdup(propval));
    }

    // phone numbers
    // home
    if (vf_get_property(&prop, vfobj, VFGP_FIND, NULL, "TEL", "HOME", (char*)0) && (propval = vf_get_prop_value_string(prop, 0))) {
	    item_fput(item, PHONE, xstrdup(propval)); phone_found = true;
    }
    // workphone
    if (vf_get_property(&prop, vfobj, VFGP_FIND, NULL, "TEL", "WORK", (char*)0) && (propval = vf_get_prop_value_string(prop, 0))) {
	    item_fput(item, WORKPHONE, xstrdup(propval)); phone_found = true;
    }

    // fax
    if (vf_get_property(&prop, vfobj, VFGP_FIND, NULL, "TEL", "FAX", (char*)0) && (propval = vf_get_prop_value_string(prop, 0))) {
	    item_fput(item, FAX, xstrdup(propval)); phone_found = true;
    }

    // cellphone
    if (vf_get_property(&prop, vfobj, VFGP_FIND, NULL, "TEL", "CELL", (char*)0) && (propval = vf_get_prop_value_string(prop, 0))) {
	    item_fput(item, MOBILEPHONE, xstrdup(propval)); phone_found = true;
    }

    // or grab any other one as default
    if(! phone_found && vf_get_property(&prop, vfobj, VFGP_FIND, NULL, "TEL", (char*)0) && (propval = vf_get_prop_value_string(prop, 0))) {
	    item_fput(item, PHONE, xstrdup(propval));
    }

    // nick
    if (vf_get_property(&prop, vfobj, VFGP_FIND, NULL, "NICKNAME", (char*)0)) {
	    propval = vf_get_prop_value_string(prop, 0);
	    item_fput(item, NICK, xstrdup(propval));
    }

    // url
    if (vf_get_property(&prop, vfobj, VFGP_FIND, NULL, "URL", (char*)0)) {
      propval = vf_get_prop_value_string(prop, 0);
      item_fput(item, URL, xstrdup(propval));
    }

    // notes
    if (vf_get_property(&prop, vfobj, VFGP_FIND, NULL, "NOTE", (char*)0)) {
      propval = vf_get_prop_value_string(prop, 0);
      item_fput(item, NOTES, xstrdup(propval));
    }

    // anniversary
    if (vf_get_property(&prop, vfobj, VFGP_FIND, NULL, "BDAY", (char*)0)) {
      propval = vf_get_prop_value_string(prop, 0);
      item_fput(item, ANNIVERSARY, xstrdup(propval));
    }

    // (mutt) groups
    if (vf_get_property(&prop, vfobj, VFGP_FIND, NULL, "CATEGORIES", (char*)0)) {
	    do {
		    props = 0;
		    while ((propval = vf_get_prop_value_string(prop, props++))) {
			    abook_list_append(&multivalues, propval);
		    }
	    } while (vf_get_next_property(&prop));
	    item_fput(item, GROUPS, abook_list_to_csv(multivalues));
	    abook_list_free(&multivalues);
    }

    add_item2database(item);
    item_free(&item);
  } while (vf_get_next_object(&vfobj));

  return 0;
}
