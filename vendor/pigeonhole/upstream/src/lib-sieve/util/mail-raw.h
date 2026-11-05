#ifndef MAIL_RAW_H
#define MAIL_RAW_H

#include "lib.h"
#include "master-service.h"

struct mail_raw {
	pool_t pool;
	struct mail *mail;

	struct mailbox *box;
	struct mailbox_transaction_context *trans;
};

struct mail_user *mail_raw_user_create(struct mail_user *mail_user);

struct mail_raw *
mail_raw_open_stream(struct mail_user *ruser, struct istream *input);
struct mail_raw *
mail_raw_open_file(struct mail_user *ruser, const char *path);
struct mail_raw *
mail_raw_open_data(struct mail_user *ruser, string_t *mail_data);

void mail_raw_close(struct mail_raw **mailr);

#endif
