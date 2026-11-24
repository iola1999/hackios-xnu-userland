/*
 * info, ls, stat, cat: the commands that read one path and print what they found.
 * apfs_tool.h, and they are built together by build-tool.sh.
 */

#include "apfs_tool.h"
#include <zlib.h>

/* ---- info ---- */

int
cmd_info(const char *path)
{
	struct apfs_nx nx;
	struct apfs_vol vol;
	uint32_t i;

	if (open_image(path, &nx, &vol) != 0) {
		return 1;
	}
	printf("%s: APFS container, block size %u, %" PRIu64 " blocks\n", path,
	    nx.block_size, nx.block_count);
	printf("  newest checkpoint xid %" PRIu64 "\n", nx.xid);
	printf("  container omap block %" PRIu64 "\n", nx.omap_addr);
	for (i = 0; i < APFS_NX_MAX_FILE_SYSTEMS; i++) {
		struct apfs_vol v;

		if (nx.fs_oid[i] == 0 || apfs_vol_open(&nx, i, &v) != 0) {
			continue;
		}
		printf("  [%u] %s  role 0x%04x  incompatible 0x%" PRIx64 "  fs flags 0x%" PRIx64 "\n",
		    i, v.name, v.role, v.incompat, v.fs_flags);
		printf("       files %" PRIu64 "  dirs %" PRIu64 "  symlinks %" PRIu64
		    "  blocks used %" PRIu64 "\n", v.num_files, v.num_dirs, v.num_symlinks,
		    v.alloc_count);
		printf("       tree: %" PRIu64 " records in %" PRIu64 " nodes, flags 0x%x\n",
		    v.tree.key_count, v.tree.node_count, v.tree.flags);
	}
	printf("  blocks read %" PRIu64 ", cache hits %" PRIu64 "\n", nx.reads, nx.hits);
	apfs_nx_close(&nx);
	return 0;
}

/* ---- ls ---- */

struct ls_ctx {
	struct apfs_vol *vol;
};

static const char *
mode_kind(uint16_t mode)
{
	switch (mode & APFS_S_IFMT) {
	case APFS_S_IFDIR:  return "d";
	case APFS_S_IFLNK:  return "l";
	case APFS_S_IFREG:  return "-";
	case APFS_S_IFIFO:  return "p";
	case APFS_S_IFCHR:  return "c";
	case APFS_S_IFBLK:  return "b";
	case APFS_S_IFSOCK: return "s";
	default:            return "?";
	}
}

static int
ls_cb(void *ctx, const char *name, size_t name_len, uint64_t file_id, uint8_t d_type)
{
	struct ls_ctx *lc = ctx;
	struct apfs_inode ino;

	(void)name_len;
	(void)d_type;
	if (apfs_inode_get(lc->vol, file_id, &ino) != 0) {
		printf("?  %20s  %s  (no inode record)\n", "", name);
		return 0;
	}
	printf("%s %06o %4u:%-4u %12" PRIu64 "  %-20" PRIu64 " %s", mode_kind(ino.mode),
	    ino.mode & 07777, ino.owner, ino.group, ino.size, file_id, name);
	if ((ino.mode & APFS_S_IFMT) == APFS_S_IFLNK) {
		char target[1024];
		size_t n = 0;

		if (apfs_readlink(lc->vol, file_id, target, sizeof(target) - 1, &n) == 0) {
			target[n] = '\0';
			printf(" -> %s", target);
		}
	}
	printf("\n");
	return 0;
}

int
cmd_ls(const char *path, const char *where)
{
	struct apfs_nx nx;
	struct apfs_vol vol;
	struct ls_ctx lc;
	uint64_t id;
	uint8_t dt;
	int error;

	if (open_image(path, &nx, &vol) != 0) {
		return 1;
	}
	error = apfs_path_lookup(&vol, where, &id, &dt);
	if (error) {
		fprintf(stderr, "apfs-tool: %s: %s\n", where, strerror(error));
		return 1;
	}
	lc.vol = &vol;
	if (dt != APFS_DT_DIR) {
		ls_cb(&lc, where, strlen(where), id, dt);
	} else {
		error = apfs_readdir(&vol, id, ls_cb, &lc);
		if (error) {
			fprintf(stderr, "apfs-tool: %s\n", strerror(error));
			return 1;
		}
	}
	apfs_nx_close(&nx);
	return 0;
}

