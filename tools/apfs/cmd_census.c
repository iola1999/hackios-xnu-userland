/*
 * census: one line per directory entry, walked on one thread or by many at once.
 * apfs_tool.h, and they are built together by build-tool.sh.
 */

#include "apfs_tool.h"

/* ---- census: one line per entry, for diffing against apfs.py ---- */

struct census {
	struct apfs_vol *vol;
	char            *path;          /* the directory being walked, with a trailing / */
	size_t           path_len;
	size_t           path_cap;
	uint8_t         *buf;
	long             max;
	long             seen;
	size_t           chunk;         /* how much each read asks for */
	bool             meta_only;
	int              error;
};

static int census_dir(struct census *cs, uint64_t dir_id);
static int census_parallel(struct apfs_nx *nx, struct apfs_vol *vol, uint64_t start, const char *where,
    size_t chunk, bool meta_only, int n);

static uint64_t
file_hash(struct census *cs, const struct apfs_inode *ino, uint64_t size)
{
	uint64_t h = FNV_INIT, off = 0;
	struct apfs_cfile cf;

	memset(&cf, 0, sizeof(cf));
	cf.held = APFS_NO_PIECE;
	for (;;) {
		size_t got = 0;
		int error = apfs_read_file(cs->vol, ino, &cf, off, cs->buf, cs->chunk, &got);

		if (error) {
			fprintf(stderr, "apfs-tool: %s: read at %" PRIu64 ": %s\n", cs->path,
			    off, strerror(error));
			cs->error = error;
			apfs_cfile_fini(&cf);
			return 0;
		}
		if (got == 0) {
			break;
		}
		h = fnv1a(cs->buf, got, h);
		off += got;
		if (off >= size) {
			break;
		}
	}
	apfs_cfile_fini(&cf);
	return h;
}

static int
census_cb(void *ctx, const char *name, size_t name_len, uint64_t file_id, uint8_t d_type)
{
	struct census *cs = ctx;
	struct apfs_inode ino;
	size_t saved = cs->path_len;
	uint64_t h = 0, size = 0;
	int error;

	if (cs->max > 0 && cs->seen >= cs->max) {
		return EEXIST;
	}
	/* append "/name" to the path we are at */
	while (cs->path_len + name_len + 2 > cs->path_cap) {
		cs->path_cap *= 2;
		cs->path = realloc(cs->path, cs->path_cap);
		if (cs->path == NULL) {
			return ENOMEM;
		}
	}
	cs->path[cs->path_len++] = '/';
	memcpy(cs->path + cs->path_len, name, name_len);
	cs->path_len += name_len;
	cs->path[cs->path_len] = '\0';

	error = apfs_inode_get(cs->vol, file_id, &ino);
	if (error) {
		printf("%s\t?\tno-inode\n", cs->path);
		cs->error = error;
		goto done;
	}
	cs->seen++;
	if (apfs_inode_size(cs->vol, &ino, &size) != 0) {
		size = ino.size;
	}
	if (!cs->meta_only && ((ino.mode & APFS_S_IFMT) == APFS_S_IFREG ||
	    (ino.mode & APFS_S_IFMT) == APFS_S_IFLNK)) {
		h = file_hash(cs, &ino, size);
	}
	printf("%s\t%u\t%06o\t%u\t%u\t%" PRIu64 "\t%016" PRIx64 "\n", cs->path, d_type,
	    ino.mode, ino.owner, ino.group, size, h);

	if (d_type == APFS_DT_DIR) {
		error = census_dir(cs, file_id);
		if (error) {
			cs->path_len = saved;
			cs->path[cs->path_len] = '\0';
			return error;
		}
	}
done:
	cs->path_len = saved;
	cs->path[cs->path_len] = '\0';
	return 0;
}

static int
census_dir(struct census *cs, uint64_t dir_id)
{
	return apfs_readdir(cs->vol, dir_id, census_cb, cs);
}

