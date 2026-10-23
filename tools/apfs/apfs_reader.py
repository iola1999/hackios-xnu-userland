# apfs_reader.py -- the middle layer of the reader that apfs.py drives: b-trees and their
# nodes, the object maps that say where an object's block lives now, the container and its
# checkpoint, volumes with their inodes, and the contents of files however they are stored
# -- extents, compressed (decmpfs), sparse.  Below it the headers, checksums and blocks of
# apfs_super.py; above it the commands of apfs.py.

import os
import struct
import sys
import zlib

from apfs_super import *

# ---------------------------------------------------------------- b-tree

class Node:
    __slots__ = ("addr", "blk", "flags", "level", "nkeys", "toc_off", "toc_len",
                 "key_base", "val_end", "fixed", "key_size", "val_size")

    def __init__(self, addr, blk, block_size, key_size=0, val_size=0):
        self.addr = addr
        self.blk = blk
        (self.flags, self.level, self.nkeys) = struct.unpack_from("<HHI", blk, 32)
        ts_off, ts_len = struct.unpack_from("<HH", blk, 40)
        self.toc_off = BTREE_NODE_DATA_OFF + ts_off
        self.toc_len = ts_len
        # keys start right after the table of contents, values end at the end of the
        # block -- except in a root node, where btree_info sits in the last bytes
        self.key_base = self.toc_off + self.toc_len
        self.val_end = len(blk) - (BTREE_INFO_SIZE if (self.flags & BTNODE_ROOT) else 0)
        self.fixed = bool(self.flags & BTNODE_FIXED_KV_SIZE)
        self.key_size = key_size
        self.val_size = val_size

    @property
    def is_leaf(self):
        return bool(self.flags & BTNODE_LEAF)

    def entry(self, i):
        """(key bytes, value bytes) of entry i."""
        if self.fixed:
            koff, voff = struct.unpack_from("<HH", self.blk, self.toc_off + 4 * i)
            klen = self.key_size
            vlen = self.val_size if self.is_leaf else 8
        else:
            koff, klen, voff, vlen = struct.unpack_from(
                "<HHHH", self.blk, self.toc_off + 8 * i)
        k = self.blk[self.key_base + koff:self.key_base + koff + klen]
        if voff == 0xFFFF:              # a ghost: a key with no value
            return k, b""
        v = self.blk[self.val_end - voff:self.val_end - voff + vlen]
        return k, v

    def key(self, i):
        if self.fixed:
            koff = struct.unpack_from("<H", self.blk, self.toc_off + 4 * i)[0]
            klen = self.key_size
        else:
            koff, klen = struct.unpack_from("<HH", self.blk, self.toc_off + 8 * i)
        return self.blk[self.key_base + koff:self.key_base + koff + klen]

    def child_oid(self, i):
        _k, v = self.entry(i)
        return struct.unpack_from("<Q", v, 0)[0]


class Btree:
    """
    One B-tree.  A tree is either physical -- a node's identifier is its block address --
    or virtual, and then identifiers are resolved through an object map.
    """

    def __init__(self, blocks, root_oid, resolve=None, name="btree"):
        self.blocks = blocks
        self.name = name
        self.resolve = resolve
        self.root_addr = self._addr(root_oid, physical_hint=True)
        rb = blocks.get(self.root_addr)
        (bt_flags, bt_node_size, bt_key_size, bt_val_size) = struct.unpack_from(
            "<IIII", rb, len(rb) - BTREE_INFO_SIZE)
        self.bt_flags = bt_flags
        self.node_size = bt_node_size
        self.key_size = bt_key_size
        self.val_size = bt_val_size
        (self.longest_key, self.longest_val, self.key_count, self.node_count) = \
            struct.unpack_from("<IIQQ", rb, len(rb) - BTREE_INFO_SIZE + 16)
        self.physical = bool(bt_flags & BTREE_PHYSICAL)
        self.root = Node(self.root_addr, rb, blocks.block_size, bt_key_size, bt_val_size)
        self.nodes_read = 0

    def _addr(self, oid, physical_hint=False):
        if physical_hint and self.resolve is None:
            return oid
        if self.resolve is None:
            return oid
        if getattr(self, "physical", None):
            return oid
        return self.resolve(oid)

    def node(self, oid):
        addr = oid if (self.physical or self.resolve is None) else self.resolve(oid)
        blk = self.blocks.get(addr)
        self.nodes_read += 1
        return Node(addr, blk, self.blocks.block_size, self.key_size, self.val_size)

    def child(self, node, i):
        return self.node(node.child_oid(i))

    def iter_from(self, less):
        """
        Every entry from the first key for which less(key) is false, in key order.
        `less` says whether a key sorts before the point we want to start at; passing
        a function that is always false walks the whole tree.
        """
        path = []
        node = self.root
        while not node.is_leaf:
            i = 0
            for j in range(node.nkeys):
                if less(node.key(j)):
                    i = j
                else:
                    break
            path.append((node, i))
            node = self.child(node, i)
        i = 0
        while i < node.nkeys and less(node.key(i)):
            i += 1
        while True:
            while i < node.nkeys:
                yield node.entry(i)
                i += 1
            # done with this leaf: step the path to the next one
            while path and path[-1][1] + 1 >= path[-1][0].nkeys:
                path.pop()
            if not path:
                return
            pnode, pi = path[-1]
            path[-1] = (pnode, pi + 1)
            node = self.child(pnode, pi + 1)
            while not node.is_leaf:
                path.append((node, 0))
                node = self.child(node, 0)
            i = 0

    def iter_all(self):
        return self.iter_from(lambda k: False)


