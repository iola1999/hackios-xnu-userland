/*
 * A look at a mounted volume from user space, on a system that has no ls of its own (the
 * iOS root has df and ps in /bin and nothing else): what ls -l and cat would show, how
 * the volume is mounted, and a hash of a file read both ways.
 *
 * usage: vol_ls fs PATH              the file system PATH is on: type, device, mount point, flags
 *        vol_ls ls DIR...            one line an entry, sorted: kind and mode, links, owner,
 *                                    group, size, name (and where a symbolic link points)
 *        vol_ls cat FILE [BYTES]     the file's first BYTES (default 4096), as they are
 *        vol_ls hash FILE...         FNV-1a of the whole file through read(2) and through a
 *                                    mapping, and its size: the hash tools/apfs prints on the
 *                                    build machine for the same file
 *        vol_ls sysctl NAME...       a string the kernel keeps (kern.apfs_stats: where the APFS
 *                                    driver's time went -- reads, page-ins, the lock)
 *        vol_ls zero NAME...         write to it (kern.apfs_stats: its counters zeroed)
 *        vol_ls cpuload SECONDS      how busy each processor is over the next SECONDS
 *        vol_ls fsid PATH...         inspect statfs/getattrlist identity, volume UUID, and
 *                                    fsgetpath round-trip for each path
 */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/attr.h>
#include <sys/fsgetpath.h>
#include <sys/mman.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <unistd.h>
#include <mach/mach.h>
#include <mach/processor_info.h>

#define FNV_INIT  0xcbf29ce484222325ULL
#define FNV_PRIME 0x100000001b3ULL

static int
by_name(const void *a, const void *b)
{
	return strcmp(*(char *const *)a, *(char *const *)b);
}

static void
mode_string(mode_t m, char *out)
{
	const char *rwx = "rwxrwxrwx";
	int i;

	out[0] = S_ISDIR(m) ? 'd' : S_ISLNK(m) ? 'l' : S_ISREG(m) ? '-' : S_ISCHR(m) ? 'c' :
	    S_ISBLK(m) ? 'b' : S_ISFIFO(m) ? 'p' : S_ISSOCK(m) ? 's' : '?';
	for (i = 0; i < 9; i++) {
		out[1 + i] = (m & (0400 >> i)) ? rwx[i] : '-';
	}
	if (m & S_ISUID) {
		out[3] = (m & 0100) ? 's' : 'S';
	}
	if (m & S_ISGID) {
		out[6] = (m & 0010) ? 's' : 'S';
	}
	if (m & S_ISVTX) {
		out[9] = (m & 0001) ? 't' : 'T';
	}
	out[10] = '\0';
}

static int
do_fs(const char *path)
{
	struct statfs sf;

	if (statfs(path, &sf) != 0) {
		printf("vol_ls: %s: statfs: %s\n", path, strerror(errno));
		return 1;
	}
	printf("vol_ls: %s is on %s, a %s file system mounted on %s%s%s%s; %llu blocks of %u, "
	    "%llu files\n", path, sf.f_mntfromname, sf.f_fstypename, sf.f_mntonname,
	    (sf.f_flags & MNT_RDONLY) ? ", read only" : ", writable",
	    (sf.f_flags & MNT_NOSUID) ? ", nosuid" : "", (sf.f_flags & MNT_NODEV) ? ", nodev" : "",
	    (unsigned long long)sf.f_blocks, (unsigned)sf.f_bsize, (unsigned long long)sf.f_files);
	return 0;
}

