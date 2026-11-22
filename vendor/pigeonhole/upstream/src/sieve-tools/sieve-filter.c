/* Copyright (c) Pigeonhole authors, see top-level COPYING file */

#include "lib.h"
#include "lib-signals.h"
#include "ioloop.h"
#include "env-util.h"
#include "str.h"
#include "str-sanitize.h"
#include "ostream.h"
#include "array.h"
#include "mail-namespace.h"
#include "mail-storage.h"
#include "mail-search-build.h"

#include "sieve.h"
#include "sieve-extensions.h"
#include "sieve-binary.h"

#include "sieve-tool.h"

#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <pwd.h>
#include <sysexits.h>

/*
 * Print help
 */

static void print_help(void)
{
	printf(
"Usage: sieve-filter [-c <config-file>] [-C] [-D] [-e] [-m <default-mailbox>]\n"
"                    [-P <plugin>] [-q <output-mailbox>] [-Q <mail-command>]\n"
"                    [-s <script-file>] [-u <user>] [-v] [-W] [-x <extensions>]\n"
"                    <script-file> <source-mailbox> [<discard-action>]\n"
	);
}

enum sieve_filter_discard_action {
	SIEVE_FILTER_DACT_KEEP,   /* Keep discarded messages in source folder */
	SIEVE_FILTER_DACT_MOVE,   /* Move discarded messages to Trash folder */
	SIEVE_FILTER_DACT_DELETE, /* Flag discarded messages as \DELETED */
	SIEVE_FILTER_DACT_EXPUNGE /* Expunge discarded messages */
};

struct sieve_filter_data {
	enum sieve_filter_discard_action discard_action;
	struct mailbox *move_mailbox;

	struct sieve_script_env *senv;
	struct sieve_binary *main_sbin;
	struct sieve_error_handler *ehandler;

	bool execute:1;
	bool source_write:1;
	bool default_move:1;
};

struct sieve_filter_context {
	const struct sieve_filter_data *data;

	struct mailbox_transaction_context *move_trans;

	struct ostream *teststream;
};

static const char *
result_amend_log_message(const struct sieve_script_env *senv,
			 enum log_type log_type, const char *message)
{
	const struct sieve_message_data *msgdata = senv->script_context;
	string_t *str;

	if (log_type == LOG_TYPE_DEBUG)
		return message;

	str = t_str_new(256);
	str_printfa(str, "msgid=%s", (msgdata->id == NULL ?
				      "unspecified" : msgdata->id));
	str_append(str, ": ");
	str_append(str, message);

	return str_c(str);
}

