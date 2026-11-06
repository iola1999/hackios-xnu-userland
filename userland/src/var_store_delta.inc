/*
 * var_store_delta.inc — Delta Slot incremental archive engine (64 MiB).
 *
 * Layout on /dev/rufs_dsu_store (4 GiB total):
 *   Slot 0 (Base A): [0x00000000, 0x80000000) (2 GiB)
 *   Slot 1 (Base B): [0x80000000, 0xFB000000) (~1.92 GiB)
 *   Delta Slot:      [0xFB000000, 0xFF000000) (64 MiB)
 *   KLOG Buffer:     [0xFF000000, 0x100000000) (16 MiB)
 */

#define DELTA_OFFSET        0xFB000000LL
#define DELTA_BYTES         (64u << 20)
#define DELTA_SLOTS         2
#define DELTA_SLOT_BYTES    (DELTA_BYTES / DELTA_SLOTS)
#define DELTA_PAYLOAD_LIMIT (DELTA_SLOT_BYTES - BLOCK)
#define DELTA_MAGIC         "RDWDEL\0"
#define DELTA_VERSION       1

struct delta_head {
	char     magic[8];          /* "RDWDEL\0" */
	uint32_t version;          /* 1 */
	uint32_t flags;            /* 0: active, 1: invalidated/merged */
	uint64_t base_generation;  /* Generation of Base Slot this delta applies to */
	uint64_t delta_seq;        /* Monotonically increasing sequence count */
	uint64_t bytes;            /* Payload byte count (excluding header) */
	uint64_t entries;          /* Number of changed entries */
	uint64_t seconds;          /* Timestamp of delta save */
	uint32_t crc;              /* CRC-32 of delta payload */
	uint32_t head_crc;         /* CRC-32 of delta_head */
};

static off_t
delta_slot_at(int dslot)
{
	return DELTA_OFFSET + (off_t)dslot * (off_t)DELTA_SLOT_BYTES;
}

static uint32_t
delta_head_checksum(const struct delta_head *dh)
{
	struct delta_head copy = *dh;
	copy.head_crc = 0;
	return crc_add(0xffffffffu, &copy, sizeof(copy)) ^ 0xffffffffu;
}

static int
delta_read_head_slot(int dslot, struct delta_head *dh)
{
	static uint8_t block[BLOCK];
	if (!device_read(delta_slot_at(dslot), block, BLOCK)) {
		return 0;
	}
	memcpy(dh, block, sizeof(*dh));
	if (memcmp(dh->magic, DELTA_MAGIC, sizeof(dh->magic)) != 0 ||
	    dh->version != DELTA_VERSION ||
	    dh->bytes > DELTA_PAYLOAD_LIMIT ||
	    dh->head_crc != delta_head_checksum(dh)) {
		return 0;
	}
	return 1;
}

static int
delta_newest(struct delta_head *out_dh)
{
	int best = -1;

	for (int dslot = 0; dslot < DELTA_SLOTS; dslot++) {
		struct delta_head dh;
		if (!delta_read_head_slot(dslot, &dh)) {
			continue;
		}
		if (best < 0 || dh.base_generation > out_dh->base_generation ||
		    (dh.base_generation == out_dh->base_generation && dh.delta_seq > out_dh->delta_seq)) {
			best = dslot;
			*out_dh = dh;
		}
	}
	return best;
}

static int
delta_read_head(struct delta_head *dh)
{
	return delta_newest(dh) >= 0;
}

static int
delta_reset(uint64_t new_base_generation)
{
	static uint8_t block[BLOCK];
	struct delta_head *dh = (struct delta_head *)block;
	if (slots <= 1) {
		return 1;
	}
	memset(block, 0, sizeof(block));
	memcpy(dh->magic, DELTA_MAGIC, sizeof(dh->magic));
	dh->version = DELTA_VERSION;
	dh->flags = 1; /* inactive / merged */
	dh->base_generation = new_base_generation;
	dh->head_crc = delta_head_checksum(dh);
	for (int dslot = 0; dslot < DELTA_SLOTS; dslot++) {
		if (!device_write(delta_slot_at(dslot), block, BLOCK)) {
			return 0;
		}
	}
	return 1;
}

static int
delta_is_near_full(void)
{
	struct delta_head dh;
	int temp_open = 0, near = 0;

	if (slots <= 1) {
		return 0;
	}
	if (fd < 0) {
		fd = open(DEVICE, O_RDONLY, 0);
		if (fd < 0) {
			return 0;
		}
		temp_open = 1;
	}
	if (delta_read_head(&dh) && dh.flags == 0) {
		near = dh.bytes >= (DELTA_SLOT_BYTES - (4u << 20)); /* within 4 MiB of 32 MiB slot limit */
	}
	if (temp_open) {
		close(fd);
		fd = -1;
	}
	return near;
}

