/*
 * write, writable and the store commands: the paths that open the image for writing.
 * apfs_tool.h, and they are built together by build-tool.sh.
 */

#include "apfs_tool.h"

/*
 * Replace bytes inside a file of the image: the kernel's own apfs_write.c, run here where a
 * ruined image costs nothing.  The bytes come from a file or from standard input.
 * --fail-after N stops the device after N blocks, which is this program's power failure.
 */
int
cmd_write(const char *path, const char *in_path, const char *at_str, const char *from)
{
	struct apfs_nx nx;
	struct apfs_vol vol;
	struct apfs_write_report rep;
	uint64_t at = strtoull(at_str, NULL, 0);
	uint8_t *data = NULL;
	size_t len = 0, cap = 0;
	uint64_t id = 0;
	uint8_t type = 0;
	FILE *f;
	int error;

	f = strcmp(from, "-") == 0 ? stdin : fopen(from, "rb");
	if (f == NULL) {
		perror(from);
		return 1;
	}
	for (;;) {
		size_t n;

		if (len == cap) {
			cap = cap ? cap * 2 : 65536;
			data = realloc(data, cap);
			if (data == NULL) {
				fprintf(stderr, "apfs-tool: out of memory\n");
				return 1;
			}
		}
		n = fread(data + len, 1, cap - len, f);
		len += n;
		if (n == 0) {
			break;
		}
	}
	if (f != stdin) {
		fclose(f);
	}

	want_write = true;
	if (open_image(path, &nx, &vol) != 0) {
		return 1;
	}
	if (apfs_path_lookup(&vol, in_path, &id, &type) != 0) {
		fprintf(stderr, "apfs-tool: %s: no such path in the volume\n", in_path);
		return 1;
	}
	error = apfs_write_in_place(&vol, id, at, data, len, &rep);
	printf("%s: %zu bytes at %" PRIu64 " of %s (inode %" PRIu64 "): %s\n", path, len, at,
	    in_path, id, error == 0 ? "written" : (rep.why ? rep.why : strerror(error)));
	printf("  blocks %" PRIu64 " from %" PRIu64 ", of them patched whole %" PRIu64
	    ", writes %" PRIu64 "\n", rep.blocks, rep.first_block, rep.partial, rep.calls);
	apfs_nx_close(&nx);
	free(data);
	return error == 0 ? 0 : 1;
}

/* Would this volume take a write at all, and what does it say about itself? */
int
cmd_writable(const char *path)
{
	struct apfs_nx nx;
	struct apfs_vol vol;
	const char *why = NULL;
	int error;

	want_write = true;
	if (open_image(path, &nx, &vol) != 0) {
		return 1;
	}
	error = apfs_write_volume_ok(&vol, &why);
	printf("%s: volume %s: %s\n", path, vol.name, error == 0 ? "writable in place" : why);
	printf("  incompatible 0x%" PRIx64 "  snapshots %" PRIu64 "  integrity metadata %" PRIu64
	    "  extent-reference tree %s\n", vol.incompat, vol.num_snapshots, vol.integrity_meta_oid,
	    vol.extentref_oid ? "yes" : "no");
	printf("  checkpoint rings: descriptor %" PRIu64 "+%u, data %" PRIu64 "+%u; container omap %"
	    PRIu64 "\n", nx.xp_desc_base, nx.xp_desc_blocks, nx.xp_data_base, nx.xp_data_blocks,
	    nx.omap_addr);
	apfs_nx_close(&nx);
	return error == 0 ? 0 : 1;
}


/* ---- the store of fixed-length slot files (bsd/miscfs/apfs/apfs_store.c) ---- */

#define STORE_SCRATCH (1u << 20)

static int
store_open(const char *path, struct apfs_nx *nx, struct apfs_vol *vol, struct apfs_store *st,
    bool for_writing)
{
	const char *why = NULL;

	want_write = for_writing;
	if (open_image(path, nx, vol) != 0) {
		return 1;
	}
	if (apfs_store_open(vol, st, &why) != 0) {
		fprintf(stderr, "apfs-tool: %s is not a store: %s\n", path,
		    why != NULL ? why : "no reason given");
		return 1;
	}
	return 0;
}

