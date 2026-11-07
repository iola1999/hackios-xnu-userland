#!/usr/bin/env python3
"""Run flash_root from init.c with simulated mounts and root switching.

Requires Python 3 and a host C compiler (CC, or clang). This exercises the C control
flow for both system layouts and five failures; it does not mount APFS or test the
kernel halt/loader-return path. Run from any working directory.
"""

import os
from pathlib import Path
import shlex
import subprocess
import tempfile

HEADER = r'''

#include <assert.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#define MNT_RDONLY 1
#define MNT_UNION 2
enum scenario_id {
    SINGLE_CONTAINER,
    THREE_CONTAINERS,
    MISSING_CRYPTEX,
    BIND_FAILURE,
    SWITCH_FAILURE,
    ROOT_FAILURE,
    HELPER_FAILURE,
    SCENARIO_COUNT
};
static int scenario, switches, binds, xpc_bound, synths, cryptexes;
static char created[12][128];
static int fake_lstat(const char *p, struct stat *s) {
    memset(s, 0, sizeof(*s));
    s->st_mode = S_IFDIR;
    if (!strcmp(p, "/fw") || !strcmp(p, "/jb-bin") || strstr(p, "RedwoodMetal.bundle")) {
        errno = ENOENT;
        return -1;
    }
    if (strstr(p, "/System/Cryptexes/") && scenario == MISSING_CRYPTEX) {
        errno = ENOENT;
        return -1;
    }
    if (strstr(p, "NanoLaunchDaemons") || strstr(p, "LaunchAngels")) {
        for (int i = 0; i < synths; i++)
            if (!strcmp(p, created[i]))
                return 0;
        errno = ENOENT;
        return -1;
    }
    return 0;
}
static int fake_mount(const char *type, const char *to, int flags, void *data) {
    (void)flags;
    if (!strcmp(type, "apfs")) {
        if (!strcmp(to, "/flash")) {
            if (scenario == ROOT_FAILURE) {
                errno = EIO;
                return -1;
            }
            return 0;
        }
        if (scenario == THREE_CONTAINERS && strstr((char *)data, "ufs_darwin")) {
            cryptexes++;
            return 0;
        }
        errno = EINVAL;
        return -1;
    }
    binds++;
    if (strstr(to, "/xpc"))
        xpc_bound++;
    if (scenario == BIND_FAILURE && strstr(to, "NanoLaunchDaemons")) {
        errno = EIO;
        return -1;
    }
    if (scenario == HELPER_FAILURE && !strcmp(to, "/redwood")) {
        errno = EIO;
        return -1;
    }
    struct stat st;
    if (fake_lstat(to, &st))
        return -1;
    return 0;
}
static int fake_sysctl(const char *name, void *a, size_t *b, void *data, size_t n) {
    (void)a;
    (void)b;
    (void)n;
    if (!strcmp(name, "kern.apfs_synthetic")) {
        assert(synths < 12);
        strcpy(created[synths++], data);
        return 0;
    }
    if (!strcmp(name, "kern.redwood_switch_root")) {
        switches++;
        if (scenario == SWITCH_FAILURE) {
            errno = EIO;
            return -1;
        }
        return 0;
    }
    return -1;
}
static int fake_open(const char *p, int f, ...) {
    (void)p;
    (void)f;
    errno = ENOENT;
    return -1;
}
static ssize_t fake_entries(int f, char *b, size_t n, off_t *p) {
    (void)f;
    (void)b;
    (void)n;
    (void)p;
    return 0;
}
static int fake_chdir(const char *p) {
    (void)p;
    return 0;
}
#define lstat fake_lstat
#define stat(p, s) fake_lstat(p, s)
#define mount fake_mount
#define sysctlbyname fake_sysctl
#define open fake_open
#define getdirentries fake_entries
#define chdir fake_chdir
'''
TEST_MAIN = r'''

int main(void) {
    base = "/redwood";
    for (scenario = SINGLE_CONTAINER; scenario < SCENARIO_COUNT; scenario++) {
        checks = failures = flash_root_mode = switches = binds = xpc_bound = synths = cryptexes = 0;
        int ok = flash_root("ufs_darwin");
        assert(ok == (scenario == SINGLE_CONTAINER || scenario == THREE_CONTAINERS));
        assert(flash_root_mode == ok);
        if (scenario != ROOT_FAILURE)
            assert(xpc_bound == 1);
        if (scenario == SINGLE_CONTAINER || scenario == THREE_CONTAINERS)
            assert(synths == 2);
        if (scenario == THREE_CONTAINERS)
            assert(cryptexes == 2);
        if (scenario == BIND_FAILURE || scenario == ROOT_FAILURE)
            assert(switches == 0);
    }
    puts("PASS: seven flash_root scenarios (single/three containers, bind failure, cryptex "
         "failure, switch failure, root failure, helper failure)");
}
'''


def main():
    root = Path(__file__).resolve().parents[2]
    source = (root / "userland/src/boot/init.c").read_text()
    # Compile the current implementation, including its check() failure accounting.
    start = source.index("#define PATTERN_SIZE")
    end = source.index("/* Same formula as build.sh", start)
    compiler = shlex.split(os.environ.get("CC", "clang"))
    with tempfile.TemporaryDirectory(prefix="flash-root-test-") as temp:
        directory = Path(temp)
        c_file = directory / "flash-root.c"
        binary = directory / "flash-root-test"
        c_file.write_text(HEADER + source[start:end] + TEST_MAIN)
        subprocess.run(compiler + ["-Wall", "-Wextra", "-Werror", "-Wno-unused-function",
                                   str(c_file), "-o", str(binary)], check=True)
        subprocess.run([str(binary)], check=True)


if __name__ == "__main__":
    main()