/* ---- stat ---- */

static int
xattr_print_cb(void *ctx, const char *name, size_t name_len, const struct apfs_xattr *xa)
{
	(void)ctx;
	(void)name_len;
	printf("  xattr %-28s flags 0x%x  %u bytes%s\n", name, xa->flags, xa->data_len,
	    (xa->flags & APFS_XATTR_DATA_STREAM) ? " (in a data stream)" : "");
	return 0;
}

int
cmd_stat(const char *path, const char *where)
{
	struct apfs_nx nx;
	struct apfs_vol vol;
	struct apfs_inode ino;
	struct apfs_cfile cf;
	uint64_t id;
	uint8_t dt;
	int error;

	if (open_image(path, &nx, &vol) != 0) {
		return 1;
	}
	error = apfs_path_lookup(&vol, where, &id, &dt);
	if (error) {
		fprintf(stderr, "apfs-tool: %s: %s\n", where, strerror(error));
		return 1;
	}
	error = apfs_inode_get(&vol, id, &ino);
	if (error) {
		fprintf(stderr, "apfs-tool: inode %" PRIu64 ": %s\n", id, strerror(error));
		return 1;
	}
	printf("path        %s\n", where);
	printf("inode       %" PRIu64 "  parent %" PRIu64 "  private %" PRIu64 "\n",
	    ino.obj_id, ino.parent_id, ino.private_id);
	printf("mode        %06o  uid %u  gid %u\n", ino.mode, ino.owner, ino.group);
	printf("size        %" PRIu64 "  allocated %" PRIu64 "\n", ino.size,
	    ino.alloced_size);
	printf("flags       internal 0x%" PRIx64 "  bsd 0x%x%s\n", ino.internal_flags,
	    ino.bsd_flags, ino.is_sparse ? "  sparse" : "");
	memset(&cf, 0, sizeof(cf));
	cf.held = APFS_NO_PIECE;
	if (apfs_cfile_init(&vol, &ino, &cf) == 0 && cf.compressed) {
		printf("decmpfs     type %u  uncompressed %" PRIu64 "  %s\n", cf.type, cf.size,
		    cf.in_rsrc ? "in the resource fork" : "in the attribute");
	}
	apfs_cfile_fini(&cf);
	apfs_xattr_list(&vol, id, xattr_print_cb, NULL);
	apfs_nx_close(&nx);
	return 0;
}

/* ---- cat ---- */

int
cmd_cat(const char *path, const char *where)
{
	struct apfs_nx nx;
	struct apfs_vol vol;
	struct apfs_inode ino;
	struct apfs_cfile cf;
	uint64_t id, off = 0;
	uint8_t dt;
	uint8_t *buf;
	int error;

	if (open_image(path, &nx, &vol) != 0) {
		return 1;
	}
	error = apfs_path_lookup(&vol, where, &id, &dt);
	if (error == 0) {
		error = apfs_inode_get(&vol, id, &ino);
	}
	if (error) {
		fprintf(stderr, "apfs-tool: %s: %s\n", where, strerror(error));
		return 1;
	}
	buf = malloc(65536);
	if (buf == NULL) {
		return 1;
	}
	memset(&cf, 0, sizeof(cf));
	cf.held = APFS_NO_PIECE;
	{
		uint64_t size = 0;

		/* (a compressed file's size is in its decmpfs header, not its inode) */
		if (apfs_inode_size(&vol, &ino, &size) == 0) {
			ino.size = size;
		}
	}
	while (off < ino.size) {
		size_t got = 0;

		error = apfs_read_file(&vol, &ino, &cf, off, buf, 65536, &got);
		if (error) {
			fprintf(stderr, "apfs-tool: read at %" PRIu64 ": %s\n", off,
			    strerror(error));
			free(buf);
			return 1;
		}
		if (got == 0) {
			break;
		}
		if (fwrite(buf, 1, got, stdout) != got) {
			free(buf);
			return 1;
		}
		off += got;
	}
	apfs_cfile_fini(&cf);
	free(buf);
	apfs_nx_close(&nx);
	return 0;
}

/*
 * One compressed piece, in detail: the table entry, the extent it resolves to,
 * the stored bytes (hash + dump) and a plain zlib inflate of them.
 */
