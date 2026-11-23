/* Local Maildir adapter, derived from Pigeonhole 2.4.5 sieve-filter.c.
 * Copyright (c) Pigeonhole authors. See vendor/pigeonhole/upstream/COPYING. */
#define _GNU_SOURCE
#include "lib.h"
#include "array.h"
#include "istream.h"
#include "ostream.h"
#include "ostream-null.h"
#include "mail-namespace.h"
#include "mail-search-build.h"
#include "mail-storage.h"
#include "mail-storage-hooks.h"
#include "mail-storage-private.h"
#include "mailbox-list.h"
#include "message-header-parser.h"
#include "sieve.h"
#include "sieve-ast.h"
#include "sieve-actions.h"
#include "sieve-binary.h"
#include "sieve-code.h"
#include "sieve-dump.h"
#include "sieve-extensions.h"
#include "sieve-execute.h"
#include "sieve-generator.h"
#include "sieve-interpreter.h"
#include "sieve-result.h"
#include "sieve-script.h"
#include "sieve-tool.h"

#include <fcntl.h>
#include <signal.h>
#include <linux/landlock.h>
#include <linux/openat2.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

/* The local Dovecot save transaction receives the original Maildir basename. */
const char *jaro_sieve_filename;
static const char *source_name, *batch_path;
static int destination_fd = -1;
static volatile sig_atomic_t stopping;
static FILE *decisions;

struct decision {
	const char *mailbox, *location;
	unsigned int index;
	bool identified;
};

static void stop_batch(int signum)
{
	stopping = signum;
}

static void bind_claimed_mailbox(struct mailbox *box)
{
	if (!strcmp(mailbox_get_vname(box), source_name))
		box->_path = p_strdup(box->pool, batch_path);
}

static const struct mail_storage_hooks local_hooks = {
	.mailbox_allocated = bind_claimed_mailbox
};

/* Keep the storage driver's later pathname lookups under the pinned directory
 * inodes even if a mailbox component is replaced after preflight. */
static void confine_storage(bool preview)
{
	const uint64_t writes = LANDLOCK_ACCESS_FS_WRITE_FILE | LANDLOCK_ACCESS_FS_TRUNCATE |
		LANDLOCK_ACCESS_FS_REMOVE_FILE | LANDLOCK_ACCESS_FS_REMOVE_DIR |
		LANDLOCK_ACCESS_FS_MAKE_REG | LANDLOCK_ACCESS_FS_MAKE_DIR | LANDLOCK_ACCESS_FS_REFER;
	const uint64_t reads = LANDLOCK_ACCESS_FS_READ_FILE | LANDLOCK_ACCESS_FS_READ_DIR;
	struct landlock_ruleset_attr policy = { .handled_access_fs = reads | writes };
	int rules = syscall(SYS_landlock_create_ruleset, &policy, sizeof(policy), 0);
	if (rules < 0)
		i_fatal("Cannot confine Maildir access (requires Linux Landlock ABI 3): %m");
	const char *paths[] = {".", "incoming/mail", "folders"};
	for (unsigned int i = 0; i < N_ELEMENTS(paths); i++) {
		int fd = open(paths[i], O_RDONLY | O_DIRECTORY);
		struct landlock_path_beneath_attr entry = {
			.parent_fd = fd, .allowed_access = reads | (preview && i != 0 ? 0 : writes)
		};
		if (fd < 0 || syscall(SYS_landlock_add_rule, rules,
				      LANDLOCK_RULE_PATH_BENEATH, &entry, 0) < 0)
			i_fatal("Cannot bind Maildir scope %s: %m", paths[i]);
		i_close_fd(&fd);
	}
	if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) < 0 ||
	    syscall(SYS_landlock_restrict_self, rules, 0) < 0)
		i_fatal("Cannot activate Maildir scope: %m");
	i_close_fd(&rules);
}

/* Validate every literal path before the first message, including destinations
 * in constant-false branches. Only :create and literal fileinto are enabled. */
