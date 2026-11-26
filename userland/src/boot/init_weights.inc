/*
 * The espresso weights of the media analysis models, shown inside the frameworks of the
 * read-only system volume without writing that volume: every target path is a new name in
 * its directory, so a synthetic entry can take it (kern.apfs_synthetic, an empty directory
 * the mount then covers) and a bindfs mount puts the staged content there. File targets bind
 * single files; the mubb backbone targets bind whole directory trees (the jit compile products
 * under the universal.bundle name). The mechanism and its limits are in
 * out/goal-1124/batch5/weights-deploy-channel.md.
 *
 * The staging tree is /private/var/annibale-weights on the data volume, pushed over USB or
 * sftp (tools/k90auto/push-weights.py): persistent, paged in on demand, no resident memory.
 * The entries are etc/weights.list of this image (mkweightslist.py), one tab-separated line
 * each: "f SIZE SRC TARGET SHA256" for a file, "d SIZE SRC TARGET -" for a tree whose SIZE is
 * its total bytes. Runs after mount_writable -- the sources live on the data volume -- and
 * before launchd. A single entry's failure is said and counted and never stops the boot:
 * the models that entry feeds simply keep their shipped (stripped) state.
 */

#define WEIGHTS_ROOT    "/private/var/annibale-weights"

/* The total size of the regular files in a tree, or -1 when it cannot be walked. */
static long
weight_tree_bytes(const char *path)
{
	/* the buffer is a local on purpose: a recursion level's entries must survive the levels below it */
	char buf[4096], child[640];
	struct stat st;
	off_t position = 0;
	ssize_t n;
	long total = 0;
	int fd = open(path, O_RDONLY | O_DIRECTORY, 0);

	if (fd < 0) {
		return -1;
	}
	while ((n = getdirentries(fd, buf, sizeof(buf), &position)) > 0) {
		for (ssize_t at = 0; at < n; at += ((struct dirent *)(buf + at))->d_reclen) {
			const char *name = ((struct dirent *)(buf + at))->d_name;
			long sub;

			if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) {
				continue;
			}
			if (strlen(path) + strlen(name) + 2 > sizeof(child)) {
				close(fd);
				return -1;
			}
			strcpy(child, path);
			strcpy(child + strlen(child), "/");
			strcpy(child + strlen(child), name);
			if (lstat(child, &st) != 0) {
				close(fd);
				return -1;
			}
			if ((st.st_mode & S_IFMT) == S_IFDIR) {
				sub = weight_tree_bytes(child);
				if (sub < 0) {
					close(fd);
					return -1;
				}
				total += sub;
			} else {
				total += (long)st.st_size;
			}
		}
	}
	close(fd);
	return n < 0 ? -1 : total;
}

/*
 * One line of weights.list. Returns 1 when the entry ends bound, 0 when its source is not
 * staged (a partial push: expected, reported in the summary alone), 2 when the volume already
 * answers at the target (a system image with the weight baked in), -1 on a real failure.
 */
static int
weight_entry(char kind, long size, const char *src_rel, const char *target)
{
	char src[640];
	struct stat st;
	int type_bad, want_dir = kind == 'd';
	long bytes;

	if (strlen(WEIGHTS_ROOT) + 1 + strlen(src_rel) >= sizeof(src)) {
		printf("  %s: the staging path is too long\n", src_rel);
		return -1;
	}
	strcpy(src, WEIGHTS_ROOT "/");
	strcpy(src + strlen(src), src_rel);
	if (lstat(target, &st) == 0) {
		return 2;       /* the volume answers there itself (an image with the weight baked in) */
	}
	if (lstat(src, &st) != 0) {
		return 0;
	}
	type_bad = want_dir ? (st.st_mode & S_IFMT) != S_IFDIR : (st.st_mode & S_IFMT) != S_IFREG;
	if (type_bad) {
		printf("  %s: the staging source is not a %s\n", src, want_dir ? "directory" : "regular file");
		return -1;
	}
	if (!want_dir && st.st_size != (off_t)size) {
		printf("  %s: %lld bytes staged, %lld expected (a truncated push?)\n", src,
		    (long long)st.st_size, (long long)size);
		return -1;
	}
	if (want_dir) {
		bytes = weight_tree_bytes(src);
		if (bytes < 0) {
			printf("  %s: the staging tree cannot be walked\n", src);
			return -1;
		}
		if (bytes != size) {
			printf("  %s: the tree holds %ld bytes, %ld expected (an incomplete push?)\n", src, bytes, size);
			return -1;
		}
	}
	if (!synthetic_dir(target)) {
		return -1;
	}
	if (!bind_dir(src, target)) {
		return -1;
	}
	printf("  %s\n", target);
	return 1;
}

static void
bind_weights(void)
{
	static char list[16384];
	struct stat st;
	int entries = 0, bound = 0, failed = 0, absent = 0, present = 0;

	if (read_whole(in_image("/etc/weights.list"), list, sizeof(list)) < 0) {
		return;         /* an image without the list has no weights to bind */
	}
	printf("\n-- espresso weights --\n");
	if (lstat(WEIGHTS_ROOT, &st) != 0 || (st.st_mode & S_IFMT) != S_IFDIR) {
		printf("  nothing staged in " WEIGHTS_ROOT "\n");
		return;
	}
	for (char *line = list; *line;) {
		char *end = strchr(line, '\n'), *f[6];
		int count;

		if (end) {
			*end = 0;
		}
		count = split_tabs(line, f, 6);
		if (count >= 4 && (f[0][0] == 'f' || f[0][0] == 'd') && f[0][1] == 0) {
			int result = weight_entry(f[0][0], parse_number(f[1], 10), f[2], f[3]);

			entries++;
			bound += result == 1;
			absent += result == 0;
			present += result == 2;
			failed += result < 0;
		}
		if (!end) {
			break;
		}
		line = end + 1;
	}
	printf("  %d entries: %d bound, %d already on the volume, %d not staged, %d failed\n",
	    entries, bound, present, absent, failed);
	check(entries == 0 || failed == 0, "the espresso weights of the media analysis models");
}