int
cmd_piece(const char *path, const char *where, const char *which)
{
	struct apfs_nx nx;
	struct apfs_vol vol;
	struct apfs_inode ino;
	struct apfs_cfile cf;
	struct apfs_extent ext;
	uint64_t id, n, poff;
	uint8_t dt;
	uint8_t *buf;
	size_t got = 0, i;
	int error;

	n = strtoull(which, NULL, 0);
	if (open_image(path, &nx, &vol) != 0) {
		return 1;
	}
	error = apfs_path_lookup(&vol, where, &id, &dt);
	if (error == 0) {
		error = apfs_inode_get(&vol, id, &ino);
	}
	if (error) {
		fprintf(stderr, "apfs-tool: %s: %s\n", where, strerror(error));
		return 1;
	}
	memset(&cf, 0, sizeof(cf));
	cf.held = APFS_NO_PIECE;
	error = apfs_cfile_init(&vol, &ino, &cf);
	if (error) {
		fprintf(stderr, "apfs-tool: cfile_init: %s\n", strerror(error));
		return 1;
	}
	printf("type %u size %llu rsrc_id %llu rsrc_size %llu table_base %llu count %u\n",
	    cf.type, (unsigned long long)cf.size, (unsigned long long)cf.rsrc_id,
	    (unsigned long long)cf.rsrc_size, (unsigned long long)cf.table_base,
	    cf.piece_count);
	if (n >= cf.piece_count) {
		fprintf(stderr, "apfs-tool: piece %llu past %u\n", (unsigned long long)n,
		    cf.piece_count);
		return 1;
	}
	poff = cf.table_base + cf.piece_off[n];
	printf("piece %llu off %u len %u poff %llu want %u\n", (unsigned long long)n,
	    cf.piece_off[n], cf.piece_len[n], (unsigned long long)poff,
	    (unsigned)((n + 1) * 65536 <= cf.size ? 65536 :
	    (unsigned)(cf.size - n * 65536)));
	error = apfs_extent_at(&vol, cf.rsrc_id, poff, &ext);
	if (error) {
		printf("extent_at: %s\n", strerror(error));
	} else {
		printf("extent logical %llu len %llu phys %llu -> dev byte %llu\n",
		    (unsigned long long)ext.logical, (unsigned long long)ext.length,
		    (unsigned long long)ext.phys,
		    (unsigned long long)(ext.phys * 4096 + (poff - ext.logical)));
	}
	buf = malloc(cf.piece_len[n]);
	error = apfs_read_stream(&vol, cf.rsrc_id, cf.rsrc_size, poff, buf,
	    cf.piece_len[n], &got);
	printf("read err %d got %zu fnv %016llx\n", error, got,
	    (unsigned long long)fnv1a(buf, got, FNV_INIT));
	printf("bytes:");
	for (i = 0; i < got && i < 40; i++) {
		printf(" %02x", buf[i]);
	}
	printf("\nPIECE-DATA %zu\n", got);
	{
		static const char b64[] =
		    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
		size_t bi;
		for (bi = 0; bi + 3 <= got; bi += 3) {
			uint32_t v = (buf[bi] << 16) | (buf[bi+1] << 8) | buf[bi+2];
			putchar(b64[v >> 18]); putchar(b64[(v >> 12) & 63]);
			putchar(b64[(v >> 6) & 63]); putchar(b64[v & 63]);
		}
		if (bi < got) {
			uint32_t v = buf[bi] << 16 | (bi + 1 < got ? buf[bi+1] << 8 : 0);
			putchar(b64[v >> 18]); putchar(b64[(v >> 12) & 63]);
			putchar(bi + 1 < got ? b64[(v >> 6) & 63] : '=');
			putchar('=');
		}
	}
	printf("\nPIECE-END\n");
	{
		uLongf dlen = 1u << 20;
		uint8_t *d = malloc(dlen);
		int z = uncompress(d, &dlen, buf, (uLong)got);
		printf("zlib uncompress: z=%d dlen=%lu fnv %016llx\n", z,
		    (unsigned long)dlen,
		    (unsigned long long)fnv1a(d, dlen, FNV_INIT));
		free(d);
	}
	free(buf);
	apfs_cfile_fini(&cf);
	apfs_nx_close(&nx);
	return 0;
}
