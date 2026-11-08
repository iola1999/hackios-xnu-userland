/*
 * The data volume of iOS between two boots: /private/var packed into the phone's flash and
 * unpacked out of it again.
 *
 * /private/var is a memory filesystem (bsd/miscfs/ramfs), mounted empty by init at every boot and
 * given the directories of the template iOS ships with (ios165/mkvarskel.py), so nothing iOS keeps
 * there outlives the boot that wrote it: no home screen layout, no application data, no baked
 * wallpaper. The kernel's APFS driver reads and does not write, and writing APFS is not the point
 * here -- what is needed is that the bytes come back. So the tree is packed into a plain archive
 * and the archive is written into the one place on the phone's flash this port owns: the last
 * 4 GiB of the slot Android's DSU installer keeps the iOS system image in, past the image. The
 * loader lists it as a partition of its own (xnuloader/loader_dsu.inc), and the kernel takes
 * writes on /dev/rufs_dsu_store alone and refuses anything else (bsd/dev/annibale_ufs.c,
 * ufs_write=1). Installing the DSU again (tools/k90auto/dsu.py install) zeroes the store.
 *
 *   var_store save          pack /private/var into the store
 *   var_store load          unpack the newest good store into /private/var (init, at boot)
 *   var_store show          what the store holds, both slots
 *   var_store erase         forget both slots (the next boot starts empty again)
 *   var_store stamp PATH    write a line into PATH saying when: a marker for a run
 *
 * save, load and show take a FILE after them to use a file in the flash's place: one slot, its
 * header at the start of the file. `load` without one reads the flash first, and when that holds
 * no good archive takes etc/var.seed from its own image: a seed is what `save FILE` packed at the
 * end of an earlier boot (tools/k90auto/package.py SEED=...), so even a first boot starts with the
 * LaunchServices database, the icon cache and PosterBoard's data that boot spent its first
 * minutes building.
 *
 * One saver at a time: a write to the store is refused unless the writer holds the claim
 * (kern.annibale.var_busy), so two of them cannot interleave into one slot. Even if they did, the
 * slot being written is never the one holding the newest good archive, and a header is written
 * last and over a zeroed one -- so the worst an interrupted or doubled save can do is lose itself.
 *
 * Two slots, written in turn: the one not holding the newest good archive is the one written, its
 * header first zeroed, the archive next, the header last. So a save that does not finish -- the
 * phone reset in the middle of it -- loses nothing: the other slot still holds the archive from
 * before, and `load` takes the newest slot whose header and contents both check out.
 *
 * `load` reads the archive twice: once to see that its checksum is the one in the header, and only
 * then to make files. A store that was torn or is half a different archive therefore leaves
 * /private/var as init made it rather than half filled.
 *
 * What is left out: sockets and devices, which the daemons of a new boot make for themselves, and
 * the directories in SKIP below, which hold what only this boot can mean (its runtime sockets, its
 * swap, its logs) or what is grafted from a read-only image at every boot anyway.
 */
#include "mini.h"

#define BLOCK           4096
#define BUFFER          (4u << 20)              /* the loader's own buffer: one request a write */
#define STORE_AT        0LL
#define SLOT_BYTES      0x80000000LL            /* 2 GiB a slot, two slots (xnuloader/loader_dsu.inc DSU_STORE_SLOT) */
#define DELTA_OFFSET    0xFB000000LL
#define DELTA_BYTES     (64u << 20)
#define SLOTS           2
#define DEVICE_DARWIN_VAR "/dev/rufs_darwin_var"
#define DEVICE_DSU      "/dev/rufs_dsu_store"
#define DEVICE          (default_device())
#define ROOT            "/private/var"
#define MAX_PATH        1024
#define MAX_DEPTH       32
#define MAX_XATTR       (256u << 10)            /* one attribute's value; iOS keeps small ones */
#define XPACK           (512u << 10)            /* all the attributes of one file, packed */
#define LINKS           8192                    /* names remembered for hard links */
#define LINK_PATH       256
/*
 * The file flags put back: the ones a tree of files can mean. UF_COMPRESSED is not among them
 * (a ramfs file is not a compressed HFS one, and the flag would make it read as empty), nor are
 * the flags only a single-user boot can clear.
 */
