#!/usr/bin/env python3
# Read an APFS container: the volumes in it, their directory trees, and the contents of
# files -- including the ones stored compressed.  Read only, and it never writes to the
# image.
#
# The on-disk structures are described in bsd/miscfs/apfs/apfs_format.h (exported to
# xnu/files/); the layout the reader files assume is checked against that header by
# tools/apfs/run-tests.sh, so the two cannot drift apart silently.
#
# usage: apfs.py info   IMAGE
#        apfs.py ls     IMAGE [PATH]
#        apfs.py tree   IMAGE [PATH]
#        apfs.py cat    IMAGE PATH
#        apfs.py stat   IMAGE PATH
#        apfs.py census IMAGE            (walk everything, count what is in there)
# IMAGE is a raw APFS partition, or a .dmg -- the APFS partition inside it is used.
# -v VOLUME picks a volume by name or index (default: the first one with a root directory).


# Everything the reader does lives in apfs_super.py and apfs_reader.py and is re-exported
# from here: tools/apfs/verify.py, mksparse.py, test_sparse.py, txn-probe.py and the rest
# keep importing apfs and seeing the same names.

import os
import struct
import sys
import zlib

from apfs_super import *
from apfs_reader import *

# A traceback says "apfs.ApfsError" the way it did before this file was split: it is one and
# the same class, only the module it was defined in moved, and callers print the name through.
ApfsError.__module__ = "apfs"

# ---------------------------------------------------------------- walking

DT_CHAR = {1: "p", 2: "c", 4: "d", 6: "b", 8: "-", 10: "l", 12: "s", 14: "w"}


def walk(vol, oid, path="", depth=0, max_depth=None):
    """Depth-first over a directory tree, yielding (path, name, file_id, d_type)."""
    if max_depth is not None and depth > max_depth:
        return
    for name, fid, dt in sorted(vol.dir_entries(oid)):
        p = path + "/" + name
        yield p, name, fid, dt
        if dt == 4:
            yield from walk(vol, fid, p, depth + 1, max_depth)


# ---------------------------------------------------------------- commands

def pick_volume(c, want):
    vols = c.volumes()
    if not vols:
        raise ApfsError("no volumes in this container")
    if want is None:
        for v in vols:
            if v.inode(ROOT_DIR_INO_NUM) is not None:
                return v
        return vols[0]
    for v in vols:
        if v.volname == want or str(v.index) == str(want):
            return v
    raise ApfsError("no volume named %r (have: %s)"
                    % (want, ", ".join(v.volname for v in vols)))


def cmd_info(path, want):
    c = Container(path)
    n = c.nx
    print("%s: APFS container, block size %d, %d blocks (%.2f GB)"
          % (path, c.block_size, n["block_count"], n["block_count"] * c.block_size / 1e9))
    print("  uuid              %s" % _uuid(n["uuid"]))
    print("  newest checkpoint xid %d (%d container superblocks in the descriptor area)"
          % (c.xid, c.checkpoints_seen))
    print("  checkpoint desc   base %d, %d blocks" % (n["xp_desc_base"], n["xp_desc_blocks"]))
    print("  container omap    block %d" % n["omap_oid"])
    print("  features 0x%x  ro-compat 0x%x  incompatible 0x%x"
          % (n["features"], n["ro_compat"], n["incompat"]))
    vols = c.volumes()
    print("  %d volumes:" % len(vols))
    for v in vols:
        print("    [%d] %-22s role 0x%04x  xid %d" % (v.index, v.volname, v.role, v.xid))
        print("         files %d  dirs %d  symlinks %d  other %d  snapshots %d"
              % (v.num_files, v.num_directories, v.num_symlinks, v.num_other,
                 v.num_snapshots))
        print("         incompatible 0x%x%s%s  fs flags 0x%x"
              % (v.incompat,
                 " case-insensitive" if v.case_insensitive else "",
                 " sealed" if v.sealed else "", v.fs_flags))
        print("         omap block %d  root tree oid %d  used %d blocks (%.2f GB)"
              % (v.omap_oid, v.root_tree_oid, v.alloc_count,
                 v.alloc_count * c.block_size / 1e9))
    v = pick_volume(c, want)
    t = v.tree
    print("  volume %r file-system tree: %d records, %d nodes, flags 0x%x, "
          "longest key %d, longest value %d"
          % (v.volname, t.key_count, t.node_count, t.bt_flags, t.longest_key,
             t.longest_val))
    print("  blocks read %d, cache hits %d, checksums verified %d, bad %d"
          % (c.blocks.cksum_checked, c.blocks.cache_hits, c.blocks.cksum_checked,
             c.blocks.cksum_bad))
    c.close()