static void check_folder(int base, const char *name)
{
	struct mail_user *user = sieve_tool_get_mail_user(sieve_tool);
	struct mail_namespace *ns = mail_namespace_find_inbox(user->namespaces);
	struct mailbox *box = mailbox_alloc(ns->list, name, MAILBOX_FLAG_READONLY);
	if (mailbox_verify_create_name(box) < 0) {
		enum mail_error error;
		i_fatal("Invalid mailbox %s: %s", name, mailbox_get_last_internal_error(box, &error));
	}
	mailbox_free(&box);
	const char *const *parts = t_strsplit(name, "/");
	int dir = dup(base);
	if (dir < 0)
		i_fatal("dup failed: %m");
	if (*name == '\0')
		i_fatal("Empty mailbox name");
	for (unsigned int i = 0; parts[i] != NULL; i++) {
		const char *part = parts[i];
		if (!*part || !strcmp(part, ".") || !strcmp(part, "..") ||
		    !strcmp(part, "cur") || !strcmp(part, "new") ||
		    !strcmp(part, "tmp") || !strcmp(part, ".jaro-source") ||
		    *part == '~' || strpbrk(part, "\r\n") != NULL)
			i_fatal("Invalid destination mailbox: %s", name);
		if (dir < 0)
			continue; /* Remaining missing components may be created. */
		int next = openat(dir, part, O_DIRECTORY | O_NOFOLLOW | O_RDONLY);
		if (next < 0 && errno != ENOENT)
			i_fatal("Unsafe destination mailbox %s: %m", name);
		i_close_fd(&dir);
		dir = next;
	}
	if (dir >= 0) {
		const char *sub[] = {"cur", "new", "tmp"};
		for (unsigned int i = 0; i < N_ELEMENTS(sub); i++) {
			int fd = openat(dir, sub[i], O_DIRECTORY | O_NOFOLLOW | O_RDONLY);
			if (fd < 0 && errno != ENOENT)
				i_fatal("Invalid Maildir directory %s/%s: %m", name, sub[i]);
			if (fd >= 0)
				i_close_fd(&fd);
		}
		i_close_fd(&dir);
	}
}

/* Check the loaded instruction stream independently of the source AST.
 * Pigeonhole's operand readers advance over each tested expression. */
static void check_binary_operation(struct sieve_dumptime_env *env, int base)
{
	static const char *const allowed[] = {"JMP", "JMPTRUE", "JMPFALSE", "STOP", "KEEP",
		"ADDRESS", "HEADER", "EXISTS", "ENVELOPE", "MAILBOXEXISTS", "SIZE-OVER", "SIZE-UNDER", NULL};
	struct sieve_operation op;
	if (!sieve_operation_read(env->sblock, &env->offset, &op))
		i_fatal("Invalid operation in cached Sieve binary");
	env->oprtn = &op;
	env->cdumper = sieve_code_dumper_create(env);
	if (!strcmp(sieve_operation_mnemonic(&op), "FILEINTO")) {
		struct sieve_operand operand;
		string_t *name;
		if (sieve_action_opr_optional_dump(env, &env->offset, NULL) != 0 ||
		    !sieve_operand_read(env->sblock, &env->offset, NULL, &operand) ||
		    !sieve_operand_is_string_literal(&operand) ||
		    !sieve_binary_read_string(env->sblock, &env->offset, &name))
			i_fatal("Invalid fileinto in cached Sieve binary");
		check_folder(base, strcasecmp(str_c(name), "INBOX") ? str_c(name) : "INBOX");
	} else {
		if (!str_array_find(allowed, sieve_operation_mnemonic(&op)))
			i_fatal("Unsupported operation in cached Sieve binary: %s", sieve_operation_mnemonic(&op));
		if (op.def->dump != NULL && !op.def->dump(env, &env->offset))
			i_fatal("Invalid operands in cached Sieve binary");
	}
	sieve_code_dumper_free(&env->cdumper);
}

static void check_cached_binary(struct sieve_binary *binary, int base)
{
	static const char *const allowed[] = {"fileinto", "mailbox", "regex", "envelope", NULL};
	struct sieve_dumptime_env env = {
		.svinst = sieve_binary_svinst(binary), .sbin = binary,
		.sblock = sieve_binary_block_get(binary, SBIN_SYSBLOCK_MAIN_PROGRAM)
	};
	unsigned int debug, count;
	if (env.sblock == NULL || !sieve_binary_read_unsigned(env.sblock, &env.offset, &debug) ||
	    sieve_binary_block_get(binary, debug) == NULL ||
	    !sieve_binary_read_unsigned(env.sblock, &env.offset, &count))
		i_fatal("Invalid cached Sieve binary header");
	for (unsigned int i = 0; i < count; i++) {
		const struct sieve_extension *extension;
		unsigned int code = 0, deferred;
		if (!sieve_binary_read_extension(env.sblock, &env.offset, &code, &extension) ||
		    !sieve_binary_read_byte(env.sblock, &env.offset, &deferred) || extension == NULL ||
		    extension->def == NULL || !str_array_find(allowed, sieve_extension_name(extension)))
			i_fatal("Unsupported extension in cached Sieve binary");
	}
	env.stream = o_stream_create_null();
	while (env.offset < sieve_binary_block_get_size(env.sblock)) T_BEGIN {
		check_binary_operation(&env, base);
	} T_END;
	o_stream_destroy(&env.stream);
}

