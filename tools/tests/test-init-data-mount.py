#!/usr/bin/env python3
"""Exercise init's actual data-volume mount and subsequent startup control flow.

Host stubs record filesystem/process calls; no volume is mounted and no child is
started. This checks mount-error propagation and the choice between the APFS data
volume and ramfs by darwin_var's block 0, not the kernel's persistence policy.
"""
import os
from pathlib import Path
import shlex
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
PRELUDE = r'''
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#define RB_HALT 8
#define reboot fake_reboot
static int mount_error, checks, failures, skeleton_reads, restores, processes, rc_runs, halts;
static int var_apfs, apfs_mounts, ramfs_mounts, raw_open, cam_vol_unlinks, mg_cache_unlinks;
static unsigned long long var_bytes = 104ULL << 30;
static int unexpected(void) { assert(!"unexpected host filesystem/process operation"); return -1; }
static int fake_mount(const char *type, const char *path, int flags, void *data) {
    assert(!strcmp(path, "/private/var") && !flags);
    if (!strcmp(type, "apfs")) {
        assert(data && !strcmp(data, "/dev/disk1s2")); apfs_mounts++;
    } else {
        assert(!strcmp(type, "ramfs") && !data); ramfs_mounts++;
    }
    errno = mount_error;
    return mount_error ? -1 : 0;
}
/* darwin_var's block 0, read through /dev/rdisk1s2 */
static int fake_open(const char *path, int flags, int mode) {
    assert(!strcmp(path, "/dev/rdisk1s2") && flags == O_RDONLY && !mode && !raw_open); raw_open = 1; return 7;
}
static ssize_t fake_pread(int fd, void *buf, size_t n, off_t at) {
    assert(fd == 7 && raw_open && n == 4096 && at == 0);
    memset(buf, 0, n);
    if (var_apfs) memcpy((char *)buf + 32, "NXSB", 4);
    return (ssize_t)n;
}
static int fake_close(int fd) { assert(fd == 7 && raw_open); raw_open = 0; return 0; }
static int fake_lstat(const char *path, struct stat *st) {
    (void)path; memset(st, 0, sizeof(*st)); st->st_mode = S_IFDIR; return 0;
}
static int no_package(const char *path, struct stat *st) { (void)path; (void)st; return -1; }
static int fake_chmod(const char *path, int mode) {
    assert(!strcmp(path, "/dev/console") && mode == 0666); return 0;
}
static int fake_unlink(const char *path) {
    if (!strcmp(path, "/private/var/mobile/Library/Preferences/com.apple.cameracapture.volatile.plist") ||
        !strcmp(path, "/private/var/root/Library/Preferences/com.apple.cameracapture.volatile.plist")) {
        cam_vol_unlinks++;
        return 0;
    }
    if (!strcmp(path, "/private/var/containers/Shared/SystemGroup/systemgroup.com.apple.mobilegestaltcache/Library/Caches/com.apple.MobileGestalt.plist")) {
        mg_cache_unlinks++;
        return 0;
    }
    assert(!"unexpected unlink path");
    return 0;
}
static void check(int ok, const char *what) { (void)what; checks++; failures += !ok; }
static void launchservices_rebuild_once(void) {}
static const char *in_image(const char *path) { return path; }
static ssize_t read_whole(const char *path, char *buf, size_t size) {
    assert(!strcmp(path, "/etc/var.skel") && size >= 103);
    skeleton_reads++;
    for (int i = 0; i < 51; i++) { buf[2*i] = 'd'; buf[2*i+1] = '\n'; }
    buf[102] = 0; return 102;
}
static int split_tabs(char *line, char **fields, int n) {
    (void)line; assert(n == 6);
    fields[0] = "d"; fields[1] = "755"; fields[2] = "0";
    fields[3] = "0"; fields[4] = "/private/var/test"; return 5;
}
static unsigned long parse_number(const char *s, int base) { return strtoul(s, NULL, base); }
/* init reads its switches from the kernel command line; this run enables the self-tests */
static const char *kernel_bootargs = "debug=0x10e init_selftest=1";
static int fake_sysctlbyname(const char *name, void *old, size_t *oldlen, void *new, size_t newlen) {
    assert(old && oldlen && !new && !newlen);
    if (!strcmp(name, "kern.annibale.darwin_var_bytes")) {
        assert(*oldlen == sizeof(var_bytes)); memcpy(old, &var_bytes, sizeof(var_bytes)); return 0;
    }
    assert(!strcmp(name, "kern.bootargs"));
    size_t n = strlen(kernel_bootargs);
    if (n > *oldlen) n = *oldlen;
    memcpy(old, kernel_bootargs, n); *oldlen = n; return 0;
}
#define sysctlbyname fake_sysctlbyname
#define mount fake_mount
#define lstat fake_lstat
#define stat(p, s) no_package(p, s)
#define chmod fake_chmod
#define mkdir(...) unexpected()
#define chown(...) unexpected()
#define copy_file(...) unexpected()
#define open fake_open
#define pread fake_pread
#define close fake_close
#define symlink(...) unexpected()
#define unlink fake_unlink
#define graft_tree(...) unexpected()
#define own_tree(...) unexpected()
#define fork(...) unexpected()
#define execve(...) unexpected()
#define wait4(...) unexpected()
#define _exit(...) unexpected()
'''
STUBS = r'''
static void show_system(void) {}
static void restore_var(void) { restores++; }
static void test_files(void) {}
static void test_writable(void) {}
static void test_memory(void) {}
static void test_processes(void) { processes++; }
static void test_dynamic(void) {}
static void test_devices(void) {}
static void test_time(void) {}
static void bind_weights(void) {}
static void configure_inject(void) {}
static void configure_vnodes(void) {}
static void run_rc(const char *path) { assert(!strcmp(path, "/etc/rc")); rc_runs++; }
static int reboot(int flags) { assert(flags == RB_HALT); halts++; return -1; }
'''
SCENARIOS = r'''
int main(void) {
    const int errors[] = { EIO, ENXIO, ENOSPC, 0 };
    /* 1. darwin_var present and formatted as APFS: mounts APFS or halts on mount error */
    var_bytes = 104ULL << 30;
    var_apfs = 1;
    for (unsigned i = 0; i < sizeof(errors)/sizeof(errors[0]); i++) {
        mount_error = errors[i]; checks = failures = skeleton_reads = restores = processes = rc_runs = halts = 0;
        apfs_mounts = ramfs_mounts = cam_vol_unlinks = mg_cache_unlinks = 0;
        int result = startup();
        assert(halts == 1 && !raw_open);
        assert(apfs_mounts == 1 && ramfs_mounts == 0);
        if (mount_error) {
            assert(result == 1 && failures == 1);
            assert(!skeleton_reads && !restores && !processes && !rc_runs);
            assert(!cam_vol_unlinks && !mg_cache_unlinks);
        } else {
            assert(result == 0 && failures == 0);
            assert(skeleton_reads == 1 && restores == 1 && processes == 1 && rc_runs == 1);
            assert(cam_vol_unlinks == 2 && mg_cache_unlinks == 1);
        }
    }
    /* 2. darwin_var present and NOT APFS (legacy ANNFS001 or unformatted): refuses ramfs and halts */
    var_bytes = 104ULL << 30;
    var_apfs = 0;
    mount_error = 0; checks = failures = skeleton_reads = restores = processes = rc_runs = halts = 0;
    apfs_mounts = ramfs_mounts = 0;
    assert(startup() == 1 && halts == 1 && !raw_open);
    assert(apfs_mounts == 0 && ramfs_mounts == 0 && failures == 1);
    assert(!skeleton_reads && !restores && !processes && !rc_runs);

    /* 3. Memory-only boot without darwin_var or root_apfs: mounts pure in-memory ramfs */
    var_bytes = 0; var_apfs = 0; mount_error = 0;
    checks = failures = skeleton_reads = restores = processes = rc_runs = halts = 0;
    apfs_mounts = ramfs_mounts = cam_vol_unlinks = mg_cache_unlinks = 0;
    assert(startup() == 0 && halts == 1 && !raw_open);
    assert(ramfs_mounts == 1 && apfs_mounts == 0 && failures == 0);
    assert(skeleton_reads == 1 && restores == 1 && processes == 1 && rc_runs == 1);
    assert(cam_vol_unlinks == 2 && mg_cache_unlinks == 1);

    /* 4. Flash-root boot (root_apfs=) with missing darwin_var: refuses volatile ramfs and halts */
    kernel_bootargs = "debug=0x10e init_selftest=1 root_apfs=ufs_darwin";
    var_bytes = 0; var_apfs = 0; mount_error = 0;
    checks = failures = skeleton_reads = restores = processes = rc_runs = halts = 0;
    apfs_mounts = ramfs_mounts = 0;
    assert(startup() == 1 && halts == 1 && !raw_open);
    assert(apfs_mounts == 0 && ramfs_mounts == 0 && failures == 1);
    assert(!skeleton_reads && !restores && !processes && !rc_runs);

    puts("PASS: init requires APFS on persistent darwin_var, halts with migration notice on non-APFS, "
         "and mounts ramfs only on memory-only runs");
    return 0;
}
'''