#define FLAGS_KEPT      (0x1u | 0x2u | 0x4u | 0x8u | 0x8000u)   /* nodump, immutable, append, opaque, hidden */

/* the directories whose contents are not kept (the directory itself is) */
static const char *const SKIP[] = {
	"tmp",                  /* its name says it */
	"run",                  /* runtime sockets and pid files: this boot's, not the last one's */
	"vm",                   /* the compressor's swap */
	"log", "logs",          /* what the last boot said */
	"db/diagnostics", "db/uuidtext",        /* logd's archive: tens of MB a boot, and remade */
	"jb",                   /* the package prefix, grafted from the read-only image at every boot */
	NULL,
};

enum { ENT_DIR = 1, ENT_FILE = 2, ENT_SYMLINK = 3, ENT_HARDLINK = 4, ENT_DIRTIME = 5, ENT_DELETE = 6 };

#define HEAD_MAGIC      "ANNVAR\0"
#define HEAD_VERSION    1

struct head {
	char     magic[8];
	uint32_t version;
	uint32_t slot;
	uint64_t generation;            /* the newest good slot is the one with the highest */
	uint64_t bytes;                 /* the archive, without the padding to a whole block */
	uint64_t entries;
	uint64_t seconds;               /* when it was written */
	uint32_t crc;                   /* of those bytes */
	uint32_t head_crc;              /* of this structure with head_crc zero */
};

struct entry {
	uint32_t kind;
	uint32_t mode;                  /* permission bits */
	uint32_t uid, gid;
	uint32_t flags;                 /* st_flags */
	uint32_t pathlen;
	uint32_t nxattr;
	uint32_t mtime_nsec;
	int64_t  mtime_sec;
	uint64_t size;                  /* a file's contents, a link's target, a hard link's first name */
};

/* ---- the kernel's side of it: the claim ---- */

static int
sysctl_set_int(const char *name, int value)
{
	return sysctlbyname(name, NULL, NULL, &value, sizeof(value));
}

/* The claim on the store: nothing may be written without it, and only one program may hold it. */
static int
claim(void)
{
	if (sysctl_set_int("kern.annibale.var_busy", 1) != 0) {
		printf("var_store: the store is claimed by another saver (%s)\n", strerror(errno));
		return 0;
	}
	return 1;
}

static void
release(void)
{
	(void)sysctl_set_int("kern.annibale.var_busy", 0);
}

/* ---- checksum (the same one the loader reads a write back with) ---- */

static uint32_t crc_table[256];

static void
crc_init(void)
{
	for (uint32_t i = 0; i < 256; i++) {
		uint32_t c = i;

		for (int k = 0; k < 8; k++) {
			c = (c >> 1) ^ (0xedb88320u & (0u - (c & 1)));
		}
		crc_table[i] = c;
	}
}

static uint32_t
crc_add(uint32_t c, const void *p, uint64_t n)
{
	const uint8_t *b = p;

	for (uint64_t i = 0; i < n; i++) {
		c = crc_table[(c ^ b[i]) & 0xff] ^ (c >> 8);
	}
	return c;
}

static uint32_t
head_checksum(const struct head *h)
{
	struct head copy = *h;

	copy.head_crc = 0;
	return crc_add(0xffffffffu, &copy, sizeof(copy)) ^ 0xffffffffu;
}

/* ---- the store, as a stream of bytes in one slot ---- */

static int fd = -1;
static const char *store = DEVICE_DARWIN_VAR;   /* or a FILE: see the top */
static off_t store_at = STORE_AT;
static int slots = SLOTS;

static const char *
default_device(void)
{
	struct stat st;
	if (stat(DEVICE_DARWIN_VAR, &st) == 0) {
		return DEVICE_DARWIN_VAR;
	}
	if (stat(DEVICE_DSU, &st) == 0) {
		return DEVICE_DSU;
	}
	return DEVICE_DARWIN_VAR;
}
static uint8_t *buffer;
static uint8_t *already;                /* what the slot holds there now, to not write it again */
static uint64_t skipped_blocks, written_blocks;
static uint64_t buffered;               /* bytes in the buffer: to be written, or still to be read */
static uint64_t taken;                  /* of those, already read out (load) */
static off_t at;                        /* where in the device the buffer begins */
static uint64_t stream_bytes;           /* the archive's own bytes so far */
static uint64_t stream_left;            /* bytes of the archive still to come (load) */
static off_t stream_limit;              /* the end of the slot: nothing is read or written past it */
static uint32_t stream_crc;
static uint64_t entries;