static int
do_ls(const char *dir)
{
	DIR *d = opendir(dir);
	struct dirent *de;
	char **names = NULL;
	size_t n = 0, cap = 0, i;
	int bad = 0;

	if (d == NULL) {
		printf("vol_ls: %s: %s\n", dir, strerror(errno));
		return 1;
	}
	while ((de = readdir(d)) != NULL) {
		if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0) {
			continue;
		}
		if (n == cap) {
			cap = cap ? cap * 2 : 64;
			names = realloc(names, cap * sizeof(*names));
			if (names == NULL) {
				closedir(d);
				return 1;
			}
		}
		names[n++] = strdup(de->d_name);
	}
	closedir(d);
	qsort(names, n, sizeof(*names), by_name);
	printf("vol_ls: %s: %zu entries\n", dir, n);
	for (i = 0; i < n; i++) {
		char path[PATH_MAX], mode[11], target[PATH_MAX];
		struct stat st;

		snprintf(path, sizeof(path), "%s/%s", dir, names[i]);
		if (lstat(path, &st) != 0) {
			printf("  %s: %s\n", names[i], strerror(errno));
			bad++;
			free(names[i]);
			continue;
		}
		mode_string(st.st_mode, mode);
		if (S_ISLNK(st.st_mode)) {
			ssize_t len = readlink(path, target, sizeof(target) - 1);

			target[len > 0 ? len : 0] = '\0';
			/* the time of writing as the seconds of the epoch: whether a name is this boot's */
			printf("  %s %3u %4u %4u %12lld %11lld %s -> %s\n", mode, (unsigned)st.st_nlink,
			    (unsigned)st.st_uid, (unsigned)st.st_gid, (long long)st.st_size,
			    (long long)st.st_mtimespec.tv_sec, names[i], target);
		} else {
			printf("  %s %3u %4u %4u %12lld %11lld %s\n", mode, (unsigned)st.st_nlink,
			    (unsigned)st.st_uid, (unsigned)st.st_gid, (long long)st.st_size,
			    (long long)st.st_mtimespec.tv_sec, names[i]);
		}
		free(names[i]);
	}
	free(names);
	return bad;
}

static int
do_cat(const char *file, long want)
{
	char *buf;
	ssize_t got;
	int fd = open(file, O_RDONLY);

	if (fd < 0) {
		printf("vol_ls: %s: %s\n", file, strerror(errno));
		return 1;
	}
	buf = malloc((size_t)want);
	got = buf != NULL ? read(fd, buf, (size_t)want) : -1;
	close(fd);
	if (got < 0) {
		printf("vol_ls: %s: read: %s\n", file, strerror(errno));
		free(buf);
		return 1;
	}
	printf("vol_ls: %s, the first %zd bytes:\n", file, got);
	fwrite(buf, 1, (size_t)got, stdout);
	if (got > 0 && buf[got - 1] != '\n') {
		putchar('\n');
	}
	free(buf);
	return 0;
}

static int
do_hash(const char *file)
{
	static unsigned char buf[65536];
	unsigned long long h_read = FNV_INIT, h_map = FNV_INIT;
	struct stat st;
	ssize_t got;
	off_t i;
	int fd = open(file, O_RDONLY);

	if (fd < 0 || fstat(fd, &st) != 0) {
		printf("vol_ls: %s: %s\n", file, strerror(errno));
		if (fd >= 0) {
			close(fd);
		}
		return 1;
	}
	while ((got = read(fd, buf, sizeof(buf))) > 0) {
		for (i = 0; i < got; i++) {
			h_read = (h_read ^ buf[i]) * FNV_PRIME;
		}
	}
	if (st.st_size > 0) {
		const unsigned char *m = mmap(NULL, (size_t)st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);

		if (m == MAP_FAILED) {
			printf("vol_ls: %s: mmap: %s\n", file, strerror(errno));
			close(fd);
			return 1;
		}
		for (i = 0; i < st.st_size; i++) {
			h_map = (h_map ^ m[i]) * FNV_PRIME;
		}
		munmap((void *)m, (size_t)st.st_size);
	}
	close(fd);
	printf("vol_ls: %s: %lld bytes, read %016llx, mapped %016llx%s\n", file,
	    (long long)st.st_size, h_read, h_map, h_read == h_map ? "" : " -- they differ");
	return h_read == h_map ? 0 : 1;
}

