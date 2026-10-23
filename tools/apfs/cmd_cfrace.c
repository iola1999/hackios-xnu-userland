/*
 * cfrace: the volume's compressed files, read whole by several threads through one shared cfile.
 * apfs_tool.h, and they are built together by build-tool.sh.
 */

#include "apfs_tool.h"

/*
 * cfrace IMAGE N ROUNDS: the compressed files of the volume (up to 400, taken from a walk),
 * each read whole by N threads at once through one shared apfs_cfile -- as two readers of one
 * vnode share it in the kernel -- with the cache emptied before every round, so that setting
 * up the file and its read-ahead let go of the lock in the middle; every read must hash the
 * same as a read on one thread.
 */
struct cf_file {
	uint64_t           id;
	struct apfs_inode  ino;
	uint64_t           size;
	uint64_t           want;        /* the hash on one thread */
	struct apfs_cfile  cf;          /* shared by the threads, touched with the lock held */
};

static struct cf_file *cf_files;
static size_t cf_nfiles;
static struct apfs_vol *cf_vol;
static atomic_uint cf_bad, cf_reads;

static int
cf_collect_cb(void *ctx, const char *name, size_t name_len, uint64_t file_id, uint8_t d_type)
{
	struct apfs_inode ino;
	struct apfs_cfile cf;
	uint64_t size;

	(void)ctx;
	(void)name;
	(void)name_len;
	if (cf_nfiles >= 400) {
		return EEXIST;
	}
	if (d_type == APFS_DT_DIR) {
		return apfs_readdir(cf_vol, file_id, cf_collect_cb, NULL) == EEXIST ? EEXIST : 0;
	}
	if (d_type != APFS_DT_REG || apfs_inode_get(cf_vol, file_id, &ino) != 0 ||
	    apfs_inode_size(cf_vol, &ino, &size) != 0 || size < 3 * 65536) {
		return 0;
	}
	memset(&cf, 0, sizeof(cf));
	cf.held = APFS_NO_PIECE;
	if (apfs_cfile_init(cf_vol, &ino, &cf) == 0 && cf.compressed && cf.in_rsrc) {
		cf_files[cf_nfiles].id = file_id;
		cf_files[cf_nfiles].ino = ino;
		cf_files[cf_nfiles].size = size;
		cf_nfiles++;
	}
	apfs_cfile_fini(&cf);
	return 0;
}

static uint64_t
cf_read_whole(struct cf_file *f, struct apfs_cfile *cf, uint8_t *buf, size_t chunk, bool locked)
{
	uint64_t h = FNV_INIT, off = 0;

	while (off < f->size) {
		size_t got = 0;
		int error;

		if (locked) {
			op_begin();
		}
		error = apfs_read_file(cf_vol, &f->ino, cf, off, buf, chunk, &got);
		if (locked) {
			op_end();
		}
		if (error || got == 0) {
			return 0;
		}
		h = fnv1a(buf, got, h);
		off += got;
	}
	return h;
}

static void *
cf_thread(void *p)
{
	uint8_t *buf = malloc(65536);

	(void)p;
	for (size_t i = 0; i < cf_nfiles; i++) {
		uint64_t h = cf_read_whole(&cf_files[i], &cf_files[i].cf, buf, 65536, true);

		atomic_fetch_add(&cf_reads, 1);
		if (h != cf_files[i].want) {
			atomic_fetch_add(&cf_bad, 1);
			fprintf(stderr, "apfs-tool: cfrace: inode %" PRIu64 " read DIFFERENTLY\n", cf_files[i].id);
		}
	}
	free(buf);
	return NULL;
}

int
cmd_cfrace(const char *path, int n, int rounds)
{
	struct apfs_nx nx;
	struct apfs_vol vol;
	pthread_t th[16];
	uint8_t *buf = malloc(65536);

	want_lock = true;
	if (open_image(path, &nx, &vol) != 0 || buf == NULL || n < 2 || n > 16) {
		return 1;
	}
	cf_vol = &vol;
	cf_files = calloc(400, sizeof(*cf_files));
	op_begin();
	(void)apfs_readdir(&vol, APFS_ROOT_DIR_INO_NUM, cf_collect_cb, NULL);
	op_end();
	for (size_t i = 0; i < cf_nfiles; i++) {
		struct apfs_cfile cf;

		memset(&cf, 0, sizeof(cf));
		cf.held = APFS_NO_PIECE;
		cf_files[i].want = cf_read_whole(&cf_files[i], &cf, buf, 65536, true);
		op_begin();
		apfs_cfile_fini(&cf);
		op_end();
	}
	fprintf(stderr, "apfs-tool: cfrace: %zu compressed files of 192 KB or more, %d threads, %d rounds\n", cf_nfiles,
	    n, rounds);
	if (nx.parallel == NULL) {
		nx.parallel = host_parallel;    /* read-ahead needs helpers to spare */
		nx.parallel_max = 4;
	}
	for (int r = 0; r < rounds; r++) {
		pthread_mutex_lock(&host_lock);
		for (size_t i = 0; i < cf_nfiles; i++) {
			apfs_cfile_fini(&cf_files[i].cf);
		}
		apfs_nx_cache_flush(&nx);
		pthread_mutex_unlock(&host_lock);
		for (int t = 0; t < n; t++) {
			pthread_create(&th[t], NULL, cf_thread, NULL);
		}
		for (int t = 0; t < n; t++) {
			pthread_join(th[t], NULL);
		}
	}
	fprintf(stderr, "apfs-tool: cfrace: %u reads, %u different; metadata read without the lock %" PRIu64
	    " times, %" PRIu64 " of them already in; read ahead %" PRIu64 " times\n", atomic_load(&cf_reads),
	    atomic_load(&cf_bad), nx.unlocked_fills, nx.fill_races, nx.ra_batches);
	/* all given back, so that a leak checker sees only what the library lost */
	for (size_t i = 0; i < cf_nfiles; i++) {
		apfs_cfile_fini(&cf_files[i].cf);
	}
	free(cf_files);
	free(buf);
	{
		int fd = (int)(intptr_t)nx.dev.ctx;

		apfs_nx_close(&nx);
		close(fd);
	}
	return atomic_load(&cf_bad) != 0;
}