static off_t
slot_at(int slot)
{
	return store_at + (off_t)slot * SLOT_BYTES;
}

static void
set_background_qos(void)
{
	setpriority(PRIO_PROCESS, 0, 19);
	setpriority(PRIO_DARWIN_PROCESS, 0, PRIO_DARWIN_BG);
	setpriority(PRIO_DARWIN_THREAD, 0, PRIO_DARWIN_BG);
}

static int
device_write(off_t where, const void *data, uint64_t n)
{
	const uint8_t *p = data;

	while (n > 0) {
		uint64_t chunk = n > BUFFER ? BUFFER : n;
		ssize_t wrote = pwrite(fd, p, (size_t)chunk, where);

		if (wrote <= 0) {
			printf("var_store: writing %llu bytes at %lld: %s\n", (unsigned long long)chunk,
			    (long long)where, strerror(errno));
			return 0;
		}
		p += wrote;
		where += wrote;
		n -= (uint64_t)wrote;
		usleep(10000);
	}
	return 1;
}

static int
device_read(off_t where, void *data, uint64_t n)
{
	uint8_t *p = data;

	while (n > 0) {
		ssize_t got = pread(fd, p, (size_t)n, where);

		if (got <= 0) {
			printf("var_store: reading %llu bytes at %lld: %s\n", (unsigned long long)n,
			    (long long)where, strerror(errno));
			return 0;
		}
		p += got;
		where += got;
		n -= (uint64_t)got;
	}
	return 1;
}

/* ---- writing the archive out ---- */

static int
out_flush(int last)
{
	uint64_t n = buffered;

	if (n == 0) {
		return 1;
	}
	if (last) {
		/* whole blocks only: the tail is padded, and the header says where the archive ends */
		while (n % BLOCK != 0) {
			buffer[n++] = 0;
		}
	}
	if (at + (off_t)n > stream_limit) {
		printf("var_store: the archive does not fit in its slot: nothing more is written\n");
		return 0;
	}
	/*
	 * What is already there is read first, and if it is what we were about to write, it is not
	 * written. A volume that iOS has stopped touching packs to the very same bytes -- measured:
	 * two saves a minute apart came out with the same checksum -- and the slots hold alternate
	 * copies of it, so a save that changes nothing then costs reads and no writes at all. Reads
	 * of this storage are several times cheaper than writes, and flash wears by writing.
	 */
	if (already != NULL && device_read(at, already, n) && memcmp(already, buffer, (size_t)n) == 0) {
		skipped_blocks += n / BLOCK;
	} else if (!device_write(at, buffer, n)) {
		return 0;
	} else {
		written_blocks += n / BLOCK;
	}
	at += (off_t)n;
	buffered = 0;
	return 1;
}

static int
out(const void *data, uint64_t n)
{
	const uint8_t *p = data;

	stream_crc = crc_add(stream_crc, data, n);
	stream_bytes += n;
	while (n > 0) {
		uint64_t room = BUFFER - buffered;

		if (room == 0) {
			if (!out_flush(0)) {
				return 0;
			}
			continue;
		}
		if (room > n) {
			room = n;
		}
		memcpy(buffer + buffered, p, (size_t)room);
		buffered += room;
		p += room;
		n -= room;
	}
	return 1;
}

static int
out_pad(void)
{
	static const uint8_t zero[8] = { 0 };
	uint64_t over = stream_bytes % 8;

	return over == 0 ? 1 : out(zero, 8 - over);
}

/* ---- reading the archive in ---- */

