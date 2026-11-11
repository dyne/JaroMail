/* Message-only Maildir snapshots. Paths are walked through directory
 * descriptors without following symlinks. External writers MUST be quiescent:
 * identity/content rescans detect observed changes, not a filesystem snapshot.
 * Payload and manifest are fsynced before the hash-bound completion marker.
 * No source is ever opened for writing. No account/configuration is restored.
 */
#define _POSIX_C_SOURCE 200809L
#include "filter/sha256.h"
#include <sys/stat.h>
#include <sys/types.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#ifdef __APPLE__
#define st_mtim st_mtimespec
#define st_ctim st_ctimespec
#endif

#define PATH_LIMIT 4096
#define FIELD_LIMIT 4096
#define WORK_PATH_LIMIT (PATH_LIMIT + 128)
struct message { char *path; struct stat original; char digest[65]; };
struct inventory { struct message *items; size_t count, capacity; };
static volatile sig_atomic_t interrupted;
#ifdef JARO_SNAPSHOT_TESTING
static volatile sig_atomic_t resumed;
#endif
static void stop(int sig)
{
#ifdef JARO_SNAPSHOT_TESTING
	if (sig == SIGUSR1) { resumed = 1; return; }
#endif
	interrupted = sig;
}

/* Test controls do not exist in the installed binary. Pauses signal readiness
 * on stderr, allowing the fixture to mutate input or signal this exact child. */
static int point(const char *phase)
{
#ifdef JARO_SNAPSHOT_TESTING
	const char *fault = getenv("JARO_SNAPSHOT_FAULT");
	const char *pause_at = getenv("JARO_SNAPSHOT_PAUSE");
	if (pause_at && !strcmp(pause_at, phase)) {
		sigset_t held, previous;
		sigemptyset(&held); sigaddset(&held, SIGUSR1); sigaddset(&held, SIGTERM);
		sigaddset(&held, SIGINT); sigaddset(&held, SIGHUP);
		if (sigprocmask(SIG_BLOCK, &held, &previous)) return -1;
		fprintf(stderr, "SNAPSHOT-TEST pause reached: %s\n", phase); fflush(stderr);
		while (!interrupted && !resumed) sigsuspend(&previous);
		if (sigprocmask(SIG_SETMASK, &previous, NULL)) return -1;
	}
	if (fault && !strcmp(fault, phase)) {
		fprintf(stderr, "SNAPSHOT-TEST fault reached: %s\n", phase);
		errno = !strcmp(phase, "payload-write") ? ENOSPC : EIO; return -1;
	}
#else
	(void)phase;
#endif
	return interrupted ? -1 : 0;
}

static int same(const struct stat *a, const struct stat *b)
{
	return a->st_dev == b->st_dev && a->st_ino == b->st_ino &&
		a->st_size == b->st_size && a->st_mode == b->st_mode &&
		a->st_mtim.tv_sec == b->st_mtim.tv_sec && a->st_mtim.tv_nsec == b->st_mtim.tv_nsec &&
		a->st_ctim.tv_sec == b->st_ctim.tv_sec && a->st_ctim.tv_nsec == b->st_ctim.tv_nsec;
}

static int write_all(int fd, const void *data, size_t size)
{
	const char *p = data;
	while (size && !interrupted) {
		ssize_t n = write(fd, p, size);
		if (n < 0 && errno == EINTR) continue;
		if (n <= 0) return -1;
		p += n; size -= (size_t)n;
	}
	return interrupted ? -1 : 0;
}

static int hash_copy(int fd, int output, char digest[65], uintmax_t *size)
{
	struct jf_sha256 hash;
	struct stat before, after;
	char buf[32768];
	ssize_t n;
	*size = 0;
	if (fstat(fd, &before) || !S_ISREG(before.st_mode) || lseek(fd, 0, SEEK_SET) < 0) return -1;
	jf_sha256_init(&hash);
	while (!interrupted && (n = read(fd, buf, sizeof buf)) != 0) {
		if (n < 0) { if (errno == EINTR) continue; return -1; }
		jf_sha256_update(&hash, buf, (size_t)n); *size += (uintmax_t)n;
		if (output >= 0 && (write_all(output, buf, (size_t)n) || point("payload-write"))) return -1;
	}
	if (interrupted || fstat(fd, &after) || !same(&before, &after) ||
		*size != (uintmax_t)before.st_size) return -1;
	jf_sha256_hex(&hash, digest);
	return 0;
}

