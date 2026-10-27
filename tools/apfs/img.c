/*
 * The image as a device, the power failure stood in for, and the kernel's lock.
 * apfs_tool.h, and they are built together by build-tool.sh.
 */

#include "apfs_tool.h"

/* ---- the image as a device ---- */

static int
file_read(void *ctx, uint64_t off, void *buf, size_t len)
{
	int fd = (int)(intptr_t)ctx;
	uint8_t *p = buf;

	while (len > 0) {
		ssize_t n = pread(fd, p, len, (off_t)off);

		if (n <= 0) {
			return EIO;
		}
		p += n;
		off += (uint64_t)n;
		len -= (size_t)n;
	}
	return 0;
}

/* The same hash the Python side computes, so contents can be compared without the bytes. */
uint64_t
fnv1a(const uint8_t *p, size_t n, uint64_t h)
{
	size_t i;

	for (i = 0; i < n; i++) {
		h ^= p[i];
		h *= 0x100000001b3ULL;
	}
	return h;
}

#define FNV_INIT 0xcbf29ce484222325ULL

/*
 * APFS_TOOL_THREADS=N: nx->parallel on threads of this program, so that what the kernel
 * does on several processors -- whole pieces of one request read and decompressed at once
 * -- runs here the same way (and can be run under ThreadSanitizer).  A thread per worker
 * per call: slow, and only for checking.
 */
struct host_par {
	int (*fn)(void *arg, uint32_t item, uint32_t worker);
	void *arg;
	uint32_t items;
	atomic_uint next;
	atomic_int error;
};

struct host_par_arg {
	struct host_par *hp;
	uint32_t worker;
};

static void
host_par_work(struct host_par *hp, uint32_t worker)
{
	for (;;) {
		uint32_t item = atomic_fetch_add(&hp->next, 1);
		int error, none = 0;

		if (item >= hp->items) {
			break;
		}
		error = hp->fn(hp->arg, item, worker);
		if (error != 0) {
			atomic_compare_exchange_strong(&hp->error, &none, error);
		}
	}
}

static void *
host_par_thread(void *p)
{
	struct host_par_arg *a = p;

	host_par_work(a->hp, a->worker);
	return NULL;
}

int
host_parallel(void *ctx, uint32_t items, uint32_t workers,
    int (*fn)(void *arg, uint32_t item, uint32_t worker), void *arg)
{
	struct host_par hp;
	struct host_par_arg a[16];
	pthread_t th[16];
	uint32_t w, started = 1;

	(void)ctx;
	hp.fn = fn;
	hp.arg = arg;
	hp.items = items;
	atomic_init(&hp.next, 0);
	atomic_init(&hp.error, 0);
	for (w = 1; w < workers && w < 16; w++) {
		a[w].hp = &hp;
		a[w].worker = w;
		if (pthread_create(&th[w], NULL, host_par_thread, &a[w]) != 0) {
			break;
		}
		started++;
	}
	host_par_work(&hp, 0);
	for (w = 1; w < started; w++) {
		pthread_join(th[w], NULL);
	}
	return atomic_load(&hp.error);
}

bool want_lock;                 /* the library under a lock, as in the kernel (walkers, cfrace) */
static void host_locking(struct apfs_nx *nx);

/*
 * The image opened for writing, and the power failure stood in for: after `write_fail_after`
 * blocks the device refuses every write, as a phone whose battery came out mid-write would.
 * Without it nothing refuses; --fail-after 0 refuses the very first block.
 */
bool want_write;
bool write_fails;                /* --fail-after was given; 0 of them is a failure too */
bool write_corrupts;             /* --corrupt-after: a device that lies about a write */
uint64_t write_fail_after;
static uint64_t writes_done;

static int
file_write(void *ctx, uint64_t off, const void *buf, size_t len)
{
	int fd = (int)(intptr_t)ctx;
	const uint8_t *p = buf;

	uint8_t *copy = NULL;

	if (write_fails && writes_done >= write_fail_after) {
		return EIO;
	}
	if (write_corrupts && writes_done >= write_fail_after) {
		/*
		 * A device that says the write went through and puts something else on the
		 * platter: the one failure no checking before the write can catch, and what the
		 * read-back comparison in apfs_write.c is there for.
		 */
		copy = malloc(len);
		if (copy == NULL) {
			return EIO;
		}
		memcpy(copy, p, len);
		copy[0] ^= 0xff;
		p = copy;
	}
	writes_done++;
	while (len > 0) {
		ssize_t n = pwrite(fd, p, len, (off_t)off);

		if (n <= 0) {
			free(copy);
			return EIO;
		}
		p += n;
		off += (uint64_t)n;
		len -= (size_t)n;
	}
	free(copy);
	return 0;
}

