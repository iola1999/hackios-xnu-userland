/*
 * namehash, lookupnames and lookups: the name hashing and the lookups it must serve.
 * apfs_tool.h, and they are built together by build-tool.sh.
 */

#include "apfs_tool.h"

/*
 * namehash IMAGE OUT: every directory record's name hashed the way the lookup hashes it,
 * against the hash APFS stored with it; and into OUT, for tools/apfs/test_namehash.py to work
 * out independently, a line per record: directory, the name in hex, the stored hash, the hash
 * as on a case-sensitive volume and as on a case-insensitive one, the file's id.
 */
struct nh_ctx {
	FILE    *out;
	bool     fold;
	uint64_t total, ascii, other, same, differ, unhashable;
};

static int
namehash_cb(void *ctx, uint64_t dir_id, const char *name, size_t name_len, uint32_t stored, uint64_t file_id)
{
	struct nh_ctx *c = ctx;
	uint32_t cs = 0, ci = 0, mine;
	bool ok_cs = apfs_name_hash(name, name_len, false, &cs), ok_ci = apfs_name_hash(name, name_len, true, &ci);
	bool ascii = true;

	for (size_t i = 0; i < name_len; i++) {
		ascii &= (uint8_t)name[i] < 0x80;
	}
	c->total++;
	if (ascii) {
		c->ascii++;
	} else {
		c->other++;
	}
	mine = c->fold ? ci : cs;
	if (!(c->fold ? ok_ci : ok_cs)) {
		c->unhashable++;
	} else if (mine == stored) {
		c->same++;
	} else {
		c->differ++;
		fprintf(stderr, "apfs-tool: namehash: dir %" PRIu64 ": %.*s: stored %06x, worked out %06x\n", dir_id,
		    (int)name_len, name, stored, mine);
	}
	if (c->out != NULL) {
		fprintf(c->out, "%" PRIu64 "\t", dir_id);
		for (size_t i = 0; i < name_len; i++) {
			fprintf(c->out, "%02x", (uint8_t)name[i]);
		}
		fprintf(c->out, "\t%06x\t%06x\t%06x\t%" PRIu64 "\n", stored, ok_cs ? cs : 0xffffffu, ok_ci ? ci : 0xffffffu,
		    file_id);
	}
	return 0;
}

int
cmd_namehash(const char *path, const char *out)
{
	struct apfs_nx nx;
	struct apfs_vol vol;
	struct nh_ctx c;
	int error;

	if (open_image(path, &nx, &vol) != 0) {
		return 1;
	}
	memset(&c, 0, sizeof(c));
	c.fold = (vol.incompat & APFS_INCOMPAT_CASE_INSENSITIVE) != 0;
	c.out = out != NULL ? fopen(out, "w") : NULL;
	error = apfs_dirrec_walk(&vol, namehash_cb, &c);
	if (c.out != NULL) {
		fclose(c.out);
	}
	fprintf(stderr, "apfs-tool: namehash: %s volume: %" PRIu64 " directory records (%" PRIu64 " plain ASCII, %" PRIu64
	    " not): hash as stored %" PRIu64 ", not %" PRIu64 ", cannot be hashed %" PRIu64 "%s\n",
	    c.fold ? "case-insensitive" : "case-sensitive", c.total, c.ascii, c.other, c.same, c.differ, c.unhashable,
	    error ? " (WALK FAILED)" : "");
	return (error || c.differ || c.unhashable) ? 1 : 0;
}

/*
 * lookupnames IMAGE LIST: a line of LIST is a directory, a name in hex and what looking it up
 * must give (an id, or "none"); each looked up the way the kernel's vnop_lookup does it.
 */
int
cmd_lookupnames(const char *path, const char *list)
{
	struct apfs_nx nx;
	struct apfs_vol vol;
	FILE *f = fopen(list, "r");
	char line[4096];
	uint64_t total = 0, right = 0, hashed0, scanned0;

	if (f == NULL || open_image(path, &nx, &vol) != 0) {
		return 1;
	}
	hashed0 = nx.lookups_hashed;
	scanned0 = nx.lookups_scanned;
	while (fgets(line, sizeof(line), f) != NULL) {
		char hex[2048], want[32], name[1024];
		uint64_t dir, id = 0;
		size_t n = 0;
		uint8_t dt;
		int error;

		if (sscanf(line, "%" SCNu64 " %2047s %31s", &dir, hex, want) != 3) {
			continue;
		}
		for (size_t i = 0; hex[2 * i] != 0 && hex[2 * i + 1] != 0 && n < sizeof(name); i++) {
			unsigned v;

			sscanf(hex + 2 * i, "%2x", &v);
			name[n++] = (char)v;
		}
		total++;
		error = apfs_lookup(&vol, dir, name, n, &id, &dt);
		if ((strcmp(want, "none") == 0 && error == ENOENT) ||
		    (strcmp(want, "none") != 0 && error == 0 && id == strtoull(want, NULL, 0))) {
			right++;
		} else if (total - right <= 20) {
			fprintf(stderr, "apfs-tool: lookupnames: dir %" PRIu64 " %s: want %s, got %s %" PRIu64 "\n", dir, hex, want,
			    error ? strerror(error) : "id", id);
		}
	}
	fclose(f);
	fprintf(stderr, "apfs-tool: lookupnames: %" PRIu64 " names, %" PRIu64 " as expected, %" PRIu64 " not; %" PRIu64
	    " found by the hash, %" PRIu64 " by reading the directory\n", total, right, total - right,
	    nx.lookups_hashed - hashed0, nx.lookups_scanned - scanned0);
	return right == total ? 0 : 1;
}