static int
in(void *data, uint64_t n)
{
	uint8_t *p = data;

	if (n > stream_left) {
		printf("var_store: the archive ends in the middle of an entry\n");
		return 0;
	}
	stream_left -= n;
	stream_bytes += n;
	while (n > 0) {
		uint64_t have = buffered - taken;

		if (have == 0) {
			uint64_t want = BUFFER;

			if (at + (off_t)want > stream_limit) {
				want = (uint64_t)(stream_limit - at);
			}
			if (want == 0 || !device_read(at, buffer, want)) {
				return 0;
			}
			at += (off_t)want;
			buffered = want;
			taken = 0;
			continue;
		}
		if (have > n) {
			have = n;
		}
		if (p != NULL) {
			memcpy(p, buffer + taken, (size_t)have);
			p += have;
		}
		stream_crc = crc_add(stream_crc, buffer + taken, have);
		taken += have;
		n -= have;
	}
	return 1;
}

/* n bytes of the archive passed over, their checksum counted in */
static int
in_skip(uint64_t n)
{
	uint8_t waste[512];

	while (n > 0) {
		uint64_t piece = n > sizeof(waste) ? sizeof(waste) : n;

		if (!in(waste, piece)) {
			return 0;
		}
		n -= piece;
	}
	return 1;
}

static int
in_pad(void)
{
	uint64_t over = stream_bytes % 8;

	return over == 0 ? 1 : in_skip(8 - over);
}

static void
stream_start(int slot, uint64_t bytes)
{
	at = slot_at(slot) + BLOCK;
	/* a load reads no further than the archive's last block: a store in a file ends there */
	off_t slot_limit = (slot == 0 || store_at == 0) ? slot_at(slot) + SLOT_BYTES : (off_t)DELTA_OFFSET;
	stream_limit = bytes != 0 ? at + (off_t)((bytes + BLOCK - 1) / BLOCK * BLOCK) : slot_limit;
	buffered = taken = 0;
	stream_bytes = 0;
	stream_left = bytes;
	stream_crc = 0xffffffffu;
}

/* ---- the slots ---- */

/* The slot's header, if it is one of ours and says of itself what it should. */
static int
read_head(int slot, struct head *into)
{
	static uint8_t block[BLOCK];
	struct head *h = (struct head *)block;

	if (!device_read(slot_at(slot), block, BLOCK)) {
		return 0;
	}
	if (memcmp(h->magic, HEAD_MAGIC, sizeof(h->magic)) != 0 || h->version != HEAD_VERSION ||
	    h->slot != (uint32_t)slot || h->bytes > (uint64_t)SLOT_BYTES - BLOCK ||
	    h->head_crc != head_checksum(h)) {
		return 0;
	}
	*into = *h;
	return 1;
}

/* The newest slot whose header checks out, or -1. */
static int
newest(struct head *out_head)
{
	int best = -1;

	for (int slot = 0; slot < slots; slot++) {
		struct head h;

		if (!read_head(slot, &h)) {
			continue;
		}
		if (best < 0 || h.generation > out_head->generation) {
			best = slot;
			*out_head = h;
		}
	}
	return best;
}

/* ---- packing ---- */

/*
 * How much of this save is new since the last one: an entry whose time of writing is later than
 * the second the previous generation was saved at. It costs nothing to count -- the walk has every
 * entry's time in hand anyway -- and it is what says whether saving only the difference would be
 * worth building, or whether iOS rewrites the volume faster than any such thing could help.
 */
static uint64_t last_saved_at, changed_entries, changed_bytes;

static char path[MAX_PATH];               /* the whole path, under ROOT */
static struct { uint64_t ino; char path[LINK_PATH]; } *links;
static uint64_t skipped_kinds, skipped_big;

static int delta_mode = 0;

#include "var_store_dirty.inc"

static int
is_skipped(const char *relative)
{
	return is_transient_path(relative);
}

/* The name this inode was first met under, if it has one; otherwise this one is remembered. */
static const char *
link_name(uint64_t ino, const char *relative)
{
	uint64_t slot = (ino * 2654435761u) % LINKS;

	for (uint64_t tries = 0; tries < LINKS; tries++, slot = (slot + 1) % LINKS) {
		if (links[slot].ino == ino) {
			return links[slot].path;
		}
		if (links[slot].ino == 0) {
			if (strlen(relative) < LINK_PATH) {
				links[slot].ino = ino;
				strcpy(links[slot].path, relative);
			}
			return NULL;
		}
	}
	return NULL;                    /* the table is full: this one is written out whole */
}

