/* Copyright (c) Pigeonhole authors, see top-level COPYING file */

#include "lib.h"
#include "lib-signals.h"
#include "ioloop.h"
#include "env-util.h"
#include "str.h"
#include "ostream.h"
#include "array.h"
#include "mail-namespace.h"
#include "mail-storage.h"
#include "master-service.h"
#include "master-service-settings.h"
#include "mail-storage-service.h"

#include "sieve.h"
#include "sieve-binary.h"
#include "sieve-extensions.h"

#include "sieve-tool.h"

#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <pwd.h>
#include <sysexits.h>


/*
 * Configuration
 */

#define DEFAULT_SENDMAIL_PATH "/usr/lib/sendmail"

/*
 * Print help
 */

static void print_help(void)
{
	printf(
"Usage: sieve-test [-a <orig-recipient-address] [-c <config-file>]\n"
"                  [-C] [-D] [-d <dump-filename>] [-e]\n"
"                  [-f <envelope-sender>] [-l <mail-location>]\n"
"                  [-m <default-mailbox>] [-P <plugin>]\n"
"                  [-r <recipient-address>] [-s <script-file>]\n"
"                  [-t <trace-file>] [-T <trace-option>] [-x <extensions>]\n"
"                  <script-file> <mail-file>\n"
	);
}

/*
 * Dummy SMTP session
 */

static void *
sieve_smtp_start(const struct sieve_script_env *senv ATTR_UNUSED,
		 const struct smtp_address *mail_from)
{
	struct ostream *output;

	i_info("sending message from <%s>:", smtp_address_encode(mail_from));

	output = o_stream_create_fd(STDOUT_FILENO, (size_t)-1);
	o_stream_set_no_error_handling(output, TRUE);
	return output;
}

static void
sieve_smtp_add_rcpt(const struct sieve_script_env *senv ATTR_UNUSED,
		    void *handle ATTR_UNUSED,
		    const struct smtp_address *rcpt_to)
{
	printf("\nRECIPIENT: %s\n", smtp_address_encode(rcpt_to));
}

static struct ostream *
sieve_smtp_send(const struct sieve_script_env *senv ATTR_UNUSED, void *handle)
{
	printf("START MESSAGE:\n");

	return (struct ostream *)handle;
}

static void
sieve_smtp_abort(const struct sieve_script_env *senv ATTR_UNUSED, void *handle)
{
	struct ostream *output = (struct ostream *)handle;

	printf("#### ABORT MESSAGE ####\n\n");
	o_stream_unref(&output);
}

static int
sieve_smtp_finish(const struct sieve_script_env *senv ATTR_UNUSED, void *handle,
		  const char **error_r ATTR_UNUSED)
{
	struct ostream *output = (struct ostream *)handle;

	printf("END MESSAGE\n\n");
	o_stream_unref(&output);
	return 1;
}

/*
 * Dummy duplicate check implementation
 */

static void *
duplicate_transaction_begin(const struct sieve_script_env *senv ATTR_UNUSED)
{
	return NULL;
}

static void duplicate_transaction_commit(void **_dup_trans ATTR_UNUSED)
{
}

static void duplicate_transaction_rollback(void **_dup_trans ATTR_UNUSED)
{
}

static int
duplicate_check(void *_dup_trans ATTR_UNUSED,
		const struct sieve_script_env *senv,
		const void *id ATTR_UNUSED, size_t id_size ATTR_UNUSED)
{
	i_info("checked duplicate for user %s.\n", senv->user->username);
	return 0;
}

static void
duplicate_mark(void *_dup_trans ATTR_UNUSED,
	       const struct sieve_script_env *senv,
	       const void *id ATTR_UNUSED, size_t id_size ATTR_UNUSED,
	       time_t time ATTR_UNUSED)
{
	i_info("marked duplicate for user %s.\n", senv->user->username);
}

/*
 * Result logging
 */

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

/*
 * Tool implementation
 */