static void check_commands(const struct sieve_ast_node *block, int base)
{
	for (struct sieve_ast_node *cmd = sieve_ast_command_first(block);
	     cmd != NULL; cmd = sieve_ast_command_next(cmd)) {
		const char *id = cmd->identifier;
		if (!strcasecmp(id, "fileinto")) {
			struct sieve_ast_argument *arg = sieve_ast_argument_first(cmd);
			if (arg != NULL && arg->type == SAAT_TAG &&
			    !strcasecmp(sieve_ast_argument_tag(arg), "create"))
				arg = sieve_ast_argument_next(arg);
			if (arg == NULL || arg->type != SAAT_STRING || arg->next != NULL)
				i_fatal("line %u: expected fileinto [:create] \"mailbox\"", cmd->source_line);
			const char *name = sieve_ast_argument_strc(arg);
			check_folder(base, strcasecmp(name, "INBOX") == 0 ? "INBOX" : name);
		} else if (strcasecmp(id, "require") && strcasecmp(id, "if") &&
			   strcasecmp(id, "elsif") && strcasecmp(id, "else") &&
			   strcasecmp(id, "keep") && strcasecmp(id, "stop")) {
			i_fatal("line %u: unsupported delivery action '%s'", cmd->source_line, id);
		}
		check_commands(cmd, base);
	}
}

static struct sieve_binary *open_program(struct sieve_instance *svinst,
	struct sieve_error_handler *errors, bool cached)
{
	struct sieve_binary *binary = NULL;
	int fd = open("script", O_RDONLY | O_NOFOLLOW);
	if (fd < 0)
		i_fatal("Could not open pinned filter inputs: %m");
	struct istream *input = i_stream_create_fd_autoclose(&fd, IO_BLOCK_SIZE);
	struct sieve_script *script = sieve_data_script_create_from_input(
		svinst, SIEVE_SCRIPT_CAUSE_ANY, "Filters", input);
	i_stream_unref(&input);
	struct sieve_ast *ast = sieve_parse(script, errors, NULL);
	if (ast == NULL)
		i_fatal("Invalid Sieve source");
	check_commands(sieve_ast_root(ast), destination_fd);
	if (!sieve_validate(ast, errors, SIEVE_COMPILE_FLAG_NOGLOBAL, NULL))
		i_fatal("Invalid Sieve folder-delivery script");
	if (cached) {
		if (sieve_load(svinst, "binary", &binary, NULL) < 0)
			i_fatal("Cannot load pinned Sieve binary");
		check_cached_binary(binary, destination_fd);
	} else {
		struct sieve_generator *generator = sieve_generator_create(
			ast, errors, SIEVE_COMPILE_FLAG_NOGLOBAL);
		binary = sieve_generator_run(generator, NULL);
		sieve_generator_free(&generator);
		if (binary == NULL)
			i_fatal("Cannot generate Sieve executable");
	}
	sieve_ast_unref(&ast);
	sieve_script_unref(&script);
	return binary;
}

/* Inspect the actual result before executing it. Generated programs have one
 * fileinto per winner; the interpreter supplies both mailbox and source line. */