static void
stream_start_delta_slot(int dslot, uint64_t bytes)
{
	at = delta_slot_at(dslot) + BLOCK;
	stream_limit = bytes != 0 ? at + (off_t)((bytes + BLOCK - 1) / BLOCK * BLOCK)
	                          : delta_slot_at(dslot) + (off_t)DELTA_SLOT_BYTES;
	buffered = taken = 0;
	stream_bytes = 0;
	stream_left = bytes;
	stream_crc = 0xffffffffu;
}

static int unpack_one(int make);
static int save(void);

struct base_reader {
	off_t    at;
	uint64_t left;
	uint64_t bytes;
	uint64_t have;
	uint64_t taken;
};

static uint8_t base_read_buf[65536];

static int
base_in(struct base_reader *br, void *dst, uint64_t n)
{
	uint8_t *p = dst;

	if (n > br->left) {
		return 0;
	}
	br->left -= n;
	br->bytes += n;
	while (n > 0) {
		uint64_t avail = br->have - br->taken;
		if (avail == 0) {
			uint64_t want = sizeof(base_read_buf);
			if (want > ((br->left + n + BLOCK - 1) / BLOCK * BLOCK)) {
				want = (br->left + n + BLOCK - 1) / BLOCK * BLOCK;
			}
			if (want == 0 || !device_read(br->at, base_read_buf, want)) {
				return 0;
			}
			br->at += (off_t)want;
			br->have = want;
			br->taken = 0;
			continue;
		}
		if (avail > n) {
			avail = n;
		}
		if (p != NULL) {
			memcpy(p, base_read_buf + br->taken, (size_t)avail);
			p += avail;
		}
		br->taken += avail;
		n -= avail;
	}
	return 1;
}

static int
pack_delete_entry(const char *relative, uint32_t pathlen)
{
	struct entry de;

	memset(&de, 0, sizeof(de));
	de.kind = ENT_DELETE;
	de.pathlen = pathlen;
	if (!out(&de, sizeof(de)) || !out(relative, pathlen) || !out_pad()) {
		return 0;
	}
	entries++;
	return 1;
}

/*
 * Scan the active Base Slot and emit ENT_DELETE tombstones for any entry that
 * existed in the base archive and has since been removed from /private/var.
 * Files and links are emitted at their entry; directories are emitted at
 * ENT_DIRTIME (post-order, after children have been unlinked).
 */
static int
delta_pack_deletes(int base_slot, const struct head *bh)
{
	struct base_reader br;
	static char rel[MAX_PATH];
	static char check_path[MAX_PATH];

	br.at = slot_at(base_slot) + BLOCK;
	br.left = bh->bytes;
	br.bytes = 0;
	br.have = br.taken = 0;

	for (uint64_t i = 0; i < bh->entries; i++) {
		struct entry e;
		uint64_t over;

		if (!base_in(&br, &e, sizeof(e)) || e.pathlen >= MAX_PATH || e.nxattr > 4096) {
			return 1;
		}
		if (!base_in(&br, rel, e.pathlen)) {
			return 1;
		}
		rel[e.pathlen] = 0;
		for (uint32_t x = 0; x < e.nxattr; x++) {
			uint32_t nlen = 0, vlen = 0;
			if (!base_in(&br, &nlen, sizeof(nlen)) || !base_in(&br, &vlen, sizeof(vlen)) ||
			    !base_in(&br, NULL, (uint64_t)nlen + (uint64_t)vlen)) {
				return 1;
			}
		}
		if ((e.kind == ENT_FILE || e.kind == ENT_SYMLINK || e.kind == ENT_HARDLINK) &&
		    e.size > 0 && !base_in(&br, NULL, e.size)) {
			return 1;
		}
		over = br.bytes % 8;
		if (over != 0 && !base_in(&br, NULL, 8 - over)) {
			return 1;
		}
		if (e.kind == ENT_FILE || e.kind == ENT_SYMLINK ||
		    e.kind == ENT_HARDLINK || e.kind == ENT_DIRTIME) {
			if (e.pathlen > 0 && sizeof(ROOT) + e.pathlen < sizeof(check_path)) {
				struct stat st;
				memcpy(check_path, ROOT "/", sizeof(ROOT));
				memcpy(check_path + sizeof(ROOT), rel, e.pathlen + 1);
				if (lstat(check_path, &st) != 0) {
					if (!pack_delete_entry(rel, e.pathlen)) {
						return 0;
					}
				}
			}
		}
	}
	return 1;
}

