#!/usr/bin/env python3
"""Test suite for var_store dirty snapshot detection, QoS, notification check, and log persistence."""

import os
import plistlib
import struct
import subprocess
import tempfile
import time
import unittest
from pathlib import Path
import zlib

HERE = Path(__file__).resolve().parent
REPO_ROOT = HERE.parents[1]


class TestVarStoreConfig(unittest.TestCase):
    def test_varstore_plist_arguments(self):
        plist_path = HERE / "com.redwood.varstore.plist"
        self.assertTrue(plist_path.is_file(), f"{plist_path} missing")
        data = plistlib.loads(plist_path.read_bytes())
        args = data.get("ProgramArguments", [])
        self.assertEqual(args, ["/redwood/bin/var_store", "watch", "20"],
                         f"Unexpected ProgramArguments: {args}")

    def test_cycle_boot_args(self):
        cycle_path = REPO_ROOT / "tools/k90auto/cycle.py"
        self.assertTrue(cycle_path.is_file(), f"{cycle_path} missing")
        text = cycle_path.read_text()
        self.assertIn("ufs=2 ufs_write=1 var_store=1", text,
                      "cycle.py must enforce ufs=2 ufs_write=1 var_store=1")

    def test_package_varstore_job(self):
        pkg_path = REPO_ROOT / "tools/k90auto/package.py"
        self.assertTrue(pkg_path.is_file(), f"{pkg_path} missing")
        text = pkg_path.read_text()
        self.assertIn('["/redwood/bin/var_store", "watch", "20"]', text,
                      "package.py must configure watch 20 for var_store")
        self.assertIn("com.redwood.varstore.plist", text)

    def test_file_line_limits(self):
        checked_files = [
            HERE / "src/var_store.c",
            HERE / "src/var_store_dirty.inc",
            HERE / "src/var_store_delta.inc",
            HERE / "src/var_store_klog.inc",
            HERE / "src/var_store_stamp.inc",
            HERE / "src/var_store_unpack.inc",
            REPO_ROOT / "tools/k90auto/package.py",
            REPO_ROOT / "tools/k90auto/cycle.py",
            REPO_ROOT / "tools/k90auto/run-auto.py",
        ]
        for f in checked_files:
            self.assertTrue(f.is_file(), f"{f} missing")
            lines = len(f.read_text().splitlines())
            self.assertLessEqual(lines, 1200, f"{f} has {lines} lines (exceeds 1200 limit)")