def _uuid(b):
    h = b.hex()
    return "%s-%s-%s-%s-%s" % (h[0:8], h[8:12], h[12:16], h[16:20], h[20:32])


def cmd_ls(path, where, want):
    c = Container(path)
    v = pick_volume(c, want)
    hit = v.lookup(where)
    if hit is None:
        raise ApfsError("%s: no such path on volume %r" % (where, v.volname))
    oid, dt = hit
    if dt != 4:
        _print_one(v, where.rsplit("/", 1)[-1] or "/", oid)
        c.close()
        return
    for name, fid, t in sorted(v.dir_entries(oid)):
        _print_one(v, name, fid)
    c.close()


def _print_one(v, name, fid):
    ino = v.inode(fid)
    if ino is None:
        print("?         %20s  %s  (no inode record)" % ("", name))
        return
    kind = DT_CHAR.get({S_IFDIR: 4, S_IFREG: 8, S_IFLNK: 10, S_IFIFO: 1, S_IFCHR: 2,
                        S_IFBLK: 6, S_IFSOCK: 12}.get(ino.ifmt, 0), "?")
    extra = ""
    if ino.is_link:
        extra = " -> " + (ino.symlink_target_bytes() or b"").decode("utf-8", "replace")
    cmp_ = ""
    h = ino.decmpfs
    if h is not None and len(h) >= 16:
        cmp_ = " [decmpfs %d]" % struct.unpack_from("<I", h, 4)[0]
    if ino.is_sparse:
        cmp_ += " [sparse]"
    print("%s%s %4d:%-4d %12d  %-8d %s%s%s"
          % (kind, _modestr(ino.mode), ino.owner, ino.group, ino.size, fid, name,
             extra, cmp_))


def _modestr(mode):
    s = ""
    for who, special, mark in ((6, 0o4000, "s"), (3, 0o2000, "s"), (0, 0o1000, "t")):
        s += "r" if mode & (0o4 << who) else "-"
        s += "w" if mode & (0o2 << who) else "-"
        x = mode & (0o1 << who)
        # set-user-ID, set-group-ID and sticky, as ls shows them (upper case without the x under them)
        s += (mark if x else mark.upper()) if mode & special else ("x" if x else "-")
    return s


def cmd_tree(path, where, want):
    c = Container(path)
    v = pick_volume(c, want)
    hit = v.lookup(where)
    if hit is None:
        raise ApfsError("%s: no such path" % where)
    n = 0
    for p, _name, _fid, dt in walk(v, hit[0], where.rstrip("/")):
        print(("%s/" % p) if dt == 4 else p)
        n += 1
    print("# %d entries" % n, file=sys.stderr)
    c.close()


def cmd_cat(path, where, want):
    c = Container(path)
    v = pick_volume(c, want)
    hit = v.lookup(where)
    if hit is None:
        raise ApfsError("%s: no such path" % where)
    ino = v.inode(hit[0])
    sys.stdout.buffer.write(ino.contents())
    c.close()


