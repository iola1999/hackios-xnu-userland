/*
 * Cross-reboot kernel log persistence in UFS dsu_store tail (0xFF000000).
 */

#define KLOG_OFFSET     0xFF000000LL
#define KLOG_RESERVE_SZ (16u << 20)
#define KLOG_MAX_BYTES  (KLOG_RESERVE_SZ - BLOCK)
#define KLOG_MAGIC      "RDWKLOG\0"
#define KLOG_VERSION    1
#define LAST_BOOT_LOG   "/private/var/db/last-boot.log"

struct klog_head {
	char     magic[8];
	uint32_t version;
	uint32_t reserved;
	uint64_t bytes;
	uint64_t seconds;
	uint32_t crc;
	uint32_t head_crc;
};

static uint32_t
klog_head_checksum(const struct klog_head *h)
{
	struct klog_head copy = *h;
	copy.head_crc = 0;
	return crc_add(0xffffffffu, &copy, sizeof(copy)) ^ 0xffffffffu;
}

static void
klog_restore(void)
{
	static uint8_t head_buf[BLOCK];
	struct klog_head *kh = (struct klog_head *)head_buf;

	if (store_at != 0 && slots > 1) {
		if (!device_read(KLOG_OFFSET, head_buf, BLOCK)) {
			return;
		}
		if (memcmp(kh->magic, KLOG_MAGIC, sizeof(kh->magic)) != 0 ||
		    kh->version != KLOG_VERSION ||
		    kh->head_crc != klog_head_checksum(kh) ||
		    kh->bytes == 0 || kh->bytes > KLOG_MAX_BYTES) {
			return;
		}
		uint8_t *log_buf = mmap(NULL, (size_t)kh->bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
		if (log_buf == MAP_FAILED) {
			return;
		}
		if (device_read(KLOG_OFFSET + BLOCK, log_buf, kh->bytes)) {
			uint32_t crc = crc_add(0xffffffffu, log_buf, kh->bytes) ^ 0xffffffffu;
			if (crc == kh->crc) {
				mkdir("/private/var/db", 0755);
				int out_fd = open(LAST_BOOT_LOG, O_WRONLY | O_CREAT | O_TRUNC, 0644);
				if (out_fd >= 0) {
					write(out_fd, log_buf, (size_t)kh->bytes);
					close(out_fd);
					printf("var_store: restored %llu bytes of kernel log to %s\n",
					    (unsigned long long)kh->bytes, LAST_BOOT_LOG);
				}
			}
		}
		munmap(log_buf, (size_t)kh->bytes);
	}
}

static void
klog_flush(void)
{
	static uint8_t piece[64 * 1024];
	static uint8_t head_buf[BLOCK];
	struct klog_head *kh = (struct klog_head *)head_buf;
	ssize_t got = 0;
	int kfd;

	if (store_at == 0 || slots <= 1) {
		return;
	}

	kfd = open("/dev/klog", O_RDONLY | O_NONBLOCK, 0);
	if (kfd < 0) {
		return;
	}
	got = read(kfd, piece, sizeof(piece));
	close(kfd);

	if (got <= 0) {
		return;
	}

	memset(head_buf, 0, sizeof(head_buf));
	if (device_read(KLOG_OFFSET, head_buf, BLOCK) &&
	    memcmp(kh->magic, KLOG_MAGIC, sizeof(kh->magic)) == 0 &&
	    kh->version == KLOG_VERSION &&
	    kh->head_crc == klog_head_checksum(kh) &&
	    kh->bytes + (uint64_t)got <= KLOG_MAX_BYTES) {
		/* Append to existing log */
		off_t write_pos = KLOG_OFFSET + BLOCK + (off_t)kh->bytes;
		if (device_write(write_pos, piece, (uint64_t)got)) {
			kh->crc = crc_add(kh->crc ^ 0xffffffffu, piece, (uint64_t)got) ^ 0xffffffffu;
			kh->bytes += (uint64_t)got;
			struct timeval tv = { 0, 0 };
			gettimeofday(&tv, NULL);
			kh->seconds = (uint64_t)tv.tv_sec;
			kh->head_crc = klog_head_checksum(kh);
			device_write(KLOG_OFFSET, head_buf, BLOCK);
		}
	} else {
		/* Start fresh log */
		memset(kh, 0, sizeof(*kh));
		memcpy(kh->magic, KLOG_MAGIC, sizeof(kh->magic));
		kh->version = KLOG_VERSION;
		kh->bytes = (uint64_t)got;
		struct timeval tv = { 0, 0 };
		gettimeofday(&tv, NULL);
		kh->seconds = (uint64_t)tv.tv_sec;
		kh->crc = crc_add(0xffffffffu, piece, (uint64_t)got) ^ 0xffffffffu;
		kh->head_crc = klog_head_checksum(kh);

		if (device_write(KLOG_OFFSET + BLOCK, piece, (uint64_t)got)) {
			device_write(KLOG_OFFSET, head_buf, BLOCK);
		}
	}
}