# ---------------------------------------------------------------- object map

class Omap:
    """An object map: (object identifier, transaction) -> block address."""

    def __init__(self, blocks, omap_addr):
        blk = blocks.get(omap_addr)
        oid, xid, otype, _sub = obj_hdr(blk)
        if (otype & OBJECT_TYPE_MASK) != OBJECT_TYPE_OMAP:
            raise ApfsError("block %d is not an object map (type 0x%x)" % (omap_addr, otype))
        (self.flags, self.snap_count, self.tree_type, self.snapshot_tree_type) = \
            struct.unpack_from("<IIII", blk, 32)
        (self.tree_oid, self.snapshot_tree_oid) = struct.unpack_from("<QQ", blk, 48)
        self.tree = Btree(blocks, self.tree_oid, name="omap")
        self.cache = {}

    def lookup(self, oid, xid=None):
        """
        Where an object lives: the newest version at or before xid (the newest of all, if
        xid is None).  The map is keyed by (identifier, transaction), so the versions of
        one object are a run; we take the last one that is not newer than we asked for.
        """
        hit = self.cache.get(oid)
        if hit is not None:
            return hit
        want_xid = (1 << 64) - 1 if xid is None else xid

        def less(k):
            o, _x = struct.unpack_from("<QQ", k, 0)
            return o < oid

        best = None
        for k, v in self.tree.iter_from(less):
            o, x = struct.unpack_from("<QQ", k, 0)
            if o != oid:
                break
            if x <= want_xid:
                best = v
        if best is None:
            raise ApfsError("object %d is not in the object map" % oid)
        _flags, _size, paddr = struct.unpack_from("<IIq", best, 0)
        self.cache[oid] = paddr
        return paddr

    def resolver(self):
        return lambda oid: self.lookup(oid)


# ---------------------------------------------------------------- container