/*
 * Every name in the volume looked up by name, the way the kernel's vnop_lookup does it,
 * against what reading its directory gave; and each name with "#" after it, which must
 * not be found.  Also how long that took against finding each name by reading its
 * directory from the start, which is what apfs_lookup did before it went by the hash.
 */
struct lk_entry {
	char     name[256];
	size_t   len;
	uint64_t id;
	uint8_t  type;
};

struct lk_dir {
	struct lk_entry *e;
	size_t n, cap;
};

static int
lk_collect(void *ctx, const char *name, size_t name_len, uint64_t file_id, uint8_t d_type)
{
	struct lk_dir *d = ctx;

	if (d->n == d->cap) {
		size_t cap = d->cap ? d->cap * 2 : 64;
		struct lk_entry *e = realloc(d->e, cap * sizeof(*e));

		if (e == NULL) {
			return ENOMEM;
		}
		d->e = e;
		d->cap = cap;
	}
	if (name_len >= sizeof(d->e[0].name)) {
		return EIO;
	}
	memcpy(d->e[d->n].name, name, name_len);
	d->e[d->n].name[name_len] = '\0';
	d->e[d->n].len = name_len;
	d->e[d->n].id = file_id;
	d->e[d->n].type = d_type;
	d->n++;
	return 0;
}

struct lk_scan {
	const char *name;
	size_t      len;
	bool        found;
};

static int
lk_scan_cb(void *ctx, const char *name, size_t name_len, uint64_t file_id, uint8_t d_type)
{
	struct lk_scan *sc = ctx;

	(void)file_id;
	(void)d_type;
	if (name_len == sc->len && memcmp(name, sc->name, name_len) == 0) {
		sc->found = true;
		return EEXIST;
	}
	return 0;
}

static double
lk_now(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

int
cmd_lookups(const char *path)
{
	struct apfs_nx nx;
	struct apfs_vol vol;
	uint64_t *stack = NULL;
	size_t depth = 0, cap = 0;
	uint64_t dirs = 0, names = 0, good = 0, bad = 0, absent_good = 0, absent_bad = 0;
	double t_hash = 0, t_scan = 0;

	if (open_image(path, &nx, &vol) != 0) {
		return 1;
	}
	stack = malloc(sizeof(*stack) * 64);
	cap = 64;
	stack[depth++] = APFS_ROOT_DIR_INO_NUM;
	while (depth > 0) {
		uint64_t dir = stack[--depth];
		struct lk_dir d = { 0 };
		size_t i;
		int error = apfs_readdir(&vol, dir, lk_collect, &d);

		if (error) {
			fprintf(stderr, "apfs-tool: readdir %" PRIu64 ": %s\n", dir, strerror(error));
			bad++;
			continue;
		}
		dirs++;
		for (i = 0; i < d.n; i++) {
			struct lk_entry *e = &d.e[i];
			struct lk_scan sc = { e->name, e->len, false };
			char absent[260];
			uint64_t id = 0;
			uint8_t type = 0;
			double t0;

			names++;
			t0 = lk_now();
			error = apfs_lookup(&vol, dir, e->name, e->len, &id, &type);
			t_hash += lk_now() - t0;
			if (error == 0 && id == e->id && type == e->type) {
				good++;
			} else {
				bad++;
				if (bad <= 10) {
					fprintf(stderr, "apfs-tool: lookup %s in %" PRIu64 ": %d, %" PRIu64
					    " (readdir: %" PRIu64 ")\n", e->name, dir, error, id, e->id);
				}
			}
			t0 = lk_now();
			error = apfs_readdir(&vol, dir, lk_scan_cb, &sc);
			t_scan += lk_now() - t0;
			if (!sc.found) {
				bad++;
			}
			snprintf(absent, sizeof(absent), "%s#", e->name);
			error = apfs_lookup(&vol, dir, absent, e->len + 1, NULL, NULL);
			if (error == ENOENT) {
				absent_good++;
			} else {
				absent_bad++;
			}
			if (e->type == APFS_DT_DIR) {
				if (depth == cap) {
					cap *= 2;
					stack = realloc(stack, sizeof(*stack) * cap);
				}
				stack[depth++] = e->id;
			}
		}
		free(d.e);
	}
	printf("directories %" PRIu64 ", names %" PRIu64 ": found as readdir has them %" PRIu64
	    ", not %" PRIu64 "; with \"#\" after them not found %" PRIu64 ", found %" PRIu64 "\n",
	    dirs, names, good, bad, absent_good, absent_bad);
	printf("time for all of them: apfs_lookup %.0f ms, reading the directory from its start "
	    "%.0f ms\n", t_hash * 1000, t_scan * 1000);
	free(stack);
	apfs_nx_close(&nx);
	return (bad == 0 && absent_bad == 0) ? 0 : 1;
}