static int report_action(struct sieve_binary *binary, const struct sieve_message_data *msg,
	struct sieve_script_env *env, struct sieve_error_handler *errors, bool preview,
	struct decision *decision)
{
	pool_t pool = pool_alloconly_create("JaroMail Sieve result", 4096);
	struct sieve_execute_env execution;
	struct sieve_instance *svinst = sieve_binary_svinst(binary);
	sieve_execute_init(&execution, svinst, pool, msg, env, SIEVE_EXECUTE_FLAG_NOGLOBAL);
	struct sieve_result *result = sieve_result_create(svinst, pool, &execution);
	struct sieve_interpreter *interp = sieve_interpreter_create(binary, NULL, &execution, errors);
	int ret = SIEVE_EXEC_BIN_CORRUPT;
	if (interp != NULL) {
		ret = sieve_interpreter_run(interp, result);
		sieve_interpreter_free(&interp);
	}
	if (ret == SIEVE_EXEC_OK) {
		struct sieve_result_iterate_context *it = sieve_result_iterate_init(result);
		const struct sieve_action *action = sieve_result_iterate_next(it, NULL);
		if (action == NULL || action->def != &act_store || action->context == NULL ||
		    action->location == NULL || sieve_result_iterate_next(it, NULL) != NULL)
			ret = SIEVE_EXEC_FAILURE;
		else {
			const struct act_store_context *store = action->context;
			decision->mailbox = t_strdup(store->mailbox);
			decision->location = t_strdup(action->location);
		}
	}
	if (!preview && ret == SIEVE_EXEC_OK) {
		struct sieve_result_execution *actions = sieve_result_execution_create(result, pool);
		ret = sieve_result_execute(actions, ret, TRUE, errors, NULL);
		sieve_result_execution_destroy(&actions);
		sieve_execute_finish(&execution, ret);
	}
	sieve_result_unref(&result);
	sieve_execute_deinit(&execution);
	pool_unref(&pool);
	return ret;
}

static int filter_message(struct sieve_binary *binary, struct mail *mail,
	struct sieve_script_env *env, struct sieve_error_handler *errors,
	struct ostream *preview, struct decision *decision)
{
	struct istream *input;
	if (mail_get_stream(mail, NULL, NULL, &input) < 0)
		return -1;
	struct message_header_parser_ctx *parser = message_parse_header_init(input, NULL, 0);
	struct message_header_line *line;
	bool has_header = FALSE, valid = TRUE;
	while (message_parse_header_next(parser, &line) > 0 && !line->eoh) {
		if (!line->continued && line->name_len == 0)
			valid = FALSE;
		has_header = TRUE;
	}
	valid = valid && has_header && !message_parse_header_has_nuls(parser) && !input->stream_errno;
	message_parse_header_deinit(&parser);
	i_stream_seek(input, 0);
	if (!valid) {
		i_error("Malformed message '%s'; retained", jaro_sieve_filename);
		return -1;
	}
	struct sieve_message_data msg = { .mail = mail, .auth_user = env->user->username };
	struct sieve_exec_status status = { 0 };
	(void)mail_get_message_id(mail, &msg.id);
	sieve_tool_get_envelope_data(&msg, mail, NULL, NULL, NULL);
	env->exec_status = &status;
	int ret = decisions != NULL ? report_action(binary, &msg, env, errors, preview != NULL, decision) :
		preview == NULL ?
		sieve_execute(binary, &msg, env, errors, errors, SIEVE_EXECUTE_FLAG_NOGLOBAL) :
		sieve_test(binary, &msg, env, errors, preview, SIEVE_EXECUTE_FLAG_NOGLOBAL);
	env->exec_status = NULL;
	if (ret != SIEVE_EXEC_OK || status.store_failed) {
		i_error("Sieve failed for source UID %u (status %d); retained", mail->uid, ret);
		return -1;
	}
	return preview == NULL && status.message_saved && !status.keep_original ? 1 : 0;
}

struct source_entry {
	const char *name, *identity;
};
ARRAY_DEFINE_TYPE(source_entry, struct source_entry);

static bool source_unchanged(const char *path, const struct source_entry *entry, bool preview)
{
	struct stat st;
	if ((preview ? stat(path, &st) : lstat(path, &st)) < 0 || !S_ISREG(st.st_mode))
		return FALSE;
	const char *identity = t_strdup_printf("%"PRIuMAX" %"PRIuMAX" %"PRIdMAX" %"PRIdMAX" %ld\n",
		(uintmax_t)st.st_dev, (uintmax_t)st.st_ino, (intmax_t)st.st_size,
		(intmax_t)st.st_mtim.tv_sec, st.st_mtim.tv_nsec);
	return strcmp(identity, entry->identity) == 0;
}

/* The driver records the inode it published. Before expunging the sole
 * source, verify every target still names that file and contains its bytes. */
