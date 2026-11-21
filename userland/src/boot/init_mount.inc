/*
 * The rootless prefix of a package manager, /var/jb: the Procursus bootstrap with Sileo in it
 * (tools/mk-jb.sh) rides in the system image as /jb (or in the boot volume, /private/preboot/jb,
 * with the root on the flash), an APFS image that cannot be written to, while dpkg and apt must be
 * able to write anywhere in their own prefix.
 *
 * Copying all 124 MB of it into the memory filesystem is more than this machine has to spare (the
 * system image is already 2.8 GB of its 4.3 GB), so the prefix is grafted instead: every directory
 * is made for real, and every file and link in it becomes a symbolic link to its twin in /jb. The
 * directories are then writable -- a package that puts a file in /var/jb/usr/bin puts it in memory,
 * next to the links -- while the 114 MB of programs and libraries stay where they are, read only
 * and shared. What dpkg and apt write over rather than beside (their databases and settings) is
 * copied for real: GRAFT_COPY below.
 */
static int
copy_tree(const char *from, const char *to, int *files)
{
	char buf[4096], f[512], t[512], target[512];
	struct stat st;
	off_t position = 0;
	ssize_t n;
	int ok = 1, fd, mode;

	/* The mode of this directory, kept aside: st is used for its entries below. */
	if (lstat(from, &st) != 0) {
		return 0;
	}
	mode = st.st_mode & 07777;
	if ((mkdir(to, mode) != 0 && errno != EEXIST) || chown(to, 0, 0) != 0) {
		return 0;
	}
	if ((fd = open(from, O_RDONLY | O_DIRECTORY, 0)) < 0) {
		return 0;
	}
	while (ok && (n = getdirentries(fd, buf, sizeof(buf), &position)) > 0) {
		for (ssize_t at = 0; ok && at < n; at += ((struct dirent *)(buf + at))->d_reclen) {
			const char *name = ((struct dirent *)(buf + at))->d_name;

			if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) {
				continue;
			}
			if (strlen(from) + strlen(name) + 2 > sizeof(f) || strlen(to) + strlen(name) + 2 > sizeof(t)) {
				printf("  %s/%s: the path is too long\n", from, name);
				ok = 0;
				break;
			}
			strcpy(f, from);
			strcpy(f + strlen(f), "/");
			strcpy(f + strlen(f), name);
			strcpy(t, to);
			strcpy(t + strlen(t), "/");
			strcpy(t + strlen(t), name);
			if (lstat(f, &st) != 0) {
				ok = 0;
			} else if ((st.st_mode & S_IFMT) == S_IFDIR) {
				ok = copy_tree(f, t, files);
			} else if ((st.st_mode & S_IFMT) == S_IFLNK) {
				ssize_t length = readlink(f, target, sizeof(target) - 1);

				target[length > 0 ? length : 0] = 0;
				if (lstat(t, &st) == 0) {
					(void)unlink(t);
				}
				ok = length > 0 && symlink(target, t) == 0;
				(*files)++;
			} else {
				ok = copy_file(f, t, st.st_mode & 07777, 0, 0);
				(*files)++;
			}
			if (!ok) {
				printf("  %s: %s\n", f, strerror(errno));
			}
		}
	}
	close(fd);
	return ok && chmod(to, mode) == 0;
}

/* The parts of the prefix that are written over, not just added to, and so are copied for real. */
static const char *const graft_copy[] = { "Library", "etc", "var", "tmp" };

