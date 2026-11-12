#!/usr/bin/env python3
"""Exercise the actual annsh process query with native and failed Mach/BSD calls."""
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

SOURCE = Path(__file__).resolve().parents[2] / "userland/src/apps/annsh.c"

NATIVE = r'''
#define main annsh_daemon_main
#include "annsh.c"
#undef main
int main(int argc, char **argv) {
    char word[32];
    if (argc == 2 && strcmp(argv[1], "self") == 0) {
        snprintf(word, sizeof(word), "%d", getpid());
        return do_process(word);
    }
    if (argc == 2 && strcmp(argv[1], "child") == 0) {
        int ready[2];
        if (pipe(ready)) return 2;
        pid_t child = fork();
        if (child < 0) return 2;
        if (child == 0) {
            close(ready[1]);
            char byte;
            (void)read(ready[0], &byte, 1);
            _exit(0);
        }
        close(ready[0]);
        snprintf(word, sizeof(word), "%d", child);
        int status = do_process(word);
        close(ready[1]);
        waitpid(child, NULL, 0);
        return status;
    }
    if (argc == 2 && strcmp(argv[1], "exited") == 0) {
        pid_t child = fork();
        if (child < 0) return 2;
        if (child == 0) _exit(0);
        waitpid(child, NULL, 0);
        snprintf(word, sizeof(word), "%d", child);
        return do_process(word);
    }
    return argc == 2 ? do_process(argv[1]) : 2;
}
'''

FAULTS = r'''
#include <errno.h>
#include <mach/mach.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/sysctl.h>
static const char *scenario;
static int snapshots, audits, released;
static int is(const char *name) { return strcmp(scenario, name) == 0; }
static int mock_sysctl(int *mib, unsigned n, void *data, size_t *size, void *value, size_t len) {
    (void)mib; (void)n; (void)value; (void)len;
    snapshots++;
    if (is("missing") || (is("exit") && snapshots == 2)) { *size = 0; return 0; }
    if (is("sysctl-error")) { errno = EPERM; return -1; }
    struct kinfo_proc *p = data;
    memset(p, 0, sizeof(*p));
    p->kp_proc.p_starttime.tv_sec = is("reuse") ? snapshots : 100;
    *size = sizeof(*p);
    return 0;
}
static int mock_path(int pid, void *buffer, uint32_t size) {
    (void)pid;
    if (is("path-error")) { errno = EPERM; return 0; }
    if (is("path-full")) { memset(buffer, 'x', size); return size; }
    const char *path = is("stat-error") ? "/annsh-test-missing/executable" : "/bin/ls";
    return (int)strlcpy(buffer, path, size);
}
static kern_return_t mock_name(mach_port_t self, int pid, mach_port_t *port) {
    (void)self; (void)pid;
    if (is("name-error")) return KERN_FAILURE;
    *port = 42;
    return KERN_SUCCESS;
}
static kern_return_t mock_info(mach_port_t port, task_flavor_t flavor, task_info_t result,
                              mach_msg_type_number_t *count) {
    (void)port;
    if (flavor == TASK_AUDIT_TOKEN) {
        audits++;
        if (is("audit-error") || (is("audit-end-error") && audits == 2)) return KERN_FAILURE;
        audit_token_t *token = (audit_token_t *)result;
        memset(token, 0, sizeof(*token));
        token->val[7] = is("audit-change") ? audits : 1;
        if (is("audit-short") || (is("audit-end-short") && audits == 2)) --*count;
    } else if (flavor == MACH_TASK_BASIC_INFO) {
        if (is("info-error")) return KERN_FAILURE;
        mach_task_basic_info_t info = (mach_task_basic_info_t)result;
        memset(info, 0, sizeof(*info));
        info->suspend_count = 2;
        info->resident_size = 4096;
        if (is("info-short")) --*count;
    } else abort();
    return KERN_SUCCESS;
}
static kern_return_t mock_release(mach_port_t self, mach_port_t port) {
    (void)self;
    if (port != 42) abort();
    released++;
    return is("release-error") ? KERN_FAILURE : KERN_SUCCESS;
}
#define sysctl mock_sysctl
#define proc_pidpath mock_path
#define task_name_for_pid mock_name
#define task_info mock_info
#define mach_port_deallocate mock_release
#define main annsh_daemon_main
#include "annsh.c"
#undef main
int main(int argc, char **argv) {
    if (argc != 2) return 2;
    scenario = argv[1];
    int status = do_process("42");
    printf("released=%d\n", released);
    return status;
}
'''