class Container:
    def __init__(self, path):
        self.image = open_image(path)
        self.path = path
        head = self.image.read(0, 4096)
        if head[32:36] != NX_MAGIC:
            raise ApfsError("%s: no NXSB magic at offset 32 -- not an APFS container" % path)
        block_size = struct.unpack_from("<I", head, 36)[0]
        if not (4096 <= block_size <= 65536) or (block_size & (block_size - 1)):
            raise ApfsError("%s: block size %d makes no sense" % (path, block_size))
        self.blocks = Blocks(self.image, block_size)
        self.block_size = block_size
        self.sb = self._newest_superblock()
        self.nx = self._parse_nx(self.sb)
        self.omap = Omap(self.blocks, self.nx["omap_oid"])

    def _parse_nx(self, blk):
        d = {}
        (d["magic"], d["block_size"], d["block_count"]) = struct.unpack_from("<IIQ", blk, 32)
        (d["features"], d["ro_compat"], d["incompat"]) = struct.unpack_from("<QQQ", blk, 48)
        d["uuid"] = blk[72:88]
        (d["next_oid"], d["next_xid"]) = struct.unpack_from("<QQ", blk, 88)
        (d["xp_desc_blocks"], d["xp_data_blocks"]) = struct.unpack_from("<II", blk, 104)
        (d["xp_desc_base"], d["xp_data_base"]) = struct.unpack_from("<qq", blk, 112)
        (d["xp_desc_next"], d["xp_data_next"], d["xp_desc_index"], d["xp_desc_len"],
         d["xp_data_index"], d["xp_data_len"]) = struct.unpack_from("<IIIIII", blk, 128)
        (d["spaceman_oid"], d["omap_oid"], d["reaper_oid"]) = struct.unpack_from("<QQQ", blk, 152)
        d["max_file_systems"] = struct.unpack_from("<I", blk, 180)[0]
        d["fs_oid"] = list(struct.unpack_from("<100Q", blk, 184))
        _, _, _, d["xid"] = obj_hdr(blk)
        return d

    def _newest_superblock(self):
        """
        Block zero's copy can be stale.  The live container superblock is the one with the
        largest transaction identifier in the checkpoint descriptor area, so read block
        zero only to find that area, then search it.
        """
        b0 = self.blocks.get(BLOCK0, verify=False)
        if not checksum_ok(b0):
            raise ApfsError("the container superblock in block 0 fails its checksum")
        base = struct.unpack_from("<q", b0, 112)[0]
        count = struct.unpack_from("<I", b0, 104)[0]
        best, best_xid = b0, obj_hdr(b0)[1]
        self.checkpoints_seen = 0
        for i in range(count):
            blk = self.blocks.raw(base + i)
            if len(blk) != self.block_size or not checksum_ok(blk):
                continue
            oid, xid, otype, _sub = obj_hdr(blk)
            if (otype & OBJECT_TYPE_MASK) != OBJECT_TYPE_NX_SUPERBLOCK:
                continue
            if blk[32:36] != NX_MAGIC or oid != OID_NX_SUPERBLOCK:
                continue
            self.checkpoints_seen += 1
            if xid > best_xid:
                best, best_xid = blk, xid
        self.xid = best_xid
        return best

    def volumes(self):
        out = []
        for i, oid in enumerate(self.nx["fs_oid"]):
            if not oid:
                continue
            try:
                addr = self.omap.lookup(oid, self.xid)
                blk = self.blocks.get(addr)
            except ApfsError:
                continue
            if blk[32:36] != APFS_MAGIC:
                continue
            out.append(Volume(self, i, oid, addr, blk))
        return out

    def close(self):
        self.image.close()


# ---------------------------------------------------------------- volume