static int
graft_tree(const char *from, const char *to, int top, int *made, int *linked)
{
	char buf[4096], f[512], t[512];
	struct stat st;
	off_t position = 0;
	ssize_t n;
	int ok = 1, fd, mode;

	/* The mode of this directory, kept aside: st is used for its entries below. */
	if (lstat(from, &st) != 0) {
		return 0;
	}
	mode = st.st_mode & 07777;
	if ((mkdir(to, mode) != 0 && errno != EEXIST) || chown(to, 0, 0) != 0) {
		return 0;
	}
	(*made)++;
	if ((fd = open(from, O_RDONLY | O_DIRECTORY, 0)) < 0) {
		return 0;
	}
	while (ok && (n = getdirentries(fd, buf, sizeof(buf), &position)) > 0) {
		for (ssize_t at = 0; ok && at < n; at += ((struct dirent *)(buf + at))->d_reclen) {
			const char *name = ((struct dirent *)(buf + at))->d_name;
			int copy_it = 0;

			if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) {
				continue;
			}
			if (strlen(from) + strlen(name) + 2 > sizeof(f) || strlen(to) + strlen(name) + 2 > sizeof(t)) {
				printf("  %s/%s: the path is too long\n", from, name);
				ok = 0;
				break;
			}
			strcpy(f, from);
			strcpy(f + strlen(f), "/");
			strcpy(f + strlen(f), name);
			strcpy(t, to);
			strcpy(t + strlen(t), "/");
			strcpy(t + strlen(t), name);
			for (unsigned i = 0; top && i < sizeof(graft_copy) / sizeof(graft_copy[0]); i++) {
				copy_it = copy_it || strcmp(name, graft_copy[i]) == 0;
			}
			if (lstat(f, &st) != 0) {
				ok = 0;
			} else if (copy_it) {
				ok = copy_tree(f, t, linked);
			} else if ((st.st_mode & S_IFMT) == S_IFDIR) {
				ok = graft_tree(f, t, 0, made, linked);
			} else {
				if (lstat(t, &st) == 0) {
					(void)unlink(t);
				}
				ok = symlink(f, t) == 0;
				(*linked)++;
			}
			if (!ok) {
				printf("  %s: %s\n", f, strerror(errno));
			}
		}
	}
	close(fd);
	return ok && chmod(to, mode) == 0;
}

/*
 * A configuration file read whole into a fixed buffer, and loudly when it does not fit.
 *
 * Three places here read a file of ours this way, and all three used to do it with one
 * read() and no check at all, so a file longer than its buffer simply lost its tail. What
 * that looks like is different every time and never looks like a short read: half a program
 * name handed to kern.exec_inject, which then matches nothing and the insertion quietly does
 * not happen; directories of /private/var that are merely absent, which is what cost the
 * package manager's work a long hunt for a missing /var/mobile/tmp; job lines that do not
 * run, which reads as a test that found nothing.
 *
 * The loop matters as much as the size: read() is allowed to return less than it was asked
 * for without being at the end of the file, so a single read() is not enough even when the
 * file does fit. The buffers are still fixed -- this is an init with no allocator worth the
 * name -- but a file that does not fit now says so instead of going quiet.
 *
 * The length, or -1 if there is no such file.
 */
static ssize_t
read_whole(const char *path, char *buf, size_t size)
{
	int fd = open(path, O_RDONLY, 0);
	size_t got = 0;
	ssize_t n;
	char over;

	if (fd < 0) {
		return -1;
	}
	while (got < size - 1 && (n = read(fd, buf + got, size - 1 - got)) > 0) {
		got += (size_t)n;
	}
	/* one byte left to read means the buffer ended, not the file */
	if (got == size - 1 && read(fd, &over, 1) > 0) {
		printf("init: %s does not fit in %zu bytes: the rest of it is not used\n", path, size - 1);
	}
	close(fd);
	buf[got] = 0;
	return (ssize_t)got;
}

/*
 * darwin_var holds an APFS container (block 0: "NXSB" at byte 32), as tools/apfs/annfs-to-apfs.py
 * leaves it.  Returns 1 when darwin_var is an APFS container, 0 on memory-only runs without a
 * darwin_var partition or flash root, and -1 when a persistent data volume is expected but block 0
 * is not an APFS container.
 */