int
cmd_census(const char *path, const char *where, bool meta_only)
{
	struct apfs_nx nx;
	struct apfs_vol vol;
	struct census cs;
	uint64_t start = APFS_ROOT_DIR_INO_NUM;
	uint8_t dt = APFS_DT_DIR;
	int error;

	if (open_image(path, &nx, &vol) != 0) {
		return 1;
	}
	if (where != NULL) {
		int e;

		if (want_lock) {
			op_begin();
		}
		e = apfs_path_lookup(&vol, where, &start, &dt);
		if (want_lock) {
			op_end();
		}
		if (e != 0) {
			fprintf(stderr, "apfs-tool: %s: not found\n", where);
			return 1;
		}
	}
	memset(&cs, 0, sizeof(cs));
	cs.vol = &vol;
	cs.path_cap = 4096;
	cs.path = malloc(cs.path_cap);
	/*
	 * APFS_TOOL_CHUNK: reads of another size than 64 KB (say 4096, or 12345 and 200000,
	 * which never line up with a compressed file's 64 KB pieces), so that the roads through
	 * apfs_read_file the census does not otherwise take are checked over a whole volume too.
	 */
	cs.chunk = 65536;
	if (getenv("APFS_TOOL_CHUNK") != NULL) {
		long c = strtol(getenv("APFS_TOOL_CHUNK"), NULL, 0);

		if (c < 1 || c > 16 * 1024 * 1024) {
			fprintf(stderr, "apfs-tool: APFS_TOOL_CHUNK %ld is not 1..16M\n", c);
			return 2;
		}
		cs.chunk = (size_t)c;
	}
	cs.buf = malloc(cs.chunk);
	cs.max = 0;
	cs.meta_only = meta_only;
	if (cs.path == NULL || cs.buf == NULL) {
		return 1;
	}
	cs.path[0] = '\0';
	if (where != NULL) {
		size_t n = strlen(where);

		while (n > 0 && where[n - 1] == '/') {
			n--;
		}
		memcpy(cs.path, where, n);
		cs.path_len = n;
		cs.path[n] = '\0';
	}
	if (getenv("APFS_TOOL_WALKERS") != NULL && strtol(getenv("APFS_TOOL_WALKERS"), NULL, 0) > 1) {
		long n = strtol(getenv("APFS_TOOL_WALKERS"), NULL, 0);

		return census_parallel(&nx, &vol, start, where, cs.chunk, meta_only, (int)(n > 16 ? 16 : n));
	}
	error = census_dir(&cs, start);
	if (error && error != EEXIST) {
		fprintf(stderr, "apfs-tool: census: %s\n", strerror(error));
		return 1;
	}
	fprintf(stderr, "apfs-tool: %ld entries, %" PRIu64 " blocks read, %" PRIu64
	    " cache hits; %" PRIu64 " neighbours brought in, %" PRIu64 " of them asked for\n",
	    cs.seen, nx.reads, nx.hits, nx.prefetched, nx.prefetch_used);
	fprintf(stderr, "apfs-tool: inflate %" PRIu64 " calls, %" PRIu64 " bytes in, %" PRIu64
	    " bytes out, %" PRIu64 " by zlib\n", nx.inflate_calls, nx.inflate_in, nx.inflate_out,
	    nx.inflate_fallbacks);
	fprintf(stderr, "apfs-tool: %" PRIu64 " requests of whole pieces, %" PRIu64 " pieces in "
	    "them, on %u threads at most; read ahead %" PRIu64 " times, %" PRIu64 " pieces, %"
	    PRIu64 " of them used\n", nx.batch_calls, nx.batch_pieces,
	    nx.parallel != NULL ? nx.parallel_max : 1, nx.ra_batches, nx.ra_pieces, nx.ra_hits);
	fprintf(stderr, "apfs-tool: highest block touched %" PRIu64 " (%.2f GB in), of which "
	    "file contents reach %" PRIu64 " (%.2f GB in)\n", nx.max_addr,
	    nx.max_addr * (double)nx.block_size / 1e9, nx.max_data_addr,
	    nx.max_data_addr * (double)nx.block_size / 1e9);
	free(cs.path);
	free(cs.buf);
	apfs_nx_close(&nx);
	return cs.error ? 1 : 0;
}

/*
 * APFS_TOOL_WALKERS=N with census: N threads walk at once, taking directories off one shared
 * stack, each line hashed and the hashes added up (order does not matter): the same sum the
 * kernel's apfs_check prints. Lines are not printed.
 */
struct pjob {
	uint64_t dir_id;
	char    *path;
};

static pthread_mutex_t pmtx = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t pcond = PTHREAD_COND_INITIALIZER;
static struct pjob *pstack;
static size_t pcount, pcap;
static int pbusy;

struct pwalker {
	struct apfs_vol *vol;
	const char      *dir;           /* the directory being walked */
	uint8_t         *buf;
	size_t           chunk;
	bool             meta_only;
	uint64_t         sum, entries, bytes;
	int              error;
};

static void
ppush(uint64_t dir_id, const char *path)
{
	pthread_mutex_lock(&pmtx);
	if (pcount == pcap) {
		pcap = pcap ? 2 * pcap : 1024;
		pstack = realloc(pstack, pcap * sizeof(*pstack));
		if (pstack == NULL) {
			abort();
		}
	}
	pstack[pcount].dir_id = dir_id;
	pstack[pcount].path = strdup(path);
	pcount++;
	pthread_cond_signal(&pcond);
	pthread_mutex_unlock(&pmtx);
}

