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
#define DELTA_PAYLOAD_LIMIT (DELTA_BYTES - BLOCK)
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

static uint32_t
delta_head_checksum(const struct delta_head *dh)
{
	struct delta_head copy = *dh;
	copy.head_crc = 0;
	return crc_add(0xffffffffu, &copy, sizeof(copy)) ^ 0xffffffffu;
}

static int
delta_read_head(struct delta_head *dh)
{
	static uint8_t block[BLOCK];
	if (!device_read(DELTA_OFFSET, block, BLOCK)) {
		return 0;
	}
	memcpy(dh, block, sizeof(*dh));
	if (memcmp(dh->magic, DELTA_MAGIC, sizeof(dh->magic)) != 0 ||
	    dh->version != DELTA_VERSION ||
	    dh->head_crc != delta_head_checksum(dh)) {
		return 0;
	}
	return 1;
}

static int
delta_reset(uint64_t new_base_generation)
{
	static uint8_t block[BLOCK];
	struct delta_head *dh = (struct delta_head *)block;
	if (store_at == 0 || slots <= 1) {
		return 1;
	}
	memset(block, 0, sizeof(block));
	memcpy(dh->magic, DELTA_MAGIC, sizeof(dh->magic));
	dh->version = DELTA_VERSION;
	dh->flags = 1; /* inactive / merged */
	dh->base_generation = new_base_generation;
	dh->head_crc = delta_head_checksum(dh);
	return device_write(DELTA_OFFSET, block, BLOCK);
}

static int
delta_is_near_full(void)
{
	struct delta_head dh;
	if (delta_read_head(&dh) && dh.flags == 0) {
		return dh.bytes >= (DELTA_BYTES - (4u << 20)); /* within 4 MiB of 64 MiB limit (60 MiB) */
	}
	return 0;
}

static void
stream_start_delta(uint64_t bytes)
{
	at = DELTA_OFFSET + BLOCK;
	stream_limit = bytes != 0 ? at + (off_t)((bytes + BLOCK - 1) / BLOCK * BLOCK) : DELTA_OFFSET + DELTA_BYTES;
	buffered = taken = 0;
	stream_bytes = 0;
	stream_left = bytes;
	stream_crc = 0xffffffffu;
}

static int unpack_one(int make);
static int save(void);

static int
delta_save(void)
{
	struct head base_h;
	struct delta_head prev_dh, dh;
	static uint8_t block[BLOCK];
	uint64_t delta_seq = 1;
	int best;

	if (store_at == 0 || slots <= 1) {
		return 1;
	}
	set_background_qos();
	memset(&base_h, 0, sizeof(base_h));
	best = newest(&base_h);
	if (best < 0) {
		/* No base slot yet: fallback to full base save */
		return save();
	}

	if (delta_read_head(&prev_dh) && prev_dh.base_generation == base_h.generation) {
		delta_seq = prev_dh.delta_seq + 1;
	}

	/* Invalidate delta header block before writing new delta payload */
	memset(block, 0, sizeof(block));
	if (!device_write(DELTA_OFFSET, block, BLOCK)) {
		return 0;
	}

	stream_start_delta(0);
	entries = 0;
	last_saved_at = base_h.seconds;
	changed_entries = changed_bytes = 0;
	skipped_blocks = written_blocks = 0;
	arena_used = names_used = unsorted_dirs = 0;
	memset(links, 0, LINKS * sizeof(*links));
	skipped_kinds = skipped_big = 0;
	strcpy(path, ROOT);

	delta_mode = 1;
	int pack_ok = pack_tree("") && out_flush(1);
	delta_mode = 0;

	if (!pack_ok) {
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
	if (!device_write(DELTA_OFFSET, block, BLOCK)) {
		return 0;
	}

	printf("var_store: delta slot saved: seq %llu (base gen %llu), %llu entries, %llu bytes, crc %08x\n",
	    (unsigned long long)dh.delta_seq, (unsigned long long)dh.base_generation,
	    (unsigned long long)dh.entries, (unsigned long long)dh.bytes, dh.crc);
	klog_flush();
	return 1;
}

static int
delta_restore(uint64_t base_gen)
{
	struct delta_head dh;

	if (store_at == 0 || slots <= 1) {
		return 1;
	}
	if (!delta_read_head(&dh)) {
		return 1;
	}
	if (dh.flags != 0) {
		return 1;
	}
	if (dh.base_generation != base_gen) {
		printf("var_store: delta base gen %llu != base %llu: skipping delta\n",
		    (unsigned long long)dh.base_generation, (unsigned long long)base_gen);
		return 1;
	}
	if (dh.bytes == 0 || dh.bytes > DELTA_PAYLOAD_LIMIT) {
		return 1;
	}

	printf("var_store: delta slot: seq %llu, %llu entries, %llu bytes, crc %08x\n",
	    (unsigned long long)dh.delta_seq, (unsigned long long)dh.entries,
	    (unsigned long long)dh.bytes, dh.crc);

	/* Pass 0: check integrity */
	stream_start_delta(dh.bytes);
	for (uint64_t i = 0; i < dh.entries; i++) {
		if (!unpack_one(0)) {
			printf("var_store: delta verify failed at entry %llu\n", (unsigned long long)i);
			return 0;
		}
	}
	if ((stream_crc ^ 0xffffffffu) != dh.crc) {
		printf("var_store: delta CRC mismatch (%08x != %08x)\n",
		    stream_crc ^ 0xffffffffu, dh.crc);
		return 0;
	}

	/* Pass 1: apply unpacked entries */
	stream_start_delta(dh.bytes);
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