int
open_image(const char *path, struct apfs_nx *nx, struct apfs_vol *vol)
{
	struct apfs_dev dev;
	int fd = open(path, want_write ? O_RDWR : O_RDONLY);
	uint32_t i;
	int error;

	if (fd < 0) {
		perror(path);
		return 1;
	}
	memset(&dev, 0, sizeof(dev));
	dev.ctx = (void *)(intptr_t)fd;
	dev.size = (uint64_t)lseek(fd, 0, SEEK_END);
	dev.read = file_read;
	dev.read_data = file_read;      /* pread is as good without a lock as with one */
	if (want_write) {
		dev.write = file_write;
	}

	error = apfs_nx_open(nx, &dev);
	if (error) {
		fprintf(stderr, "apfs-tool: cannot read the container: %s\n", strerror(error));
		return 1;
	}
	if (getenv("APFS_TOOL_THREADS") != NULL) {
		long t = strtol(getenv("APFS_TOOL_THREADS"), NULL, 0);

		if (t > 1) {
			nx->parallel = host_parallel;
			nx->parallel_max = (uint32_t)(t < 16 ? t : 16);
		}
	}
	if (getenv("APFS_TOOL_WALKERS") != NULL && strtol(getenv("APFS_TOOL_WALKERS"), NULL, 0) > 1) {
		want_lock = true;
	}
	if (want_lock) {
		/* handed over now, as the kernel's mount does; from here on every call is with the lock */
		host_locking(nx);
		op_begin();
	}
	/* the first volume that has a root directory, like apfs.py picks it; or APFS_TOOL_VOLUME=N */
	for (i = 0; i < APFS_NX_MAX_FILE_SYSTEMS; i++) {
		if (getenv("APFS_TOOL_VOLUME") != NULL && (long)i != strtol(getenv("APFS_TOOL_VOLUME"), NULL, 0)) {
			continue;
		}
		struct apfs_inode ino;

		if (nx->fs_oid[i] == 0) {
			continue;
		}
		if (apfs_vol_open(nx, i, vol) != 0) {
			continue;
		}
		if (apfs_inode_get(vol, APFS_ROOT_DIR_INO_NUM, &ino) == 0) {
			if (want_lock) {
				op_end();
			}
			return 0;
		}
	}
	if (want_lock) {
		op_end();
	}
	fprintf(stderr, "apfs-tool: no readable volume with a root directory\n");
	return 1;
}
/*
 * ---- several threads on the library at once, as in the kernel ----
 *
 * The kernel holds one lock round apfs_lib for each operation and lets apfs_lib let go of it
 * in the middle (reading file contents, and a block of metadata not in the cache); a block
 * leaving the cache keeps its memory until no operation that began before can be reading it
 * (apfs_nx_reclaim). The same here, with a pthread mutex, so that it can be run under
 * ThreadSanitizer: op_begin/op_end are the kernel's apfs_lock/apfs_unlock.
 */
pthread_mutex_t host_lock;      /* error checking: unlocking one not held aborts, as the kernel panics */
static struct apfs_nx *host_nx;

static void
host_lock_check(int error, const char *what)
{
	if (error != 0) {
		fprintf(stderr, "apfs-tool: %s the library's lock: %s\n", what, strerror(error));
		abort();
	}
}
#define HOST_OPS 64
static struct {
	pthread_t th;
	uint64_t  epoch;
	bool      used;
} host_ops[HOST_OPS];

static void
host_unlock(void *ctx)
{
	(void)ctx;
	host_lock_check(pthread_mutex_unlock(&host_lock), "letting go of");
}

static void
host_relock(void *ctx)
{
	(void)ctx;
	host_lock_check(pthread_mutex_lock(&host_lock), "taking");
}

void
op_begin(void)
{
	host_lock_check(pthread_mutex_lock(&host_lock), "taking");
	for (int i = 0; i < HOST_OPS; i++) {
		if (!host_ops[i].used) {
			host_ops[i].used = true;
			host_ops[i].th = pthread_self();
			host_ops[i].epoch = host_nx->epoch;
			return;
		}
	}
	fprintf(stderr, "apfs-tool: more operations at once than %d\n", HOST_OPS);
	abort();
}

void
op_end(void)
{
	uint64_t oldest = host_nx->epoch;

	for (int i = 0; i < HOST_OPS; i++) {
		if (host_ops[i].used && pthread_equal(host_ops[i].th, pthread_self())) {
			host_ops[i].used = false;
		} else if (host_ops[i].used && host_ops[i].epoch < oldest) {
			oldest = host_ops[i].epoch;
		}
	}
	apfs_nx_reclaim(host_nx, oldest);
	host_lock_check(pthread_mutex_unlock(&host_lock), "letting go of");
}

static void
host_locking(struct apfs_nx *nx)
{
	pthread_mutexattr_t a;

	pthread_mutexattr_init(&a);
	pthread_mutexattr_settype(&a, PTHREAD_MUTEX_ERRORCHECK);
	pthread_mutex_init(&host_lock, &a);
	host_nx = nx;
	nx->unlock = host_unlock;
	nx->relock = host_relock;
	nx->lock_ctx = NULL;
}