static uint64_t
pfile_hash(struct pwalker *w, const struct apfs_inode *ino, uint64_t size, const char *path)
{
	uint64_t h = FNV_INIT, off = 0;
	struct apfs_cfile cf;

	memset(&cf, 0, sizeof(cf));
	cf.held = APFS_NO_PIECE;
	for (;;) {
		size_t got = 0;
		int error = apfs_read_file(w->vol, ino, &cf, off, w->buf, w->chunk, &got);

		if (error) {
			fprintf(stderr, "apfs-tool: %s: read at %" PRIu64 ": %s\n", path, off, strerror(error));
			w->error = error;
			break;
		}
		if (got == 0) {
			break;
		}
		h = fnv1a(w->buf, got, h);
		off += got;
		w->bytes += got;
		if (off >= size) {
			break;
		}
	}
	apfs_cfile_fini(&cf);
	return h;
}

static int
pcensus_cb(void *ctx, const char *name, size_t name_len, uint64_t file_id, uint8_t d_type)
{
	struct pwalker *w = ctx;
	struct apfs_inode ino;
	uint64_t h = 0, size = 0;
	char path[4096], line[4096 + 128];
	int n;

	n = snprintf(path, sizeof(path), "%s/%.*s", w->dir, (int)name_len, name);
	if (n < 0 || (size_t)n >= sizeof(path)) {
		w->error = ENAMETOOLONG;
		return 0;
	}
	if (apfs_inode_get(w->vol, file_id, &ino) != 0) {
		fprintf(stderr, "apfs-tool: %s: no inode\n", path);
		w->error = ENOENT;
		return 0;
	}
	if (apfs_inode_size(w->vol, &ino, &size) != 0) {
		size = ino.size;
	}
	if (!w->meta_only && ((ino.mode & APFS_S_IFMT) == APFS_S_IFREG || (ino.mode & APFS_S_IFMT) == APFS_S_IFLNK)) {
		h = pfile_hash(w, &ino, size, path);
	}
	n = snprintf(line, sizeof(line), "%s\t%u\t%06o\t%u\t%u\t%" PRIu64 "\t%016" PRIx64 "\n", path, d_type,
	    ino.mode, ino.owner, ino.group, size, h);
	w->sum += fnv1a((const uint8_t *)line, (size_t)n, FNV_INIT);
	w->entries++;
	if (d_type == APFS_DT_DIR) {
		ppush(file_id, path);
	}
	return 0;
}

static void *
pwalker_thread(void *p)
{
	struct pwalker *w = p;

	for (;;) {
		struct pjob j;
		int error;

		pthread_mutex_lock(&pmtx);
		while (pcount == 0 && pbusy > 0) {
			pthread_cond_wait(&pcond, &pmtx);
		}
		if (pcount == 0) {
			pthread_cond_broadcast(&pcond);
			pthread_mutex_unlock(&pmtx);
			return NULL;
		}
		j = pstack[--pcount];
		pbusy++;
		pthread_mutex_unlock(&pmtx);

		w->dir = j.path;
		op_begin();
		error = apfs_readdir(w->vol, j.dir_id, pcensus_cb, w);
		op_end();
		if (error) {
			fprintf(stderr, "apfs-tool: %s: readdir: %s\n", j.path, strerror(error));
			w->error = error;
		}
		free(j.path);

		pthread_mutex_lock(&pmtx);
		pbusy--;
		pthread_cond_broadcast(&pcond);
		pthread_mutex_unlock(&pmtx);
	}
}

static int
census_parallel(struct apfs_nx *nx, struct apfs_vol *vol, uint64_t start, const char *where, size_t chunk,
    bool meta_only, int n)
{
	struct pwalker w[16];
	pthread_t th[16];
	uint64_t sum = 0, entries = 0, bytes = 0;
	int error = 0;

	ppush(start, where != NULL ? where : "");
	for (int i = 0; i < n; i++) {
		memset(&w[i], 0, sizeof(w[i]));
		w[i].vol = vol;
		w[i].chunk = chunk;
		w[i].meta_only = meta_only;
		w[i].buf = malloc(chunk);
		if (w[i].buf == NULL || pthread_create(&th[i], NULL, pwalker_thread, &w[i]) != 0) {
			abort();
		}
	}
	for (int i = 0; i < n; i++) {
		pthread_join(th[i], NULL);
		sum += w[i].sum;
		entries += w[i].entries;
		bytes += w[i].bytes;
		if (w[i].error) {
			error = w[i].error;
		}
		free(w[i].buf);
	}
	fprintf(stderr, "apfs-tool: %d walkers: %" PRIu64 " entries, %" PRIu64 " content bytes, line-hash sum %016" PRIx64
	    ", %s\n", n, entries, bytes, sum, error ? "ERRORS" : "no errors");
	fprintf(stderr, "apfs-tool: metadata read without the lock %" PRIu64 " times, %" PRIu64 " of them already in; "
	    "retired blocks %" PRIu64 " now, %" PRIu64 " at most\n", nx->unlocked_fills, nx->fill_races, nx->retired_now,
	    nx->retired_most);
	free(pstack);
	{
		int fd = (int)(intptr_t)nx->dev.ctx;

		apfs_nx_close(nx);
		close(fd);
	}
	return error ? 1 : 0;
}