struct published_file {
	char *path;
	struct stat st;
};
static ARRAY(struct published_file) publications;
static int open_destination(const char *path, int flags)
{
	const struct open_how how = {
		.flags = flags | O_CLOEXEC,
		.resolve = RESOLVE_BENEATH | RESOLVE_NO_SYMLINKS
	};
	return syscall(SYS_openat2, destination_fd, path, &how, sizeof(how));
}

int jaro_sieve_publish(const char *mailbox, const char *temporary, const char *name);

int jaro_sieve_publish(const char *mailbox, const char *temporary, const char *name)
{
	/* Bind both ends of the rename to the mailbox chosen by fileinto. */
	int box = open_destination(mailbox, O_RDONLY | O_DIRECTORY);
	if (box < 0)
		return -1;
	const int flags = O_DIRECTORY | O_RDONLY | O_NOFOLLOW;
	int tmp = openat(box, "tmp", flags);
	int cur = tmp < 0 ? -1 : openat(box, "cur", flags);
	struct published_file saved;
	int ret = -1;
	if (cur >= 0 && fstatat(tmp, temporary, &saved.st, AT_SYMLINK_NOFOLLOW) == 0) {
		ret = renameat2(tmp, temporary, cur, name, RENAME_NOREPLACE);
		if (ret == 0) {
			saved.path = i_strdup(t_strconcat(mailbox, "/cur/", name, NULL));
			array_push_back(&publications, &saved);
		}
	}
	int error = errno;
	if (cur >= 0) i_close_fd(&cur);
	if (tmp >= 0) i_close_fd(&tmp);
	i_close_fd(&box);
	errno = error;
	return ret;
}

static bool copy_matches(int original, const struct published_file *saved)
{
	struct stat st;
	unsigned char source[16384], dest[sizeof(source)];
	int fd = open_destination(saved->path, O_RDONLY | O_NONBLOCK);
	if (fd < 0)
		return FALSE;
	bool same = fstat(fd, &st) == 0 && S_ISREG(st.st_mode) &&
		st.st_dev == saved->st.st_dev && st.st_ino == saved->st.st_ino;
	for (off_t off = 0; same; ) {
		ssize_t n = pread(original, source, sizeof(source), off);
		ssize_t m = pread(fd, dest, sizeof(dest), off);
		same = n >= 0 && n == m && memcmp(source, dest, n) == 0;
		if (n <= 0 || !same)
			break;
		off += n;
	}
	i_close_fd(&fd);
	if (!same)
		return FALSE;
	/* Resolve again from --to after the byte comparison. */
	fd = open_destination(saved->path, O_PATH);
	if (fd < 0)
		return FALSE;
	same = fstat(fd, &st) == 0 && st.st_dev == saved->st.st_dev &&
		st.st_ino == saved->st.st_ino && st.st_size == saved->st.st_size &&
		st.st_mtim.tv_sec == saved->st.st_mtim.tv_sec &&
		st.st_mtim.tv_nsec == saved->st.st_mtim.tv_nsec;
	i_close_fd(&fd);
	return same;
}

static bool check_publications(const char *path)
{
	int fd = open(path, O_RDONLY | O_NOFOLLOW | O_NONBLOCK);
	if (fd < 0)
		return FALSE;
	bool complete = array_count(&publications) > 0;
	const struct published_file *saved;
	array_foreach(&publications, saved) {
		if (!copy_matches(fd, saved)) {
			i_error("Destination %s changed before source expunge", saved->path);
			complete = FALSE;
		}
	}
	i_close_fd(&fd);
	return complete;
}