static int
var_is_apfs(void)
{
	static unsigned char blk[4096];
	unsigned long long var_bytes = 0;
	size_t len = sizeof(var_bytes);
	int fd, is = 0;

	if (sysctlbyname("kern.annibale.darwin_var_bytes", &var_bytes, &len, NULL, 0) != 0 || var_bytes == 0) {
		return boot_arg("root_apfs") != NULL ? -1 : 0;
	}
	fd = open("/dev/rdisk1s2", O_RDONLY, 0);
	if (fd >= 0) {
		is = pread(fd, blk, sizeof(blk), 0) == (ssize_t)sizeof(blk) && memcmp(blk + 32, "NXSB", 4) == 0;
		close(fd);
	}
	return is ? 1 : -1;
}

/*
 * Before the cryptexes were grafted, lsd dropped the OS cryptex's WebKit extensions from its
 * database at the boot after the one that built it ("Marking container at
 * /private/preboot/Cryptexes/OS/ for removal"), and nothing registers them again in a database it
 * keeps: Safari then starts no WebKit process.  On the first boot with the OS cryptex grafted the
 * database is removed once, and lsd builds a new one when it starts, as after an iOS update.
 */
static int os_cryptex_grafted;          /* this boot: set by mount_cryptex */

static void
launchservices_rebuild_once(void)
{
	static const char marker[] = "/private/var/db/.annibale-cryptex-grafted";
	static const char daemons[] = "/private/var/mobile/Containers/Data/InternalDaemon/";
	static char outer[4096], inner[4096];
	char caches[160], file[320];
	off_t at_outer = 0;
	ssize_t n, m;
	struct stat st;
	int fd, removed = 0;

	if (!os_cryptex_grafted || lstat(marker, &st) == 0) {
		return;
	}
	fd = open(daemons, O_RDONLY | O_DIRECTORY, 0);
	while (fd >= 0 && (n = getdirentries(fd, outer, sizeof(outer), &at_outer)) > 0) {
		for (ssize_t i = 0; i < n; i += ((struct dirent *)(outer + i))->d_reclen) {
			const char *container = ((struct dirent *)(outer + i))->d_name;
			off_t at_inner = 0;
			int cfd;

			if (container[0] == '.' ||
			    strlen(daemons) + strlen(container) + sizeof("/Library/Caches/") > sizeof(caches)) {
				continue;
			}
			strcpy(caches, join(daemons, container, "/Library/Caches/"));
			cfd = open(caches, O_RDONLY | O_DIRECTORY, 0);
			while (cfd >= 0 && (m = getdirentries(cfd, inner, sizeof(inner), &at_inner)) > 0) {
				for (ssize_t k = 0; k < m; k += ((struct dirent *)(inner + k))->d_reclen) {
					const char *name = ((struct dirent *)(inner + k))->d_name;
					size_t len = strlen(name);
					const char *store = strncmp(name, "SystemDataOnly-", 15) == 0 ? name + 15 : name;

					if (strncmp(store, "com.apple.LaunchServices-", 25) != 0 || len < 8 ||
					    strcmp(name + len - 8, ".csstore") != 0 || strlen(caches) + len >= sizeof(file)) {
						continue;
					}
					strcpy(file, join(caches, name, ""));
					removed += unlink(file) == 0;
				}
			}
			if (cfd >= 0) {
				close(cfd);
			}
		}
	}
	if (fd >= 0) {
		close(fd);
	}
	fd = open(marker, O_WRONLY | O_CREAT, 0644);
	if (fd >= 0) {
		close(fd);
	}
	printf("  first boot with the OS cryptex grafted: %d LaunchServices database files removed for lsd to rebuild\n", removed);
}