struct fsid_obj_attr {
	uint32_t length;
	dev_t dev;
	fsid_t fsid;
	fsobj_id_t objid;
	uint64_t fileid;
};

struct fsid_vol_uuid_attr {
	uint32_t length;
	unsigned char uuid[16];
};

static void
format_uuid(const unsigned char u[16], char out[37])
{
	snprintf(out, 37,
	    "%02X%02X%02X%02X-%02X%02X-%02X%02X-%02X%02X-%02X%02X%02X%02X%02X%02X",
	    u[0], u[1], u[2], u[3], u[4], u[5], u[6], u[7],
	    u[8], u[9], u[10], u[11], u[12], u[13], u[14], u[15]);
}

static int
do_fsid(const char *path)
{
	struct stat st, st2;
	struct statfs sf;
	struct attrlist al_obj, al_vol;
	struct fsid_obj_attr obj, obj2;
	struct fsid_vol_uuid_attr vol;
	char uuid_str[37] = "none";
	char resolved[PATH_MAX];
	const char *src = "none";
	const char *fail_reason = NULL;
	fsid_t query_fsid;
	ssize_t got = -1;
	int err = 0;

	if (stat(path, &st) != 0) {
		printf("vol_ls: fsid result=fail stage=stat errno=%d err=\"%s\" path=\"%s\"\n",
		    errno, strerror(errno), path);
		return 1;
	}
	if (statfs(path, &sf) != 0) {
		printf("vol_ls: fsid result=fail stage=statfs errno=%d err=\"%s\" path=\"%s\"\n",
		    errno, strerror(errno), path);
		return 1;
	}

	memset(&al_obj, 0, sizeof(al_obj));
	al_obj.bitmapcount = ATTR_BIT_MAP_COUNT;
	al_obj.commonattr = ATTR_CMN_DEVID | ATTR_CMN_FSID | ATTR_CMN_OBJID | ATTR_CMN_FILEID;
	memset(&obj, 0, sizeof(obj));
	if (getattrlist(path, &al_obj, &obj, sizeof(obj), 0) != 0) {
		printf("vol_ls: fsid result=fail stage=getattrlist errno=%d err=\"%s\" path=\"%s\"\n",
		    errno, strerror(errno), path);
		return 1;
	}
	if (obj.length < sizeof(obj)) {
		printf("vol_ls: fsid result=fail stage=getattrlist err=\"short reply %u\" path=\"%s\"\n",
		    obj.length, path);
		return 1;
	}

	memset(&al_vol, 0, sizeof(al_vol));
	al_vol.bitmapcount = ATTR_BIT_MAP_COUNT;
	al_vol.volattr = ATTR_VOL_INFO | ATTR_VOL_UUID;
	memset(&vol, 0, sizeof(vol));
	if (getattrlist(sf.f_mntonname, &al_vol, &vol, sizeof(vol), 0) == 0 &&
	    vol.length >= sizeof(vol)) {
		format_uuid(vol.uuid, uuid_str);
	}

	resolved[0] = '\0';
	query_fsid = sf.f_fsid;
	got = fsgetpath(resolved, sizeof(resolved), &query_fsid, obj.fileid);
	if (got > 0 && (size_t)got <= sizeof(resolved)) {
		src = "statfs";
	} else {
		err = errno;
		if (obj.fsid.val[0] != sf.f_fsid.val[0] || obj.fsid.val[1] != sf.f_fsid.val[1]) {
			query_fsid = obj.fsid;
			got = fsgetpath(resolved, sizeof(resolved), &query_fsid, obj.fileid);
			if (got > 0 && (size_t)got <= sizeof(resolved)) {
				src = "getattrlist";
				err = 0;
			} else {
				err = errno;
			}
		}
	}

	if (obj.dev != st.st_dev || obj.fileid != (uint64_t)st.st_ino) {
		fail_reason = "stat_getattrlist_mismatch";
	} else if (strcmp(src, "none") == 0) {
		fail_reason = "fsgetpath_failed";
	} else if (stat(resolved, &st2) != 0) {
		err = errno;
		fail_reason = "restat_failed";
	} else if (st2.st_dev != st.st_dev || st2.st_ino != st.st_ino) {
		fail_reason = "restat_mismatch";
	} else {
		memset(&obj2, 0, sizeof(obj2));
		if (getattrlist(resolved, &al_obj, &obj2, sizeof(obj2), 0) != 0 ||
		    obj2.length < sizeof(obj2)) {
			err = errno;
			fail_reason = "regetattrlist_failed";
		} else if (obj2.fileid != obj.fileid ||
		    obj2.fsid.val[0] != obj.fsid.val[0] || obj2.fsid.val[1] != obj.fsid.val[1]) {
			fail_reason = "regetattrlist_mismatch";
		}
	}

	printf("vol_ls: fsid result=%s fs=%s dev=0x%x ino=%llu sf_fsid=%08x:%08x ga_fsid=%08x:%08x "
	    "objid=%08x:%08x fileid=%llu vol_uuid=%s roundtrip=%s src=%s",
	    fail_reason == NULL ? "ok" : "fail", sf.f_fstypename, (unsigned)st.st_dev,
	    (unsigned long long)st.st_ino, (unsigned)sf.f_fsid.val[0], (unsigned)sf.f_fsid.val[1],
	    (unsigned)obj.fsid.val[0], (unsigned)obj.fsid.val[1],
	    (unsigned)obj.objid.fid_objno, (unsigned)obj.objid.fid_generation,
	    (unsigned long long)obj.fileid, uuid_str,
	    fail_reason == NULL ? "ok" : "fail", src);
	if (fail_reason != NULL) {
		printf(" reason=%s", fail_reason);
		if (err != 0) {
			printf(" errno=%d err=\"%s\"", err, strerror(err));
		}
	}
	printf(" path=\"%s\" mnt=\"%s\" resolved=\"%s\"\n", path, sf.f_mntonname, resolved);
	return fail_reason == NULL ? 0 : 1;
}