/*
 * The extended attributes of path -- iOS keeps container metadata and much else in them -- packed
 * into a buffer of their own, because the entry's head says how many there are and has to go out
 * before them. One that does not fit is left out and counted.
 */
static uint8_t xpack[XPACK];

static uint64_t
pack_xattrs(uint32_t *count)
{
	static char names[16384];
	static uint8_t value[MAX_XATTR];
	ssize_t n = listxattr(path, names, sizeof(names), XATTR_NOFOLLOW);
	uint64_t used = 0;

	*count = 0;
	if (n <= 0) {
		return 0;
	}
	for (ssize_t i = 0; i < n && i < (ssize_t)sizeof(names); i += (ssize_t)strlen(names + i) + 1) {
		uint32_t namelen = (uint32_t)strlen(names + i), vlen;
		ssize_t size = getxattr(path, names + i, value, sizeof(value), 0, XATTR_NOFOLLOW);

		if (size < 0) {
			continue;
		}
		vlen = (uint32_t)size;
		if (used + 8 + namelen + vlen > XPACK) {
			skipped_big++;
			continue;
		}
		memcpy(xpack + used, &namelen, sizeof(namelen));
		memcpy(xpack + used + 4, &vlen, sizeof(vlen));
		memcpy(xpack + used + 8, names + i, namelen);
		memcpy(xpack + used + 8 + namelen, value, vlen);
		used += 8 + namelen + vlen;
		(*count)++;
	}
	return used;
}

/* One entry: its head, its name, its extended attributes, its contents. */
static int
pack_one(const char *relative, const struct stat *st)
{
	struct entry e;
	static char target[MAX_PATH];
	const char *first;
	uint32_t kind = 0;
	uint64_t size = 0;
	const void *tail = NULL;
	int in_fd = -1;

	switch (st->st_mode & S_IFMT) {
	case S_IFDIR:
		kind = ENT_DIR;
		break;
	case S_IFLNK: {
		ssize_t n = readlink(path, target, sizeof(target));

		if (n <= 0) {
			return 1;
		}
		kind = ENT_SYMLINK;
		size = (uint64_t)n;
		tail = target;
		break;
	}
	case S_IFREG:
		kind = ENT_FILE;
		size = (uint64_t)st->st_size;
		if (st->st_nlink > 1) {
			first = link_name(st->st_ino, relative);
			if (first != NULL) {
				kind = ENT_HARDLINK;
				size = strlen(first);
				tail = first;
				break;
			}
		}
		in_fd = open(path, O_RDONLY, 0);
		if (in_fd < 0) {
			return 1;       /* gone, or not ours to read: it is not in the archive */
		}
		break;
	default:
		skipped_kinds++;        /* a socket, a fifo, a device: a new boot makes its own */
		return 1;
	}

	memset(&e, 0, sizeof(e));
	e.kind = kind;
	e.mode = st->st_mode & 07777;
	e.uid = st->st_uid;
	e.gid = st->st_gid;
	e.flags = st->st_flags & FLAGS_KEPT;
	e.pathlen = (uint32_t)strlen(relative);
	e.mtime_sec = st->st_mtimespec.tv_sec;
	e.mtime_nsec = (uint32_t)st->st_mtimespec.tv_nsec;
	e.size = size;
	if (last_saved_at != 0 && (uint64_t)st->st_mtimespec.tv_sec > last_saved_at) {
		changed_entries++;
		changed_bytes += size;
	}
	{
		uint64_t xlen = pack_xattrs(&e.nxattr);

		if (!out(&e, sizeof(e)) || !out(relative, e.pathlen) || !out(xpack, xlen)) {
			if (in_fd >= 0) {
				close(in_fd);
			}
			return 0;
		}
	}
	if (tail != NULL && !out(tail, size)) {
		if (in_fd >= 0) {
			close(in_fd);
		}
		return 0;
	}
	if (in_fd >= 0) {
		static uint8_t piece[256 * 1024];
		uint64_t left = size;

		while (left > 0) {
			uint64_t want = left > sizeof(piece) ? sizeof(piece) : left;
			ssize_t got = read(in_fd, piece, (size_t)want);

			if (got <= 0) {
				/* the file shrank under us: the rest is zeros, so the archive stays whole */
				memset(piece, 0, (size_t)want);
				got = (ssize_t)want;
			}
			if (!out(piece, (uint64_t)got)) {
				close(in_fd);
				return 0;
			}
			left -= (uint64_t)got;
		}
		close(in_fd);
	}
	if (!out_pad()) {
		return 0;
	}
	entries++;
	return 1;
}