static int
delta_save(void)
{
	struct head base_h;
	struct delta_head prev_dh, dh;
	static uint8_t block[BLOCK];
	uint64_t delta_seq = 1;
	int best, prev_dslot, dslot;

	if (slots <= 1) {
		return 1;
	}
	set_background_qos();
	memset(&base_h, 0, sizeof(base_h));
	best = newest(&base_h);
	if (best < 0) {
		/* No base slot yet: fallback to full base save */
		return save();
	}

	memset(&prev_dh, 0, sizeof(prev_dh));
	prev_dslot = delta_newest(&prev_dh);
	if (prev_dslot >= 0 && prev_dh.base_generation == base_h.generation) {
		delta_seq = prev_dh.delta_seq + 1;
		dslot = (prev_dslot + 1) % DELTA_SLOTS;
	} else {
		dslot = 0;
	}

	/* Invalidate target delta slot header block before writing new delta payload */
	memset(block, 0, sizeof(block));
	if (!device_write(delta_slot_at(dslot), block, BLOCK)) {
		return 0;
	}

	stream_start_delta_slot(dslot, 0);
	entries = 0;
	last_saved_at = base_h.seconds;
	changed_entries = changed_bytes = 0;
	skipped_blocks = written_blocks = 0;
	arena_used = names_used = unsorted_dirs = 0;
	memset(links, 0, LINKS * sizeof(*links));
	skipped_kinds = skipped_big = 0;
	strcpy(path, ROOT);

	delta_mode = 1;
	int pack_ok = pack_tree("") && delta_pack_deletes(best, &base_h) && out_flush(1);
	delta_mode = 0;

	if (!pack_ok) {
		if (at + (off_t)buffered >= stream_limit || stream_bytes > DELTA_PAYLOAD_LIMIT) {
			printf("var_store: delta payload (%llu bytes) exceeds limit: merging base slot\n",
			    (unsigned long long)stream_bytes);
			return save();
		}
		return 0;
	}

	if (stream_bytes > DELTA_PAYLOAD_LIMIT) {
		printf("var_store: delta payload (%llu bytes) exceeds limit: merging base slot\n",
		    (unsigned long long)stream_bytes);
		return save();
	}

	memset(&dh, 0, sizeof(dh));
	memcpy(dh.magic, DELTA_MAGIC, sizeof(dh.magic));
	dh.version = DELTA_VERSION;
	dh.flags = 0; /* active */
	dh.base_generation = base_h.generation;
	dh.delta_seq = delta_seq;
	dh.bytes = stream_bytes;
	dh.entries = entries;
	{
		struct timeval tv = { 0, 0 };
		gettimeofday(&tv, NULL);
		dh.seconds = (uint64_t)tv.tv_sec;
	}
	dh.crc = stream_crc ^ 0xffffffffu;
	dh.head_crc = delta_head_checksum(&dh);

	memset(block, 0, sizeof(block));
	memcpy(block, &dh, sizeof(dh));
	if (!device_write(delta_slot_at(dslot), block, BLOCK)) {
		return 0;
	}

	printf("var_store: delta slot %d saved: seq %llu (base gen %llu), %llu entries, %llu bytes, crc %08x\n",
	    dslot, (unsigned long long)dh.delta_seq, (unsigned long long)dh.base_generation,
	    (unsigned long long)dh.entries, (unsigned long long)dh.bytes, dh.crc);
	klog_flush();
	return 1;
}

static int
delta_verify_slot(int dslot, const struct delta_head *dh)
{
	stream_start_delta_slot(dslot, dh->bytes);
	for (uint64_t i = 0; i < dh->entries; i++) {
		if (!unpack_one(0)) {
			return 0;
		}
	}
	return (stream_crc ^ 0xffffffffu) == dh->crc;
}

static int
delta_restore(uint64_t base_gen)
{
	struct delta_head dh;
	int dslot;

	if (slots <= 1) {
		return 1;
	}
	memset(&dh, 0, sizeof(dh));
	dslot = delta_newest(&dh);
	if (dslot < 0 || dh.flags != 0 || dh.base_generation != base_gen ||
	    dh.bytes == 0 || dh.bytes > DELTA_PAYLOAD_LIMIT) {
		return 1;
	}

	if (!delta_verify_slot(dslot, &dh)) {
		int other = (dslot + 1) % DELTA_SLOTS;
		struct delta_head other_dh;
		if (delta_read_head_slot(other, &other_dh) && other_dh.flags == 0 &&
		    other_dh.base_generation == base_gen && other_dh.bytes > 0 &&
		    delta_verify_slot(other, &other_dh)) {
			dslot = other;
			dh = other_dh;
		} else {
			printf("var_store: delta slot %d verify failed\n", dslot);
			return 0;
		}
	}

	printf("var_store: delta slot %d: seq %llu, %llu entries, %llu bytes, crc %08x\n",
	    dslot, (unsigned long long)dh.delta_seq, (unsigned long long)dh.entries,
	    (unsigned long long)dh.bytes, dh.crc);

	stream_start_delta_slot(dslot, dh.bytes);
	for (uint64_t i = 0; i < dh.entries; i++) {
		if (!unpack_one(1)) {
			printf("var_store: delta unpack failed at entry %llu\n", (unsigned long long)i);
			return 0;
		}
	}
	printf("var_store: delta applied: %llu entries updated\n",
	    (unsigned long long)dh.entries);
	return 1;
}