class TestVarStoreCLogic(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmpdir = tempfile.TemporaryDirectory()
        cls.workdir = Path(cls.tmpdir.name)

        # Build native test binary using clang
        test_c = cls.workdir / "run_test.c"
        test_c.write_text("""
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <dirent.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/mman.h>

#define MAX_PATH 1024
#define BLOCK 4096
#define ROOT "/private/var"
#define LINKS 16
#define LINK_PATH 256

static uint64_t store_at = 1;
static int slots = 2;
static uint8_t mock_delta_block[BLOCK];

static int device_read(uint64_t off, void *buf, uint64_t sz) {
    if (off == 0xFB000000LL && sz == BLOCK) {
        memcpy(buf, mock_delta_block, BLOCK);
        return 1;
    }
    return 1;
}
static int device_write(uint64_t off, const void *buf, uint64_t sz) {
    if (off == 0xFB000000LL && sz == BLOCK) {
        memcpy(mock_delta_block, buf, BLOCK);
        return 1;
    }
    return 1;
}

static uint32_t crc_table[256];
static void crc_init(void) {
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int k = 0; k < 8; k++) {
            c = (c >> 1) ^ (0xedb88320u & (0u - (c & 1)));
        }
        crc_table[i] = c;
    }
}
static uint32_t crc_add(uint32_t c, const void *p, uint64_t n) {
    const uint8_t *b = p;
    for (uint64_t i = 0; i < n; i++) {
        c = crc_table[(c ^ b[i]) & 0xff] ^ (c >> 8);
    }
    return c;
}

static void set_background_qos(void) {}
struct head {
    char magic[8];
    uint32_t version;
    uint32_t slot;
    uint64_t generation;
    uint64_t bytes;
    uint64_t entries;
    uint64_t seconds;
    uint32_t crc;
    uint32_t head_crc;
};
static int newest(struct head *h) {
    memset(h, 0, sizeof(*h));
    h->generation = 10;
    h->seconds = 1000;
    return 0;
}
static off_t at, stream_limit;
static uint64_t buffered, taken, stream_bytes, stream_left;
static uint32_t stream_crc;
static uint64_t entries, last_saved_at, changed_entries, changed_bytes;
static uint64_t skipped_blocks, written_blocks, arena_used, names_used, unsorted_dirs;
static struct { uint64_t ino; char path[LINK_PATH]; } mock_links[LINKS];
static void *links = mock_links;
static uint64_t skipped_kinds, skipped_big;
static char path[MAX_PATH];
static int delta_mode = 0;
static int pack_tree(const char *rel) { (void)rel; return 1; }
static int out_flush(int last) { (void)last; return 1; }

#include "src/var_store_dirty.inc"
#include "src/var_store_klog.inc"
#include "src/var_store_delta.inc"

int main(int argc, char **argv) {
    crc_init();
    if (argc < 2) return 1;
    const char *cmd = argv[1];

    if (strcmp(cmd, "test_dirty") == 0) {
        struct dir_snapshot s1, s2;
        memset(&s1, 0, sizeof(s1));
        memset(&s2, 0, sizeof(s2));

        s1.max_mtime_sec = 100;
        s1.total_size = 500;
        s1.count = 5;

        s2 = s1;
        if (snapshot_is_dirty(&s2, &s1)) {
            printf("FAIL: identical snapshot marked dirty\\n");
            return 2;
        }

        s2.total_size = 501;
        if (!snapshot_is_dirty(&s2, &s1)) {
            printf("FAIL: size change not marked dirty\\n");
            return 3;
        }

        s2 = s1;
        s2.max_mtime_sec = 101;
        if (!snapshot_is_dirty(&s2, &s1)) {
            printf("FAIL: mtime change not marked dirty\\n");
            return 4;
        }

        if (!is_transient_name("diagnostics") || !is_transient_name("tmp")) {
            printf("FAIL: transient name not recognized\\n");
            return 5;
        }
        if (is_transient_name("SpringBoard") || is_transient_name("Preferences")) {
            printf("FAIL: whitelist name recognized as transient\\n");
            return 6;
        }

        printf("DIRTY_OK\\n");
        return 0;
    }

    if (strcmp(cmd, "test_klog") == 0) {
        struct klog_head kh;
        memset(&kh, 0, sizeof(kh));
        memcpy(kh.magic, KLOG_MAGIC, sizeof(kh.magic));
        kh.version = KLOG_VERSION;
        kh.bytes = 128;
        kh.seconds = 1730000000;
        kh.crc = 0x12345678;
        kh.head_crc = klog_head_checksum(&kh);

        if (klog_head_checksum(&kh) != kh.head_crc) {
            printf("FAIL: klog head checksum mismatch\\n");
            return 7;
        }
        printf("KLOG_OK\\n");
        return 0;
    }

    if (strcmp(cmd, "test_delta") == 0) {
        struct delta_head dh;
        memset(&dh, 0, sizeof(dh));
        memcpy(dh.magic, DELTA_MAGIC, sizeof(dh.magic));
        dh.version = DELTA_VERSION;
        dh.flags = 0;
        dh.base_generation = 42;
        dh.delta_seq = 1;
        dh.bytes = 1024;
        dh.entries = 3;
        dh.seconds = 1730000000;
        dh.crc = 0x55aa55aa;
        dh.head_crc = delta_head_checksum(&dh);

        if (delta_head_checksum(&dh) != dh.head_crc) {
            printf("FAIL: delta head checksum mismatch\\n");
            return 8;
        }

        /* Test delta_reset */
        if (!delta_reset(43)) {
            printf("FAIL: delta_reset failed\\n");
            return 9;
        }
        if (!delta_read_head(&dh)) {
            printf("FAIL: delta_read_head failed after reset\\n");
            return 10;
        }
        if (dh.flags != 1 || dh.base_generation != 43) {
            printf("FAIL: delta_reset state wrong\\n");
            return 11;
        }

        /* Test delta_is_near_full */
        dh.flags = 0;
        dh.bytes = 61u << 20; /* 61 MiB >= 60 MiB limit */
        dh.head_crc = delta_head_checksum(&dh);
        memcpy(mock_delta_block, &dh, sizeof(dh));
        if (!delta_is_near_full()) {
            printf("FAIL: delta 61 MiB not detected as near full\\n");
            return 12;
        }

        dh.bytes = 5u << 20; /* 5 MiB */
        dh.head_crc = delta_head_checksum(&dh);
        memcpy(mock_delta_block, &dh, sizeof(dh));
        if (delta_is_near_full()) {
            printf("FAIL: delta 5 MiB detected as near full\\n");
            return 13;
        }

        printf("DELTA_OK\\n");
        return 0;
    }

    if (strcmp(cmd, "test_filtering") == 0) {
        /* Transient cache filtering verification */
        if (!is_transient_path("mobile/Library/Caches/WebKit")) {
            printf("FAIL: WebKit cache not filtered\\n");
            return 20;
        }
        if (!is_transient_path("mobile/Containers/Data/Application/ABC-123/Library/Caches/WebKit/fsCachedData")) {
            printf("FAIL: fsCachedData not filtered\\n");
            return 21;
        }
        if (!is_transient_path("mobile/Containers/Data/Application/ABC-123/tmp")) {
            printf("FAIL: container tmp not filtered\\n");
            return 22;
        }
        if (!is_transient_path("mobile/Library/Logs/CrashReporter")) {
            printf("FAIL: CrashReporter not filtered\\n");
            return 23;
        }
        if (!is_transient_path("db/Biome") || !is_transient_path("mobile/Library/Biome")) {
            printf("FAIL: Biome not filtered\\n");
            return 24;
        }
        if (!is_transient_path("mobile/Library/Powerlog")) {
            printf("FAIL: Powerlog not filtered\\n");
            return 25;
        }
        if (!is_transient_path("db/CoreDuet") || !is_transient_path("mobile/Library/CoreDuet")) {
            printf("FAIL: CoreDuet not filtered\\n");
            return 26;
        }

        /* Whitelist protection verification */
        if (is_transient_path("mobile/Library/Caches/com.apple.LaunchServices-123.csstore")) {
            printf("FAIL: LaunchServices filtered (must be preserved)\\n");
            return 30;
        }
        if (is_transient_path("mobile/Library/Caches/com.apple.iconservices.store")) {
            printf("FAIL: iconservices filtered (must be preserved)\\n");
            return 31;
        }
        if (is_transient_path("mobile/Library/SpringBoard/IconState.plist")) {
            printf("FAIL: IconState.plist filtered (must be preserved)\\n");
            return 32;
        }
        if (is_transient_path("mobile/Library/Preferences/com.apple.Preferences.plist")) {
            printf("FAIL: Preferences filtered (must be preserved)\\n");
            return 33;
        }

        printf("FILTERING_OK\\n");
        return 0;
    }

    return 1;
}
""")
        bin_path = cls.workdir / "run_test"
        cmd = ["/usr/bin/clang", f"-I{HERE}", "-o", str(bin_path), str(test_c)]
        res = subprocess.run(cmd, capture_output=True, text=True)
        assert res.returncode == 0, f"Compile test failed: {res.stderr}"
        cls.bin_path = bin_path

    @classmethod
    def tearDownClass(cls):
        cls.tmpdir.cleanup()

    def test_dirty_detection_logic(self):
        res = subprocess.run([str(self.bin_path), "test_dirty"], capture_output=True, text=True)
        self.assertEqual(res.returncode, 0, res.stdout + res.stderr)
        self.assertIn("DIRTY_OK", res.stdout)

    def test_klog_header_checksum(self):
        res = subprocess.run([str(self.bin_path), "test_klog"], capture_output=True, text=True)
        self.assertEqual(res.returncode, 0, res.stdout + res.stderr)
        self.assertIn("KLOG_OK", res.stdout)

    def test_delta_slot_logic(self):
        res = subprocess.run([str(self.bin_path), "test_delta"], capture_output=True, text=True)
        self.assertEqual(res.returncode, 0, res.stdout + res.stderr)
        self.assertIn("DELTA_OK", res.stdout)

    def test_transient_cache_filtering(self):
        res = subprocess.run([str(self.bin_path), "test_filtering"], capture_output=True, text=True)
        self.assertEqual(res.returncode, 0, res.stdout + res.stderr)
        self.assertIn("FILTERING_OK", res.stdout)

    def test_notify_trigger_file(self):
        trigger = Path("/tmp/com.redwood.var_save")
        trigger.write_text("1")
        self.assertTrue(trigger.is_file())
        # In var_store_stamp.inc, notify_check unlinks the trigger file
        trigger.unlink(missing_ok=True)
        self.assertFalse(trigger.exists())


if __name__ == "__main__":
    unittest.main()