int main(int argc, char **argv)
{
	struct sieve_instance *svinst;
	ARRAY_TYPE (const_string) scriptfiles;
	const char *scriptfile, *mailbox, *dumpfile, *tracefile, *mailfile,
		*errstr;
	struct smtp_address *rcpt_to, *final_rcpt_to, *mail_from;
	struct sieve_trace_config trace_config;
	struct mail *mail;
	struct sieve_binary *main_sbin, *sbin = NULL;
	struct sieve_message_data msgdata;
	struct sieve_script_env scriptenv;
	struct sieve_exec_status estatus;
	enum sieve_execute_flags exflags = SIEVE_EXECUTE_FLAG_LOG_RESULT;
	struct sieve_error_handler *ehandler;
	struct ostream *teststream = NULL;
	struct sieve_trace_log *trace_log = NULL;
	bool force_compile = FALSE, execute = FALSE;
	int exit_status = EXIT_SUCCESS;
	int ret, c;

	sieve_tool = sieve_tool_init("sieve-test", &argc, &argv,
				     "r:a:f:m:d:s:eCt:T:u:", FALSE);

	ehandler = NULL;
	t_array_init(&scriptfiles, 16);

	/* Parse arguments */
	mailbox = dumpfile = tracefile = NULL;
	mail_from = final_rcpt_to = rcpt_to = NULL;
	i_zero(&trace_config);
	trace_config.level = SIEVE_TRLVL_ACTIONS;
	while ((c = sieve_tool_getopt(sieve_tool)) > 0) {
		switch (c) {
		case 'r':
			/* final recipient address */
			if (smtp_address_parse_mailbox(
				pool_datastack_create(), optarg,
				SMTP_ADDRESS_PARSE_FLAG_ALLOW_LOCALPART,
				&final_rcpt_to, &errstr) < 0)
				i_fatal("Invalid -r parameter: %s", errstr);
			break;
		case 'a':
			/* original recipient address */
			if (smtp_address_parse_mailbox(
				pool_datastack_create(), optarg,
				SMTP_ADDRESS_PARSE_FLAG_ALLOW_LOCALPART,
				&rcpt_to, &errstr) < 0)
				i_fatal("Invalid -a parameter: %s", errstr);
			break;
		case 'f':
			/* envelope sender address */
			if (smtp_address_parse_mailbox(
				pool_datastack_create(), optarg,
				0, &mail_from, &errstr) < 0)
				i_fatal("Invalid -f parameter: %s", errstr);
			break;
		case 'm':
			/* default mailbox (keep box) */
			mailbox = optarg;
			break;
		case 't':
			/* trace file */
			tracefile = optarg;
			break;
			/* trace options */
		case 'T':
			sieve_tool_parse_trace_option(&trace_config, optarg);
			break;
		case 'd':
			/* dump file */
			dumpfile = optarg;
			break;
		case 's':
			/* scriptfile executed before main script */
			{
				const char *file;

				file = t_strdup(optarg);
				array_append(&scriptfiles, &file, 1);
			}
			break;
			/* execution mode */
		case 'e':
			execute = TRUE;
			break;
			/* force script compile */
		case 'C':
			force_compile = TRUE;
			break;
		default:
			/* unrecognized option */
			print_help();
			i_fatal_status(EX_USAGE, "Unknown argument: %c", c);
			break;
		}
	}

	if (optind < argc)
		scriptfile = argv[optind++];
	else {
		print_help();
		i_fatal_status(EX_USAGE, "Missing <script-file> argument");
	}

	if (optind < argc)
		mailfile = argv[optind++];
	else {
		print_help();
		i_fatal_status(EX_USAGE, "Missing <mail-file> argument");
	}

	if (optind != argc) {
		print_help();
		i_fatal_status(EX_USAGE, "Unknown argument: %s", argv[optind]);
	}

	/* Finish tool initialization */
	svinst = sieve_tool_init_finish(sieve_tool, TRUE, FALSE);

	/* Enable debug extension */
	sieve_enable_debug_extension(svinst);

	/* Create error handler */
	ehandler = sieve_stderr_ehandler_create(svinst, 0);
	sieve_error_handler_accept_infolog(ehandler, TRUE);
	sieve_error_handler_accept_debuglog(ehandler, svinst->debug);

	/* Compile main sieve script */
	if (force_compile) {
		main_sbin = sieve_tool_script_compile(sieve_tool, scriptfile);
		if (main_sbin != NULL)
			(void)sieve_save(main_sbin, TRUE, NULL);
	} else {
		main_sbin = sieve_tool_script_open(sieve_tool, scriptfile);
	}

	if (main_sbin == NULL) {
		exit_status = EXIT_FAILURE;
	} else {
		/* Dump script */
		sieve_tool_dump_binary_to(main_sbin, dumpfile, FALSE);

		/* Initialize raw mail object */
		mail = sieve_tool_open_file_as_mail(sieve_tool, mailfile);

		if (mailbox == NULL)
			mailbox = "INBOX";

		/* Collect necessary message data */
		i_zero(&msgdata);
		msgdata.mail = mail;
		msgdata.auth_user = sieve_tool_get_username(sieve_tool);
		(void)mail_get_message_id(mail, &msgdata.id);

		sieve_tool_get_envelope_data(&msgdata, mail,
					     mail_from, rcpt_to, final_rcpt_to);

		/* Create streams for test and trace output */

		if (!execute) {
			teststream = o_stream_create_fd(1, 0);
			o_stream_set_no_error_handling(teststream, TRUE);
		}

		if (tracefile != NULL) {
			(void)sieve_trace_log_create(
				svinst, (strcmp(tracefile, "-") == 0 ?
					 NULL : tracefile), &trace_log);
		}

		/* Compose script environment */
		if (sieve_script_env_init(
			&scriptenv, sieve_tool_get_mail_user(sieve_tool),
			&errstr) < 0) {
			i_fatal("Failed to initialize script execution: %s",
				errstr);
		}

		scriptenv.default_mailbox = mailbox;
		scriptenv.smtp_start = sieve_smtp_start;
		scriptenv.smtp_add_rcpt = sieve_smtp_add_rcpt;
		scriptenv.smtp_send = sieve_smtp_send;
		scriptenv.smtp_abort = sieve_smtp_abort;
		scriptenv.smtp_finish = sieve_smtp_finish;
		scriptenv.duplicate_transaction_begin =
			duplicate_transaction_begin;
		scriptenv.duplicate_transaction_commit =
			duplicate_transaction_commit;
		scriptenv.duplicate_transaction_rollback =
			duplicate_transaction_rollback;
		scriptenv.duplicate_mark = duplicate_mark;
		scriptenv.duplicate_check = duplicate_check;
		scriptenv.result_amend_log_message = result_amend_log_message;
		scriptenv.trace_log = trace_log;
		scriptenv.trace_config = trace_config;
		scriptenv.script_context = &msgdata;

		i_zero(&estatus);
		scriptenv.exec_status = &estatus;

		/* Run the test */
		ret = 1;
		if (array_count(&scriptfiles) == 0) {
			/* Single script */
			sbin = main_sbin;
			main_sbin = NULL;

			/* Execute/Test script */
			if (execute) {
				ret = sieve_execute(sbin, &msgdata, &scriptenv,
						    ehandler, ehandler,
						    exflags);
			} else {
				ret = sieve_test(sbin, &msgdata, &scriptenv,
						 ehandler, teststream, exflags);
			}
		} else {
			/* Multiple scripts */
			const char *const *sfiles;
			unsigned int i, count;
			struct sieve_multiscript *mscript;
			bool more = TRUE;

			if (execute)
				mscript = sieve_multiscript_start_execute(
					svinst, &msgdata, &scriptenv);
			else
				mscript = sieve_multiscript_start_test(
					svinst, &msgdata, &scriptenv,
					teststream);

			/* Execute scripts sequentially */
			sfiles = array_get(&scriptfiles, &count);
			for (i = 0; i < count && more; i++) {
				if (teststream != NULL) {
					o_stream_nsend_str(
						teststream,
						t_strdup_printf("\n## Executing script: %s\n",
								sfiles[i]));
				}

				/* Close previous script */
				if (sbin != NULL)
					sieve_close(&sbin);

				/* Compile sieve script */
				if (force_compile) {
					sbin = sieve_tool_script_compile(
						sieve_tool, sfiles[i]);
					if (sbin != NULL)
						(void)sieve_save(sbin, FALSE, NULL);
				} else {
					sbin = sieve_tool_script_open(
						sieve_tool, sfiles[i]);
				}

				if (sbin == NULL) {
					ret = SIEVE_EXEC_FAILURE;
					break;
				}

				/* Execute/Test script */
				more = sieve_multiscript_run(
					mscript, sbin, ehandler, ehandler,
					exflags);
			}

			/* Execute/Test main script */
			if (more && ret > 0) {
				if (teststream != NULL) {
					o_stream_nsend_str(
						teststream,
						t_strdup_printf("## Executing script: %s\n",
								scriptfile));
				}

				/* Close previous script */
				if (sbin != NULL)
					sieve_close(&sbin);

				sbin = main_sbin;
				main_sbin = NULL;

				(void)sieve_multiscript_run(
					mscript, sbin, ehandler, ehandler,
					exflags);
			}

			ret = sieve_multiscript_finish(&mscript, ehandler,
						       exflags, ret);
		}

		/* Run */
		switch (ret) {
		case SIEVE_EXEC_OK:
			i_info("final result: success");
			break;
		case SIEVE_EXEC_RESOURCE_LIMIT:
			i_info("resource limit exceeded");
			exit_status = EXIT_FAILURE;
			break;
		case SIEVE_EXEC_BIN_CORRUPT:
			i_info("corrupt binary deleted.");
			i_unlink_if_exists(sieve_binary_path(sbin));
			/* fall through */
		case SIEVE_EXEC_FAILURE:
			i_info("final result: failed; "
			       "resolved with successful implicit keep");
			exit_status = EXIT_FAILURE;
			break;
		case SIEVE_EXEC_TEMP_FAILURE:
			i_info("final result: temporary failure");
			exit_status = EXIT_FAILURE;
			break;
		case SIEVE_EXEC_KEEP_FAILED:
			i_info("final result: utter failure");
			exit_status = EXIT_FAILURE;
			break;
		}

		if (teststream != NULL)
			o_stream_destroy(&teststream);
		if (trace_log != NULL)
			sieve_trace_log_free(&trace_log);

		/* Cleanup remaining binaries */
		if (sbin != NULL)
			sieve_close(&sbin);
		if (main_sbin != NULL)
			sieve_close(&main_sbin);
	}

	/* Cleanup error handler */
	sieve_error_handler_unref(&ehandler);

	sieve_tool_deinit(&sieve_tool);

	return exit_status;
}
