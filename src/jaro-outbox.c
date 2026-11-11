/* Private SMTP recovery journal. All mutations require the caller's outbox
 * lock. Never infer SMTP non-delivery from its process exit status.
 *
 * A record directory is a durable claim on a Maildir basename (without its
 * :2, flags); identity binds it to SHA-256(message). Incomplete claims fail
 * closed. Payload + prepared precede inflight; inflight is fsynced before
 * transport. accepted precedes any archive/unlink. Every publication checks
 * both file fsync and parent-directory fsync, including existing directories.
 * A failed fsync may leave visible progress, but never authorizes transport.
 */
#define _POSIX_C_SOURCE 200809L
#include "filter/sha256.h"
#include <sys/stat.h>
#include <sys/types.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

struct record {
	int root, journal, directory, queue, source, claims;
	char id[65], digest[65], state[32];
	const char *name;
	struct stat original;
};

/* Fault points are absent from the installed binary. */
static int point(const char *name)
{
#ifdef JARO_OUTBOX_TESTING
	const char *fault = getenv("JARO_OUTBOX_FAULT");
	const char *crash = getenv("JARO_OUTBOX_CRASH");
	if (crash && !strcmp(crash, name)) {
		fprintf(stderr, "OUTBOX-TEST crash reached: %s\n", name);
		fflush(stderr); _exit(99);
	}
	if (fault && !strcmp(fault, name)) {
		fprintf(stderr, "OUTBOX-TEST fault reached: %s\n", name);
		errno = EIO; return -1;
	}
#else
	(void)name;
#endif
	return 0;
}

static int sync_at(int fd, const char *phase)
{
	return point(phase) || fsync(fd);
}

static int write_all(int fd, const void *data, size_t length)
{
	const char *p = data;
	while (length) {
		ssize_t n = write(fd, p, length);
		if (n < 0 && errno == EINTR) continue;
		if (n <= 0) return -1;
		p += n; length -= (size_t)n;
	}
	return 0;
}

static int regular(int fd, int private)
{
	struct stat s;
	return fstat(fd, &s) || !S_ISREG(s.st_mode) ||
		(private && (s.st_uid != geteuid() || (s.st_mode & 077) || s.st_nlink != 1)) ? -1 : 0;
}