@unittest.skipUnless(sys.platform == "darwin", "requires Darwin Mach and BSD headers")
class ProcessQueryTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory(prefix="annsh-process-")
        cls.addClassCleanup(cls.temp.cleanup)
        cls.directory = Path(cls.temp.name)
        for name, source in (("native", NATIVE), ("faults", FAULTS)):
            path = cls.directory / f"{name}.c"
            path.write_text(source)
            subprocess.run(["xcrun", "--sdk", "macosx", "clang", "-O1", "-Wall", "-Wextra", "-Werror",
                            "-I", str(SOURCE.parent), str(path), "-o", str(cls.directory / name)],
                           check=True, capture_output=True, text=True, timeout=60)

    def query(self, executable, argument):
        return subprocess.run([str(self.directory / executable), argument],
                              capture_output=True, text=True, timeout=10)

    def test_native_self_and_child(self):
        for target in ("self", "child"):
            with self.subTest(target=target):
                result = self.query("native", target)
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertIn("snapshot complete", result.stderr)
                self.assertIn("task user_stop_count 0", result.stderr)
                self.assertIn("process: audit ", result.stderr)
                self.assertIn("path_stat dev ", result.stderr)

    def test_native_exited_process(self):
        result = self.query("native", "exited")
        self.assertEqual(result.returncode, 1, result.stderr)
        self.assertIn("unavailable", result.stderr)
        self.assertNotIn("task user_stop_count", result.stderr)

    def test_invalid_pid_does_not_query_process(self):
        for word in ("", "0", "-1", "2147483648", "999999999999999999999", "12suffix"):
            with self.subTest(word=word):
                result = self.query("native", word)
                self.assertEqual(result.returncode, 1, result.stderr)
                self.assertIn("invalid pid", result.stderr)
                self.assertNotIn("process: audit", result.stderr)

    def test_reports_nonzero_suspension_count_and_releases_port(self):
        result = self.query("faults", "complete")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("user_stop_count 2 resident_bytes 4096", result.stderr)
        self.assertIn("snapshot complete", result.stderr)
        self.assertEqual(result.stdout, "released=1\n")

    def test_unavailable_initial_identity_stops_query(self):
        for scenario in ("missing", "sysctl-error"):
            with self.subTest(scenario=scenario):
                result = self.query("faults", scenario)
                self.assertEqual(result.returncode, 1)
                self.assertIn("unavailable", result.stderr)
                self.assertNotIn("process: path", result.stderr)
                self.assertEqual(result.stdout, "released=0\n")

    def test_failed_queries_are_incomplete_and_release_acquired_port(self):
        for scenario in ("name-error", "audit-error", "audit-end-error", "audit-short", "audit-end-short", "info-error",
                         "info-short", "path-error", "path-full", "stat-error", "release-error"):
            with self.subTest(scenario=scenario):
                result = self.query("faults", scenario)
                self.assertEqual(result.returncode, 1, result.stderr)
                self.assertIn("snapshot incomplete", result.stderr)
                self.assertEqual(result.stdout, f"released={0 if scenario == 'name-error' else 1}\n")
                if scenario in ("name-error", "info-error", "info-short"):
                    self.assertNotIn("task user_stop_count", result.stderr)

    def test_audit_or_start_time_change_or_exit_invalidates_snapshot(self):
        for scenario in ("reuse", "audit-change", "exit"):
            with self.subTest(scenario=scenario):
                result = self.query("faults", scenario)
                self.assertEqual(result.returncode, 1, result.stderr)
                self.assertIn("snapshot incomplete", result.stderr)
                self.assertIn("changed", result.stderr)
                self.assertEqual(result.stdout, "released=1\n")


if __name__ == "__main__":
    unittest.main()