static int filter_message(struct sieve_filter_context *sfctx, struct mail *mail)
{
	struct sieve_error_handler *ehandler = sfctx->data->ehandler;
	enum sieve_execute_flags exflags = SIEVE_EXECUTE_FLAG_LOG_RESULT;
	struct sieve_script_env *senv = sfctx->data->senv;
	struct sieve_exec_status estatus;
	struct sieve_binary *sbin;
	struct sieve_message_data msgdata;
	bool execute = sfctx->data->execute;
	bool source_write = sfctx->data->source_write;
	const char *subject, *date;
	uoff_t size = 0;
	int ret;

	/* Initialize execution status */
	i_zero(&estatus);
	senv->exec_status = &estatus;

	/* Collect necessary message data */
	i_zero(&msgdata);
	msgdata.mail = mail;
	msgdata.auth_user = senv->user->username;
	(void)mail_get_message_id(mail, &msgdata.id);
	senv->script_context = &msgdata;

	sieve_tool_get_envelope_data(&msgdata, mail, NULL, NULL, NULL);

	if (mail_get_virtual_size(mail, &size) < 0) {
		senv->exec_status = NULL;
		senv->script_context = NULL;

		if (mail->expunged)
			return 1;

		sieve_error(ehandler, NULL, "failed to obtain message size; "
			    "skipping this message (id=%s)",
			    (msgdata.id == NULL ? "none" : msgdata.id));
		return 0;
	}

	if (mail_get_first_header(mail, "date", &date) <= 0)
		date = "";
	if (mail_get_first_header(mail, "subject", &subject) <= 0)
		subject = "";

	/* Single script */
	sbin = sfctx->data->main_sbin;

	/* Execute script */
	if (execute) {
		sieve_info(ehandler, NULL,
			   "filtering: [%s; %"PRIuUOFF_T" bytes] '%s'",
			   date, size, str_sanitize(subject, 40));

		ret = sieve_execute(sbin, &msgdata, senv, ehandler, ehandler,
				    exflags);
	} else {
		o_stream_nsend_str(
			sfctx->teststream,
			t_strdup_printf(">> Filtering message:\n\n"
					"  ID:      %s\n"
					"  Date:    %s\n"
					"  Size:    %"PRIuUOFF_T" bytes\n"
					"  Subject: %s\n",
					(msgdata.id == NULL ?
					 "none" : msgdata.id), date, size,
					str_sanitize(subject, 40)));

		ret = sieve_test(sbin, &msgdata, senv, ehandler,
				 sfctx->teststream, exflags);
	}

	senv->exec_status = NULL;
	senv->script_context = NULL;


	/* Handle message in source folder */
	if (ret > 0) {
		struct mailbox *move_box = sfctx->data->move_mailbox;
		enum sieve_filter_discard_action discard_action =
			sfctx->data->discard_action;

		if (!source_write) {
			/* READ-ONLY; Do nothing */
		} else if (estatus.keep_original) {
			/* Explicitly 'stored' in source box; just keep it there */
			sieve_info(ehandler, NULL,
				   "message kept in source mailbox");
		} else if (estatus.message_saved) {
			sieve_info(
				ehandler, NULL,
				"message expunged from source mailbox upon successful move");

			if (execute)
				mail_expunge(mail);
		} else {
			switch (discard_action) {
			/* Leave it there */
			case SIEVE_FILTER_DACT_KEEP:
				sieve_info(ehandler, NULL,
					   "message left in source mailbox");
				break;
			/* Move message to indicated folder */
			case SIEVE_FILTER_DACT_MOVE:
				sieve_info(
					ehandler, NULL,
					"message in source mailbox moved to mailbox '%s'",
					mailbox_get_name(move_box));

				if (execute && move_box != NULL) {
					struct mailbox_transaction_context *t =
						sfctx->move_trans;
					struct mail_save_context *save_ctx;

					save_ctx = mailbox_save_alloc(t);

					if (mailbox_copy(&save_ctx, mail) < 0) {
						enum mail_error error;
						const char *errstr;

						errstr = mail_storage_get_last_error(
							mailbox_get_storage(move_box), &error);

						sieve_error(
							ehandler, NULL,
							"failed to move message to mailbox %s: %s",
							mailbox_get_name(move_box), errstr);
						return -1;
					}
					mail_expunge(mail);
				}
				break;
			/* Flag message as \DELETED */
			case SIEVE_FILTER_DACT_DELETE:
				sieve_info(
					ehandler, NULL,
					"message flagged as deleted in source mailbox");
				if (execute)
					mail_update_flags(mail, MODIFY_ADD, MAIL_DELETED);
				break;
			/* Expunge the message immediately */
			case SIEVE_FILTER_DACT_EXPUNGE:
				sieve_info(ehandler, NULL,
					   "message expunged from source mailbox");
				if (execute)
					mail_expunge(mail);
				break;
			/* Unknown */
			default:
				i_unreached();
			}
		}
	}

	switch (ret) {
	case SIEVE_EXEC_OK:
		break;
	case SIEVE_EXEC_RESOURCE_LIMIT:
		sieve_error(ehandler, NULL, "sieve resource limit exceeded");
		return -1;
	case SIEVE_EXEC_BIN_CORRUPT:
		sieve_error(ehandler, NULL, "sieve script binary is corrupt");
		return -1;
	case SIEVE_EXEC_FAILURE:
	case SIEVE_EXEC_TEMP_FAILURE:
		if (source_write && execute && sfctx->data->default_move &&
		    !estatus.keep_original && estatus.message_saved) {
			/* The implicit keep action moved message to default
			   mailbox, so the source message still needs to be
			   expunged */
			sieve_error(
				ehandler, NULL,
				"sieve script execution failed for this message; "
				"message moved to default mailbox");
			mail_expunge(mail);
			return 0;
		}
		/* Fall through */
	case SIEVE_EXEC_KEEP_FAILED:
		sieve_error(
			ehandler, NULL,
			"sieve script execution failed for this message; "
			"message left in source mailbox");
		return 0;
	}

	return 1;
}