def main():
    mounts = (ROOT / 'userland/src/boot/init_mount.inc').read_text()
    init = (ROOT / 'userland/src/boot/init.c').read_text()
    def function(text, head):
        start = text.index(head)
        return text[start:text.index('\n}\n', start) + 3]
    mount_function = (function(init, 'static const char *\nboot_arg(const char *name)')
                      + function(mounts, 'static int\nvar_is_apfs(void)')
                      + function(mounts, 'static int\nmount_writable(void)'))
    startup = init[init.index('\tshow_system();'):]
    source = PRELUDE + mount_function + STUBS + '\nstatic int startup(void) {\n' + startup + SCENARIOS
    compiler = shlex.split(os.environ.get('CC', 'clang'))
    mutations = {
        'mount failure falls through to seed': ('\tif (!ok) {\n\t\treturn 0;\n\t}', ''),
        'caller ignores mount failure': ('if (!mount_writable())', 'if ((mount_writable(), 0))'),
        'block 0 ignored': ('if (kind > 0) {', 'if ((void)kind, 0) {'),
        'non-apfs darwin_var falls back to ramfs': ('} else if (kind == 0) {', '} else if (1) {'),
    }
    with tempfile.TemporaryDirectory(prefix='init-data-mount-') as temporary:
        cfile, binary = Path(temporary) / 'test.c', Path(temporary) / 'test'
        for name, replacement in [('production', None), *mutations.items()]:
            content = source
            if replacement:
                before, after = replacement
                assert content.count(before) == 1, name
                content = content.replace(before, after)
            cfile.write_text(content)
            subprocess.run(compiler + ['-Wall', '-Wextra', '-Werror', '-Wno-unused-variable',
                                       str(cfile), '-o', str(binary)], check=True)
            result = subprocess.run([str(binary)], capture_output=True, text=True)
            if replacement:
                assert result.returncode != 0, name
                print('mutation rejected:', name)
            else:
                assert result.returncode == 0, result.stdout + result.stderr
                print(result.stdout, end='')


if __name__ == '__main__':
    main()
