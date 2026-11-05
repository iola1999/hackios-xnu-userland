/*
 * Incremental dirty snapshot detection and transient filtering for var_store.
 * Monitors mtime and sizes on key whitelist paths:
 *   /private/var/mobile/Library/SpringBoard
 *   /private/var/mobile/Library/Preferences
 *   /private/var/containers
 *   /private/var/db
 */

static const char *const WATCH_DIRS[] = {
	"/private/var/mobile/Library/SpringBoard",
	"/private/var/mobile/Library/Preferences",
	"/private/var/containers",
	"/private/var/db",
	NULL,
};

struct dir_snapshot {
	int64_t  max_mtime_sec;
	uint32_t max_mtime_nsec;
	uint64_t total_size;
	uint32_t count;
};

static const char *
str_find(const char *haystack, const char *needle)
{
	size_t nlen;
	if (!haystack || !needle) {
		return NULL;
	}
	if (!*needle) {
		return haystack;
	}
	nlen = strlen(needle);
	for (; *haystack; haystack++) {
		if (*haystack == *needle && strncmp(haystack, needle, nlen) == 0) {
			return haystack;
		}
	}
	return NULL;
}

static int
path_has_component(const char *p_path, const char *comp)
{
	size_t clen = strlen(comp);
	const char *p = p_path;

	while ((p = str_find(p, comp)) != NULL) {
		if ((p == p_path || *(p - 1) == '/') &&
		    (*(p + clen) == '\0' || *(p + clen) == '/')) {
			return 1;
		}
		p += clen;
	}
	return 0;
}

static int
is_transient_path(const char *relative)
{
	/* strictly preserve LaunchServices and icon caches */
	if (str_find(relative, "LaunchServices") != NULL ||
	    str_find(relative, "iconservices") != NULL ||
	    str_find(relative, "iconcache") != NULL) {
		return 0;
	}
	/* filter WebKit disk caches, URLCache fsCachedData, CrashReporter, Biome, Powerlog, CoreDuet */
	if (str_find(relative, "WebKit") != NULL ||
	    str_find(relative, "fsCachedData") != NULL ||
	    str_find(relative, "CrashReporter") != NULL ||
	    str_find(relative, "Biome") != NULL ||
	    str_find(relative, "Powerlog") != NULL ||
	    str_find(relative, "CoreDuet") != NULL ||
	    str_find(relative, "Safari/Favicon") != NULL ||
	    str_find(relative, "Safari/Touch Icons") != NULL) {
		return 1;
	}
	/* filter transient components across path levels */
	if (path_has_component(relative, "tmp") ||
	    path_has_component(relative, "run") ||
	    path_has_component(relative, "vm") ||
	    path_has_component(relative, "log") ||
	    path_has_component(relative, "logs") ||
	    path_has_component(relative, "diagnostics") ||
	    path_has_component(relative, "uuidtext") ||
	    path_has_component(relative, "jb")) {
		return 1;
	}
	return 0;
}

static int
is_transient_name(const char *name)
{
	if (str_find(name, "LaunchServices") != NULL ||
	    str_find(name, "iconservices") != NULL) {
		return 0;
	}
	return strcmp(name, "diagnostics") == 0 ||
	       strcmp(name, "uuidtext") == 0 ||
	       strcmp(name, "tmp") == 0 ||
	       strcmp(name, "run") == 0 ||
	       strcmp(name, "vm") == 0 ||
	       strcmp(name, "log") == 0 ||
	       strcmp(name, "logs") == 0 ||
	       strcmp(name, "CrashReporter") == 0 ||
	       strcmp(name, "Biome") == 0 ||
	       strcmp(name, "Powerlog") == 0 ||
	       strcmp(name, "CoreDuet") == 0 ||
	       strcmp(name, "fsCachedData") == 0 ||
	       strcmp(name, "WebKit") == 0;
}

static void
scan_path_snapshot(const char *dir_path, int depth, struct dir_snapshot *snap)
{
	static char child[MAX_PATH];
	static char dentry_buf[2048];
	struct stat st;
#ifdef MINI_H
	off_t pos = 0;
#else
	long pos = 0;
#endif
	ssize_t n;
	int dfd;

	if (lstat(dir_path, &st) != 0) {
		return;
	}
	if (st.st_mtimespec.tv_sec > snap->max_mtime_sec ||
	    (st.st_mtimespec.tv_sec == snap->max_mtime_sec &&
	     (uint32_t)st.st_mtimespec.tv_nsec > snap->max_mtime_nsec)) {
		snap->max_mtime_sec = st.st_mtimespec.tv_sec;
		snap->max_mtime_nsec = (uint32_t)st.st_mtimespec.tv_nsec;
	}
	snap->total_size += (uint64_t)st.st_size;
	snap->count++;

	if ((st.st_mode & S_IFMT) != S_IFDIR || depth >= 4) {
		return;
	}

	dfd = open(dir_path, O_RDONLY | O_DIRECTORY, 0);
	if (dfd < 0) {
		return;
	}

	size_t base_len = strlen(dir_path);
	while ((n = getdirentries(dfd, dentry_buf, sizeof(dentry_buf), &pos)) > 0) {
		for (ssize_t i = 0; i < n;) {
			struct dirent *d = (struct dirent *)(dentry_buf + i);
			const char *name = d->d_name;
			size_t name_len = strlen(name);
			i += d->d_reclen;

			if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) {
				continue;
			}
			if (is_transient_name(name)) {
				continue;
			}
			if (base_len + 1 + name_len >= sizeof(child)) {
				continue;
			}
			memcpy(child, dir_path, base_len);
			child[base_len] = '/';
			memcpy(child + base_len + 1, name, name_len);
			child[base_len + 1 + name_len] = 0;

			scan_path_snapshot(child, depth + 1, snap);
		}
	}
	close(dfd);
}

static void
compute_snapshot(struct dir_snapshot *snap)
{
	memset(snap, 0, sizeof(*snap));
	for (int i = 0; WATCH_DIRS[i] != NULL; i++) {
		scan_path_snapshot(WATCH_DIRS[i], 0, snap);
	}
}

static int
snapshot_is_dirty(const struct dir_snapshot *current, const struct dir_snapshot *previous)
{
	if (previous->count == 0 && previous->max_mtime_sec == 0) {
		return current->count > 0;
	}
	return current->max_mtime_sec != previous->max_mtime_sec ||
	       current->max_mtime_nsec != previous->max_mtime_nsec ||
	       current->total_size != previous->total_size ||
	       current->count != previous->count;
}