/* FIXME: introduce this into Dovecot */
static void
mail_search_build_add_flags(struct mail_search_args *args,
			    enum mail_flags flags, bool not)
{
	struct mail_search_arg *arg;

	arg = p_new(args->pool, struct mail_search_arg, 1);
	arg->type = SEARCH_FLAGS;
	arg->value.flags = flags;
	arg->match_not = not;

	arg->next = args->args;
	args->args = arg;
}

static int
filter_mailbox(const struct sieve_filter_data *sfdata, struct mailbox *src_box)
{
	struct sieve_filter_context sfctx;
	struct mailbox *move_box = sfdata->move_mailbox;
	struct sieve_error_handler *ehandler = sfdata->ehandler;
	struct mail_search_args *search_args;
	struct mailbox_transaction_context *t;
	struct mail_search_context *search_ctx;
	struct mail *mail;
	int ret = 1;

	/* Sync source mailbox */

	if (mailbox_sync(src_box, MAILBOX_SYNC_FLAG_FULL_READ) < 0) {
		sieve_error(ehandler, NULL, "failed to sync source mailbox");
		return -1;
	}

	/* Initialize */

	i_zero(&sfctx);
	sfctx.data = sfdata;

	/* Create test stream */
	if (!sfdata->execute) {
		sfctx.teststream = o_stream_create_fd(1, 0);
		o_stream_set_no_error_handling(sfctx.teststream, TRUE);
	}

	/* Start move mailbox transaction */

	if (move_box != NULL) {
		sfctx.move_trans = mailbox_transaction_begin(
			move_box, MAILBOX_TRANSACTION_FLAG_EXTERNAL,
			"sieve_filter_data move_box");
	}

	/* Search non-deleted messages in the source folder */

	search_args = mail_search_build_init();
	mail_search_build_add_flags(search_args, MAIL_DELETED, TRUE);

	t = mailbox_transaction_begin(src_box, 0,
				      "sieve_filter_data src_box");
	search_ctx = mailbox_search_init(t, search_args, NULL, 0, NULL);
	mail_search_args_unref(&search_args);

	/* Iterate through all requested messages */

	while (ret >= 0 && mailbox_search_next(search_ctx, &mail))
		ret = filter_message(&sfctx, mail);

	/* Cleanup */

	if (mailbox_search_deinit(&search_ctx) < 0)
		ret = -1;

	if (sfctx.move_trans != NULL) {
		if (mailbox_transaction_commit(&sfctx.move_trans) < 0)
			ret = -1;
	}

	if (mailbox_transaction_commit(&t) < 0)
		ret = -1;

	if (sfctx.teststream != NULL)
		o_stream_destroy(&sfctx.teststream);

	if (ret < 0) return ret;

	/* Sync mailbox */

	if (sfdata->execute) {
		if (mailbox_sync(src_box, 0) < 0) {
			sieve_error(ehandler, NULL,
				    "failed to sync source mailbox");
			return -1;
		}
	}

	return ret;
}

/*
 * Tool implementation
 */