static int
mount_writable(void)
{
	static char skeleton[16384];
	static const char *const markers[] = { "/private/var/db/diagnostics/.migrated",
		                               "/private/var/db/diagnostics/timesync/.migrated", "/private/var/db/uuidtext/.migrated" };
	int ok = 0, kind, fd, made = 0, files = 0;
	const char *jb;
	struct stat st;

	printf("\n-- writable filesystem --\n");
	kind = var_is_apfs();
	if (kind > 0) {
		ok = mount("apfs", "/private/var", 0, (void *)"/dev/disk1s2") == 0;
		if (ok) {
			printf("  mounted the APFS data volume on /private/var (/dev/disk1s2)\n");
		} else {
			printf("  mount of the APFS data volume (/dev/disk1s2): %s\n", strerror(errno));
		}
	} else if (kind == 0) {
		ok = mount("ramfs", "/private/var", 0, NULL) == 0;
		if (!ok) {
			printf("  mount: %s\n", strerror(errno));
		}
	} else {
		printf("  darwin_var is not formatted as APFS; run python3 hackios-xnu-userland/tools/apfs/annfs-to-apfs.py to migrate (or --format for an empty volume)\n");
		ok = 0;
	}
	check(ok, "mount the persistent data volume on /private/var");
	if (!ok) {
		return 0;
	}

	/* the directories of the data volume, as listed in the template that ships with iOS (ios26/full-system.py) */
	read_whole(in_image("/etc/var.skel"), skeleton, sizeof(skeleton));
	for (char *line = skeleton; ok && *line;) {
		char *end = strchr(line, '\n'), *f[6];
		int count;

		if (end) {
			*end = 0;
		}
		count = split_tabs(line, f, 6);
		if (count == 5 && f[0][0] == 'd') {
			int mode = (int)parse_number(f[1], 8);

			if (lstat(f[4], &st) != 0) {
				ok = mkdir(f[4], mode) == 0 && chmod(f[4], mode) == 0 &&
				    chown(f[4], (int)parse_number(f[2], 10), (int)parse_number(f[3], 10)) == 0;
				if (!ok) {
					printf("  %s: %s\n", f[4], strerror(errno));
				}
			}
			made++;
		} else if (count == 3 && f[0][0] == 'l') {
			symlink(f[1], f[2]);
		} else if (count == 6 && f[0][0] == 'f') {
			/* a file of ours to start with (build.sh: the var/ directory): keep existing persistent file */
			if (lstat(f[4], &st) != 0) {
				if (!copy_file(in_image(f[5]), f[4], (int)parse_number(f[1], 8),
				    (int)parse_number(f[2], 10), (int)parse_number(f[3], 10))) {
					printf("  %s: %s\n", f[4], strerror(errno));
				}
			}
			files++;
		}
		if (!end) {
			break;
		}
		line = end + 1;
	}
	/*
	 * logd takes a log directory without this marker for one left by an
	 * older system, to be converted by logd_helper; these are brand new.
	 */
	for (unsigned i = 0; ok && i < sizeof(markers) / sizeof(markers[0]); i++) {
		if (lstat(markers[i], &st) != 0) {
			fd = open(markers[i], O_WRONLY | O_CREAT, 0644);
			ok = fd >= 0 && chown(markers[i], 272, 272) == 0;
			close(fd);
		}
	}
	printf("  %d directories, %d files\n", made, files);
	check(ok && made > 50, "create the directories of the data volume");
	/* Clear cached camera source backings so mounted V57 changes take effect on every boot. */
	(void)unlink("/private/var/mobile/Library/Preferences/com.apple.cameracapture.volatile.plist");
	(void)unlink("/private/var/root/Library/Preferences/com.apple.cameracapture.volatile.plist");
	launchservices_rebuild_once();

	/*
	 * and the package manager's prefix, when the image was built with one (tools/mk-jb.sh): /jb of
	 * the system image in RAM, or, with the iOS volume on the flash as the root, /private/preboot/jb
	 * -- the boot volume, where a rootless jailbreak on an iPhone keeps its Procursus too
	 * (/var/jb leads into /private/preboot there)
	 */
	jb = stat("/jb", &st) == 0 && (st.st_mode & S_IFMT) == S_IFDIR ? "/jb" :
	    stat("/private/preboot/jb", &st) == 0 && (st.st_mode & S_IFMT) == S_IFDIR ? "/private/preboot/jb" : NULL;
	if (ok && jb != NULL) {
		const char *to = "/private/var/jb";
		int dirs = 0, entries = 0, grafted, users = 0;

		/*
		 * With the root on the flash, the prefix's writable copy goes into a memory filesystem of its own
		 * on the boot volume's jb-rw, as procursus, and /var/jb is a link to it: where /var/jb leads on
		 * an iPhone with a rootless jailbreak, into /private/preboot. LaunchServices registers an
		 * application from there, and from /private/var not at all -- everything a package put in
		 * /var/jb/Applications was "Unable to register", with nothing on the files even looked at.
		 */
		if (strcmp(jb, "/private/preboot/jb") == 0 && stat("/private/preboot/jb-rw", &st) == 0) {
			if (lstat("/private/var/jb", &st) == 0 && (st.st_mode & S_IFMT) == S_IFLNK) {
				(void)unlink("/private/var/jb");
			}
			if (mount("ramfs", "/private/preboot/jb-rw", 0, NULL) == 0 &&
			    symlink("/private/preboot/jb-rw/procursus", "/private/var/jb") == 0) {
				to = "/private/preboot/jb-rw/procursus";
			} else {
				printf("  the prefix on /private/preboot/jb-rw: %s\n", strerror(errno));
			}
		}
		grafted = graft_tree(jb, to, 1, &dirs, &entries);

		/*
		 * All of it belongs to root, as it does on a phone, but for the user's corner of it: an
		 * application of the prefix runs as mobile and keeps its data there (Sileo's database).
		 */
		static const char *const mobiles[] = { "/private/var/jb/var/mobile", "/private/var/jb/var/mobile/Library",
			                               "/private/var/jb/var/mobile/Library/Caches",
			                               "/private/var/jb/var/mobile/Library/Preferences",
			                               "/private/var/jb/var/mobile/Documents",
			                               /*
			                                * Sileo keeps the indexes of its repositories apart from
			                                * apt's own (it runs apt with -oDir::State::lists=
			                                * sileolists/), and it is mobile who writes them.
			                                */
			                               "/private/var/jb/var/lib/apt/sileolists",
			                               "/private/var/jb/var/lib/apt/sileolists/partial" };

		for (unsigned i = 0; i < sizeof(mobiles) / sizeof(mobiles[0]); i++) {
			mkdir(mobiles[i], 0755);
			grafted = chown(mobiles[i], 501, 501) == 0 && grafted;
		}
		grafted = own_tree("/private/var/jb/var/mobile", 501) && grafted;
		/*
		 * The prefix's own user database. Its programs do not ask iOS who a user is: they are
		 * built against libiosexec, which carries an ie_getpwnam of its own and reads
		 * /var/jb/etc/pwd.db -- a Berkeley database that pwd_mkdb makes from master.passwd and
		 * that the bootstrap does not ship, only the text it is made from. Without it every
		 * program of the prefix answers "no such user": an sshd from here refused every login
		 * with "Invalid user root" while iOS's own /bin/ps resolved users perfectly -- and the
		 * asymmetry is exactly this, not anything about iOS's own database or the launchd domain
		 * its server sits in, because a program of the prefix never asks iOS at all. The
		 * writable copy of the prefix is new at every boot, so the database is made at every
		 * boot; it takes milliseconds.
		 */
		if (stat("/private/var/jb/usr/sbin/pwd_mkdb", &st) == 0) {
			char *argv[] = { "/var/jb/usr/sbin/pwd_mkdb", "-p", "/var/jb/etc/master.passwd", NULL };
			char *envp[] = { "PATH=/var/jb/usr/sbin:/var/jb/usr/bin:/usr/bin:/bin", NULL };
			int status = 0;
			pid_t pid = fork();

			if (pid == 0) {
				execve(argv[0], argv, envp);
				_exit(127);
			}
			if (pid > 0 && wait4(pid, &status, 0, NULL) == pid && WIFEXITED(status) && WEXITSTATUS(status) == 0) {
				users = 1;
			}
		}
		printf("  /var/jb from %s, in %s: %d directories, %d files and links%s\n", jb, to, dirs, entries,
		    users ? ", users resolvable" : "");
		check(grafted, "graft the package manager's prefix onto the data volume");
	}
	return 1;
}