def cmd_stat(path, where, want):
    c = Container(path)
    v = pick_volume(c, want)
    hit = v.lookup(where)
    if hit is None:
        raise ApfsError("%s: no such path" % where)
    ino = v.inode(hit[0])
    print("path        %s" % where)
    print("inode       %d  parent %d  private %d" % (ino.obj_id, ino.parent_id,
                                                     ino.private_id))
    print("mode        %06o (%s)  uid %d  gid %d" % (ino.mode, _modestr(ino.mode),
                                                     ino.owner, ino.group))
    print("size        %d" % ino.size)
    print("nlink/nchildren %d" % ino.nchildren)
    print("flags       internal 0x%x  bsd 0x%x%s" % (ino.internal_flags, ino.bsd_flags,
                                                     "  sparse" if ino.is_sparse else ""))
    if ino._dstream:
        print("dstream     size %d  allocated %d" % ino._dstream)
    h = ino.decmpfs
    if h is not None and len(h) >= 16:
        magic, ctype, usize = struct.unpack_from("<IIQ", h, 0)
        print("decmpfs     type %d  uncompressed %d  attribute %d bytes"
              % (ctype, usize, len(h)))
    xs = ino.xattrs
    if xs:
        print("xattrs      %s" % ", ".join("%s(%d,flags 0x%x)" % (k, len(d), f)
                                           for k, (f, d) in sorted(xs.items())))
    ex = v.extents(ino.private_id)
    if ex:
        print("extents     %d:" % len(ex))
        for logical, length, phys in ex[:16]:
            print("  logical %12d  len %9d  block %d%s"
                  % (logical, length, phys, "  (hole)" if phys == 0 else ""))
        if len(ex) > 16:
            print("  ... %d more" % (len(ex) - 16))
    if ino.is_link:
        print("symlink     -> %s" % (ino.symlink_target_bytes() or b"").decode(
            "utf-8", "replace"))
    c.close()


def cmd_census(path, want):
    """Walk the whole tree and count what is in it: the quickest honest sanity check."""
    c = Container(path)
    v = pick_volume(c, want)
    counts = {}
    cmp_types = {}
    total = 0
    dirs = files = links = other = 0
    bytes_logical = 0
    for p, _name, fid, dt in walk(v, ROOT_DIR_INO_NUM):
        total += 1
        counts[dt] = counts.get(dt, 0) + 1
        if dt == 4:
            dirs += 1
            continue
        ino = v.inode(fid)
        if ino is None:
            other += 1
            continue
        if ino.is_link:
            links += 1
            continue
        if not ino.is_reg:
            other += 1
            continue
        files += 1
        h = ino.decmpfs
        if h is not None and len(h) >= 16:
            ct = struct.unpack_from("<I", h, 4)[0]
            cmp_types[ct] = cmp_types.get(ct, 0) + 1
        bytes_logical += ino.size
        if total % 20000 == 0:
            sys.stderr.write("\r  %d entries..." % total)
            sys.stderr.flush()
    sys.stderr.write("\r")
    print("volume %r: %d directory entries" % (v.volname, total))
    print("  directories %d  regular %d  symlinks %d  other %d" % (dirs, files, links, other))
    print("  d_type counts: %s" % ", ".join("%s=%d" % (DT_CHAR.get(k, k), n)
                                            for k, n in sorted(counts.items())))
    print("  logical bytes in regular files: %d (%.2f GB)"
          % (bytes_logical, bytes_logical / 1e9))
    print("  decmpfs compression types in use: %s"
          % (", ".join("type %d: %d files" % (k, n) for k, n in sorted(cmp_types.items()))
             or "none"))
    print("  superblock says: files %d, dirs %d, symlinks %d, other %d"
          % (v.num_files, v.num_directories, v.num_symlinks, v.num_other))
    print("  blocks: %d checksums verified, %d bad, %d cache hits"
          % (c.blocks.cksum_checked, c.blocks.cksum_bad, c.blocks.cache_hits))
    c.close()


def main(argv):
    want = None
    args = []
    i = 1
    while i < len(argv):
        if argv[i] == "-v" and i + 1 < len(argv):
            want = argv[i + 1]
            i += 2
            continue
        args.append(argv[i])
        i += 1
    if not args:
        print(__doc__ or "", file=sys.stderr)
        print("usage: apfs.py {info|ls|tree|cat|stat|census} IMAGE [PATH] [-v VOLUME]",
              file=sys.stderr)
        return 2
    cmd = args[0]
    try:
        if cmd == "info":
            cmd_info(args[1], want)
        elif cmd == "ls":
            cmd_ls(args[1], args[2] if len(args) > 2 else "/", want)
        elif cmd == "tree":
            cmd_tree(args[1], args[2] if len(args) > 2 else "/", want)
        elif cmd == "cat":
            cmd_cat(args[1], args[2], want)
        elif cmd == "stat":
            cmd_stat(args[1], args[2], want)
        elif cmd == "census":
            cmd_census(args[1], want)
        else:
            raise ApfsError("unknown command %r" % cmd)
    except ApfsError as e:
        print("apfs.py: %s" % e, file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
