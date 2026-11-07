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

static int
mount_writable(void)
{
	static char skeleton[16384];
	static const char *const markers[] = { "/private/var/db/diagnostics/.migrated",
		                               "/private/var/db/diagnostics/timesync/.migrated", "/private/var/db/uuidtext/.migrated" };
	int ok, fd, made = 0, files = 0;
	const char *jb;
	struct stat st;

	printf("\n-- writable filesystem --\n");
	ok = mount("ramfs", "/private/var", 0, NULL) == 0;
	if (!ok) {
		printf("  mount: %s\n", strerror(errno));
	}
	check(ok, "mount the persistent data volume on /private/var");
	if (!ok) {
		return 0;
	}

	/* the directories of the data volume, as listed in the template that ships with iOS (ios165/mkvarskel.py) */
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
 * What the last boot left in /private/var, back out of the phone's flash (var_store=1): the
 * memory filesystem is empty at every boot, so everything iOS writes -- the home screen's layout,
 * the applications' own data, the snapshots and the baked wallpaper -- would otherwise be new
 * again every time. userland/src/var_store.c keeps a copy at the end of the DSU slot, past the
 * system image, and puts it back here, over the skeleton: a name the skeleton has and
 * the store has too comes out as the store has it. A store that is not there or does not check
 * out leaves what the skeleton made, and the boot goes on either way -- so a bad store cannot
 * keep the phone from starting.
 */
static void
restore_var(void)
{
	const char *wanted = boot_arg("var_store");
	int disk_loaded = 0, status = 0, vfd;
	size_t len = sizeof(disk_loaded);
	char *argv[3];
	pid_t pid;

	if (sysctlbyname("kern.redwood.var_disk_loaded", &disk_loaded, &len, NULL, 0) == 0 && disk_loaded != 0) {
		printf("\n-- the data volume of the last boot --\n");
		printf("  /private/var loaded directly from persistent disk (RDWFS001)\n");
	} else if (wanted != NULL && strcmp(wanted, "1") == 0) {
		printf("\n-- the data volume of the last boot --\n");
		argv[0] = (char *)in_image("/bin/var_store");
		argv[1] = (char *)"load";
		argv[2] = NULL;
		pid = fork();
		if (pid == 0) {
			execve(argv[0], argv, environ);
			printf("  %s: %s\n", argv[0], strerror(errno));
			_exit(127);
		}
		if (pid < 0 || wait4(pid, &status, 0, NULL) != pid || !WIFEXITED(status)) {
			printf("  var_store did not run\n");
		}
	}

	/* Ensure essential directories exist even when loaded directly from persistent disk */
	mkdir("/private/var/mobile/Library/SyncedPreferences", 0755);
	chown("/private/var/mobile/Library/SyncedPreferences", 501, 501);
	mkdir("/private/var/mobile/Library/Weather", 0755);
	chown("/private/var/mobile/Library/Weather", 501, 501);
	mkdir("/private/var/db/locationd", 0755);
	chown("/private/var/db/locationd", 205, 205);
	mkdir("/private/var/db/redwood-aircache", 0777);
	chmod("/private/var/db/redwood-aircache", 0777);
	mkdir("/private/var/db/redwood-ir3cache", 0777);
	chmod("/private/var/db/redwood-ir3cache", 0777);
	mkdir("/private/var/preferences", 0755);
	mkdir("/private/var/root", 0750);
	mkdir("/private/var/root/Library", 0750);
	mkdir("/private/var/root/Library/Preferences", 0750);
	{
		struct stat gp_st;
		if (stat("/private/var/mobile/Library/Preferences/.GlobalPreferences.plist", &gp_st) == 0 && gp_st.st_size > 0) {
			(void)copy_file("/private/var/mobile/Library/Preferences/.GlobalPreferences.plist",
			    "/private/var/preferences/.GlobalPreferences.plist", 0644, 0, 0);
			(void)copy_file("/private/var/mobile/Library/Preferences/.GlobalPreferences.plist",
			    "/private/var/root/Library/Preferences/.GlobalPreferences.plist", 0644, 0, 0);
		}
	}

	int seed_done = 1;
	(void)sysctlbyname("kern.redwood.var_seed_done", NULL, NULL, &seed_done, sizeof(seed_done));

	vfd = open("/private/var", O_RDONLY | O_DIRECTORY, 0);
	if (vfd >= 0) {
		(void)fsync(vfd);
		close(vfd);
	}
}