int main(int argc, char **argv)
{
	struct sieve_instance *svinst;
	ARRAY_TYPE(const_string) scriptfiles;
	const char *scriptfile,	*src_mailbox, *dst_mailbox, *move_mailbox;
	struct sieve_filter_data sfdata;
	enum sieve_filter_discard_action discard_action =
		SIEVE_FILTER_DACT_KEEP;
	struct mail_user *mail_user;
	struct sieve_binary *main_sbin;
	struct sieve_script_env scriptenv;
	struct sieve_error_handler *ehandler;
	bool force_compile, execute, source_write, verbose, default_move;
	struct mail_namespace *ns;
	struct mailbox *src_box = NULL, *move_box = NULL;
	enum mailbox_flags open_flags = MAILBOX_FLAG_IGNORE_ACLS;
	enum mail_error error;
	const char *errstr;
	int c;

	sieve_tool = sieve_tool_init("sieve-filter", &argc, &argv,
				     "m:s:u:q:Q:CevW", FALSE);

	t_array_init(&scriptfiles, 16);

	/* Parse arguments */
	dst_mailbox = move_mailbox = NULL;
	force_compile = execute = source_write = default_move = FALSE;
	verbose = FALSE;
	while ((c = sieve_tool_getopt(sieve_tool)) > 0) {
		switch (c) {
		case 'm':
			/* default mailbox (keep box) */
			dst_mailbox = optarg;
			break;
		case 's':
			/* scriptfile executed before main script */
			{
				const char *file;

				file = t_strdup(optarg);
				array_append(&scriptfiles, &file, 1);

				/* FIXME: */
				i_fatal_status(
					EX_USAGE,
					"The -s argument is currently NOT IMPLEMENTED");
			}
			break;
		case 'q':
			i_fatal_status(
				EX_USAGE,
				"The -q argument is currently NOT IMPLEMENTED");
			break;
		case 'Q':
			i_fatal_status(
				EX_USAGE,
				"The -Q argument is currently NOT IMPLEMENTED");
			break;
		case 'e':
			/* execution mode */
			execute = TRUE;
			break;
		case 'C':
			/* force script compile */
			force_compile = TRUE;
			break;
		case 'W':
			/* enable source mailbox write */
			source_write = TRUE;
			break;
		case 'v':
			/* enable verbose output */
			verbose = TRUE;
			break;
		default:
			/* unrecognized option */
			print_help();
			i_fatal_status(EX_USAGE, "Unknown argument: %c", c);
			break;
		}
	}

	/* Script file argument */
	if (optind < argc)
		scriptfile = t_strdup(argv[optind++]);
	else {
		print_help();
		i_fatal_status(EX_USAGE, "Missing <script-file> argument");
	}

	/* Source mailbox argument */
	if (optind < argc)
		src_mailbox = t_strdup(argv[optind++]);
	else {
		print_help();
		i_fatal_status(EX_USAGE, "Missing <source-mailbox> argument");
	}

	/* Source action argument */
	if (optind < argc) {
		const char *srcact = argv[optind++];

		if (strcmp(srcact, "keep") == 0) {
			discard_action = SIEVE_FILTER_DACT_KEEP;
		} else if (strcmp(srcact, "move") == 0) {
			discard_action = SIEVE_FILTER_DACT_MOVE;
			if (optind < argc)
				move_mailbox = t_strdup(argv[optind++]);
			else {
				print_help();
				i_fatal_status(
					EX_USAGE,
					"Invalid <discard-action> argument: "
					"the 'move' action requires mailbox argument");
			}
		} else if (strcmp(srcact, "delete") == 0) {
			discard_action = SIEVE_FILTER_DACT_DELETE;
		} else if (strcmp(srcact, "expunge") == 0) {
			discard_action = SIEVE_FILTER_DACT_EXPUNGE;
		} else {
			print_help();
			i_fatal_status(EX_USAGE,
				       "Invalid <discard-action> argument");
		}
	}

	if (optind != argc) {
		print_help();
		i_fatal_status(EX_USAGE, "Unknown argument: %s", argv[optind]);
	}

	if (dst_mailbox == NULL) {
		dst_mailbox = src_mailbox;
	} else {
		/* The (implicit) keep action will move the message */
		default_move = TRUE;
	}

	/* Finish tool initialization */
	svinst = sieve_tool_init_finish(sieve_tool, TRUE, FALSE);

	/* Enable debug extension */
	sieve_enable_debug_extension(svinst);

	/* Create error handler */
	ehandler = sieve_stderr_ehandler_create(svinst, 0);
	sieve_error_handler_accept_infolog(ehandler, verbose);
	sieve_error_handler_accept_debuglog(ehandler, svinst->debug);

	/* Compile main sieve script */
	if (force_compile) {
		main_sbin = sieve_tool_script_compile(sieve_tool, scriptfile);
		if (main_sbin != NULL)
			(void)sieve_save(main_sbin, TRUE, NULL);
	} else {
		main_sbin = sieve_tool_script_open(sieve_tool, scriptfile);
	}

	/* Initialize mail user */
	mail_user = sieve_tool_get_mail_user(sieve_tool);

	/* Open the source mailbox */

	ns = mail_namespace_find(mail_user->namespaces, src_mailbox);
	if (ns == NULL) {
		i_fatal("Unknown namespace for source mailbox '%s'",
			src_mailbox);
	}

	if (!source_write || !execute)
		open_flags |= MAILBOX_FLAG_READONLY;

	src_box = mailbox_alloc(ns->list, src_mailbox, open_flags);
	if (mailbox_open(src_box) < 0) {
		i_fatal("Couldn't open source mailbox '%s': %s",
			src_mailbox, mailbox_get_last_internal_error(src_box, &error));
	}

	/* Open move box if necessary */

	if (execute && discard_action == SIEVE_FILTER_DACT_MOVE &&
	    move_mailbox != NULL) {
		ns = mail_namespace_find(mail_user->namespaces, move_mailbox);
		if (ns == NULL)
			i_fatal("Unknown namespace for mailbox '%s'",
				move_mailbox);

		move_box = mailbox_alloc(ns->list, move_mailbox, open_flags);
		if (mailbox_open(move_box) < 0) {
			i_fatal("Couldn't open mailbox '%s': %s",
				move_mailbox,
				mailbox_get_last_internal_error(move_box, &error));
		}

		if (mailbox_backends_equal(src_box, move_box))
			i_fatal("Source mailbox and mailbox for move action are identical.");
	}

	/* Compose script environment */
	if (sieve_script_env_init(&scriptenv, mail_user, &errstr) < 0)
		i_fatal("Failed to initialize script execution: %s", errstr);
	scriptenv.mailbox_autocreate = FALSE;
	scriptenv.default_mailbox = dst_mailbox;
	scriptenv.result_amend_log_message = result_amend_log_message;

	/* Compose filter context */
	i_zero(&sfdata);
	sfdata.senv = &scriptenv;
	sfdata.discard_action = discard_action;
	sfdata.move_mailbox = move_box;
	sfdata.main_sbin = main_sbin;
	sfdata.ehandler = ehandler;
	sfdata.execute = execute;
	sfdata.source_write = source_write;
	sfdata.default_move = default_move;

	/* Apply Sieve filter to all messages found */
	(void)filter_mailbox(&sfdata, src_box);

	/* Close the source mailbox */
	if (src_box != NULL)
		mailbox_free(&src_box);

	/* Close the move mailbox */
	if (move_box != NULL)
		mailbox_free(&move_box);

	/* Close the script binary */
	if (main_sbin != NULL)
		sieve_close(&main_sbin);

	/* Cleanup error handler */
	sieve_error_handler_unref(&ehandler);

	sieve_tool_deinit(&sieve_tool);

	return 0;
}