class Volume:
    def __init__(self, container, index, oid, addr, blk):
        self.c = container
        self.index = index
        self.oid = oid
        self.addr = addr
        b = blk
        (self.magic, self.fs_index) = struct.unpack_from("<II", b, 32)
        (self.features, self.ro_compat, self.incompat) = struct.unpack_from("<QQQ", b, 40)
        self.unmount_time = struct.unpack_from("<Q", b, 64)[0]
        (self.reserve_blocks, self.quota_blocks, self.alloc_count) = \
            struct.unpack_from("<QQQ", b, 72)
        # apfs_meta_crypto is 20 bytes, packed, right after apfs_fs_alloc_count
        off = 96 + 20
        (self.root_tree_type, self.extentref_tree_type, self.snap_meta_tree_type) = \
            struct.unpack_from("<III", b, off)
        off += 12
        (self.omap_oid, self.root_tree_oid, self.extentref_tree_oid,
         self.snap_meta_tree_oid) = struct.unpack_from("<QQQQ", b, off)
        off += 32
        (self.revert_to_xid, self.revert_to_sblock_oid) = struct.unpack_from("<QQ", b, off)
        off += 16
        self.next_obj_id = struct.unpack_from("<Q", b, off)[0]
        off += 8
        (self.num_files, self.num_directories, self.num_symlinks,
         self.num_other, self.num_snapshots) = struct.unpack_from("<QQQQQ", b, off)
        off += 40
        (self.blocks_alloced, self.blocks_freed) = struct.unpack_from("<QQ", b, off)
        off += 16
        self.vol_uuid = b[off:off + 16]
        off += 16
        self.last_mod_time = struct.unpack_from("<Q", b, off)[0]
        off += 8
        self.fs_flags = struct.unpack_from("<Q", b, off)[0]
        off += 8
        off += 48 * 9                           # apfs_formatted_by + apfs_modified_by[8]
        self.volname = b[off:off + 256].split(b"\0", 1)[0].decode("utf-8", "replace")
        off += 256
        self.next_doc_id = struct.unpack_from("<I", b, off)[0]
        off += 4
        self.role = struct.unpack_from("<H", b, off)[0]
        self.xid = obj_hdr(b)[1]
        self._omap = None
        self._tree = None

    @property
    def case_insensitive(self):
        return bool(self.incompat & INCOMPAT_CASE_INSENSITIVE)

    @property
    def sealed(self):
        return bool(self.incompat & INCOMPAT_SEALED_VOLUME)

    @property
    def omap(self):
        if self._omap is None:
            self._omap = Omap(self.c.blocks, self.omap_oid)
        return self._omap

    @property
    def tree(self):
        if self._tree is None:
            self._tree = Btree(self.c.blocks, self.root_tree_oid,
                               resolve=self.omap.resolver(), name="fstree")
        return self._tree

    # -------- records

    def records(self, obj_id, rec_type):
        """
        Every record of one kind belonging to one object, in key order.

        A file-system tree sorts by object identifier first and record kind second, so the
        records of one (object, kind) are a contiguous run.  That is all the ordering this
        needs to know -- how keys of the same kind sort among themselves (by name, by hash,
        by offset) differs per kind and is left to the caller, which just reads the run.
        """
        want = (obj_id, rec_type)

        def less(k):
            v = struct.unpack_from("<Q", k, 0)[0]
            return (v & OBJ_ID_MASK, v >> OBJ_TYPE_SHIFT) < want

        for k, v in self.tree.iter_from(less):
            x = struct.unpack_from("<Q", k, 0)[0]
            if (x & OBJ_ID_MASK, x >> OBJ_TYPE_SHIFT) != want:
                return
            yield k, v

    def inode(self, obj_id):
        for _k, v in self.records(obj_id, TYPE_INODE):
            return Inode(self, obj_id, v)
        return None

    def dir_entries(self, obj_id):
        """(name, file_id, d_type) for each entry of a directory."""
        out = []
        for k, v in self.records(obj_id, TYPE_DIR_REC):
            # iOS volumes use the hashed key: a 32-bit field holding the name length and
            # a hash of the name, then the name itself
            nlh = struct.unpack_from("<I", k, 8)[0]
            name_len = nlh & J_DREC_LEN_MASK
            name = k[12:12 + name_len].split(b"\0", 1)[0]
            file_id, _date, flags = struct.unpack_from("<QQH", v, 0)
            out.append((name.decode("utf-8", "surrogateescape"), file_id,
                        flags & DREC_TYPE_MASK))
        return out

    def xattrs(self, obj_id):
        """{name: (flags, xdata bytes)} of one object's extended attributes."""
        out = {}
        for k, v in self.records(obj_id, TYPE_XATTR):
            name_len = struct.unpack_from("<H", k, 8)[0]
            name = k[10:10 + name_len].split(b"\0", 1)[0].decode("utf-8", "replace")
            flags, xdata_len = struct.unpack_from("<HH", v, 0)
            out[name] = (flags, v[4:4 + xdata_len])
        return out

    def extents(self, stream_id):
        """(logical offset, length, physical block) for one data stream, in order."""
        out = []
        for k, v in self.records(stream_id, TYPE_FILE_EXTENT):
            logical = struct.unpack_from("<Q", k, 8)[0]
            len_and_flags, phys, _crypto = struct.unpack_from("<QQQ", v, 0)
            out.append((logical, len_and_flags & J_FILE_EXTENT_LEN_MASK, phys))
        out.sort()
        return out

    # -------- reading a data stream

    def read_stream(self, stream_id, size):
        """
        The bytes of a data stream.  Gaps between extents, and extents whose physical
        block is zero, are holes and read as zeros -- that is how a sparse file is stored.
        """
        bs = self.c.block_size
        out = bytearray(size)
        for logical, length, phys in self.extents(stream_id):
            if logical >= size:
                continue
            n = min(length, size - logical)
            if phys == 0:
                continue                    # a hole; the buffer is already zero
            nblocks = (length + bs - 1) // bs
            data = self.c.blocks.raw(phys, nblocks)
            out[logical:logical + n] = data[:n]
        return bytes(out)

    def xattr_bytes(self, obj_id, name, xattrs=None):
        """One attribute's value, whether it is stored inline or in its own stream."""
        xs = self.xattrs(obj_id) if xattrs is None else xattrs
        if name not in xs:
            return None
        flags, xdata = xs[name]
        if flags & XATTR_DATA_EMBEDDED:
            return xdata
        if flags & XATTR_DATA_STREAM:
            xattr_obj_id, size = struct.unpack_from("<QQ", xdata, 0)
            return self.read_stream(xattr_obj_id, size)
        return xdata

    # -------- paths

    def lookup(self, path):
        """(object identifier, d_type) for a path, or None. Case folded if the volume is."""
        oid = ROOT_DIR_INO_NUM
        dtype = 4
        for part in [p for p in path.split("/") if p and p != "."]:
            ents = self.dir_entries(oid)
            hit = None
            for name, fid, t in ents:
                if name == part:
                    hit = (fid, t)
                    break
            if hit is None and self.case_insensitive:
                lp = part.lower()
                for name, fid, t in ents:
                    if name.lower() == lp:
                        hit = (fid, t)
                        break
            if hit is None:
                return None
            oid, dtype = hit
        return oid, dtype


