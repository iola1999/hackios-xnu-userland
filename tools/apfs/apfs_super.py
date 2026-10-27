# apfs_super.py -- the bottom layer of the reader that apfs.py drives: what an APFS object
# header says, the Fletcher-64 that guards every block, the image -- raw or inside a .dmg --
# and the cache of blocks read through it.  The b-tree, object map, volume and file layers
# sit above in apfs_reader.py; commands and the usage in apfs.py.
#
# The on-disk structures are described in bsd/miscfs/apfs/apfs_format.h (exported to
# xnu/files/); the layout is checked against that header by tools/apfs/run-tests.sh.

import os
import struct
import sys
import zlib

BLOCK0 = 0

# ---------------------------------------------------------------- object header

OBJ_HDR_SIZE = 32
OBJECT_TYPE_MASK = 0x0000FFFF
OBJ_STORAGETYPE_MASK = 0xC0000000
OBJ_VIRTUAL = 0x00000000
OBJ_EPHEMERAL = 0x80000000
OBJ_PHYSICAL = 0x40000000
OBJ_NOHEADER = 0x20000000

OBJECT_TYPE_NX_SUPERBLOCK = 0x01
OBJECT_TYPE_BTREE = 0x02
OBJECT_TYPE_BTREE_NODE = 0x03
OBJECT_TYPE_OMAP = 0x0B
OBJECT_TYPE_CHECKPOINT_MAP = 0x0C
OBJECT_TYPE_FS = 0x0D
OBJECT_TYPE_FSTREE = 0x0E

NX_MAGIC = b"NXSB"
APFS_MAGIC = b"APSB"

OID_NX_SUPERBLOCK = 1

# ---------------------------------------------------------------- b-tree

BTNODE_ROOT = 0x0001
BTNODE_LEAF = 0x0002
BTNODE_FIXED_KV_SIZE = 0x0004
BTNODE_HASHED = 0x0008
BTNODE_NOHEADER = 0x0010

BTREE_PHYSICAL = 0x00000010

BTREE_NODE_DATA_OFF = 56      # where btn_data starts, i.e. sizeof(btree_node_phys)
BTREE_INFO_SIZE = 40          # sizeof(btree_info), kept at the end of a root node

# ---------------------------------------------------------------- record kinds

OBJ_ID_MASK = 0x0FFFFFFFFFFFFFFF
OBJ_TYPE_SHIFT = 60

TYPE_SNAP_METADATA = 1
TYPE_EXTENT = 2
TYPE_INODE = 3
TYPE_XATTR = 4
TYPE_SIBLING_LINK = 5
TYPE_DSTREAM_ID = 6
TYPE_CRYPTO_STATE = 7
TYPE_FILE_EXTENT = 8
TYPE_DIR_REC = 9
TYPE_DIR_STATS = 10
TYPE_SNAP_NAME = 11
TYPE_SIBLING_MAP = 12
TYPE_FILE_INFO = 13

TYPE_NAMES = {
    1: "snap_metadata", 2: "extent", 3: "inode", 4: "xattr", 5: "sibling_link",
    6: "dstream_id", 7: "crypto_state", 8: "file_extent", 9: "dir_rec",
    10: "dir_stats", 11: "snap_name", 12: "sibling_map", 13: "file_info",
}

ROOT_DIR_INO_NUM = 2

J_DREC_LEN_MASK = 0x000003FF
J_DREC_HASH_SHIFT = 10
DREC_TYPE_MASK = 0x000F

J_FILE_EXTENT_LEN_MASK = 0x00FFFFFFFFFFFFFF

XATTR_DATA_STREAM = 0x01
XATTR_DATA_EMBEDDED = 0x02

INO_EXT_TYPE_NAME = 4
INO_EXT_TYPE_DSTREAM = 8
INO_EXT_TYPE_SPARSE_BYTES = 13
INO_EXT_TYPE_RDEV = 14

INODE_IS_SPARSE = 0x00000200
INODE_HAS_UNCOMPRESSED_SIZE = 0x00040000

S_IFMT = 0o170000
S_IFIFO, S_IFCHR, S_IFDIR, S_IFBLK = 0o010000, 0o020000, 0o040000, 0o060000
S_IFREG, S_IFLNK, S_IFSOCK = 0o100000, 0o120000, 0o140000

SYMLINK_EA_NAME = "com.apple.fs.symlink"
DECMPFS_XATTR_NAME = "com.apple.decmpfs"
RSRC_FORK_XATTR_NAME = "com.apple.ResourceFork"