static int directory(int parent, const char *name, int create, int private)
{
	int fd;
	struct stat s;
	if (create && mkdirat(parent, name, 0700) && errno != EEXIST) return -1;
	fd = openat(parent, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
	if (fd < 0) return -1;
	if (fstat(fd, &s) || (private && (s.st_uid != geteuid() || (s.st_mode & 077))) ||
		(create && sync_at(parent, "directory-sync"))) {
		close(fd); return -1;
	}
	return fd;
}

static int hash_fd(int fd, char digest[65], int copy)
{
	struct jf_sha256 hash;
	char buffer[32768];
	ssize_t n;
	if (lseek(fd, 0, SEEK_SET) < 0 || regular(fd, 0)) return -1;
	jf_sha256_init(&hash);
	while ((n = read(fd, buffer, sizeof buffer)) != 0) {
		if (n < 0) { if (errno == EINTR) continue; return -1; }
		jf_sha256_update(&hash, buffer, (size_t)n);
		if (copy >= 0 && write_all(copy, buffer, (size_t)n)) return -1;
	}
	jf_sha256_hex(&hash, digest);
	return 0;
}

static int same(const struct stat *a, const struct stat *b)
{
	return a->st_dev == b->st_dev && a->st_ino == b->st_ino && a->st_size == b->st_size &&
		a->st_mtim.tv_sec == b->st_mtim.tv_sec && a->st_mtim.tv_nsec == b->st_mtim.tv_nsec &&
		a->st_ctim.tv_sec == b->st_ctim.tv_sec && a->st_ctim.tv_nsec == b->st_ctim.tv_nsec;
}

static int queue_current(struct record *r)
{
	struct stat s, path;
	char digest[65];
	return fstat(r->source, &s) || !same(&s, &r->original) ||
		fstatat(r->queue, r->name, &path, AT_SYMLINK_NOFOLLOW) || !same(&s, &path) ||
		hash_fd(r->source, digest, -1) || strcmp(digest, r->digest) ? -1 : 0;
}

static int read_text(int parent, const char *name, char *text, size_t size)
{
	int fd = openat(parent, name, O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
	ssize_t n, extra;
	char tail;
	if (fd < 0) return -1;
	if (regular(fd, 1)) { close(fd); return -1; }
	n = read(fd, text, size - 1);
	extra = n >= 0 ? read(fd, &tail, 1) : -1;
	close(fd);
	if (n <= 0 || extra != 0 || text[n - 1] != '\n' || memchr(text, 0, (size_t)n - 1)) return -1;
	text[n - 1] = 0;
	return 0;
}

static int write_state(struct record *r, const char *state)
{
	char phase[80];
	int fd, rc = -1;
	/* Only our private, lock-protected staging name is removed on restart. */
	if (unlinkat(r->directory, "state.next", 0) && errno != ENOENT) return -1;
	fd = openat(r->directory, "state.next", O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
	if (fd < 0) return -1;
	if (write_all(fd, state, strlen(state)) || write_all(fd, "\n", 1)) goto done;
	snprintf(phase, sizeof phase, "%s-file-sync", state);
	if (sync_at(fd, phase)) goto done;
	snprintf(phase, sizeof phase, "%s-publish", state);
	if (point(phase) || renameat(r->directory, "state.next", r->directory, "state")) goto done;
	snprintf(phase, sizeof phase, "%s-directory-sync", state);
	if (sync_at(r->directory, phase)) goto done;
	snprintf(phase, sizeof phase, "%s-durable", state);
	if (point(phase)) goto done;
	strcpy(r->state, state);
	rc = 0;
done:
	if (close(fd)) rc = -1;
	return rc;
}

static int valid_state(const char *s)
{
	return !strcmp(s, "prepared") || !strcmp(s, "inflight") || !strcmp(s, "uncertain") ||
		!strcmp(s, "accepted") || !strcmp(s, "verified") || !strcmp(s, "complete");
}

static const char *outcome(const char *state)
{
	if (!strcmp(state, "prepared")) return "transport-not-started";
	if (!strcmp(state, "inflight")) return "interrupted-or-in-progress";
	if (!strcmp(state, "uncertain")) return "transport-unknown";
	if (!strcmp(state, "accepted")) return "smtp-accepted";
	if (!strcmp(state, "verified")) return "externally-verified";
	return "locally-archived";
}

static int load_record(struct record *r)
{
	char identity[96], actual[65];
	int payload;
	if (read_text(r->directory, "identity", identity, sizeof identity) ||
		strlen(identity) != 68 || strncmp(identity, "JO1 ", 4) ||
		strspn(identity + 4, "0123456789abcdef") != 64 ||
		read_text(r->directory, "state", r->state, sizeof r->state) || !valid_state(r->state)) return -1;
	strcpy(r->digest, identity + 4);
	payload = openat(r->directory, "message", O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
	if (payload < 0) return -1;
	if (regular(payload, 1) || hash_fd(payload, actual, -1) || strcmp(actual, r->digest)) {
		close(payload); return -1;
	}
	close(payload);
	return 0;
}

static int create_record(struct record *r)
{
	char identity[80];
	int payload = -1, meta = -1, rc = -1;
	if (point("claim-created")) goto done;
	payload = openat(r->directory, "message", O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
	if (payload < 0 || hash_fd(r->source, r->digest, payload) ||
		sync_at(payload, "payload-file-sync") || queue_current(r)) goto done;
	meta = openat(r->directory, "identity", O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
	snprintf(identity, sizeof identity, "JO1 %s\n", r->digest);
	if (meta < 0 || write_all(meta, identity, strlen(identity)) ||
		sync_at(meta, "identity-file-sync") || sync_at(r->directory, "payload-directory-sync") ||
		point("payload-durable") || write_state(r, "prepared")) goto done;
	rc = 0;
done:
	if (meta >= 0 && close(meta)) rc = -1;
	if (payload >= 0 && close(payload)) rc = -1;
	return rc;
}

static int matched_file(int parent, const char *name, const char *digest)
{
	char actual[65];
	int fd = openat(parent, name, O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC), rc;
	if (fd < 0) return -1;
	rc = hash_fd(fd, actual, -1) || strcmp(actual, digest) || fsync(fd);
	close(fd);
	return rc ? -1 : 0;
}

/* Recognize a deterministic sent copy even after a MUA moves it into cur and
 * changes Maildir flags. A conflicting entry is never overwritten. */
static int sent_copy(struct record *r, int sent, int newdir, int cur, int tmp)
{
	char name[96], actual[65];
	DIR *entries;
	struct dirent *entry;
	struct stat s;
	int fd = -1, payload = -1, found = 0, rc = -1;
	snprintf(name, sizeof name, "jaro-outbox-%s", r->id);
	if (!fstatat(newdir, name, &s, AT_SYMLINK_NOFOLLOW)) {
		if (matched_file(newdir, name, r->digest)) return -1;
		found = 1;
	} else if (errno != ENOENT) return -1;
	entries = fdopendir(dup(cur));
	if (!entries) return -1;
	errno = 0;
	while ((entry = readdir(entries))) {
		if (!strcmp(entry->d_name, name) ||
			(!strncmp(entry->d_name, name, strlen(name)) && !strncmp(entry->d_name + strlen(name), ":2,", 3))) {
			if (matched_file(cur, entry->d_name, r->digest)) { closedir(entries); return -1; }
			found = 1;
		}
		errno = 0;
	}
	if (errno) { closedir(entries); return -1; }
	closedir(entries);
	if (found) {
		if (sync_at(newdir, "sent-directory-sync") || fsync(cur) || fsync(sent)) return -1;
		if (unlinkat(tmp, name, 0) && errno != ENOENT) return -1;
		return fsync(tmp);
	}
	if (unlinkat(tmp, name, 0) && errno != ENOENT) return -1;
	fd = openat(tmp, name, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
	payload = openat(r->directory, "message", O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
	if (fd < 0 || payload < 0 || hash_fd(payload, actual, fd) || strcmp(actual, r->digest) ||
		sync_at(fd, "sent-file-sync") || point("sent-publish") || linkat(tmp, name, newdir, name, 0) ||
		sync_at(newdir, "sent-directory-sync") || point("sent-durable")) goto done;
	if (unlinkat(tmp, name, 0) || fsync(tmp) || fsync(sent)) goto done;
	rc = 0;
done:
	if (payload >= 0) close(payload);
	if (fd >= 0 && close(fd)) rc = -1;
	return rc;
}

static int finish(struct record *r)
{
	int sent = -1, newdir = -1, cur = -1, tmp = -1, rc = -1;
	if ((strcmp(r->state, "accepted") && strcmp(r->state, "verified") && strcmp(r->state, "complete")) || queue_current(r)) return -1;
	/* A prior invocation may have published a confirmation but failed its
	 * directory fsync. Re-establish durability before any local archive work. */
	if (sync_at(r->directory, "acceptance-recovery-sync")) return -1;
	sent = directory(r->root, "sent", 1, 0);
	if (sent < 0) goto done;
	newdir = directory(sent, "new", 1, 0);
	cur = directory(sent, "cur", 1, 0);
	tmp = directory(sent, "tmp", 1, 0);
	if (newdir < 0 || cur < 0 || tmp < 0 || sent_copy(r, sent, newdir, cur, tmp) ||
		queue_current(r) || write_state(r, "complete") || point("queue-unlink") ||
		unlinkat(r->queue, r->name, 0) || sync_at(r->queue, "queue-directory-sync") ||
		point("queue-removed")) goto done;
	rc = 0;
done:
	if (tmp >= 0) close(tmp);
	if (cur >= 0) close(cur);
	if (newdir >= 0) close(newdir);
	if (sent >= 0) close(sent);
	return rc;
}

static void close_record(struct record *r)
{
	if (r->source >= 0) close(r->source);
	if (r->queue >= 0) close(r->queue);
	if (r->directory >= 0) close(r->directory);
	if (r->journal >= 0) close(r->journal);
	if (r->claims >= 0) close(r->claims);
	if (r->root >= 0) close(r->root);
}

static int unattempted(struct record *r)
{
	struct stat s;
	int journal = directory(r->root, ".outbox-state", 0, 1), rc;
	if (journal < 0) return errno == ENOENT ? 0 : -1;
	rc = fstatat(journal, r->id, &s, AT_SYMLINK_NOFOLLOW);
	if (!rc || errno != ENOENT) rc = -1;
	else rc = 0;
	close(journal);
	return rc;
}

int main(int argc, char **argv)
{
	struct record r;
	struct jf_sha256 hash;
	char *identity = NULL, *flags;
	const char *op, *relative;
	int created = 0, rc = 1, inspect, source_needed;
	memset(&r, 0, sizeof r);
	r.root = r.journal = r.directory = r.queue = r.source = r.claims = -1;
	umask(077);
	if (argc != 4) { fprintf(stderr, "usage: jaro-outbox OP ROOT {new,cur}/NAME\n"); return 2; }
	op = argv[1]; relative = argv[3]; inspect = !strcmp(op, "inspect");
	if (strcmp(op, "prepare") && !inspect && strcmp(op, "inflight") && strcmp(op, "accepted") &&
		strcmp(op, "uncertain") && strcmp(op, "not-started") && strcmp(op, "finish") &&
		strcmp(op, "retry") && strcmp(op, "archive")) goto done;
	source_needed = !strcmp(op, "prepare") || inspect || !strcmp(op, "inflight") ||
		!strcmp(op, "finish") || !strcmp(op, "retry") || !strcmp(op, "archive");
	if (strncmp(relative, "new/", 4) && strncmp(relative, "cur/", 4)) goto done;
	r.name = relative + 4;
	if (!*r.name || strchr(r.name, '/') || !strcmp(r.name, ".") || !strcmp(r.name, "..")) goto done;
	identity = strdup(r.name);
	if (!identity) goto done;
	flags = strstr(identity, ":2,"); if (flags) *flags = 0;
	if (!*identity) goto done;
	jf_sha256_init(&hash); jf_sha256_update(&hash, identity, strlen(identity)); jf_sha256_hex(&hash, r.id);
	r.root = open(argv[2], O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
	if (r.root < 0) goto done;
	/* Acceptance must still be recorded if an external writer changed or
	 * removed the queue entry while SMTP was running. Cleanup checks it later. */
	if (source_needed) {
		int outbox = directory(r.root, "outbox", 0, 0);
		if (outbox < 0) goto done;
		r.queue = directory(outbox, relative[0] == 'n' ? "new" : "cur", 0, 0);
		close(outbox);
		if (r.queue < 0) goto done;
		r.source = openat(r.queue, r.name, O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
		if (r.source < 0 || regular(r.source, 0) || fstat(r.source, &r.original)) goto done;
	}
	{
		int outbox = directory(r.root, "outbox", 0, 0);
		int claim;
		struct stat s;
		if (outbox < 0) goto done;
		r.claims = directory(outbox, ".recovery-claims", !strcmp(op, "prepare"), 1);
		close(outbox);
		if (r.claims < 0) {
			if (inspect && errno == ENOENT && !unattempted(&r)) { printf("%s eligible no-attempt\n", r.id); rc = 0; }
			goto done;
		}
		claim = openat(r.claims, r.id, O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
		if (claim < 0 && errno == ENOENT && !strcmp(op, "prepare")) {
			/* Independent durable tombstones prevent a missing per-message
			 * journal directory from being mistaken for an unattempted item. */
			claim = openat(r.claims, r.id, O_RDWR | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
			created = 1;
		}
		if (claim < 0) {
			if (inspect && errno == ENOENT && !unattempted(&r)) { printf("%s eligible no-attempt\n", r.id); rc = 0; }
			goto done;
		}
		if (regular(claim, 1) || fstat(claim, &s) || s.st_size != 0 ||
			(!inspect && (sync_at(claim, "claim-file-sync") || sync_at(r.claims, "claim-directory-sync")))) {
			close(claim); goto done;
		}
		close(claim);
	}
	r.journal = directory(r.root, ".outbox-state", !strcmp(op, "prepare"), 1);
	if (r.journal < 0) {
		goto done;
	}
	if (created) {
		if (mkdirat(r.journal, r.id, 0700)) goto done;
	}
	r.directory = directory(r.journal, r.id, 0, 1);
	if (r.directory < 0) {
		goto done;
	}
	if (!inspect && sync_at(r.journal, "record-claim-sync")) goto done;
	if (!created && load_record(&r)) goto done;
	if (created && create_record(&r)) goto done;
	if (!strcmp(op, "prepare") || inspect) {
		if (queue_current(&r)) goto done;
		/* Re-establish checked durability after an earlier failed fsync. */
		if (!inspect && (fsync(r.source) || sync_at(r.queue, "queue-presend-sync") ||
			sync_at(r.directory, "record-directory-sync") || fsync(r.journal))) goto done;
		if (inspect) printf("%s %s %s\n", r.id, r.state, outcome(r.state));
		else printf("%s %s\n", r.id, r.state);
		rc = 0;
	} else if (!strcmp(op, "inflight") && !strcmp(r.state, "prepared")) {
		if (!queue_current(&r) && !write_state(&r, "inflight")) rc = 0;
	} else if ((!strcmp(op, "accepted") || !strcmp(op, "uncertain") || !strcmp(op, "not-started")) &&
		!strcmp(r.state, "inflight")) {
		if (!write_state(&r, !strcmp(op, "not-started") ? "prepared" : op)) rc = 0;
	} else if (!strcmp(op, "retry") && (!strcmp(r.state, "inflight") || !strcmp(r.state, "uncertain"))) {
		if (!queue_current(&r) && !write_state(&r, "prepared")) rc = 0;
	} else if (!strcmp(op, "archive")) {
		if (queue_current(&r)) goto done;
		if (!strcmp(r.state, "inflight") || !strcmp(r.state, "uncertain")) {
			if (write_state(&r, "verified")) goto done;
		}
		rc = finish(&r) ? 1 : 0;
	} else if (!strcmp(op, "finish")) rc = finish(&r) ? 1 : 0;
done:
	if (rc && inspect && r.source >= 0 && *r.id) printf("%s held invalid-state-or-content\n", r.id);
	if (rc) fprintf(stderr, "outbox: %s %s held: state, content or storage validation failed\n", r.id, argv[1]);
	free(identity); close_record(&r);
	return rc;
}