# ---------------------------------------------------------------- inode

def parse_xfields(buf):
    """The extended fields at the tail of an inode or directory entry: {type: bytes}."""
    if len(buf) < 4:
        return {}
    num, used = struct.unpack_from("<HH", buf, 0)
    out = {}
    hdr = 4 + 4 * num
    if hdr > len(buf):
        return {}
    off = hdr
    for i in range(num):
        xtype, _xflags, xsize = struct.unpack_from("<BBH", buf, 4 + 4 * i)
        out[xtype] = buf[off:off + xsize]
        off += (xsize + 7) & ~7            # each field is padded out to eight bytes
    return out


class Inode:
    def __init__(self, vol, obj_id, v):
        self.vol = vol
        self.obj_id = obj_id
        (self.parent_id, self.private_id) = struct.unpack_from("<QQ", v, 0)
        (self.create_time, self.mod_time, self.change_time, self.access_time) = \
            struct.unpack_from("<QQQQ", v, 16)
        self.internal_flags = struct.unpack_from("<Q", v, 48)[0]
        self.nchildren = struct.unpack_from("<i", v, 56)[0]
        self.nlink = self.nchildren
        (self.protection_class, self.write_gen, self.bsd_flags) = \
            struct.unpack_from("<III", v, 60)
        (self.owner, self.group) = struct.unpack_from("<II", v, 72)
        (self.mode, _pad) = struct.unpack_from("<HH", v, 80)
        self.uncompressed_size = struct.unpack_from("<Q", v, 84)[0]
        self.xf = parse_xfields(v[92:])
        self._xattrs = None
        self._dstream = None
        if INO_EXT_TYPE_DSTREAM in self.xf:
            d = self.xf[INO_EXT_TYPE_DSTREAM]
            (size, alloced, _crypto, written, read) = struct.unpack_from("<QQQQQ", d, 0)
            self._dstream = (size, alloced)

    @property
    def xattrs(self):
        if self._xattrs is None:
            self._xattrs = self.vol.xattrs(self.obj_id)
        return self._xattrs

    @property
    def ifmt(self):
        return self.mode & S_IFMT

    @property
    def is_dir(self):
        return self.ifmt == S_IFDIR

    @property
    def is_reg(self):
        return self.ifmt == S_IFREG

    @property
    def is_link(self):
        return self.ifmt == S_IFLNK

    @property
    def is_sparse(self):
        return bool(self.internal_flags & INODE_IS_SPARSE)

    @property
    def rdev(self):
        d = self.xf.get(INO_EXT_TYPE_RDEV)
        return struct.unpack_from("<I", d, 0)[0] if d else 0

    @property
    def decmpfs(self):
        """The com.apple.decmpfs attribute's value, or None if the file is not compressed."""
        return self.vol.xattr_bytes(self.obj_id, DECMPFS_XATTR_NAME, self.xattrs)

    @property
    def size(self):
        """The logical size: what stat would report."""
        hdr = self.decmpfs
        if hdr is not None and len(hdr) >= 16:
            magic, _ctype, usize = struct.unpack_from("<IIQ", hdr, 0)
            if magic == DECMPFS_MAGIC:
                return usize
        if self.is_link:
            t = self.symlink_target_bytes()
            return len(t) if t is not None else 0
        if self._dstream:
            return self._dstream[0]
        return 0

    def symlink_target_bytes(self):
        t = self.vol.xattr_bytes(self.obj_id, SYMLINK_EA_NAME, self.xattrs)
        if t is None:
            return None
        return t.split(b"\0", 1)[0]

    def contents(self):
        """The file's bytes, decompressing them if that is how they are stored."""
        if self.is_link:
            return self.symlink_target_bytes() or b""
        hdr = self.decmpfs
        if hdr is not None and len(hdr) >= 16 and \
                struct.unpack_from("<I", hdr, 0)[0] == DECMPFS_MAGIC:
            return self.decompress(hdr)
        if self._dstream is None:
            return b""
        return self.vol.read_stream(self.private_id, self._dstream[0])

    def decompress(self, hdr):
        magic, ctype, usize = struct.unpack_from("<IIQ", hdr, 0)
        attr = hdr[16:]
        if ctype in (3, 9, 11):                 # the bytes are in the attribute itself
            return decmpfs_attr(ctype, attr, usize)
        if ctype in (4, 10, 12):                # the bytes are in the resource fork
            rsrc = self.vol.xattr_bytes(self.obj_id, RSRC_FORK_XATTR_NAME, self.xattrs)
            if rsrc is None:
                raise ApfsError("inode %d: compression type %d but no resource fork"
                                % (self.obj_id, ctype))
            return decmpfs_rsrc(ctype, rsrc, usize)
        raise ApfsError("inode %d: unknown decmpfs compression type %d"
                        % (self.obj_id, ctype))