/*
 * The panic log of the run before: the kernel keeps it in darwin_nvram until it is
 * copied here (kern.annibale.panic_log).
 */
static void
keep_panic_log(void)
{
	static char buf[256 * 1024];    /* loader_return.c writes at most this much (LOADER_PANIC_BUF_SIZE) */
	size_t len = sizeof(buf);
	int kept = 1, fd;

	if (sysctlbyname("kern.annibale.panic_log", buf, &len, NULL, 0) != 0 || len == 0) {
		return;
	}
	fd = open("/private/var/db/panic.log", O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (fd < 0) {
		return;
	}
	if (write(fd, buf, len) == (ssize_t)len && fsync(fd) == 0 &&
	    sysctlbyname("kern.annibale.panic_log", NULL, NULL, &kept, sizeof(kept)) == 0) {
		printf("  the panic log of the run before is in /private/var/db/panic.log (%zu bytes)\n", len);
	}
	close(fd);
}

/*
 * Finalize the persistent /private/var data volume:
 * ensure essential directories exist, propagate global preferences, and sync the volume.
 */
static void
restore_var(void)
{
	int vfd;

	mkdir("/private/var/mobile/Library/SyncedPreferences", 0755);
	chown("/private/var/mobile/Library/SyncedPreferences", 501, 501);
	mkdir("/private/var/mobile/Library/Weather", 0755);
	chown("/private/var/mobile/Library/Weather", 501, 501);
	mkdir("/private/var/db/locationd", 0755);
	chown("/private/var/db/locationd", 205, 205);
	mkdir("/private/var/db/annibale-aircache", 0777);
	chmod("/private/var/db/annibale-aircache", 0777);
	mkdir("/private/var/db/annibale-ir3cache", 0777);
	chmod("/private/var/db/annibale-ir3cache", 0777);
	mkdir("/private/var/preferences", 0755);
	mkdir("/private/var/root", 0750);
	mkdir("/private/var/root/Library", 0750);
	mkdir("/private/var/root/Library/Preferences", 0750);
	keep_panic_log();
	{
		struct stat gp_st;
		if (stat("/private/var/mobile/Library/Preferences/.GlobalPreferences.plist", &gp_st) == 0 && gp_st.st_size > 0) {
			(void)copy_file("/private/var/mobile/Library/Preferences/.GlobalPreferences.plist",
			    "/private/var/preferences/.GlobalPreferences.plist", 0644, 0, 0);
			(void)copy_file("/private/var/mobile/Library/Preferences/.GlobalPreferences.plist",
			    "/private/var/root/Library/Preferences/.GlobalPreferences.plist", 0644, 0, 0);
		}
	}

	vfd = open("/private/var", O_RDONLY | O_DIRECTORY, 0);
	if (vfd >= 0) {
		(void)fsync(vfd);
		close(vfd);
	}
}

/*
 * The cryptexes go into the Preboot volume the way iOS grafts them (graftdmg): their files
 * become Preboot's, inode numbers and all, which LaunchServices needs to keep the WebKit
 * extensions in the OS cryptex registered (bsd/miscfs/apfs/apfs_graft.inc).  The request names
 * the volume the way mount(2)'s data does.  A kernel without grafts answers ENOTTY, and the
 * cryptex is then mounted on the directory instead.
 */
struct apfs_graft_req {
	char spec[128];
};
#define APFS_IOC_GRAFT 0x80804AFAUL     /* _IOW('J', 250, struct apfs_graft_req) */
#ifndef ENOTTY
#define ENOTTY 25
#endif

static int
attach_cryptex(const char *at, const char *spec, const char **how)
{
	struct apfs_graft_req req;

	memset(&req, 0, sizeof(req));
	strcpy(req.spec, spec);
	*how = "grafted";
	if (fsctl(at, APFS_IOC_GRAFT, &req, 0) == 0) {
		return 0;
	}
	if (errno != ENOTTY) {
		return -1;
	}
	*how = "mounted";
	return mount("apfs", at, MNT_RDONLY, (void *)spec);
}

/*
 * A cryptex onto /private/preboot/Cryptexes/NAME: container C (",c=N") of md0, or, when md0 has
 * no such container (EINVAL: the RAM part of the boot is the boot volume alone), the same
 * container of the device the iOS volume came from.  Neither: said, and a check fails.
 */
static void
mount_cryptex(const char *name, const char *container, const char *flashdev)
{
	char at[64], spec[128], what[200];
	const char *how = "";
	struct stat st;
	int err;

	strcpy(at, join("/private/preboot/Cryptexes/", name, ""));
	strcpy(spec, join("/dev/md0", container, ""));
	if (attach_cryptex(at, spec, &how) != 0) {
		err = errno;
		if (err != EINVAL) {
			printf("  the %s cryptex from %s: %s\n", name, spec, strerror(err));
		}
		strcpy(spec, join(flashdev, container, ""));
		if (attach_cryptex(at, spec, &how) != 0) {
			if (lstat(join("/System/Cryptexes/", name, ""), &st) == 0 &&
			    (st.st_mode & S_IFMT) == S_IFDIR) {
				strcpy(what, join("the ", name, " cryptex is in /System/Cryptexes"));
				check(1, what);
				return;
			}
			printf("  the %s cryptex from %s: %s\n", name, spec, strerror(errno));
			strcpy(what, join("the ", name, " cryptex: in neither md0 nor the flash"));
			check(0, what);
			return;
		}
	}
	os_cryptex_grafted |= strcmp(name, "OS") == 0 && strcmp(how, "grafted") == 0;
	strcpy(what, join("the ", name, " cryptex ("));
	strcpy(what, join(what, spec, ") "));
	strcpy(what, join(what, how, " on "));
	strcpy(what, join(what, at, ""));
	check(1, what);
}

static int
flash_root(const char *dev)
{
	static char buf[8192];
	char devpath[96], name[256], from[800], to[400];
	struct stat fw_st;
	off_t pos = 0;
	ssize_t n;
	int fd, ok, apps = 0, bound = 0, before = failures;

	printf("\n-- the iOS root volume on /dev/%s as the root --\n", dev);
	strcpy(devpath, join("/dev/", dev, ""));
	ok = mount("apfs", "/flash", MNT_RDONLY, devpath) == 0;
	if (!ok) {
		printf("  mount %s: %s\n", devpath, strerror(errno));
	}
	check(ok, "mount the iOS volume read only");
	if (!ok) {
		return 0;
	}
	check(mount_dir("/flash/annibale"), "prepare /annibale");
	check(mount_dir("/flash/private/preboot"), "prepare /private/preboot");
	static const struct { const char *from, *to; } mounts[] = {
		{ "/LaunchDaemons", "/flash/System/Library/LaunchDaemons" },
		{ "/empty", "/flash/System/Library/NanoLaunchDaemons" },
		{ "/empty", "/flash/System/Library/LaunchAngels" },
		{ "/xpc", "/flash/System/Library/xpc" },
	};
	for (unsigned i = 0; i < sizeof(mounts) / sizeof(mounts[0]); i++) {
		check(mount_dir(mounts[i].to) && bind_dir(in_image(mounts[i].from), mounts[i].to), mounts[i].to);
	}
	static const struct { const char *from, *to, *what; } bundles[] = {
		{ "/lib/AnnibaleAudio.driver", "/flash/System/Library/Audio/Plug-Ins/HAL/AnnibaleAudio.driver",
		  "our audio HAL bundle in /System/Library/Audio/Plug-Ins/HAL" },
		{ "/lib/AnnibaleMetal.bundle", "/flash/System/Library/Extensions/AnnibaleMetal.bundle",
		  "our Metal driver bundle in /System/Library/Extensions" },
	};
	for (unsigned i = 0; i < sizeof(bundles) / sizeof(bundles[0]); i++) {
		if (stat(in_image(bundles[i].from), &fw_st) == 0) {
			check(mount_dir(bundles[i].to) && bind_dir(in_image(bundles[i].from), bundles[i].to),
			    bundles[i].what);
		}
	}
	if (stat("/fw", &fw_st) == 0 && lstat("/flash/fw", &fw_st) != 0) {
		/* the audio DSP's firmware and the speakers' configuration, from the RAM image as in a RAM boot */
		check(synthetic_dir("/flash/fw") && bind_dir("/fw", "/flash/fw"), "the firmware on /fw (adsp=1, sound=N)");
	}
	if (stat("/jb-bin", &fw_st) == 0) {
		/*
		 * The package manager's prefix is in the boot volume (/jb), and Sileo runs mv, cp, rm, mkdir
		 * and chmod from /bin, where iOS has df and ps alone (userland/build.sh, SILEO_BIN): the boot
		 * volume's jb-bin -- those five, links into the prefix -- over the volume's /bin as a union
		 * mount, through which what it does not have is found in the /bin below.
		 */
		ok = mount("bindfs", "/flash/bin", MNT_RDONLY | MNT_UNION, (void *)"/jb-bin") == 0;
		if (!ok) {
			printf("  bindfs /jb-bin on /flash/bin, union: %s\n", strerror(errno));
		}
		check(ok, "Sileo's programs in /bin, over the volume's own");
	}

	fd = open("/flash/private/var/staged_system_apps", O_RDONLY | O_DIRECTORY, 0);
	while (fd >= 0 && (n = getdirentries(fd, buf, sizeof(buf), &pos)) > 0) {
		for (char *p = buf; p < buf + n;) {
			struct dirent *de = (struct dirent *)(void *)p;
			size_t len = de->d_namlen;

			if (de->d_reclen == 0) {
				break;
			}
			p += de->d_reclen;
			if (len < 5 || len >= sizeof(name) || strncmp(de->d_name + len - 4, ".app", 4) != 0) {
				continue;
			}
			memcpy(name, de->d_name, len);
			name[len] = 0;
			apps++;
			/* another name for it in /Applications, on the same volume: no mount point to skip */
			strcpy(to, join("/flash/Applications/", name, "="));
			strcpy(from, join(to, "/flash/private/var/staged_system_apps/", name));
			if (synthetic_dir(from)) {
				bound++;
			}
		}
	}
	if (fd >= 0) {
		close(fd);
		printf("  %d applications waiting to be installed, %d of them in /Applications\n", apps, bound);
		check(apps > 0 && bound == apps, "the first boot's applications in /Applications");
	} else {
		check(stat("/flash/Applications/Preferences.app", &fw_st) == 0,
		    "the system applications in /Applications");
	}

	if (failures != before) {
		return 0;
	}
	ok = sysctlbyname("kern.annibale_switch_root", NULL, NULL, "/flash private/preboot", 22) == 0;
	if (!ok) {
		printf("  switching the root: %s\n", strerror(errno));
	}
	check(ok, "the iOS volume is the root, the RAM part of the boot on /private/preboot");
	if (!ok) {
		return 0;
	}
	chdir("/");
	/*
	 * The cryptexes, where iOS grafts them: the boot volume's empty Cryptexes/OS and Cryptexes/App,
	 * now /private/preboot/Cryptexes/OS and App, where the iOS volume's /System/Cryptexes/OS and App
	 * point (and the kernel takes a shared cache from the first, bsd/vm/vm_unix.c).  Each from md0 when
	 * the RAM part of the boot has it (flashboot.apfs: the second and third containers), else from
	 * the flash -- darwin, where Apple's images lie as they are after the iOS volume (containers 1 and 2
	 * of the root's own device).
	 */
	mount_cryptex("OS", ",c=1", devpath);
	mount_cryptex("App", ",c=2", devpath);
	check(bind_dir("/private/preboot/annibale", "/annibale"), "this image back on /annibale");
	if (failures != before) {
		return 0;
	}
	flash_root_mode = 1;
	return 1;
}
