#!/usr/bin/env python3
"""Exercise init's actual data-volume mount and subsequent startup control flow.

Host stubs record filesystem/process calls; no volume is mounted and no child is
started. This checks mount-error propagation, not the kernel's persistence policy.
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
static int unexpected(void) { assert(!"unexpected host filesystem/process operation"); return -1; }
static int fake_mount(const char *type, const char *path, int flags, void *data) {
    assert(!strcmp(type, "ramfs") && !strcmp(path, "/private/var") && !flags && !data);
    errno = mount_error;
    return mount_error ? -1 : 0;
}
static int fake_lstat(const char *path, struct stat *st) {
    (void)path; memset(st, 0, sizeof(*st)); st->st_mode = S_IFDIR; return 0;
}
static int no_package(const char *path, struct stat *st) { (void)path; (void)st; return -1; }
static int fake_chmod(const char *path, int mode) {
    assert(!strcmp(path, "/dev/console") && mode == 0666); return 0;
}
static void check(int ok, const char *what) { (void)what; checks++; failures += !ok; }
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
#define mount fake_mount
#define lstat fake_lstat
#define stat(p, s) no_package(p, s)
#define chmod fake_chmod
#define mkdir(...) unexpected()
#define chown(...) unexpected()
#define copy_file(...) unexpected()
#define open(...) unexpected()
#define close(...) unexpected()
#define symlink(...) unexpected()
#define unlink(...) unexpected()
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
static void configure_inject(void) {}
static void configure_vnodes(void) {}
static void run_rc(const char *path) { assert(!strcmp(path, "/etc/rc")); rc_runs++; }
static int reboot(int flags) { assert(flags == RB_HALT); halts++; return -1; }
'''
SCENARIOS = r'''
int main(void) {
    const int errors[] = { EIO, ENXIO, ENOSPC, 0 };
    for (unsigned i = 0; i < sizeof(errors)/sizeof(errors[0]); i++) {
        mount_error = errors[i]; checks = failures = skeleton_reads = restores = processes = rc_runs = halts = 0;
        int result = startup();
        assert(halts == 1);
        if (mount_error) {
            assert(result == 1 && failures == 1);
            assert(!skeleton_reads && !restores && !processes && !rc_runs);
        } else {
            assert(result == 0 && failures == 0);
            assert(skeleton_reads == 1 && restores == 1 && processes == 1 && rc_runs == 1);
        }
    }
    puts("PASS: init data mount failures stop seed/processes/rc; successful mount continues");
    return 0;
}
'''


def main():
    mounts = (ROOT / 'userland/src/boot/init_mount.inc').read_text()
    init = (ROOT / 'userland/src/boot/init.c').read_text()
    mount_function = mounts[mounts.index('static int\nmount_writable(void)'):mounts.index('\n/*\n * What the last boot')]
    startup = init[init.index('\tshow_system();'):]
    source = PRELUDE + mount_function + STUBS + '\nstatic int startup(void) {\n' + startup + SCENARIOS
    compiler = shlex.split(os.environ.get('CC', 'clang'))
    mutations = {
        'mount failure falls through to seed': ('\tif (!ok) {\n\t\treturn 0;\n\t}', ''),
        'caller ignores mount failure': ('if (!mount_writable())', 'if ((mount_writable(), 0))'),
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