static int filter_uid(struct mailbox *box, uint32_t uid, struct sieve_binary *binary,
	struct sieve_script_env *env, struct sieve_error_handler *errors, struct ostream *preview,
	const ARRAY_TYPE(source_entry) *names, struct decision *decision)
{
	struct mailbox_transaction_context *t = mailbox_transaction_begin(box, 0, "jaro-sieve-filter message");
	struct mail *mail = mail_alloc(t, 0, NULL);
	const char *id, *path = NULL;
	unsigned int index;
	int result = -1;
	i_array_init(&publications, 2);
	if (mail_set_uid(mail, uid) && mail_get_special(mail, MAIL_FETCH_STORAGE_ID, &id) == 0 &&
	    str_to_uint(id, &index) == 0 && index < array_count(names)) {
		const struct source_entry *entry = array_idx(names, index);
		decision->index = index;
		decision->identified = TRUE;
		jaro_sieve_filename = entry->name;
		path = t_strconcat(batch_path, "/cur/", id, NULL);
		if (!source_unchanged(path, entry, preview != NULL)) {
			i_error("Source %s changed before delivery; retained", entry->name);
		} else {
			result = filter_message(binary, mail, env, errors, preview, decision);
			bool saved = result > 0 || array_count(&publications) > 0;
			if (saved && (!check_publications(path) || !source_unchanged(path, entry, FALSE))) {
				i_error("Incomplete or changed delivery for %s; inspect both copies", entry->name);
				result = -1;
			}
			if (result > 0 && stopping) {
				i_error("Interrupted after destination save for %s; retaining source", entry->name);
				result = -1;
			}
			if (result > 0)
				mail_expunge(mail);
		}
	}
	struct published_file *saved;
	array_foreach_modifiable(&publications, saved)
		i_free(saved->path);
	array_free(&publications);
	mail_free(&mail);
	if (mailbox_transaction_commit(&t) < 0 ||
	    (result > 0 && mailbox_sync(box, 0) < 0)) {
		i_error("Source commit failed for UID %u; inspect retained and delivered copies", uid);
		return -1;
	}
	struct stat st;
	if (result > 0 && (lstat(path, &st) == 0 || errno != ENOENT)) {
		i_error("Source expunge incomplete for %s; inspect retained and delivered copies",
			jaro_sieve_filename);
		return -1;
	}
	return result;
}

static int filter_mailbox(struct mailbox *box, struct sieve_binary *binary,
	struct sieve_script_env *env, struct sieve_error_handler *errors, bool dry_run,
	const ARRAY_TYPE(source_entry) *names)
{
	if (mailbox_sync(box, MAILBOX_SYNC_FLAG_FULL_READ) < 0)
		return -1;
	struct mail_search_args *args = mail_search_build_init();
	mail_search_build_add_all(args);
	struct mailbox_transaction_context *trans = mailbox_transaction_begin(box, 0, "jaro-sieve-filter");
	struct mail_search_context *search = mailbox_search_init(trans, args, NULL, 0, NULL);
	mail_search_args_unref(&args);
	ARRAY(uint32_t) uids;
	t_array_init(&uids, 64);
	struct mail *mail;
	while (mailbox_search_next(search, &mail))
		array_push_back(&uids, &mail->uid);
	int ret = mailbox_search_deinit(&search);
	if (mailbox_transaction_commit(&trans) < 0 || ret < 0)
		return -1;
	if (array_count(&uids) != array_count(names)) {
		i_error("Maildir source inventory changed before mailbox search");
		ret = -1;
	}
	struct ostream *preview = dry_run ? o_stream_create_fd(STDOUT_FILENO, 0) : NULL;
	uint32_t uid;
	array_foreach_elem(&uids, uid) {
		if (stopping)
			break;
		T_BEGIN {
			struct decision decision = { 0 };
			int outcome = filter_uid(box, uid, binary, env, errors, preview, names, &decision);
			if (outcome < 0)
				ret = -1;
			if (decisions != NULL && decision.identified &&
			    fprintf(decisions, "%u%c%d%c%s%c%s%c", decision.index, 0, outcome, 0,
				    decision.location == NULL ? "" : decision.location, 0,
				    decision.mailbox == NULL ? "" : decision.mailbox, 0) < 0)
				ret = -1;
		} T_END;
	}
	if (preview != NULL) {
		if (o_stream_finish(preview) < 0)
			ret = -1;
		o_stream_destroy(&preview);
	} else if (mailbox_sync(box, 0) < 0) {
		ret = -1;
	}
	return ret;
}

