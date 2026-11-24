#define _XOPEN_SOURCE 700

#include "contact/migrate.h"
#include "contact/legacy-import.h"
#include "filter/sha256.h"
#include "sqlite3.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

#define LOCK_NAME ".contacts.sqlite.init.lock"
#define STAGE_NAME ".contacts.sqlite.import"
#define JOURNAL_NAME STAGE_NAME "-journal"
#define SOURCE_MAX 4096u

#ifdef JARO_CONTACT_MIGRATION_TEST
void jaro_contact_migration_test_point(const char *);
#define TEST_POINT(phase) jaro_contact_migration_test_point(phase)
#else
#define TEST_POINT(phase) ((void)0)
#endif

struct source {
	char *name;
	struct stat identity;
	char content[65];
	size_t records;
};

struct digest {
	struct jf_sha256 hash;
	size_t records;
};

struct book {
	int64_t id;
	struct digest expected;
};

struct import_context {
	struct jaro_contact_store *store;
	struct book *book;
	struct jf_sha256 raw;
	struct jaro_contact_import_error error;
	int status;
};

void
jaro_contact_migration_diagnostic(char *buffer, size_t size,
	const struct jaro_contact_migration_error *error)
{
	char location[96] = "";
	if (error->source && error->line)
		snprintf(location, sizeof location, "legacy .abook source %zu, line %zu: ",
			error->source, error->line);
	else if (error->source)
		snprintf(location, sizeof location, "legacy .abook source %zu: ", error->source);
	snprintf(buffer, size, "%s%s%s%s", location, error->field ? error->field : "",
		error->field ? ": " : "", error->reason);
}

static int
failure(struct jaro_contact_migration_error *error, int status, const char *reason,
	const char *field, size_t source, size_t record, size_t line, size_t field_number, int native_code)
{
	if (error)
		*error = (struct jaro_contact_migration_error){
			reason, field, source, record, line, field_number, native_code
		};
	return status;
}

static int
system_failure(struct jaro_contact_migration_error *error, const char *reason)
{
	return failure(error, JARO_CONTACT_STORAGE, reason, NULL, 0, 0, 0, 0, errno);
}

static int
store_failure(struct jaro_contact_migration_error *error, int status,
	const struct jaro_contact_error *detail, size_t source)
{
	return failure(error, status, detail->reason, detail->field, source, 0, 0, 0,
		detail->native_code);
}

static char *
join_path(const char *directory, const char *name)
{
	size_t length = strlen(directory), suffix = strlen(name);
	if (length > SIZE_MAX - suffix - 2) {
		errno = ENAMETOOLONG;
		return NULL;
	}
	char *path = malloc(length + suffix + 2);
	if (path) {
		memcpy(path, directory, length);
		path[length] = '/';
		memcpy(path + length + 1, name, suffix + 1);
	}
	return path;
}

static int
same_inode(const struct stat *a, const struct stat *b)
{
	return a->st_dev == b->st_dev && a->st_ino == b->st_ino;
}

static int
same_source(const struct stat *a, const struct stat *b)
{
#ifdef __APPLE__
	const struct timespec am = a->st_mtimespec, bm = b->st_mtimespec;
	const struct timespec ac = a->st_ctimespec, bc = b->st_ctimespec;
#else
	const struct timespec am = a->st_mtim, bm = b->st_mtim;
	const struct timespec ac = a->st_ctim, bc = b->st_ctim;
#endif
	return same_inode(a, b) && a->st_mode == b->st_mode && a->st_size == b->st_size &&
		am.tv_sec == bm.tv_sec && am.tv_nsec == bm.tv_nsec &&
		ac.tv_sec == bc.tv_sec && ac.tv_nsec == bc.tv_nsec;
}