/* The directory's own times once more, after its contents: making them moved them. */
static int
pack_dirtime(const char *relative, const struct stat *st)
{
	struct entry e;

	memset(&e, 0, sizeof(e));
	e.kind = ENT_DIRTIME;
	e.flags = st->st_flags & FLAGS_KEPT;
	e.pathlen = (uint32_t)strlen(relative);
	e.mtime_sec = st->st_mtimespec.tv_sec;
	e.mtime_nsec = (uint32_t)st->st_mtimespec.tv_nsec;
	if (!out(&e, sizeof(e)) || !out(relative, e.pathlen) || !out_pad()) {
		return 0;
	}
	entries++;
	return 1;
}

/*
 * The names of one directory, gathered and sorted. The archive is meant to come out the same
 * bytes for the same tree: the order getdirentries gives shifts as a directory is written to, and
 * then every byte after it in the archive moves, which is the difference between a save that
 * writes nothing into the flash and one that writes all of itself. Both arenas are used as stacks
 * -- a level marks them on the way in and gives them back on the way out.
 */
#define NAME_ARENA      (2u << 20)
#define NAME_MAX_NAMES  65536

static char name_arena[NAME_ARENA];
static uint32_t name_at[NAME_MAX_NAMES];
static uint64_t arena_used, names_used, unsorted_dirs;

static void
sort_names(uint32_t *at, uint64_t n)
{
	/* insertion sort: a directory holds tens of names, and this keeps the program small */
	for (uint64_t i = 1; i < n; i++) {
		uint32_t v = at[i];
		uint64_t j = i;

		while (j > 0 && strcmp(name_arena + at[j - 1], name_arena + v) > 0) {
			at[j] = at[j - 1];
			j--;
		}
		at[j] = v;
	}
}

/* Every name in the open directory into the arena, sorted. Returns the count, or -1 if it did not fit. */
static long
gather_names(int dir_fd, char *buf, size_t bufsize, uint64_t mark_a, uint64_t mark_n)
{
	off_t position = 0;
	ssize_t n;

	while ((n = getdirentries(dir_fd, buf, bufsize, &position)) > 0) {
		for (ssize_t i = 0; i < n;) {
			struct dirent *d = (struct dirent *)(buf + i);
			const char *name = d->d_name;
			size_t len = strlen(name);

			i += d->d_reclen;
			if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) {
				continue;
			}
			if (arena_used + len + 1 > NAME_ARENA || names_used >= NAME_MAX_NAMES) {
				arena_used = mark_a;
				names_used = mark_n;
				return -1;
			}
			name_at[names_used++] = (uint32_t)arena_used;
			strcpy(name_arena + arena_used, name);
			arena_used += len + 1;
		}
	}
	sort_names(name_at + mark_n, names_used - mark_n);
	return (long)(names_used - mark_n);
}