int
cmd_store_info(const char *path)
{
	struct apfs_nx nx;
	struct apfs_vol vol;
	struct apfs_store st;
	struct apfs_store_head h;
	uint8_t *scratch;
	int slot, best;

	if (store_open(path, &nx, &vol, &st, false) != 0) {
		return 1;
	}
	{
		const char *why = NULL;

		printf("%s: store on volume %s: %d slots of %" PRIu64 " bytes, %" PRIu64
		    " of payload; a save here: %s\n", path, vol.name, APFS_STORE_SLOTS,
		    st.slot_bytes, st.payload_max,
		    apfs_store_writable(&st, &why) == 0 ? "would go through" : why);
	}
	for (slot = 0; slot < APFS_STORE_SLOTS; slot++) {
		if (apfs_store_head(&st, slot, &h) != 0) {
			printf("  slot %d (inode %" PRIu64 "): nothing good\n", slot,
			    st.slot_id[slot]);
			continue;
		}
		printf("  slot %d (inode %" PRIu64 "): generation %" PRIu64 ", %" PRIu64
		    " bytes, sum %016" PRIx64 ", note %" PRIu64 ", at %" PRIu64 "\n", slot,
		    st.slot_id[slot], h.generation, h.bytes, h.sum, h.note, h.seconds);
	}
	scratch = malloc(STORE_SCRATCH);
	best = scratch == NULL ? -1 : apfs_store_newest(&st, scratch, STORE_SCRATCH, &h);
	free(scratch);
	if (best < 0) {
		printf("  the newest good slot: none\n");
	} else {
		printf("  the newest good slot: %d, generation %" PRIu64 ", %" PRIu64 " bytes\n",
		    best, h.generation, h.bytes);
	}
	apfs_nx_close(&nx);
	return 0;
}

int
cmd_store_save(const char *path, const char *from, uint64_t note)
{
	struct apfs_nx nx;
	struct apfs_vol vol;
	struct apfs_store st;
	struct apfs_store_write w;
	uint8_t *data = NULL, *scratch;
	size_t len = 0, cap = 0;
	FILE *f = strcmp(from, "-") == 0 ? stdin : fopen(from, "rb");
	int error;

	if (f == NULL) {
		perror(from);
		return 1;
	}
	for (;;) {
		size_t n;

		if (len == cap) {
			cap = cap ? cap * 2 : (1u << 16);
			data = realloc(data, cap);
			if (data == NULL) {
				fprintf(stderr, "apfs-tool: out of memory\n");
				return 1;
			}
		}
		n = fread(data + len, 1, cap - len, f);
		len += n;
		if (n == 0) {
			break;
		}
	}
	if (f != stdin) {
		fclose(f);
	}
	if (store_open(path, &nx, &vol, &st, true) != 0) {
		return 1;
	}
	scratch = malloc(STORE_SCRATCH);
	if (scratch == NULL) {
		fprintf(stderr, "apfs-tool: out of memory\n");
		return 1;
	}
	error = apfs_store_save(&st, data, len, 0, note, scratch, STORE_SCRATCH, &w);
	printf("%s: %zu bytes into slot %d as generation %" PRIu64 ": %s; %" PRIu64 " blocks\n",
	    path, len, w.slot, w.generation, error == 0 ? "saved" : strerror(error), w.blocks);
	free(scratch);
	free(data);
	apfs_nx_close(&nx);
	return error == 0 ? 0 : 1;
}

int
cmd_store_load(const char *path, const char *to)
{
	struct apfs_nx nx;
	struct apfs_vol vol;
	struct apfs_store st;
	struct apfs_store_head h;
	uint8_t *scratch, *buf;
	FILE *f;
	int best, error;

	if (store_open(path, &nx, &vol, &st, false) != 0) {
		return 1;
	}
	scratch = malloc(STORE_SCRATCH);
	if (scratch == NULL) {
		fprintf(stderr, "apfs-tool: out of memory\n");
		return 1;
	}
	best = apfs_store_newest(&st, scratch, STORE_SCRATCH, &h);
	free(scratch);
	if (best < 0) {
		fprintf(stderr, "apfs-tool: %s: no slot holds anything good\n", path);
		return 1;
	}
	buf = malloc(h.bytes ? (size_t)h.bytes : 1);
	if (buf == NULL) {
		fprintf(stderr, "apfs-tool: out of memory\n");
		return 1;
	}
	error = h.bytes == 0 ? 0 : apfs_store_read(&st, best, 0, buf, (size_t)h.bytes);
	if (error) {
		fprintf(stderr, "apfs-tool: slot %d will not read: %s\n", best, strerror(error));
		return 1;
	}
	f = strcmp(to, "-") == 0 ? stdout : fopen(to, "wb");
	if (f == NULL) {
		perror(to);
		return 1;
	}
	if (h.bytes != 0 && fwrite(buf, 1, (size_t)h.bytes, f) != h.bytes) {
		perror(to);
		return 1;
	}
	if (f != stdout) {
		fclose(f);
	}
	fprintf(stderr, "%s: slot %d, generation %" PRIu64 ", %" PRIu64 " bytes, note %" PRIu64
	    "\n", path, best, h.generation, h.bytes, h.note);
	free(buf);
	apfs_nx_close(&nx);
	return 0;
}