int
main(int argc, char **argv)
{
	int i, bad = 0;

	if (argc < 3) {
		printf("usage: vol_ls fs PATH | ls DIR... | cat FILE [BYTES] | hash FILE... | "
		    "fsid PATH... | sysctl NAME...\n");
		return 2;
	}
	if (strcmp(argv[1], "fs") == 0) {
		return do_fs(argv[2]);
	}
	if (strcmp(argv[1], "cat") == 0) {
		return do_cat(argv[2], argc > 3 ? atol(argv[3]) : 4096);
	}
	if (strcmp(argv[1], "cpuload") == 0) {
		/*
		 * How busy every processor was over the next SECONDS: user, system, idle, and its
		 * interrupts (IPIs among them).  On the phone the slower cores' (4-6) idle count runs
		 * ~56 times too fast -- the "all" line comes out at 1% user while each fast core shows
		 * ~43% -- so their "99% idle" says nothing; their IPIs and the threads that run there
		 * (kern.apfs_stats: where long waits ended) show they work.
		 */
		processor_info_array_t a = NULL, b = NULL, sa = NULL, sb = NULL;
		mach_msg_type_number_t na = 0, nb = 0, nsa = 0, nsb = 0;
		natural_t ncpu = 0, n2 = 0, n3 = 0, n4 = 0;
		unsigned seconds = (unsigned)atoi(argv[2]);
		unsigned long long tu = 0, ts = 0, ti = 0;

		if (host_processor_info(mach_host_self(), PROCESSOR_CPU_LOAD_INFO, &ncpu, &a, &na) != KERN_SUCCESS) {
			printf("cpuload: host_processor_info failed\n");
			return 1;
		}
		(void)host_processor_info(mach_host_self(), PROCESSOR_CPU_STAT64, &n3, &sa, &nsa);
		sleep(seconds);
		(void)host_processor_info(mach_host_self(), PROCESSOR_CPU_STAT64, &n4, &sb, &nsb);
		if (host_processor_info(mach_host_self(), PROCESSOR_CPU_LOAD_INFO, &n2, &b, &nb) != KERN_SUCCESS || n2 != ncpu) {
			printf("cpuload: host_processor_info failed\n");
			return 1;
		}
		for (natural_t c = 0; c < ncpu; c++) {
			const unsigned *x = (const unsigned *)a + c * CPU_STATE_MAX, *y = (const unsigned *)b + c * CPU_STATE_MAX;
			unsigned u = y[CPU_STATE_USER] - x[CPU_STATE_USER] + y[CPU_STATE_NICE] - x[CPU_STATE_NICE];
			unsigned sy = y[CPU_STATE_SYSTEM] - x[CPU_STATE_SYSTEM], id = y[CPU_STATE_IDLE] - x[CPU_STATE_IDLE];
			unsigned all = u + sy + id;

			printf("cpuload: cpu %u over %u s: user %u%%, system %u%%, idle %u%%", c, seconds,
			    all ? u * 100 / all : 0, all ? sy * 100 / all : 0, all ? id * 100 / all : 0);
			if (sa != NULL && sb != NULL && n3 == ncpu && n4 == ncpu) {
				/* (the 64-bit form: packed, 4-byte aligned, so read field by field) */
				const struct processor_cpu_stat64 *p = (const struct processor_cpu_stat64 *)sa + c;
				const struct processor_cpu_stat64 *q = (const struct processor_cpu_stat64 *)sb + c;

				printf("; interrupts %llu, of them IPIs %llu, timer %llu",
				    (unsigned long long)(q->irq_ex_cnt - p->irq_ex_cnt), (unsigned long long)(q->ipi_cnt - p->ipi_cnt),
				    (unsigned long long)(q->timer_cnt - p->timer_cnt));
			}
			printf("\n");
			tu += u;
			ts += sy;
			ti += id;
		}
		printf("cpuload: all %u processors over %u s: user %llu%%, system %llu%%, idle %llu%%\n", ncpu, seconds,
		    (tu + ts + ti) ? tu * 100 / (tu + ts + ti) : 0, (tu + ts + ti) ? ts * 100 / (tu + ts + ti) : 0,
		    (tu + ts + ti) ? ti * 100 / (tu + ts + ti) : 0);
		return 0;
	}
	if (strcmp(argv[1], "zero") == 0) {
		/* (kern.apfs_stats: writing anything zeroes the counters) */
		for (i = 2; i < argc; i++) {
			char zero = 0;

			if (sysctlbyname(argv[i], NULL, NULL, &zero, 1) != 0) {
				printf("%s: %s\n", argv[i], strerror(errno));
				bad++;
			}
		}
		return bad ? 1 : 0;
	}
	if (strcmp(argv[1], "sysctl") == 0) {
		for (i = 2; i < argc; i++) {
			static char text[16384];
			size_t len = sizeof(text) - 1;

			if (sysctlbyname(argv[i], text, &len, NULL, 0) != 0) {
				printf("%s: %s\n", argv[i], strerror(errno));
				bad++;
				continue;
			}
			text[len] = 0;
			printf("%s:\n%s\n", argv[i], text);
		}
		return bad ? 1 : 0;
	}
	for (i = 2; i < argc; i++) {
		if (strcmp(argv[1], "ls") == 0) {
			bad += do_ls(argv[i]);
		} else if (strcmp(argv[1], "hash") == 0) {
			bad += do_hash(argv[i]);
		} else if (strcmp(argv[1], "fsid") == 0) {
			bad += do_fsid(argv[i]);
		} else {
			printf("vol_ls: no such thing as %s\n", argv[1]);
			return 2;
		}
	}
	return bad ? 1 : 0;
}