int main(int argc, char **argv)
{
	sieve_tool = sieve_tool_init("jaro-sieve-filter", &argc, &argv, "", FALSE);
	if (sieve_tool_getopt(sieve_tool) > 0 || argc - optind != 4)
		i_fatal("Invalid private frontend arguments");
	source_name = argv[optind];
	const char *mode = argv[optind + 1];
	bool preview = !strcmp(mode, "preview") || !strcmp(mode, "preview-report");
	if (!strcmp(mode, "preview-report") || !strcmp(mode, "apply-report")) {
		decisions = fopen("decisions", "wbx");
		if (decisions == NULL) i_fatal("Cannot open workflow report: %m");
	}
	int ready_fd;
	if (str_to_int(argv[optind + 3], &ready_fd) < 0 || ready_fd <= STDERR_FILENO)
		i_fatal("Missing readiness descriptor");
	struct sieve_instance *svinst = sieve_tool_init_finish(sieve_tool, TRUE, TRUE);
	sieve_set_extensions(svinst, "fileinto mailbox regex envelope");
	struct sieve_error_handler *errors = sieve_stderr_ehandler_create(svinst, 0);
	confine_storage(preview);
	destination_fd = open("folders", O_RDONLY | O_DIRECTORY);
	if (destination_fd < 0)
		i_fatal("Cannot open destination root: %m");
	struct sieve_binary *binary = open_program(svinst, errors, !strcmp(argv[optind + 2], "cached"));
	if (write(ready_fd, "R", 1) != 1)
		i_fatal("Could not acknowledge validated batch");
	i_close_fd(&ready_fd);
	struct sigaction action = { .sa_handler = stop_batch };
	sigemptyset(&action.sa_mask);
	if (sigaction(SIGTERM, &action, NULL) || sigaction(SIGHUP, &action, NULL) ||
	    sigaction(SIGINT, &action, NULL) || prctl(PR_SET_PDEATHSIG, SIGTERM) < 0)
		i_fatal("Cannot handle batch interruption: %m");
	char go;
	if (read(STDIN_FILENO, &go, 1) != 1 || go != 'G' || stopping)
		i_fatal("Source selection cancelled before delivery");
	int batch_fd = open("batch", O_RDONLY | O_DIRECTORY);
	struct stat st;
	uintmax_t device, inode;
	FILE *identity = fopen("batch-identity", "r");
	if (batch_fd < 0 || fstat(batch_fd, &st) < 0 || identity == NULL ||
	    fscanf(identity, "%"SCNuMAX" %"SCNuMAX, &device, &inode) != 2 ||
	    device != (uintmax_t)st.st_dev || inode != (uintmax_t)st.st_ino)
		i_fatal("Claimed Maildir changed before execution");
	fclose(identity);
	batch_path = t_strdup_printf("/proc/self/fd/%d", batch_fd);
	ARRAY_TYPE(source_entry) names;
	t_array_init(&names, 64);
	FILE *inventory = fopen("names", "rb");
	if (inventory == NULL)
		i_fatal("Could not open source inventory: %m");
	char *name = NULL;
	size_t capacity = 0;
	ssize_t len;
	while ((len = getdelim(&name, &capacity, '\0', inventory)) > 0) {
		if (name[len-1] != '\0' || !*name || strchr(name, '/') != NULL)
			i_fatal("Invalid source inventory");
		struct source_entry entry = { .name = t_strdup(name) };
		if (getline(&name, &capacity, inventory) < 0)
			i_fatal("Missing source identity");
		entry.identity = t_strdup(name);
		array_push_back(&names, &entry);
	}
	free(name);
	if (ferror(inventory) || fclose(inventory) < 0)
		i_fatal("Cannot read source inventory");
	struct mail_user *user = sieve_tool_get_mail_user(sieve_tool);
	const struct mail_storage_hooks *hooks = &local_hooks;
	array_push_back(&user->hooks, &hooks);
	struct mail_namespace *ns = mail_namespace_find(user->namespaces, source_name);
	struct mailbox *box = mailbox_alloc(ns->list, source_name, MAILBOX_FLAG_IGNORE_ACLS |
					    (preview ? MAILBOX_FLAG_READONLY : 0));
	enum mail_error error;
	if (mailbox_open(box) < 0)
		i_fatal("Cannot open source Maildir: %s", mailbox_get_last_internal_error(box, &error));
	struct sieve_script_env env;
	const char *errstr;
	if (sieve_script_env_init(&env, user, &errstr) < 0)
		i_fatal("Cannot initialize Sieve delivery: %s", errstr);
	env.default_mailbox = source_name;
	int ret = filter_mailbox(box, binary, &env, errors, preview, &names);
	if (decisions != NULL && fclose(decisions) < 0) ret = -1;
	if (ret < 0)
		i_error("Maildir batch contains incomplete deliveries");
	mailbox_free(&box);
	i_close_fd(&batch_fd);
	i_close_fd(&destination_fd);
	sieve_close(&binary);
	sieve_error_handler_unref(&errors);
	sieve_tool_deinit(&sieve_tool);
	return stopping ? 128 + stopping : (ret < 0 ? 7 : EXIT_SUCCESS);
}
