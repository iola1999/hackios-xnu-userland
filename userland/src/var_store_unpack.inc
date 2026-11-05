/*
 * var_store unpacking and inspection helpers (included by var_store.c).
 */

#ifndef ENOENT
#define ENOENT 2
#endif

static uint64_t made_dirs, made_files, made_links, made_bytes, short_files, lost_bytes;

#define RETRY_MS        2000
static uint64_t retry_ms_left = RETRY_MS;

static int
whole_path(const char *relative, uint32_t len)
{
	if (len == 0 || len >= MAX_PATH - sizeof(ROOT) - 1 || relative[0] == '/') {
		return 0;
	}
	for (uint32_t i = 0; i + 1 < len; i++) {
		if (relative[i] == '.' && relative[i + 1] == '.' &&
		    (i == 0 || relative[i - 1] == '/') && (i + 2 == len || relative[i + 2] == '/')) {
			return 0;
		}
	}
	strcpy(path, ROOT);
	path[sizeof(ROOT) - 1] = '/';
	memcpy(path + sizeof(ROOT), relative, len);
	path[sizeof(ROOT) + len] = 0;
	return 1;
}

static void
set_times(const struct entry *e)
{
	struct timeval times[2];

	times[0].tv_sec = (long)e->mtime_sec;
	times[0].tv_usec = (int)(e->mtime_nsec / 1000);
	times[1] = times[0];
	utimes(path, times);
}

static void
make_parents(char *fpath)
{
	for (char *p = fpath + sizeof(ROOT); *p; p++) {
		if (*p == '/') {
			*p = 0;
			mkdir(fpath, 0755);
			*p = '/';
		}
	}
}

#include "var_store_acl.inc"

static int
unpack_one(int make)
{
	struct entry e;
	static char relative[MAX_PATH];
	static uint8_t value[MAX_XATTR];
	static char name[1024];
	int out_fd = -1, ok = 1;

	if (!in(&e, sizeof(e))) {
		return 0;
	}
	if (e.pathlen >= MAX_PATH || e.nxattr > 4096) {
		printf("var_store: an entry this program cannot read (kind %u, path %u bytes)\n", e.kind,
		    e.pathlen);
		return 0;
	}
	if (!in(relative, e.pathlen)) {
		return 0;
	}
	relative[e.pathlen] = 0;
	if (make && !whole_path(relative, e.pathlen)) {
		make = 0;
	}
	if (make) {
		switch (e.kind) {
		case ENT_DIR:
			if (mkdir(path, (int)e.mode) == 0) {
				made_dirs++;
			}
			break;
		case ENT_FILE:
			out_fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, (int)e.mode);
			if (out_fd < 0 && errno == ENOENT) {
				make_parents(path);
				out_fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, (int)e.mode);
			}
			if (out_fd < 0) {
				printf("var_store: %s cannot be made: %s\n", path, strerror(errno));
			} else {
				made_files++;
			}
			break;
		default:
			break;
		}
	}
	acl_restored = 0;
	for (uint32_t i = 0; i < e.nxattr; i++) {
		uint32_t namelen, vlen;

		if (!in(&namelen, sizeof(namelen)) || !in(&vlen, sizeof(vlen))) {
			ok = 0;
			break;
		}
		if (namelen >= sizeof(name) || vlen > sizeof(value)) {
			ok = in_skip(namelen) && in_skip(vlen);
			skipped_big++;
			continue;
		}
		if (!in(name, namelen) || !in(value, vlen)) {
			ok = 0;
			break;
		}
		name[namelen] = 0;
		if (make && e.kind != ENT_HARDLINK && !restore_acl(path, name, value, vlen)) {
			setxattr(path, name, value, vlen, 0, XATTR_NOFOLLOW);
		}
	}
	drop_inherited_acl(make && ok && (e.kind == ENT_DIR || e.kind == ENT_FILE) ? path : NULL);
	if (!ok) {
		if (out_fd >= 0) {
			close(out_fd);
		}
		return 0;
	}
	if (e.kind == ENT_FILE) {
		static uint8_t piece[256 * 1024];
		uint64_t left = e.size, in_file = 0;

		while (left > 0) {
			uint64_t want = left > sizeof(piece) ? sizeof(piece) : left, done = 0;

			if (!in(piece, want)) {
				ok = 0;
				break;
			}
			while (out_fd >= 0 && done < want) {
				ssize_t wrote = write(out_fd, piece + done, (size_t)(want - done));

				if (wrote > 0) {
					done += (uint64_t)wrote;
					continue;
				}
				if (retry_ms_left < 5) {
					printf("var_store: %s: %llu of %llu bytes, no more would go in: %s\n",
					    path, (unsigned long long)(in_file + done),
					    (unsigned long long)e.size, strerror(errno));
					short_files++;
					lost_bytes += e.size - in_file - done;
					close(out_fd);
					out_fd = -1;
					break;
				}
				usleep(5000);
				retry_ms_left -= 5;
			}
			in_file += done;
			left -= want;
		}
		if (make) {
			made_bytes += in_file;
		}
	} else if (e.kind == ENT_SYMLINK || e.kind == ENT_HARDLINK) {
		static char target[MAX_PATH];

		if (e.size >= MAX_PATH) {
			ok = in_skip(e.size);
		} else if (!in(target, e.size)) {
			ok = 0;
		} else {
			target[e.size] = 0;
			if (make && e.kind == ENT_SYMLINK) {
				unlink(path);
				if (symlink(target, path) == 0) {
					made_links++;
				}
			} else if (make) {
				static char from[MAX_PATH];
				char keep[MAX_PATH];

				strcpy(keep, path);
				if (whole_path(target, (uint32_t)e.size)) {
					strcpy(from, path);
					strcpy(path, keep);
					unlink(path);
					if (link(from, path) == 0) {
						made_links++;
					}
				} else {
					strcpy(path, keep);
				}
			}
		}
	}
	if (out_fd >= 0) {
		close(out_fd);
	}
	if (!ok || !in_pad()) {
		return 0;
	}
	if (!make) {
		return 1;
	}
	switch (e.kind) {
	case ENT_DIR:
		chown(path, (int)e.uid, (int)e.gid);
		chmod(path, (int)e.mode);
		break;
	case ENT_DIRTIME:
		set_times(&e);
		if (e.flags != 0) {
			chflags(path, (int)e.flags);
		}
		break;
	case ENT_SYMLINK:
		lchown(path, (int)e.uid, (int)e.gid);
		break;
	case ENT_HARDLINK:
		break;
	default:
		chown(path, (int)e.uid, (int)e.gid);
		chmod(path, (int)e.mode);
		set_times(&e);
		if (e.flags != 0) {
			chflags(path, (int)e.flags);
		}
		break;
	}
	return 1;
}