def _inflate(buf, out_len):
    """
    One zlib stream.  A leading 0xff is decmpfs's way of saying "this piece did not
    compress, the rest is the bytes as they are".
    """
    if buf[:1] == b"\xff":
        return buf[1:1 + out_len]
    return zlib.decompress(buf)


def decmpfs_attr(ctype, attr, usize):
    """
    The contents sit in the com.apple.decmpfs attribute itself, right after the header.

    Type 9 means they were stored as they are, with one marker byte in front.  All 23947
    type-9 files in the iOS 16.5 root file system have 0xcc there and exactly usize bytes
    after it, which is how the marker's length was settled -- the attribute is taken as
    bare only if it is already exactly usize bytes long.
    """
    if ctype == 9:
        if len(attr) == usize:
            return attr
        return attr[1:1 + usize]
    if ctype == 3:
        return _inflate(attr, usize)[:usize]
    if ctype == 11:
        import lzfse
        if attr[:1] == b"\xff":
            return attr[1:1 + usize]
        return lzfse.decompress(attr)[:usize]
    raise ApfsError("compression type %d in the attribute is not supported" % ctype)


def decmpfs_rsrc(ctype, rsrc, usize):
    """
    The contents are in the com.apple.ResourceFork attribute, cut into 64 KB pieces, each
    compressed on its own.

    The fork is a real HFS-Plus resource fork: sixteen big-endian bytes giving the offset
    and the length of the resource data and of the resource map.  At that data offset sits
    a four-byte big-endian length, and the piece table starts right after it -- a
    little-endian count, then an (offset, length) pair per piece, the offsets measured
    from the start of the table.  The table is not at a fixed place: the data offset is
    256 in every file here, so the table lands at 260, but it has to be read rather than
    assumed.
    """
    if len(rsrc) < 16:
        raise ApfsError("resource fork too short (%d bytes)" % len(rsrc))
    data_off, _map_off, _data_len, _map_len = struct.unpack_from(">IIII", rsrc, 0)
    base = data_off + 4
    if base + 4 > len(rsrc):
        raise ApfsError("resource fork: data offset %d is past the end of the fork "
                        "(%d bytes)" % (data_off, len(rsrc)))
    nblocks = struct.unpack_from("<I", rsrc, base)[0]
    want = (usize + DECMPFS_BLOCK_SIZE - 1) // DECMPFS_BLOCK_SIZE
    if nblocks != want:
        raise ApfsError("resource fork says %d pieces, %d bytes needs %d"
                        % (nblocks, usize, want))
    out = bytearray()
    for i in range(nblocks):
        off, length = struct.unpack_from("<II", rsrc, base + 4 + 8 * i)
        piece = rsrc[base + off:base + off + length]
        n = min(DECMPFS_BLOCK_SIZE, usize - len(out))
        if ctype == 4:
            out += _inflate(piece, n)[:n]
        elif ctype == 10:
            out += piece[:n]
        elif ctype == 12:
            import lzfse
            out += (piece[1:1 + n] if piece[:1] == b"\xff"
                    else lzfse.decompress(piece)[:n])
        else:
            raise ApfsError("compression type %d in the resource fork is not supported"
                            % ctype)
    if len(out) != usize:
        raise ApfsError("decompressed %d bytes, the header says %d" % (len(out), usize))
    return bytes(out)