DECMPFS_MAGIC = 0x636D7066
DECMPFS_BLOCK_SIZE = 65536

INCOMPAT_CASE_INSENSITIVE = 0x01
INCOMPAT_NORMALIZATION_INSENSITIVE = 0x08
INCOMPAT_SEALED_VOLUME = 0x20


class ApfsError(Exception):
    pass


# ---------------------------------------------------------------- checksum

def _fletcher64_py(buf):
    mod = 0xFFFFFFFF
    words = struct.unpack("<%dI" % (len(buf) // 4), buf)
    sum1 = sum2 = 0
    for w in words:
        sum1 += w
        sum2 += sum1
    sum1 %= mod
    sum2 %= mod
    c1 = mod - ((sum1 + sum2) % mod)
    c2 = mod - ((sum1 + c1) % mod)
    return (c2 << 32) | c1


def _make_fletcher64():
    """numpy if it is there -- this runs over every metadata block we touch."""
    try:
        import numpy as np
    except ImportError:
        return _fletcher64_py

    def f(buf):
        mod = 0xFFFFFFFF
        w = np.frombuffer(buf, dtype="<u4").astype(np.uint64)
        n = len(w)
        # sum1 = sum(w), sum2 = sum((n-i) * w[i]) -- both exact in 64 bits for a block
        sum1 = int(w.sum())
        weights = np.arange(n, 0, -1, dtype=np.uint64)
        sum2 = int((w * weights).sum())
        sum1 %= mod
        sum2 %= mod
        c1 = mod - ((sum1 + sum2) % mod)
        c2 = mod - ((sum1 + c1) % mod)
        return (c2 << 32) | c1
    return f


fletcher64 = _make_fletcher64()


def checksum_ok(block):
    """An object block carries its Fletcher-64 in the first eight bytes."""
    if len(block) % 4:
        return False
    return fletcher64(block[8:]) == int.from_bytes(block[:8], "little")


# ---------------------------------------------------------------- the image

class RawImage:
    def __init__(self, path):
        self.fd = os.open(path, os.O_RDONLY)
        self.size = os.lseek(self.fd, 0, os.SEEK_END)
        self.path = path

    def read(self, off, length):
        return os.pread(self.fd, length, off)

    def close(self):
        os.close(self.fd)


class DmgImage:
    """A .dmg, read through its APFS partition so offsets look like a raw partition."""

    def __init__(self, path):
        sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
        from dmg import Dmg, read_gpt, APFS_PART_TYPE
        self.dmg = Dmg(path)
        parts = [p for p in read_gpt(self.dmg) if p.type_guid == APFS_PART_TYPE]
        if not parts:
            raise ApfsError("%s: no APFS partition in the image" % path)
        self.base = parts[0].offset
        self.size = parts[0].length
        self.path = path

    def read(self, off, length):
        return self.dmg.read(self.base + off, length)

    def close(self):
        pass


def open_image(path):
    with open(path, "rb") as f:
        f.seek(0, 2)
        size = f.tell()
        if size > 512:
            f.seek(size - 512)
            if f.read(4) == b"koly":
                return DmgImage(path)
    return RawImage(path)


# ---------------------------------------------------------------- blocks

class Blocks:
    """Block-sized reads with a small cache, and the checksum check on every object."""

    def __init__(self, image, block_size, cache_blocks=4096):
        self.image = image
        self.block_size = block_size
        self.cache = {}
        self.cache_limit = cache_blocks
        self.reads = 0
        self.cache_hits = 0
        self.cksum_checked = 0
        self.cksum_bad = 0

    def raw(self, addr, count=1):
        return self.image.read(addr * self.block_size, count * self.block_size)

    def get(self, addr, verify=True):
        b = self.cache.get(addr)
        if b is not None:
            self.cache_hits += 1
            return b
        b = self.raw(addr)
        if len(b) != self.block_size:
            raise ApfsError("short read at block %d" % addr)
        if verify:
            self.cksum_checked += 1
            if not checksum_ok(b):
                self.cksum_bad += 1
                raise ApfsError("block %d: bad Fletcher-64 checksum" % addr)
        if len(self.cache) >= self.cache_limit:
            # cheap enough: drop a chunk of the cache rather than track an LRU order
            for k in list(self.cache)[:self.cache_limit // 4]:
                del self.cache[k]
        self.cache[addr] = b
        return b


def obj_hdr(block):
    oid, xid, otype, osub = struct.unpack_from("<QQII", block, 8)
    return oid, xid, otype, osub
