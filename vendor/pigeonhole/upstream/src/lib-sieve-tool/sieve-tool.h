#ifndef SIEVE_TOOL_H
#define SIEVE_TOOL_H

#include "sieve-common.h"

/*
 * Types
 */

typedef const char *
(*sieve_tool_setting_callback_t)(struct sieve_instance *svinst,
				 void *context, const char *identifier);

/*
 * Global variables
 */

extern struct sieve_tool *sieve_tool;

/*
 * Initialization
 */

struct sieve_tool *
sieve_tool_init(const char *name, int *argc, char **argv[],
		const char *getopt_str, bool no_config);
struct sieve_tool *sieve_tool_init_fuzzer(const char *name);

int sieve_tool_getopt(struct sieve_tool *tool);

struct sieve_instance *
sieve_tool_init_finish(struct sieve_tool *tool, bool init_mailstore,
		       bool preserve_root);

void sieve_tool_deinit(struct sieve_tool **_tool);

/*
 * Mail environment
 */

void sieve_tool_init_mail_user(struct sieve_tool *tool);

struct mail *
sieve_tool_open_file_as_mail(struct sieve_tool *tool, const char *path);
struct mail *
sieve_tool_open_data_as_mail(struct sieve_tool *tool, string_t *mail_data);

/*
 * Accessors
 */

const char *sieve_tool_get_username(struct sieve_tool *tool);
const char *sieve_tool_get_homedir(struct sieve_tool *tool);
struct mail_user *sieve_tool_get_mail_user(struct sieve_tool *tool);
struct mail_user *sieve_tool_get_mail_raw_user(struct sieve_tool *tool);
struct mail_storage_service_ctx *
sieve_tool_get_mail_storage_service(struct sieve_tool *tool);

/*
 * Configuration
 */

void sieve_tool_set_homedir(struct sieve_tool *tool, const char *homedir);
void sieve_tool_set_setting_callback(struct sieve_tool *tool,
				     sieve_tool_setting_callback_t callback,
				     void *context);

/*
 * Commonly needed functionality
 */

void sieve_tool_get_envelope_data(struct sieve_message_data *msgdata,
				  struct mail *mail,
				  const struct smtp_address *sender,
				  const struct smtp_address *rcpt_orig,
				  const struct smtp_address *rcpt_final);

/*
 * File I/O
 */

struct ostream *sieve_tool_open_output_stream(const char *filename);

/*
 * Sieve script handling
 */

struct sieve_binary *
sieve_tool_script_compile(struct sieve_tool *tool, const char *location);
struct sieve_binary *
sieve_tool_script_open(struct sieve_tool *tool, const char *location);
void sieve_tool_dump_binary_to(struct sieve_binary *sbin,
			       const char *filename, bool hexdump);

/*
 * Command line option parsing
 */

void sieve_tool_parse_trace_option(struct sieve_trace_config *tr_config,
				   const char *tr_option);

#endif
