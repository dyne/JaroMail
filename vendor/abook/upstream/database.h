#ifndef _DATABASE_H
#define _DATABASE_H

#define MAX_LIST_ITEMS		9
#define MAX_EMAIL_LEN		80
#define MAX_EMAILSTR_LEN	(MAX_LIST_ITEMS * (MAX_EMAIL_LEN + 1) + 1)
#define MAX_FIELD_LEN		81

enum field_types {
	NAME = 0, /* important */
	EMAIL,
	ADDRESS,
        ADDRESS2,
	CITY,
	STATE,
	ZIP,
	COUNTRY,
	PHONE,
	WORKPHONE,
	FAX,
	MOBILEPHONE,
	NICK,
	URL,
	NOTES,
	ANNIVERSARY,
	GROUPS,
	ITEM_FIELDS /* this is the last */
};

typedef struct {
	char *key;
	char *name;
	int type;
} abook_field;

typedef struct abook_field_list_t {
	abook_field *field;
	struct abook_field_list_t *next;
} abook_field_list;

typedef char **list_item;

enum {
	FIELD_STRING = 1,
	FIELD_EMAILS,
	FIELD_LIST,
	FIELD_DATE,
};

enum {
	ENUM_ALL,
	ENUM_SELECTED
};

struct db_enumerator {
	int item;
	int mode; /* warning: read only */
};


/*
 * Field operations
 */
inline int field_id(int i);
abook_field *find_standard_field(char *key, int do_declare);
abook_field *real_find_field(char *key, abook_field_list *list, int *nb);
#define find_field(key, list)		real_find_field(key, list, NULL)
#define find_field_number(key, pt_nb)	real_find_field(key, NULL, pt_nb)
#define find_declared_field(key)	find_field(key,NULL)
void get_field_info(int i, char **key, char **name, int *type);
void add_field(abook_field_list **list, abook_field *f);
char *declare_new_field(char *key, char *name, char *type, int accept_standard);
abook_field_list *list_custom_fields();
void init_standard_fields();

/*
 * Various database operations
 */
void prepare_database_internals();
int parse_database(FILE *in);
int load_database(char *filename);
int write_database(FILE *out, struct db_enumerator e);
int save_database(int force_save);
void remove_selected_items();
void merge_selected_items();
void remove_duplicates();
void sort_surname();
void sort_by_field(char *field);
void close_database();
int add_item2database(list_item item);
char *get_surname(char *s);
int find_item(char *str, int start, int search_fields[]);
int is_selected(int item);
int is_valid_item(int item);
int last_item();
int db_n_items();

int real_db_enumerate_items(struct db_enumerator e);
struct db_enumerator init_db_enumerator(int mode);
#define db_enumerate_items(e) \
	while( -1 != (e.item = real_db_enumerate_items(e)))

/*
 * item manipulation
 */
list_item item_create();
void item_empty(list_item item);
void item_free(list_item *item);
void item_copy(list_item dest, list_item src);
void item_duplicate(list_item dest, list_item src);
void item_merge(list_item dest, list_item src);

int item_fput(list_item item, int i, char *val);
char *item_fget(list_item item, int i);

/*
 * database field read
 */
char *real_db_field_get(int item, int i, int std);
#define db_fget(item, i)		real_db_field_get(item, i, 1)
#define db_fget_byid(item, i)		real_db_field_get(item, i, 0)
#define db_name_get(item)		db_fget(item, NAME)
char *db_email_get(int item); /* memory has to be freed by the caller */

/*
 * database field write
 */
int real_db_field_put(int item, int i, int std, char *val);
#define db_fput(item, i, val) \
			real_db_field_put(item, i, 1, val)
#define db_fput_byid(item, i, val) \
			real_db_field_put(item, i, 0, val)

/*
 * database item read
 */
list_item db_item_get(int i);


#endif /* _DATABASE_H */