/* relative: the path under ROOT, "" for ROOT itself. path holds the whole of it. */
static int
pack_tree(const char *relative)
{
	static char dirbuf[MAX_DEPTH][4096];    /* one read buffer a level */
	static int depth;
	struct stat st;
	size_t base = strlen(path), rbase = strlen(relative);
	int dir_fd, ok = 1;

	if (lstat(path, &st) != 0) {
		return 1;
	}
	if (relative[0] != 0) {
		if ((st.st_mode & S_IFMT) != S_IFDIR && is_skipped(relative)) {
			return 1;
		}
		int should_pack = 1;
		if (delta_mode) {
			should_pack = ((uint64_t)st.st_mtimespec.tv_sec >= last_saved_at ||
			               (uint64_t)st.st_ctimespec.tv_sec >= last_saved_at);
		}
		if (should_pack && !pack_one(relative, &st)) {
			return 0;
		}
	}
	if ((st.st_mode & S_IFMT) != S_IFDIR || (relative[0] != 0 && is_skipped(relative))) {
		return 1;
	}
	if (depth >= MAX_DEPTH) {
		printf("var_store: %s: deeper than this program walks\n", path);
		return 1;
	}
	if ((dir_fd = open(path, O_RDONLY | O_DIRECTORY, 0)) < 0) {
		return 1;
	}
	depth++;
	{
		uint64_t mark_a = arena_used, mark_n = names_used;
		long count = gather_names(dir_fd, dirbuf[depth - 1], sizeof(dirbuf[0]), mark_a, mark_n);

		if (count < 0) {
			/* more names than the arena holds: this one goes in whatever order it is given */
			unsorted_dirs++;
		}
		for (long k = 0; ok && k < count; k++) {
			const char *name = name_arena + name_at[mark_n + k];
			char child[MAX_PATH];           /* the caller's while this one recurses: not static */

			if (base + 1 + strlen(name) >= MAX_PATH || rbase + 1 + strlen(name) >= MAX_PATH) {
				printf("var_store: %s/%s: the path is too long\n", path, name);
				continue;
			}
			path[base] = '/';
			strcpy(path + base + 1, name);
			if (rbase > 0) {
				memcpy(child, relative, rbase);
				child[rbase] = '/';
				strcpy(child + rbase + 1, name);
			} else {
				strcpy(child, name);
			}
			ok = pack_tree(child);
			path[base] = 0;
		}
		arena_used = mark_a;
		names_used = mark_n;
	}
	depth--;
	close(dir_fd);
	if (ok && relative[0] != 0 && lstat(path, &st) == 0) {
		if (!delta_mode || (uint64_t)st.st_mtimespec.tv_sec >= last_saved_at ||
		    (uint64_t)st.st_ctimespec.tv_sec >= last_saved_at) {
			ok = pack_dirtime(relative, &st);
		}
	}
	return ok;
}

#include "var_store_klog.inc"
#include "var_store_delta.inc"

static int
save(void)
{
	struct head old, h;
	int slot, best;
	static uint8_t block[BLOCK];

	set_background_qos();
	memset(&old, 0, sizeof(old));
	best = newest(&old);
	slot = best < 0 ? 0 : (best + 1) % slots;
	printf("var_store: saving into slot %d (slot %d holds generation %llu)\n", slot, best,
	    best < 0 ? 0ULL : (unsigned long long)old.generation);

	/* the slot's header first, so that a save cut short cannot leave an old header over a new archive */
	memset(block, 0, sizeof(block));
	if (!device_write(slot_at(slot), block, BLOCK)) {
		return 0;
	}
	stream_start(slot, 0);
	entries = 0;
	last_saved_at = best < 0 ? 0 : old.seconds;
	changed_entries = changed_bytes = 0;
	skipped_blocks = written_blocks = 0;
	arena_used = names_used = unsorted_dirs = 0;
	/* a watcher saves more than once: nothing of the save before is carried into this one */
	memset(links, 0, LINKS * sizeof(*links));
	skipped_kinds = skipped_big = 0;
	strcpy(path, ROOT);
	if (!pack_tree("") || !out_flush(1)) {
		return 0;
	}
	memset(&h, 0, sizeof(h));
	memcpy(h.magic, HEAD_MAGIC, sizeof(h.magic));
	h.version = HEAD_VERSION;
	h.slot = (uint32_t)slot;
	h.generation = (best < 0 ? 0 : old.generation) + 1;
	h.bytes = stream_bytes;
	h.entries = entries;
	{
		struct timeval tv = { 0, 0 };

		gettimeofday(&tv, NULL);
		h.seconds = (uint64_t)tv.tv_sec;
	}
	h.crc = stream_crc ^ 0xffffffffu;
	h.head_crc = head_checksum(&h);
	if (h.bytes > (uint64_t)SLOT_BYTES - BLOCK) {
		printf("var_store: the archive is %llu bytes, more than a slot holds (%llu): nothing saved\n",
		    (unsigned long long)h.bytes, (unsigned long long)(SLOT_BYTES - BLOCK));
		return 0;
	}
	memset(block, 0, sizeof(block));
	memcpy(block, &h, sizeof(h));
	if (!device_write(slot_at(slot), block, BLOCK)) {
		return 0;
	}
	printf("var_store: slot %d: generation %llu, %llu entries, %llu bytes, crc %08x%s%s\n", slot,
	    (unsigned long long)h.generation, (unsigned long long)h.entries, (unsigned long long)h.bytes,
	    h.crc, skipped_kinds ? " (sockets and the like left out)" : "",
	    skipped_big ? " (something too big left out)" : "");
	if (last_saved_at != 0) {
		printf("var_store: %llu entries and %llu bytes of it were written since the last save "
		    "(%llu seconds ago)\n", (unsigned long long)changed_entries,
		    (unsigned long long)changed_bytes, (unsigned long long)(h.seconds - last_saved_at));
	}
	printf("var_store: %llu blocks written, %llu left alone (they held this already)%s\n",
	    (unsigned long long)written_blocks, (unsigned long long)skipped_blocks,
	    unsorted_dirs ? " (a directory was too big to put in order)" : "");
	delta_reset(h.generation);
	klog_flush();
	return 1;
}