static int
check_directory(const char *path, const struct stat *identity,
	struct jaro_contact_migration_error *error)
{
	struct stat named;
	if (stat(path, &named) < 0)
		return system_failure(error, "cannot inspect contact profile");
	if (!same_inode(identity, &named) || named.st_uid != geteuid() || (named.st_mode & 0022))
		return failure(error, JARO_CONTACT_STORAGE, "contact profile directory changed",
			NULL, 0, 0, 0, 0, 0);
	return JARO_CONTACT_OK;
}

static int
private_regular(const struct stat *st)
{
	return S_ISREG(st->st_mode) && st->st_uid == geteuid() && !(st->st_mode & 0077);
}

static int
remove_private(int directory, const char *name, struct jaro_contact_migration_error *error)
{
	struct stat entry;
	if (fstatat(directory, name, &entry, AT_SYMLINK_NOFOLLOW) < 0)
		return errno == ENOENT ? JARO_CONTACT_OK : system_failure(error, "cannot inspect import temporary");
	if (!private_regular(&entry))
		return failure(error, JARO_CONTACT_STORAGE, "unexpected import temporary",
			NULL, 0, 0, 0, 0, 0);
	if (unlinkat(directory, name, 0) < 0 && errno != ENOENT)
		return system_failure(error, "cannot remove import temporary");
	return JARO_CONTACT_OK;
}

static int
validate_database(const char *path, struct jaro_contact_migration_error *error)
{
	struct jaro_contact_store *store;
	struct jaro_contact_error detail;
	int status = jaro_contact_open(path, JARO_CONTACT_READ, &store, &detail);
	if (status)
		return store_failure(error, status == JARO_CONTACT_MISSING ? JARO_CONTACT_STORAGE : status,
			&detail, 0);
	status = jaro_contact_verify(store, &detail);
	if (status) {
		jaro_contact_close(store, NULL);
		return store_failure(error, status, &detail, 0);
	}
	status = jaro_contact_close(store, &detail);
	return status ? store_failure(error, status, &detail, 0) : JARO_CONTACT_OK;
}

static int
check_existing(int directory, const char *path, struct jaro_contact_migration_error *error)
{
	struct stat entry, named, temporary;
	if (fstatat(directory, JARO_CONTACT_DATABASE, &entry, AT_SYMLINK_NOFOLLOW) < 0)
		return errno == ENOENT ? JARO_CONTACT_MISSING : system_failure(error, "cannot inspect contact database");
	int status = validate_database(path, error);
	if (status)
		return status;
	if (lstat(path, &named) < 0)
		return system_failure(error, "cannot recheck contact database");
	if (!same_inode(&entry, &named))
		return failure(error, JARO_CONTACT_STORAGE, "contact database changed during validation",
			NULL, 0, 0, 0, 0, 0);

	/* The initializer keeps its temporary link until the final name has
	 * been synced. A reader can finish that sync after interruption. */
	if (named.st_nlink > 1) {
		if (fsync(directory) < 0)
			return system_failure(error, "cannot sync published contact database");
		if (!fstatat(directory, STAGE_NAME, &temporary, AT_SYMLINK_NOFOLLOW) &&
				same_inode(&named, &temporary)) {
			status = remove_private(directory, STAGE_NAME, error);
			if (status)
				return status;
			if (fsync(directory) < 0)
				return system_failure(error, "cannot sync contact temporary cleanup");
		}
	}
	return JARO_CONTACT_OK;
}