static int directory(int fd, const char *name)
{
	return openat(fd, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
}

/* User operands may contain . and .., but every traversed component must be
 * a real directory. Manifest paths instead require canonical relative names. */
static int open_directory(const char *path)
{
	char *copy, *part, *next;
	int fd, child;
	if (!*path || strlen(path) >= PATH_LIMIT) return -1;
	copy = strdup(path); if (!copy) return -1;
	fd = open(*path == '/' ? "/" : ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	for (part = strtok_r(copy, "/", &next); fd >= 0 && part; part = strtok_r(NULL, "/", &next)) {
		child = directory(fd, part); close(fd); fd = child;
	}
	free(copy); return fd;
}

static int excluded(const char *name)
{
	return !strcmp(name, "Accounts") || !strcmp(name, "tmp") ||
		!strcmp(name, ".outbox-state") || !strcmp(name, ".recovery-claims") ||
		!strcmp(name, ".notmuch") || !strcmp(name, ".git") ||
		!strcmp(name, ".password-store") || !strcmp(name, "keyrings");
}

static char *joined(const char *parent, const char *name)
{
	size_t a = strlen(parent), b = strlen(name);
	char *result;
	if (a >= WORK_PATH_LIMIT || b >= WORK_PATH_LIMIT || a + b + 2 > WORK_PATH_LIMIT) return NULL;
	result = malloc(a + b + 2);
	if (result) {
		memcpy(result, parent, a);
		if (a) result[a++] = '/';
		memcpy(result + a, name, b + 1);
	}
	return result;
}

static int add(struct inventory *list, const char *path, const struct stat *s)
{
	struct message *grown;
	if (list->count == list->capacity) {
		size_t capacity = list->capacity ? list->capacity * 2 : 64;
		if (capacity < list->capacity || capacity > SIZE_MAX / sizeof *grown) return -1;
		grown = realloc(list->items, capacity * sizeof *grown);
		if (!grown) return -1;
		list->items = grown; list->capacity = capacity;
	}
	list->items[list->count].path = strdup(path);
	if (!list->items[list->count].path) return -1;
	list->items[list->count].original = *s;
	list->items[list->count].digest[0] = 0;
	list->count++; return 0;
}

static void release(struct inventory *list)
{
	size_t i;
	for (i = 0; i < list->count; i++) free(list->items[i].path);
	free(list->items); memset(list, 0, sizeof *list);
}

static int compare(const void *a, const void *b)
{
	return strcmp(((const struct message *)a)->path, ((const struct message *)b)->path);
}

/* Only regular files in genuine Maildir new/cur pairs are messages. Metadata
 * files are not opened. Configuration/journal/index/tmp trees are excluded. */
static int scan(int fd, const char *prefix, struct inventory *list, unsigned depth, int messages, unsigned limit)
{
	DIR *dir;
	struct dirent *entry;
	struct stat s;
	int rc = -1, child, has_new = 0, has_cur = 0;
	char *path;
	if (depth > limit || interrupted) return -1;
	if (!messages) {
		if (!fstatat(fd, "new", &s, AT_SYMLINK_NOFOLLOW)) {
			if (!S_ISDIR(s.st_mode)) return -1;
			has_new = 1;
		} else if (errno != ENOENT) return -1;
		if (!fstatat(fd, "cur", &s, AT_SYMLINK_NOFOLLOW)) {
			if (!S_ISDIR(s.st_mode)) return -1;
			has_cur = 1;
		} else if (errno != ENOENT) return -1;
		if (has_new != has_cur) return -1;
	}
	/* A new open file description avoids sharing readdir offsets with fd. */
	child = directory(fd, ".");
	if (child < 0) return -1;
	dir = fdopendir(child); if (!dir) { close(child); return -1; }
	for (;;) {
		errno = 0; entry = readdir(dir);
		if (!entry) { if (!errno) rc = 0; break; }
		if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) continue;
		if (!messages && excluded(entry->d_name)) continue;
		if (interrupted || fstatat(fd, entry->d_name, &s, AT_SYMLINK_NOFOLLOW)) break;
		if (S_ISLNK(s.st_mode)) break;
		path = joined(prefix, entry->d_name); if (!path) break;
		if (messages) {
			if (!S_ISREG(s.st_mode) || add(list, path, &s)) { free(path); break; }
		} else if (S_ISDIR(s.st_mode)) {
			child = directory(fd, entry->d_name);
			if (child < 0) { free(path); break; }
			int status = scan(child, path, list, depth + 1,
				has_new && (!strcmp(entry->d_name, "new") || !strcmp(entry->d_name, "cur")), limit);
			close(child);
			if (status) { free(path); break; }
		}
		free(path);
	}
	closedir(dir); return rc;
}

static int inventory_limit(int fd, struct inventory *list, unsigned limit)
{
	if (scan(fd, "", list, 0, 0, limit)) return -1;
	if (list->count) qsort(list->items, list->count, sizeof *list->items, compare);
	return 0;
}

static int inventory(int fd, struct inventory *list) { return inventory_limit(fd, list, 128); }

static int open_message(int root, const char *path)
{
	char *copy = strdup(path), *last;
	int parent = -1, fd = -1;
	if (!copy) return -1;
	last = strrchr(copy, '/');
	if (last) {
		*last++ = 0;
		/* Walk relative to the captured root, never through symlinks. */
		parent = directory(root, ".");
		char *part, *next;
		for (part = strtok_r(copy, "/", &next); parent >= 0 && part; part = strtok_r(NULL, "/", &next)) {
			int child = directory(parent, part); close(parent); parent = child;
		}
		if (parent >= 0) fd = openat(parent, last, O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
	}
	if (parent >= 0) close(parent);
	free(copy); return fd;
}

static int source_current(int root, const struct inventory *original)
{
	struct inventory now = {0};
	size_t i;
	int rc = -1;
	/* A restored outbox gains two private container components. */
	if (inventory_limit(root, &now, 130) || now.count != original->count) goto done;
	for (i = 0; i < now.count; i++) {
		char digest[65]; uintmax_t size;
		struct stat opened;
		int fd;
		if (strcmp(now.items[i].path, original->items[i].path) ||
			!same(&now.items[i].original, &original->items[i].original)) goto done;
		fd = open_message(root, now.items[i].path);
		if (fd < 0) goto done;
		int status = fstat(fd, &opened) || !same(&opened, &original->items[i].original) ||
			hash_copy(fd, -1, digest, &size);
		close(fd);
		if (status || strcmp(digest, original->items[i].digest)) goto done;
	}
	rc = 0;
done: release(&now); return rc;
}

/* Reject a destination whose parent is the source or any descendant, using
 * inode identity rather than a string prefix (including .. operands). */
static int outside(int parent, int source)
{
	struct stat root, current, above;
	int fd = dup(parent), next, rc = -1;
	if (fd < 0 || fstat(source, &root)) goto done;
	for (;;) {
		if (fstat(fd, &current)) break;
		if (current.st_dev == root.st_dev && current.st_ino == root.st_ino) break;
		next = directory(fd, ".."); if (next < 0) break;
		if (fstat(next, &above)) { close(next); break; }
		if (current.st_dev == above.st_dev && current.st_ino == above.st_ino) { close(next); rc = 0; break; }
		close(fd); fd = next;
	}
done: if (fd >= 0) close(fd); return rc;
}

static int field(int fd, const char *text) { return write_all(fd, text, strlen(text) + 1); }

static const char *flags(const char *path)
{
	const char *name = strrchr(path, '/'), *found;
	name = name ? name + 1 : path;
	found = strstr(name, ":2,");
	return found ? found + 3 : "";
}

static int read_field(FILE *file, char value[FIELD_LIMIT])
{
	size_t i = 0;
	int ch;
	while ((ch = fgetc(file)) != EOF) {
		if (i + 1 >= FIELD_LIMIT) return -1;
		value[i++] = (char)ch;
		if (!ch) return 0;
	}
	return -1;
}

static int valid_path(const char *path)
{
	char *copy, *part, *next;
	const char *base, *parent;
	int rc = -1;
	if (!*path || *path == '/' || strlen(path) >= PATH_LIMIT || strstr(path, "//")) return -1;
	base = strrchr(path, '/');
	if (!base || !base[1]) return -1;
	parent = base;
	while (parent > path && parent[-1] != '/') parent--;
	if (!((base - parent == 3 && !memcmp(parent, "new", 3)) ||
		(base - parent == 3 && !memcmp(parent, "cur", 3)))) return -1;
	copy = strdup(path); if (!copy) return -1;
	for (part = strtok_r(copy, "/", &next); part; part = strtok_r(NULL, "/", &next)) {
		if (!strcmp(part, ".") || !strcmp(part, "..") ||
			(part != copy + (base - path) + 1 && excluded(part)) ||
			(part < copy + (parent - path) && (!strcmp(part, "new") || !strcmp(part, "cur")))) goto done;
	}
	rc = 0;
done: free(copy); return rc;
}

static int hexadecimal(const char *value)
{
	return strlen(value) == 64 && strspn(value, "0123456789abcdef") == 64;
}

static int number(const char *value, uintmax_t *n)
{
	char *end;
	if (!*value || strspn(value, "0123456789") != strlen(value) || (value[0] == '0' && value[1])) return -1;
	errno = 0; *n = strtoumax(value, &end, 10);
	return errno || *end ? -1 : 0;
}

/* Verify the entire manifest, completion binding and exact payload set. The
 * numbered payload namespace cannot interpret manifest paths as filesystem IO. */
static int verify(int snapshot, const char *manifest_name, int completed, size_t *count, struct inventory *records)
{
	int manifest = -1, payload = -1, marker = -1, rc = -1;
	FILE *file = NULL;
	char path[FIELD_LIMIT], saved_flags[FIELD_LIMIT], text[FIELD_LIMIT], expected[65], actual[65];
	char *previous = NULL, name[32], complete[66];
	char manifest_digest[65];
	uintmax_t size, actual_size;
	size_t i = 0, entries = 0;
	struct stat s;
	manifest = openat(snapshot, manifest_name, O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
	payload = directory(snapshot, "messages");
	if (manifest < 0 || payload < 0 || hash_copy(manifest, -1, actual, &actual_size)) goto done;
	strcpy(manifest_digest, actual);
	if (completed) {
		marker = openat(snapshot, "complete", O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
		if (marker < 0 || fstat(marker, &s) || !S_ISREG(s.st_mode) || s.st_size != 65 ||
			read(marker, complete, sizeof complete) != 65 || complete[64] != '\n') goto done;
		complete[64] = 0;
		if (!hexadecimal(complete) || strcmp(actual, complete)) goto done;
	}
	if (lseek(manifest, 0, SEEK_SET) < 0) goto done;
	file = fdopen(manifest, "rb"); if (!file) goto done;
	manifest = -1;
	if (read_field(file, text) || strcmp(text, "JMS1")) goto done;
	for (;;) {
		if (read_field(file, path)) goto done;
		if (!*path) break;
		if (valid_path(path) || (previous && strcmp(previous, path) >= 0) ||
			read_field(file, saved_flags) || strcmp(saved_flags, flags(path)) ||
			read_field(file, text) || number(text, &size) ||
			read_field(file, text) || !hexadecimal(text)) goto done;
		strcpy(expected, text);
		free(previous); previous = strdup(path); if (!previous) goto done;
		snprintf(name, sizeof name, "%zu", i);
		int fd = openat(payload, name, O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
		if (fd < 0) goto done;
		int status = hash_copy(fd, -1, actual, &actual_size) || fstat(fd, &s);
		close(fd);
		if (status || actual_size != size || strcmp(expected, actual)) goto done;
		if (records) {
			if (add(records, path, &s)) goto done;
			strcpy(records->items[records->count - 1].digest, actual);
		}
		i++;
	}
	if (read_field(file, text) || number(text, &size) || size != i || fgetc(file) != EOF || ferror(file)) goto done;
	{
		int fd = directory(payload, ".");
		DIR *dir = fd < 0 ? NULL : fdopendir(fd);
		struct dirent *entry;
		if (!dir) { if (fd >= 0) close(fd); goto done; }
		for (;;) {
			errno = 0; entry = readdir(dir);
			if (!entry) break;
			if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) continue;
			if (number(entry->d_name, &size) || size >= i) { closedir(dir); goto done; }
			entries++;
		}
		int error = errno; closedir(dir);
		if (error || entries != i) goto done;
	}
	if (hash_copy(fileno(file), -1, actual, &actual_size) || strcmp(actual, manifest_digest)) goto done;
	*count = i; rc = 0;
done:
	free(previous);
	if (file) fclose(file);
	if (manifest >= 0) close(manifest);
	if (marker >= 0) close(marker);
	if (payload >= 0) close(payload);
	return rc;
}

static int create(int source, const char *destination, size_t *count)
{
	struct inventory list = {0};
	char *operand = NULL, *name, *slash;
	char text[80], payload_name[32], digest[65];
	int parent = -1, target = -1, payload = -1, manifest = -1, marker = -1, rc = -1;
	uintmax_t size;
	size_t i, checked;
	struct stat before;
	if (!*destination || destination[strlen(destination) - 1] == '/') goto done;
	operand = strdup(destination); if (!operand) goto done;
	slash = strrchr(operand, '/');
	if (slash) {
		*slash = 0; name = slash + 1;
		parent = open_directory(*operand ? operand : "/");
	} else { name = operand; parent = open_directory("."); }
	if (!*name || !strcmp(name, ".") || !strcmp(name, "..") || parent < 0 || outside(parent, source) ||
		inventory(source, &list) || mkdirat(parent, name, 0700)) goto done;
	target = directory(parent, name);
	if (target < 0 || mkdirat(target, "messages", 0700)) goto done;
	payload = directory(target, "messages");
	manifest = openat(target, "manifest.pending", O_RDWR | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
	if (payload < 0 || manifest < 0 || field(manifest, "JMS1") || point("inventory-captured")) goto done;
	for (i = 0; i < list.count; i++) {
		struct message *message = &list.items[i];
		int input = open_message(source, message->path), output = -1, status = -1;
		snprintf(payload_name, sizeof payload_name, "%zu", i);
		if (input >= 0 && !fstat(input, &before) && same(&before, &message->original) && !point("source-read")) {
			output = openat(payload, payload_name, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
			if (output >= 0 && !hash_copy(input, output, message->digest, &size) &&
				!point("payload-sync") && !fsync(output)) status = 0;
		}
		if (input >= 0 && close(input)) status = -1;
		if (output >= 0 && close(output)) status = -1;
		if (status) goto done;
		snprintf(text, sizeof text, "%" PRIuMAX, size);
		if (field(manifest, message->path) || field(manifest, flags(message->path)) ||
			field(manifest, text) || field(manifest, message->digest)) goto done;
	}
	snprintf(text, sizeof text, "%zu", list.count);
	if (field(manifest, "") || field(manifest, text) || point("manifest-sync") || fsync(manifest) ||
		fsync(payload) || point("copied") || source_current(source, &list) ||
		verify(target, "manifest.pending", 0, &checked, NULL) || checked != list.count ||
		hash_copy(manifest, -1, digest, &size) || point("manifest-publish") ||
		linkat(target, "manifest.pending", target, "manifest", 0) ||
		unlinkat(target, "manifest.pending", 0) || fsync(target) || source_current(source, &list)) goto done;
	marker = openat(target, "complete.pending", O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
	snprintf(text, sizeof text, "%s\n", digest);
	if (marker < 0 || write_all(marker, text, strlen(text)) || fsync(marker) ||
		point("complete-publish") || linkat(target, "complete.pending", target, "complete", 0) ||
		point("complete-sync") || fsync(target) || fsync(parent)) goto done;
	if (unlinkat(target, "complete.pending", 0) || fsync(target)) goto done;
	*count = list.count; rc = 0;
done:
	/* Leave an explicitly incomplete private artifact for inspection on any
	 * error, including interrupted writes. Never recursively remove user paths. */
	if (rc && target >= 0) {
		if (unlinkat(target, "complete", 0) && errno != ENOENT) perror("snapshot: remove completion marker");
		if (unlinkat(target, "complete.pending", 0) && errno != ENOENT) perror("snapshot: remove pending marker");
		if (unlinkat(target, "manifest", 0) && errno != ENOENT) perror("snapshot: remove unpublished manifest");
		if (unlinkat(target, "manifest.pending", 0) && errno != ENOENT) perror("snapshot: remove pending manifest");
		if (fsync(target)) perror("snapshot: sync incomplete destination");
	}
	if (marker >= 0) close(marker);
	if (manifest >= 0) close(manifest);
	if (payload >= 0) close(payload);
	if (target >= 0) close(target);
	if (parent >= 0) close(parent);
	free(operand); release(&list); return rc;
}

static int empty_directory(int fd)
{
	int copy = directory(fd, "."), rc = -1;
	DIR *dir = copy < 0 ? NULL : fdopendir(copy);
	struct dirent *entry;
	if (!dir) { if (copy >= 0) close(copy); return -1; }
	for (;;) {
		errno = 0; entry = readdir(dir);
		if (!entry) { if (!errno) rc = 0; break; }
		if (strcmp(entry->d_name, ".") && strcmp(entry->d_name, "..")) break;
	}
	closedir(dir); return rc;
}

/* Only an absent name or an owned empty real directory may be replaced. The
 * final rename also refuses files, symlinks and populated directories. */
static int destination_ok(int parent, const char *name, struct stat *original, int *existed, int recheck)
{
	struct stat s;
	int fd, rc;
	if (fstatat(parent, name, &s, AT_SYMLINK_NOFOLLOW)) {
		if (errno != ENOENT || (recheck && *existed)) return -1;
		*existed = 0; return 0;
	}
	if (!S_ISDIR(s.st_mode) || s.st_uid != geteuid() ||
		(recheck && (!*existed || !same(&s, original)))) return -1;
	fd = directory(parent, name); if (fd < 0) return -1;
	rc = empty_directory(fd); close(fd);
	if (!rc && !recheck) { *original = s; *existed = 1; }
	return rc;
}

static int make_directory(int parent, const char *name)
{
	int fd;
	if (mkdirat(parent, name, 0700) && errno != EEXIST) return -1;
	fd = directory(parent, name);
	if (fd >= 0 && (fsync(fd) || fsync(parent))) { close(fd); return -1; }
	return fd;
}

/* Build only beneath our private stage. Every component was validated before
 * any destination mutation. Create all three Maildir directories at each leaf. */
static int restore_parent(int stage, const char *path)
{
	char *copy = strdup(path), *last, *subdir, *part, *next;
	int fd = -1, child;
	if (!copy) return -1;
	last = strrchr(copy, '/'); if (!last) goto done;
	*last = 0;
	subdir = strrchr(copy, '/');
	if (subdir) *subdir++ = 0;
	else subdir = copy;
	fd = directory(stage, ".");
	if (subdir != copy) {
		for (part = strtok_r(copy, "/", &next); fd >= 0 && part; part = strtok_r(NULL, "/", &next)) {
			child = make_directory(fd, part); close(fd); fd = child;
		}
	}
	if (fd < 0) goto done;
	const char *branches[] = {"new", "cur", "tmp"};
	for (size_t i = 0; i < 3; i++) {
		child = make_directory(fd, branches[i]);
		if (child < 0) { close(fd); fd = -1; goto done; }
		close(child);
	}
	child = directory(fd, subdir); close(fd); fd = child;
done: free(copy); return fd;
}

/* Cleanup never follows symlinks. The stage is owned by this invocation and
 * must have no other writers until restore returns. */
static int remove_contents(int fd)
{
	int copy = directory(fd, "."), rc = 0;
	DIR *dir = copy < 0 ? NULL : fdopendir(copy);
	struct dirent *entry;
	struct stat s;
	if (!dir) { if (copy >= 0) close(copy); return -1; }
	for (;;) {
		errno = 0; entry = readdir(dir);
		if (!entry) { if (errno) rc = -1; break; }
		if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) continue;
		if (fstatat(fd, entry->d_name, &s, AT_SYMLINK_NOFOLLOW)) { rc = -1; break; }
		if (S_ISDIR(s.st_mode)) {
			int child = directory(fd, entry->d_name);
			if (child < 0) { rc = -1; break; }
			int status = remove_contents(child); close(child);
			if (status || unlinkat(fd, entry->d_name, AT_REMOVEDIR)) { rc = -1; break; }
		} else if (unlinkat(fd, entry->d_name, 0)) { rc = -1; break; }
	}
	closedir(dir); return rc;
}

static int snapshot_current(int root, const struct inventory *records,
	const struct stat *manifest, const struct stat *marker)
{
	struct inventory now = {0};
	struct stat s;
	size_t count;
	int rc = -1;
	if (fstatat(root, "manifest", &s, AT_SYMLINK_NOFOLLOW) || !same(&s, manifest) ||
		fstatat(root, "complete", &s, AT_SYMLINK_NOFOLLOW) || !same(&s, marker) ||
		verify(root, "manifest", 1, &count, &now) || count != records->count) goto done;
	for (size_t i = 0; i < count; i++) {
		if (strcmp(now.items[i].path, records->items[i].path) ||
			strcmp(now.items[i].digest, records->items[i].digest) ||
			!same(&now.items[i].original, &records->items[i].original)) goto done;
	}
	rc = 0;
done: release(&now); return rc;
}

static int restore(int snapshot, const char *destination, size_t *count)
{
	struct inventory records = {0}, restored = {0};
	struct stat manifest_stat, marker_stat, original_dest, stage_stat, s;
	char *operand = NULL, *name = NULL, *slash, *mapped = NULL;
	char stage_name[80] = "", held[80], payload_name[32], digest[65];
	int parent = -1, stage = -1, payload = -1, existed = 0, published = 0, rc = -1;
	size_t captured, held_count = 0, candidate;
	uintmax_t size;
	/* No destination writes before complete verification. Cached records bind
	 * every subsequent copy to that exact verified message set. */
	if (verify(snapshot, "manifest", 1, &captured, &records) ||
		fstatat(snapshot, "manifest", &manifest_stat, AT_SYMLINK_NOFOLLOW) ||
		fstatat(snapshot, "complete", &marker_stat, AT_SYMLINK_NOFOLLOW) ||
		point("restore-verified") || !*destination || destination[strlen(destination) - 1] == '/') goto done;
	operand = strdup(destination); if (!operand) goto done;
	slash = strrchr(operand, '/');
	if (slash) { *slash = 0; name = slash + 1; parent = open_directory(*operand ? operand : "/"); }
	else { name = operand; parent = open_directory("."); }
	if (!*name || !strcmp(name, ".") || !strcmp(name, "..") || parent < 0 || outside(parent, snapshot) ||
		destination_ok(parent, name, &original_dest, &existed, 0)) goto done;
	for (unsigned attempt = 0; attempt < 64; attempt++) {
		snprintf(stage_name, sizeof stage_name, ".jaro-restore-%ld-%u", (long)getpid(), attempt);
		if (!mkdirat(parent, stage_name, 0700)) { stage = directory(parent, stage_name); break; }
		if (errno != EEXIST) goto done;
	}
	if (stage < 0 || fstat(stage, &stage_stat)) goto done;
	/* Reserve a container absent from all original top-level paths, so an
	 * already captured held-recovery folder cannot collide with quarantined mail. */
	for (candidate = 0; ; candidate++) {
		int used = 0;
		snprintf(held, sizeof held, ".jaro-recovered-outbox-%zu", candidate);
		for (size_t i = 0; i < records.count; i++) {
			if (!strncmp(records.items[i].path, held, strlen(held)) && records.items[i].path[strlen(held)] == '/') { used = 1; break; }
		}
		if (!used) break;
	}
	payload = directory(snapshot, "messages");
	if (payload < 0 || point("restore-staged")) goto done;
	for (size_t i = 0; i < records.count; i++) {
		struct message *message = &records.items[i];
		int input = -1, output = -1, destdir = -1, status = -1;
		const char *basename;
		if (!strncmp(message->path, "outbox/", 7)) {
			mapped = joined(held, message->path); held_count++;
		} else mapped = strdup(message->path);
		if (!mapped) goto done;
		basename = strrchr(mapped, '/') + 1;
		destdir = restore_parent(stage, mapped);
		snprintf(payload_name, sizeof payload_name, "%zu", i);
		input = openat(payload, payload_name, O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
		if (destdir >= 0 && input >= 0 && !fstat(input, &s) && same(&s, &message->original) && !point("restore-read")) {
			output = openat(destdir, basename, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
			if (output >= 0 && !hash_copy(input, output, digest, &size) &&
				!strcmp(digest, message->digest) && size == (uintmax_t)message->original.st_size &&
				!point("restore-file-sync") && !fsync(output) && !fstat(output, &s) &&
				!add(&restored, mapped, &s) && !fsync(destdir)) {
				strcpy(restored.items[restored.count - 1].digest, digest); status = 0;
			}
		}
		if (input >= 0 && close(input)) status = -1;
		if (output >= 0 && close(output)) status = -1;
		if (destdir >= 0) close(destdir);
		free(mapped); mapped = NULL;
		if (status) goto done;
	}
	if (restored.count) qsort(restored.items, restored.count, sizeof *restored.items, compare);
	if (point("restore-copied") || source_current(stage, &restored) ||
		snapshot_current(snapshot, &records, &manifest_stat, &marker_stat) ||
		fsync(stage) || fsync(parent) || point("restore-publish") ||
		source_current(stage, &restored) || snapshot_current(snapshot, &records, &manifest_stat, &marker_stat) ||
		destination_ok(parent, name, &original_dest, &existed, 1) ||
		renameat(parent, stage_name, parent, name)) goto done;
	published = 1;
	if (point("restore-directory-sync") || fsync(parent)) goto done;
	*count = captured; rc = 0;
	if (held_count) printf("snapshot: %zu outbox message(s) held under %s/outbox; inspect before explicitly requeueing\n", held_count, held);
done:
	if (rc && stage >= 0) {
		if (published) {
			/* Roll back only our published inode. No other writer may touch
			 * DEST until return; a conflicting name is never removed. */
			if (fstatat(parent, name, &s, AT_SYMLINK_NOFOLLOW) || s.st_dev != stage_stat.st_dev ||
				s.st_ino != stage_stat.st_ino || renameat(parent, name, parent, stage_name)) {
				fprintf(stderr, "snapshot: publication sync failed and rollback failed; inspect destination\n");
				goto close_all;
			}
			if (existed) {
				int replacement;
				if (mkdirat(parent, name, 0700) || (replacement = directory(parent, name)) < 0) {
					fprintf(stderr, "snapshot: cannot restore original empty destination\n");
					goto close_all;
				}
				if (fchmod(replacement, original_dest.st_mode & 07777) || fsync(replacement)) perror("snapshot: empty destination mode/sync");
				close(replacement);
			}
		}
		if (remove_contents(stage) || unlinkat(parent, stage_name, AT_REMOVEDIR) || fsync(parent))
			perror("snapshot: cannot remove private restore staging; inspect destination parent");
	}
close_all:
	if (payload >= 0) close(payload);
	if (stage >= 0) close(stage);
	if (parent >= 0) close(parent);
	free(mapped); free(operand); release(&records); release(&restored); return rc;
}

int main(int argc, char **argv)
{
	int root, rc;
	size_t count = 0;
	struct sigaction action;
	memset(&action, 0, sizeof action); action.sa_handler = stop; sigemptyset(&action.sa_mask);
	sigaction(SIGINT, &action, NULL); sigaction(SIGTERM, &action, NULL); sigaction(SIGHUP, &action, NULL);
#ifdef JARO_SNAPSHOT_TESTING
	sigaction(SIGUSR1, &action, NULL);
#endif
	umask(077);
	if (!((argc == 4 && (!strcmp(argv[1], "create") || !strcmp(argv[1], "restore"))) ||
		(argc == 3 && !strcmp(argv[1], "verify")))) {
		fprintf(stderr, "usage: jaro-snapshot create MAILROOT DEST | verify SNAPSHOT | restore SNAPSHOT DEST\n"); return 2;
	}
	root = open_directory(argv[2]);
	if (root < 0) { fprintf(stderr, "snapshot: cannot open root (symlinks are refused)\n"); return 1; }
	if (!strcmp(argv[1], "create")) rc = create(root, argv[3], &count);
	else if (!strcmp(argv[1], "restore")) rc = restore(root, argv[3], &count);
	else rc = verify(root, "manifest", 1, &count, NULL);
	close(root);
	/* After durable publication, a late signal must not relabel a complete
	 * artifact as a failed capture. Earlier interruptions make rc nonzero. */
	if (rc) {
		fprintf(stderr, "snapshot: %s failed: invalid paths, changed bytes or storage failure\n", argv[1]);
		return interrupted ? 128 + interrupted : 1;
	}
	printf("snapshot: %s verified %zu message(s)\n", argv[1], count);
	return 0;
}