#include "var_store_unpack.inc"

#include "var_store_stamp.inc"

int
main(int argc, char **argv, char **envp)
{
	const char *what = argc > 1 ? argv[1] : "show";
	int ok, is_file = 0;

	(void)envp;
	store = default_device();
	crc_init();
	if (strcmp(what, "stamp") == 0) {
		return stamp(argc > 2 ? argv[2] : "/private/var/annibale-stamp") ? 0 : 1;
	}
	buffer = mmap(NULL, BUFFER, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
	links = mmap(NULL, LINKS * sizeof(*links), PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
	if (buffer == MAP_FAILED || links == MAP_FAILED) {
		printf("var_store: no memory for the buffers\n");
		return 1;
	}
	/* for comparing with what the slot holds before writing over it; doing without is only slower */
	already = mmap(NULL, BUFFER, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
	if (already == MAP_FAILED) {
		already = NULL;
	}
	if (strcmp(what, "load") == 0) {
		int disk_loaded = 0;
		size_t dlen = sizeof(disk_loaded);
		if (sysctlbyname("kern.annibale.var_disk_loaded", &disk_loaded, &dlen, NULL, 0) == 0 && disk_loaded != 0) {
			printf("var_store: /private/var already loaded from persistent disk; seed unpack skipped\n");
			return 0;
		}
	}
	if (argc == 2 && strcmp(what, "load") == 0 && seed(argv[0]) != NULL) {
		struct head h;

		fd = open(store, O_RDONLY, 0);
		if (fd >= 0 && newest(&h) >= 0) {
			ok = load();
			close(fd);
			if (ok && made_files + made_dirs != 0) {
				return 0;
			}
			printf("var_store: the flash gave nothing: the seed instead\n");
		} else if (fd >= 0) {
			close(fd);
		}
	}
	if ((argc > 2 && strcmp(what, "erase") != 0) || (strcmp(what, "load") == 0 && seed(argv[0]) != NULL)) {
		store = argc > 2 ? argv[2] : seed(argv[0]);
		store_at = 0;
		slots = 1;
		already = NULL;         /* a new file holds nothing to compare with */
		is_file = 1;
	}
	fd = open(store, strcmp(what, "load") == 0 || strcmp(what, "show") == 0 ? O_RDONLY :
	    is_file ? O_RDWR | O_CREAT | O_TRUNC : O_RDWR, 0644);
	if (fd < 0) {
		printf("var_store: %s: %s (the kernel takes writes with ufs_write=1)\n", store, strerror(errno));
		return 1;
	}
	if (strcmp(what, "save") == 0 && is_file) {
		/* a zeroed header first, so that the empty file reads as holding no archive */
		ok = device_write(0, buffer, BLOCK) && save();
	} else if (strcmp(what, "save") == 0) {
		ok = claim() && save();
		release();
	} else if (strcmp(what, "delta") == 0) {
		ok = claim() && delta_save();
		release();
	} else if (strcmp(what, "load") == 0) {
		ok = load();
	} else if (strcmp(what, "erase") == 0) {
		ok = claim() && erase();        /* a write like any other: the claim first */
		release();
	} else {
		show();
		ok = 1;
	}
	close(fd);
	return ok ? 0 : 1;
}