static int
pass(const struct head *h, int make)
{
	stream_start((int)h->slot, h->bytes);
	for (uint64_t i = 0; i < h->entries; i++) {
		if (!unpack_one(make)) {
			return 0;
		}
	}
	if (stream_left != 0) {
		printf("var_store: %llu bytes left over after %llu entries\n",
		    (unsigned long long)stream_left, (unsigned long long)h->entries);
		return 0;
	}
	if ((stream_crc ^ 0xffffffffu) != h->crc) {
		printf("var_store: slot %u: the contents are not the %08x the header says (%08x)\n", h->slot,
		    h->crc, stream_crc ^ 0xffffffffu);
		return 0;
	}
	return 1;
}

static int
load(void)
{
	struct head h;
	int best;

	memset(&h, 0, sizeof(h));
	best = newest(&h);
	if (best < 0) {
		printf("var_store: no store yet: /private/var stays as init made it\n");
		klog_restore();
		return 1;
	}
	printf("var_store: slot %d: generation %llu of %llu seconds, %llu entries, %llu bytes\n", best,
	    (unsigned long long)h.generation, (unsigned long long)h.seconds,
	    (unsigned long long)h.entries, (unsigned long long)h.bytes);
	if (!pass(&h, 0)) {
		struct head other;
		int left = -1;

		for (int slot = 0; slot < slots; slot++) {
			if (slot != best && read_head(slot, &other) && pass(&other, 0)) {
				left = slot;
				break;
			}
		}
		if (left < 0) {
			printf("var_store: nothing good in the store: /private/var stays as init made it\n");
			klog_restore();
			return 1;
		}
		h = other;
		printf("var_store: slot %d is good after all: generation %llu\n", left,
		    (unsigned long long)h.generation);
	}
	if (!pass(&h, 1)) {
		printf("var_store: the archive read differently the second time: what was made stays\n");
		klog_restore();
		return 0;
	}
	printf("var_store: %llu directories, %llu files (%llu bytes), %llu links back in /private/var\n",
	    (unsigned long long)made_dirs, (unsigned long long)made_files, (unsigned long long)made_bytes,
	    (unsigned long long)made_links);
	if (short_files != 0) {
		printf("var_store: %llu files did not fit: %llu bytes of them are not there\n",
		    (unsigned long long)short_files, (unsigned long long)lost_bytes);
	}
	delta_restore(h.generation);
	klog_restore();
	return 1;
}

static void
show(void)
{
	for (int slot = 0; slot < slots; slot++) {
		static uint8_t block[BLOCK];
		struct head *h = (struct head *)block;

		if (!device_read(slot_at(slot), block, BLOCK)) {
			continue;
		}
		if (memcmp(h->magic, HEAD_MAGIC, sizeof(h->magic)) != 0) {
			printf("var_store: slot %d: empty\n", slot);
			continue;
		}
		printf("var_store: slot %d: version %u, generation %llu, %llu entries, %llu bytes, crc %08x, "
		    "written at %llu, header %s\n", slot, h->version, (unsigned long long)h->generation,
		    (unsigned long long)h->entries, (unsigned long long)h->bytes, h->crc,
		    (unsigned long long)h->seconds, h->head_crc == head_checksum(h) ? "right" : "WRONG");
	}
	if (store_at != 0 && slots > 1) {
		struct delta_head dh;
		if (delta_read_head(&dh)) {
			printf("var_store: delta slot: version %u, base_gen %llu, seq %llu, %llu entries, %llu bytes, "
			    "crc %08x, written at %llu, status %s, header %s\n", dh.version,
			    (unsigned long long)dh.base_generation, (unsigned long long)dh.delta_seq,
			    (unsigned long long)dh.entries, (unsigned long long)dh.bytes, dh.crc,
			    (unsigned long long)dh.seconds, dh.flags == 0 ? "active" : "merged",
			    dh.head_crc == delta_head_checksum(&dh) ? "right" : "WRONG");
		} else {
			printf("var_store: delta slot: empty\n");
		}
	}
}

static int
erase(void)
{
	static uint8_t block[BLOCK];

	memset(block, 0, sizeof(block));
	for (int slot = 0; slot < slots; slot++) {
		if (!device_write(slot_at(slot), block, BLOCK)) {
			return 0;
		}
	}
	delta_reset(0);
	printf("var_store: all slots forgotten\n");
	return 1;
}