static int
lock_initialization(int directory, int *result, struct jaro_contact_migration_error *error)
{
	struct stat held, named;
	int fd = openat(directory, LOCK_NAME, O_RDWR | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
	if (fd >= 0) {
		if (fchmod(fd, 0600) < 0) {
			int status = system_failure(error, "cannot secure contact initialization lock");
			close(fd);
			return status;
		}
	} else if (errno == EEXIST) {
		fd = openat(directory, LOCK_NAME, O_RDWR | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK);
	}
	if (fd < 0)
		return system_failure(error, "cannot open contact initialization lock");
	*result = fd;
	if (fstat(fd, &held) < 0)
		return system_failure(error, "cannot inspect contact initialization lock");
	if (!private_regular(&held) || held.st_nlink != 1)
		return failure(error, JARO_CONTACT_STORAGE, "invalid contact initialization lock",
			NULL, 0, 0, 0, 0, 0);
	TEST_POINT("before_lock");
	while (flock(fd, LOCK_EX) < 0)
		if (errno != EINTR)
			return system_failure(error, "cannot acquire contact initialization lock");
	if (fstatat(directory, LOCK_NAME, &named, AT_SYMLINK_NOFOLLOW) < 0)
		return system_failure(error, "cannot recheck contact initialization lock");
	if (!same_inode(&held, &named) || !private_regular(&named) || named.st_nlink != 1)
		return failure(error, JARO_CONTACT_STORAGE, "contact initialization lock changed",
			NULL, 0, 0, 0, 0, 0);
	return JARO_CONTACT_OK;
}

static int
addressbook_suffix(const char *name)
{
	size_t length = strlen(name);
	return length >= 6 && !strcmp(name + length - 6, ".abook");
}

static int
compare_sources(const void *left, const void *right)
{
	const struct source *a = left, *b = right;
	return strcmp(a->name, b->name);
}

static void
free_sources(struct source *sources, size_t count)
{
	for (size_t i = 0; i < count; i++)
		free(sources[i].name);
	free(sources);
}

static int
discover_sources(int directory, struct source **result, size_t *total,
	struct jaro_contact_migration_error *error)
{
	struct source *sources = NULL;
	size_t count = 0, capacity = 0;
	DIR *stream;
	int status = JARO_CONTACT_OK;
	int fd = openat(directory, ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	if (fd < 0)
		return system_failure(error, "cannot open contact source directory");
	stream = fdopendir(fd);
	if (!stream) {
		status = system_failure(error, "cannot scan contact source directory");
		close(fd);
		return status;
	}
	for (;;) {
		errno = 0;
		struct dirent *entry = readdir(stream);
		if (!entry) {
			if (errno)
				status = system_failure(error, "cannot continue contact source discovery");
			break;
		}
		if (!addressbook_suffix(entry->d_name))
			continue;
		if (count == SOURCE_MAX) {
			status = failure(error, JARO_CONTACT_INVALID, "too many legacy contact sources",
				NULL, 0, 0, 0, 0, 0);
			break;
		}
		if (count == capacity) {
			size_t next = capacity ? 2 * capacity : 8;
			struct source *items = realloc(sources, next * sizeof *items);
			if (!items) {
				status = system_failure(error, "cannot allocate contact source names");
				break;
			}
			sources = items;
			capacity = next;
		}
		char *name = strdup(entry->d_name);
		if (!name) {
			status = system_failure(error, "cannot allocate contact source name");
			break;
		}
		sources[count++] = (struct source){.name = name};
	}
	if (closedir(stream) < 0 && !status)
		status = system_failure(error, "cannot close contact source directory");
	if (status) {
		free_sources(sources, count);
		return status;
	}
	if (count > 1)
		qsort(sources, count, sizeof *sources, compare_sources);
	*total = count;
	*result = sources;
	return JARO_CONTACT_OK;
}

static void
hash_count(struct jf_sha256 *hash, uint64_t value)
{
	unsigned char bytes[8];
	for (size_t i = 0; i < sizeof bytes; i++, value >>= 8)
		bytes[i] = (unsigned char)value;
	jf_sha256_update(hash, bytes, sizeof bytes);
}

static void
hash_text(struct jf_sha256 *hash, struct jaro_contact_text text)
{
	hash_count(hash, text.size);
	if (text.size)
		jf_sha256_update(hash, text.data, text.size);
}

static int
hash_record(const struct jaro_contact *contact, void *data)
{
	struct digest *digest = data;
	struct jf_sha256 *hash = &digest->hash;
	hash_text(hash, contact->name);
	hash_text(hash, contact->nickname);
	hash_text(hash, contact->notes);
	hash_count(hash, contact->address_count);
	for (size_t i = 0; i < contact->address_count; i++)
		hash_text(hash, contact->addresses[i]);
	hash_count(hash, contact->field_count);
	for (size_t i = 0; i < contact->field_count; i++) {
		hash_text(hash, contact->fields[i].key);
		hash_text(hash, contact->fields[i].value);
	}
	digest->records++;
	return 0;
}

static int
save_record(const struct jaro_legacy_record *record, void *data)
{
	struct import_context *context = data;
	context->status = jaro_contact_import_record(context->store, context->book->id, record,
		hash_record, &context->book->expected, &context->error);
	return context->status;
}

static void
hash_source_bytes(const char *bytes, size_t size, void *data)
{
	struct import_context *context = data;
	jf_sha256_update(&context->raw, bytes, size);
}

static int
hash_descriptor(int fd, off_t remaining, char content[65])
{
	struct jf_sha256 hash;
	unsigned char buffer[16384];
	jf_sha256_init(&hash);
	while (remaining > 0) {
		size_t requested = remaining > (off_t)sizeof buffer ? sizeof buffer : (size_t)remaining;
		ssize_t count = read(fd, buffer, requested);
		if (count < 0 && errno == EINTR)
			continue;
		if (count <= 0) {
			if (!count)
				errno = EIO;
			return -1;
		}
		jf_sha256_update(&hash, buffer, (size_t)count);
		remaining -= count;
	}
	jf_sha256_hex(&hash, content);
	return 0;
}

static int
source_failure(struct jaro_contact_migration_error *error, int status,
	const char *reason, size_t source, int native_code)
{
	return failure(error, status, reason, NULL, source, 0, 0, 0, native_code);
}

static int
source_matches(int directory, const struct source *source, size_t index,
	struct jaro_contact_migration_error *error)
{
	struct stat current;
	if (fstatat(directory, source->name, &current, AT_SYMLINK_NOFOLLOW) < 0)
		return source_failure(error, JARO_CONTACT_STORAGE, "cannot recheck legacy source", index, errno);
	if (!same_source(&source->identity, &current))
		return source_failure(error, JARO_CONTACT_BUSY, "legacy source changed during import", index, 0);
	return JARO_CONTACT_OK;
}

static int
lock_source(int fd, size_t index, struct jaro_contact_migration_error *error)
{
	struct flock read_lock = {.l_type = F_RDLCK, .l_whence = SEEK_SET};
	if (fcntl(fd, F_SETLK, &read_lock) < 0) {
		int result = errno == EACCES || errno == EAGAIN ? JARO_CONTACT_BUSY : JARO_CONTACT_STORAGE;
		return source_failure(error, result, "cannot lock legacy source for reading", index, errno);
	}
	return JARO_CONTACT_OK;
}

static int
verify_source(int directory, const struct source *source, size_t index,
	struct jaro_contact_migration_error *error)
{
	struct stat current;
	char content[65];
	int status;
	int fd = openat(directory, source->name, O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
	if (fd < 0)
		return source_failure(error, JARO_CONTACT_STORAGE, "cannot reopen legacy source", index, errno);
	status = lock_source(fd, index, error);
	if (!status) {
		if (fstat(fd, &current) < 0)
			status = source_failure(error, JARO_CONTACT_STORAGE, "cannot recheck open legacy source", index, errno);
		else if (!same_source(&source->identity, &current))
			status = source_failure(error, JARO_CONTACT_BUSY, "legacy source changed during import", index, 0);
	}
	if (!status && hash_descriptor(fd, source->identity.st_size, content) < 0)
		status = source_failure(error, JARO_CONTACT_STORAGE, "cannot verify legacy source bytes", index, errno);
	if (!status && strcmp(content, source->content))
		status = source_failure(error, JARO_CONTACT_BUSY, "legacy source changed during import", index, 0);
	if (!status)
		status = source_matches(directory, source, index, error);
	if (close(fd) < 0 && !status)
		status = source_failure(error, JARO_CONTACT_STORAGE, "cannot close verified legacy source", index, errno);
	return status;
}

static int
read_source(int directory, struct source *source, size_t index, struct import_context *context,
	struct jaro_contact_migration_error *error)
{
	struct jaro_legacy_error legacy;
	struct stat after;
	size_t before = context->book->expected.records;
	int fd = openat(directory, source->name, O_RDONLY | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC);
	int status;
	FILE *stream;

	if (fd < 0)
		return source_failure(error, JARO_CONTACT_STORAGE, "cannot open legacy source", index, errno);
	if (fstat(fd, &source->identity) < 0) {
		status = source_failure(error, JARO_CONTACT_STORAGE, "cannot inspect legacy source", index, errno);
		goto close_descriptor;
	}
	if (!S_ISREG(source->identity.st_mode) || !(source->identity.st_mode & 0444)) {
		status = source_failure(error, JARO_CONTACT_STORAGE,
			"legacy source must be a readable regular file", index, 0);
		goto close_descriptor;
	}
	status = lock_source(fd, index, error);
	if (status)
		goto close_descriptor;
	status = source_matches(directory, source, index, error);
	if (status)
		goto close_descriptor;
	stream = fdopen(fd, "r");
	if (!stream) {
		status = source_failure(error, JARO_CONTACT_STORAGE, "cannot open legacy source stream", index, errno);
		goto close_descriptor;
	}
	jf_sha256_init(&context->raw);
	enum jaro_legacy_result read = jaro_legacy_read(stream, save_record, hash_source_bytes, context, &legacy);
	if (read == JARO_LEGACY_CONSUMER && context->status) {
		const struct jaro_contact_import_error *detail = &context->error;
		status = failure(error, context->status, detail->detail.reason, detail->detail.field, index,
			detail->record, detail->line, detail->field, detail->detail.native_code);
	} else if (read != JARO_LEGACY_OK) {
		int result = read == JARO_LEGACY_IO || read == JARO_LEGACY_MEMORY ?
			JARO_CONTACT_STORAGE : JARO_CONTACT_INVALID;
		status = failure(error, result, legacy.reason, NULL, index, legacy.record, legacy.line,
			legacy.field, legacy.system_error);
	}
	if (!status) {
		if (fstat(fd, &after) < 0)
			status = source_failure(error, JARO_CONTACT_STORAGE,
				"cannot recheck open legacy source", index, errno);
		else if (!same_source(&source->identity, &after))
			status = source_failure(error, JARO_CONTACT_BUSY, "legacy source changed while reading", index, 0);
	}
	if (fclose(stream) < 0 && !status)
		status = source_failure(error, JARO_CONTACT_STORAGE, "cannot close legacy source", index, errno);
	if (!status) {
		source->records = context->book->expected.records - before;
		jf_sha256_hex(&context->raw, source->content);
	}
	return status;

close_descriptor:
	close(fd);
	return status;
}

static struct jaro_contact_text
list_name(const char *source)
{
	if (!strcmp(source, "whitelist.abook"))
		return (struct jaro_contact_text){"allow", 5};
	if (!strcmp(source, "blacklist.abook"))
		return (struct jaro_contact_text){"block", 5};
	return (struct jaro_contact_text){source, strlen(source) - 6};
}

static int
read_and_verify_sources(int directory, struct jaro_contact_store *store,
	struct source *sources, size_t count, struct jaro_contact_migration_error *error)
{
	struct book *books = count ? calloc(count, sizeof *books) : NULL;
	size_t book_count = 0;
	struct jaro_contact_error detail;
	int status = JARO_CONTACT_OK;

	if (count && !books)
		return system_failure(error, "cannot allocate import verification data");
	for (size_t i = 0; !status && i < count; i++) {
		int64_t id;
		status = jaro_contact_book(store, list_name(sources[i].name), 1, &id, &detail);
		if (status) {
			store_failure(error, status, &detail, i + 1);
			break;
		}
		size_t book = 0;
		while (book < book_count && books[book].id != id)
			book++;
		if (book == book_count) {
			books[book_count++].id = id;
			jf_sha256_init(&books[book].expected.hash);
		}
		struct import_context context = {.store = store, .book = &books[book]};
		status = read_source(directory, &sources[i], i + 1, &context, error);
	}
	for (size_t i = 0; !status && i < book_count; i++) {
		struct digest actual = {0};
		char original[65], stored[65];
		jf_sha256_init(&actual.hash);
		status = jaro_contact_visit(store, books[i].id, (struct jaro_contact_text){0},
			hash_record, &actual, &detail);
		if (status) {
			store_failure(error, status, &detail, 0);
			break;
		}
		jf_sha256_hex(&books[i].expected.hash, original);
		jf_sha256_hex(&actual.hash, stored);
		if (actual.records != books[i].expected.records || strcmp(original, stored))
			status = failure(error, JARO_CONTACT_STORAGE, "imported contact verification mismatch",
				NULL, 0, 0, 0, 0, 0);
	}
	free(books);
	return status;
}

static int
fingerprint_stage(int directory, int synchronize, struct stat *identity, char content[65],
	struct jaro_contact_migration_error *error)
{
	struct stat named;
	int fd = openat(directory, STAGE_NAME, O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK);
	int status = JARO_CONTACT_OK;
	if (fd < 0)
		return system_failure(error, "cannot open completed contact temporary");
	if (fstat(fd, identity) < 0)
		status = system_failure(error, "cannot inspect completed contact temporary");
	else if (!private_regular(identity) || identity->st_nlink != 1 || !identity->st_size)
		status = failure(error, JARO_CONTACT_STORAGE, "invalid completed contact temporary",
			NULL, 0, 0, 0, 0, 0);
	else if (synchronize && fsync(fd) < 0)
		status = system_failure(error, "cannot sync completed contact temporary");
	else if (hash_descriptor(fd, identity->st_size, content) < 0)
		status = system_failure(error, "cannot verify completed contact temporary bytes");
	else if (fstatat(directory, STAGE_NAME, &named, AT_SYMLINK_NOFOLLOW) < 0)
		status = system_failure(error, "cannot recheck completed contact temporary");
	else if (!same_source(identity, &named))
		status = failure(error, JARO_CONTACT_STORAGE, "completed contact temporary changed",
			NULL, 0, 0, 0, 0, 0);
	if (close(fd) < 0 && !status)
		status = system_failure(error, "cannot close completed contact temporary");
	return status;
}

int
jaro_contact_initialize(const char *profile, jaro_contact_migration_notice notice,
	void *data, struct jaro_contact_migration_error *error)
{
	struct stat identity, staged, named;
	struct source *sources = NULL;
	struct jaro_contact_store *store = NULL;
	struct jaro_contact_error detail;
	size_t count = 0;
	char *real = NULL, *destination = NULL, *temporary = NULL;
	char staged_content[65], named_content[65];
	int directory = -1, lock = -1, cleanup = 0, published = 0, status = JARO_CONTACT_OK;

	if (error)
		*error = (struct jaro_contact_migration_error){0};
	real = realpath(profile, NULL);
	if (!real)
		return system_failure(error, "cannot resolve contact profile");
	destination = join_path(real, JARO_CONTACT_DATABASE);
	temporary = join_path(real, STAGE_NAME);
	if (!destination || !temporary) {
		status = system_failure(error, "cannot allocate contact database path");
		goto done;
	}
	directory = open(real, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
	if (directory < 0 || fstat(directory, &identity) < 0) {
		status = system_failure(error, "cannot open contact profile");
		goto done;
	}
	status = check_directory(real, &identity, error);
	if (status)
		goto done;
	status = check_existing(directory, destination, error);
	if (status != JARO_CONTACT_MISSING)
		goto done;
	status = lock_initialization(directory, &lock, error);
	if (status)
		goto done;
	status = check_directory(real, &identity, error);
	if (status)
		goto done;
	status = check_existing(directory, destination, error);
	if (status != JARO_CONTACT_MISSING)
		goto done;

	status = remove_private(directory, JOURNAL_NAME, error);
	if (!status)
		status = remove_private(directory, STAGE_NAME, error);
	if (status)
		goto done;
	status = discover_sources(directory, &sources, &count, error);
	if (status)
		goto done;
	status = jaro_contact_open(temporary, JARO_CONTACT_CREATE_EXCLUSIVE, &store, &detail);
	if (!status) {
		cleanup = 1;
		status = jaro_contact_begin(store, &detail);
	}
	if (status) {
		store_failure(error, status, &detail, 0);
		goto done;
	}
	TEST_POINT("before_import");
	status = read_and_verify_sources(directory, store, sources, count, error);
	if (status)
		goto done;
	TEST_POINT("before_commit");
	status = jaro_contact_commit(store, &detail);
	if (!status) {
		status = jaro_contact_close(store, &detail);
		if (!status)
			store = NULL;
	}
	if (status) {
		store_failure(error, status, &detail, 0);
		goto done;
	}
	status = validate_database(temporary, error);
	if (status)
		goto done;
	if (!fstatat(directory, JOURNAL_NAME, &named, AT_SYMLINK_NOFOLLOW) || errno != ENOENT) {
		status = failure(error, JARO_CONTACT_STORAGE, "contact temporary has an unfinished journal",
			NULL, 0, 0, 0, 0, 0);
		goto done;
	}
	status = fingerprint_stage(directory, 1, &staged, staged_content, error);
	if (status)
		goto done;
	TEST_POINT("before_publish");
	for (size_t i = 0; !status && i < count; i++)
		status = verify_source(directory, &sources[i], i + 1, error);
	if (status)
		goto done;
	status = fingerprint_stage(directory, 0, &named, named_content, error);
	if (status)
		goto done;
	if (!same_source(&named, &staged) || strcmp(named_content, staged_content)) {
		status = failure(error, JARO_CONTACT_STORAGE, "completed contact temporary changed",
			NULL, 0, 0, 0, 0, 0);
		goto done;
	}
	status = check_directory(real, &identity, error);
	if (status)
		goto done;
	if (linkat(directory, STAGE_NAME, directory, JARO_CONTACT_DATABASE, 0) < 0) {
		if (errno == EEXIST) {
			status = check_existing(directory, destination, error);
			if (status == JARO_CONTACT_MISSING)
				status = system_failure(error, "competing contact database disappeared");
		} else {
			status = system_failure(error, "cannot publish contact database");
		}
		goto done;
	}
	/* Retain the private link if directory sync fails so a later opener
	 * knows the published name still needs to be synced. */
	cleanup = 0;
	TEST_POINT("after_publish");
	if (fsync(directory) < 0) {
		status = system_failure(error, "cannot sync published contact database");
		goto done;
	}
	status = remove_private(directory, STAGE_NAME, error);
	if (!status && fsync(directory) < 0)
		status = system_failure(error, "cannot sync contact temporary cleanup");
	published = !status;

done:
	if (store) {
		jaro_contact_rollback(store, NULL);
		jaro_contact_close(store, NULL);
	}
	if (cleanup) {
		int removed = remove_private(directory, JOURNAL_NAME, status ? NULL : error);
		if (!status)
			status = removed;
		removed = remove_private(directory, STAGE_NAME, status ? NULL : error);
		if (!status)
			status = removed;
	}
	if (lock >= 0)
		close(lock);
	if (directory >= 0)
		close(directory);
	if (!status && published && notice)
		for (size_t i = 0; i < count; i++)
			notice(data, i + 1, sources[i].records);
	free_sources(sources, count);
	free(temporary);
	free(destination);
	free(real);
	return status;
}

static int
preview_sources(const char *profile, struct jaro_contact_store **store,
	struct jaro_contact_migration_error *error)
{
	struct stat identity;
	struct source *sources = NULL;
	struct jaro_contact_error detail;
	size_t count = 0;
	int directory = open(profile, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
	int status;

	if (directory < 0)
		return system_failure(error, "cannot open contact profile");
	if (fstat(directory, &identity) < 0)
		status = system_failure(error, "cannot inspect contact profile");
	else
		status = check_directory(profile, &identity, error);
	if (!status)
		status = discover_sources(directory, &sources, &count, error);
	if (!status) {
		status = jaro_contact_open_memory(store, &detail);
		if (!status)
			status = jaro_contact_begin(*store, &detail);
		if (status)
			store_failure(error, status, &detail, 0);
	}
	if (!status) {
		status = read_and_verify_sources(directory, *store, sources, count, error);
		for (size_t i = 0; !status && i < count; i++)
			status = verify_source(directory, &sources[i], i + 1, error);
		if (!status) {
			status = jaro_contact_commit(*store, &detail);
			if (status)
				store_failure(error, status, &detail, 0);
		}
	}
	close(directory);
	free_sources(sources, count);
	if (status && *store) {
		jaro_contact_close(*store, NULL);
		*store = NULL;
	}
	return status;
}

int
jaro_contact_profile_open(const char *profile, enum jaro_contact_profile_mode mode,
	struct jaro_contact_store **store, struct jaro_contact_migration_error *error)
{
	struct jaro_contact_error detail;
	struct stat identity, entry;
	int status;

	*store = NULL;
	if (error)
		*error = (struct jaro_contact_migration_error){0};
	char *real = realpath(profile, NULL);
	if (!real)
		return system_failure(error, "cannot resolve contact profile");
	char *path = NULL;
	if (stat(real, &identity) < 0) {
		status = system_failure(error, "cannot inspect contact profile");
		goto done;
	}
	status = check_directory(real, &identity, error);
	if (status)
		goto done;
	path = join_path(real, JARO_CONTACT_DATABASE);
	if (!path) {
		status = system_failure(error, "cannot allocate contact database path");
		goto done;
	}
	status = lstat(path, &entry);
	if (status < 0 && errno != ENOENT) {
		status = system_failure(error, "cannot inspect contact database");
		goto done;
	}
	if (status < 0 && mode == JARO_CONTACT_PROFILE_PREVIEW) {
		status = preview_sources(real, store, error);
		/* An initializer may have published a newer snapshot during the
		 * preview. Prefer the published book if one is now visible. */
		if (lstat(path, &entry) < 0 && errno == ENOENT)
			goto done;
		if (*store) {
			jaro_contact_close(*store, NULL);
			*store = NULL;
		}
		if (error)
			*error = (struct jaro_contact_migration_error){0};
	}
	if (mode != JARO_CONTACT_PROFILE_PREVIEW && (status < 0 || entry.st_nlink > 1)) {
		status = jaro_contact_initialize(real, NULL, NULL, error);
		if (status)
			goto done;
	}
	status = jaro_contact_open(path, mode == JARO_CONTACT_PROFILE_WRITE ? JARO_CONTACT_WRITE :
		JARO_CONTACT_READ, store, &detail);
	if (status && mode == JARO_CONTACT_PROFILE_READ && detail.native_code == SQLITE_READONLY_ROLLBACK) {
		/* Recovery is separate from the read-only query. Dry runs never
		 * trigger recovery writes. */
		status = jaro_contact_open(path, JARO_CONTACT_WRITE, store, &detail);
		if (!status) {
			status = jaro_contact_close(*store, &detail);
			*store = NULL;
		}
		if (!status)
			status = jaro_contact_open(path, JARO_CONTACT_READ, store, &detail);
	}
	if (status)
		store_failure(error, status, &detail, 0);

done:
	free(path);
	free(real);
	return status;
}
