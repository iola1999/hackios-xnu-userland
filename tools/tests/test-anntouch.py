#!/usr/bin/env python3
"""Run the actual anntouch CLI with a record-queue mock; no device access."""
import argparse
import os
from pathlib import Path
import re
import signal
import subprocess
import sys
import tempfile
import unittest

SOURCE = Path(__file__).resolve().parents[2] / "userland/src/apps/anntouch.c"
INCLUDE = SOURCE.parents[1] / "services"
ANNSH = SOURCE.with_name("annsh.c")

SPAWNER = r'''
#define main annsh_daemon_main
#include "annsh-under-test.c"
#undef main
int main(int argc, char **argv) {
    return argc > 1 ? do_spawn(argv + 1) : 2;
}
'''

HARNESS = r'''
#include <errno.h>
#include <fcntl.h>
#include <mach/mach_time.h>
#include <notify.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/qos.h>
#include <unistd.h>
#include "annibale_input.h"

static char faults[1024];
static char *next_fault;
static unsigned attempts, accepted, touching, pressed_button, notifications;
static uint64_t ticks;

static int mock_open(const char *path, int flags, ...) {
    if (strcmp(path, ANNIBALE_TOUCH_DEVICE) || flags != O_RDWR) abort();
    if (getenv("MOCK_OPEN_ERROR")) { errno = EACCES; return -1; }
    return 17;
}

static ssize_t mock_write(int fd, const void *data, size_t size) {
    if (fd != 17 || size != sizeof(struct annibale_touch)) abort();
    const struct annibale_touch *t = data;
    const char *fault = next_fault ? next_fault : "ok";
    if (strcmp(fault, "forever-eintr") != 0) {
        char *comma = next_fault ? strchr(next_fault, ',') : NULL;
        if (comma) *comma++ = 0;
        next_fault = comma;
    }
    attempts++;
    if (strcmp(fault, "forever-eintr") != 0 || attempts == 1) {
        printf("MOCK_WRITE %u %u %u %u %u %zu %s\n", attempts, t->type,
               t->finger, t->x, t->y, size, fault);
        fflush(stdout);
    }
    if (strcmp(fault, "forever-eintr") == 0) {
        usleep(1000);
        errno = EINTR;
        return -1;
    }
    if (strcmp(fault, "eintr") == 0) { errno = EINTR; return -1; }
    if (strcmp(fault, "error") == 0) { errno = EIO; return -1; }
    /* The real driver queues only after a complete record copy. */
    if (strcmp(fault, "short") == 0) { errno = EAGAIN; return size - 1; }
    if (strcmp(fault, "zero") == 0) { errno = EAGAIN; return 0; }
    if (strcmp(fault, "ok") != 0) abort();
    accepted++;
    if (t->type == ANNIBALE_TOUCH_DOWN) touching = 1;
    if (t->type == ANNIBALE_TOUCH_UP) touching = 0;
    if (t->type == ANNIBALE_BUTTON_DOWN) pressed_button = t->finger;
    if (t->type == ANNIBALE_BUTTON_UP) pressed_button = 0;
    return size;
}

static int mock_sleep(useconds_t microseconds) { ticks += (uint64_t)microseconds * 1000; return 0; }
static uint64_t mock_time(void) { return ticks; }
static kern_return_t mock_timebase(mach_timebase_info_t info) {
    info->numer = info->denom = 1;
    return KERN_SUCCESS;
}
static uint32_t mock_notify(const char *name) { (void)name; notifications++; return NOTIFY_STATUS_OK; }
static ssize_t mock_read(int fd, void *data, size_t size) { (void)fd; (void)data; (void)size; abort(); }

#define main anntouch_main
#define open mock_open
#define write mock_write
#define read mock_read
#define usleep mock_sleep
#define mach_absolute_time mock_time
#define mach_timebase_info mock_timebase
#define notify_post mock_notify
#include "anntouch-under-test.c"
#undef main

static void summary(void) {
    printf("MOCK_END attempts=%u accepted=%u touch=%u button=%u notifications=%u\n",
           attempts, accepted, touching, pressed_button, notifications);
}
int main(int argc, char **argv) {
    const char *setting = getenv("MOCK_WRITES");
    if (setting && *setting) {
        if (strlen(setting) >= sizeof(faults)) abort();
        strcpy(faults, setting);
        next_fault = faults;
    }
    atexit(summary);
    return anntouch_main(argc, argv);
}
'''


@unittest.skipUnless(sys.platform == "darwin", "requires the macOS C toolchain")
class AnntouchTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temporary = tempfile.TemporaryDirectory(prefix="anntouch-tests-")
        cls.addClassCleanup(cls.temporary.cleanup)
        directory = Path(cls.temporary.name)
        (directory / "anntouch-under-test.c").symlink_to(SOURCE.resolve())
        source = directory / "harness.c"
        source.write_text(HARNESS)
        cls.binary = directory / "anntouch-test"
        subprocess.run([
            "xcrun", "--sdk", "macosx", "clang", "-O1", "-Wall", "-Wextra", "-Werror",
            "-fsanitize=address,undefined", "-I", str(INCLUDE), str(source), "-o", str(cls.binary),
        ], check=True, capture_output=True, text=True, timeout=60)
        (directory / "annsh-under-test.c").symlink_to(ANNSH)
        source = directory / "spawner.c"
        source.write_text(SPAWNER)
        cls.spawner = directory / "annsh-spawn"
        subprocess.run([
            "xcrun", "--sdk", "macosx", "clang", "-O1", "-Wall", "-Wextra", "-Werror",
            str(source), "-o", str(cls.spawner),
        ], check=True, capture_output=True, text=True, timeout=60)

    def run_cli(self, steps, faults="", expected=0, **extra_env):
        result = subprocess.run(
            [str(self.binary), *steps.split()], capture_output=True, text=True,
            env=dict(os.environ, MOCK_WRITES=faults, **extra_env), timeout=5,
        )
        self.assertEqual(result.returncode, expected, result.stdout + result.stderr)
        self.assertNotIn("Sanitizer", result.stderr)
        rows = re.findall(r"^MOCK_WRITE (\d+) (\d+) (\d+) (\d+) (\d+) (\d+) (\S+)$", result.stdout, re.M)
        writes = [tuple(map(int, row[:6])) + (row[6],) for row in rows]
        match = re.search(r"MOCK_END attempts=(\d+) accepted=(\d+) touch=(\d+) button=(\d+) notifications=(\d+)", result.stdout)
        self.assertIsNotNone(match, result.stdout)
        return result, writes, tuple(map(int, match.groups()))

    def failed(self, steps, faults, expected_types, accepted, held=(0, 0)):
        result, writes, summary = self.run_cli(steps + " notify org.example.not-sent tap 0.1 0.1", faults, 1)
        self.assertEqual([row[1] for row in writes], expected_types, result.stdout)
        self.assertEqual(summary, (len(writes), accepted, *held, 0), result.stdout)
        self.assertNotIn("action 2", result.stdout)
        self.assertNotIn("all 3 actions done", result.stdout)
        return result, writes

    def test_successful_actions_release_and_continue(self):
        cases = [
            ("tap 0.2 0.3", [1, 3]),
            ("hold 0.2 0.3 10", [1, 3]),
            ("button home 80", [4, 5]),
            ("swipe 0.2 0.3 0.7 0.8 16", [1, 2, 2, 3]),
            ("drag 0.2 0.3 0.7 0.8 16 200 100", [1, 2, 2, 2, 2, 2, 3]),
            ("zigzag 0.5 0.2 0.8 8 2 8", [1, 2, 2, 2, 2, 3]),
        ]
        for steps, types in cases:
            with self.subTest(steps=steps):
                result, writes, summary = self.run_cli(steps + " tap 0.1 0.1")
                self.assertEqual([row[1] for row in writes], types + [1, 3])
                self.assertEqual(summary, (len(types) + 2, len(types) + 2, 0, 0, 0))
                self.assertIn("all 2 actions done", result.stdout)

    def test_switcher_candidate_queues_44_records(self):
        result, writes, summary = self.run_cli("drag 0.5 0.999 0.5 0.55 250 1000")
        self.assertEqual(summary, (44, 44, 0, 0, 0))
        self.assertEqual([row[1] for row in writes], [1] + [2] * 42 + [3])
        self.assertIn("action 1 drag end 44 events", result.stdout)

    def test_interrupted_down_retries_whole_record(self):
        result, writes, summary = self.run_cli("tap 0.2 0.3", "eintr,ok,ok")
        self.assertEqual(writes[0][1:6], writes[1][1:6])
        self.assertEqual(summary, (3, 2, 0, 0, 0))
        self.assertIn("action 1 tap end 2 events", result.stdout)

    def test_interrupted_up_retries_without_extra_release(self):
        _, writes, summary = self.run_cli("button home 80", "ok,eintr,ok")
        self.assertEqual([row[1] for row in writes], [4, 5, 5])
        self.assertEqual(writes[1][1:6], writes[2][1:6])
        self.assertEqual(summary, (3, 2, 0, 0, 0))

    def test_failed_down_stops_without_releasing_unowned_input(self):
        for steps, kind in [("tap 0.2 0.3", 1), ("hold 0.2 0.3 10", 1),
                            ("swipe 0.2 0.3 0.7 0.8 16", 1),
                            ("drag 0.2 0.3 0.7 0.8 16 200 100", 1),
                            ("zigzag 0.5 0.2 0.8 8 2 8", 1), ("button home 80", 4)]:
            with self.subTest(steps=steps):
                result, _ = self.failed(steps, "error", [kind], 0)
                self.assertNotIn("release queued", result.stdout)

    def test_failed_first_move_releases_at_accepted_down(self):
        result, writes = self.failed("swipe 0.2 0.3 0.7 0.8 16", "ok,error,ok", [1, 2, 3], 2)
        self.assertEqual(writes[2][2:6], writes[0][2:6])
        self.assertIn("release queued type 3 finger 0", result.stdout)

    def test_failed_later_move_releases_at_last_accepted_position(self):
        _, writes = self.failed("swipe 0.2 0.3 0.7 0.8 16", "ok,ok,error,ok", [1, 2, 2, 3], 3)
        self.assertEqual(writes[3][2:6], writes[1][2:6])
        self.assertNotEqual(writes[3][3:5], writes[2][3:5])

    def test_failed_touch_up_queues_only_required_release(self):
        _, writes = self.failed("tap 0.2 0.3", "ok,error,ok", [1, 3, 3], 2)
        self.assertEqual(writes[1][1:6], writes[2][1:6])

    def test_failed_button_up_releases_same_button(self):
        _, writes = self.failed("button volup 80", "ok,error,ok", [4, 5, 5], 2)
        self.assertEqual([row[2] for row in writes], [2, 2, 2])

    def test_release_failure_is_explicit_and_stops(self):
        for cleanup_fault in ["error", "zero", "short"]:
            with self.subTest(cleanup_fault=cleanup_fault):
                result, _ = self.failed("tap 0.2 0.3", "ok,error," + cleanup_fault, [1, 3, 3], 1, (1, 0))
                self.assertIn("release failed", result.stdout)
                self.assertIn("input may remain pressed", result.stdout)

    def test_zero_and_short_down_do_not_write_a_tail(self):
        for fault in ["zero", "short"]:
            with self.subTest(fault=fault):
                result, _ = self.failed("tap 0.2 0.3", fault, [1], 0)
                self.assertIn("errno 5 (Input/output error)", result.stdout)

    def test_short_move_releases_without_retrying_tail(self):
        result, writes = self.failed("swipe 0.2 0.3 0.7 0.8 16", "ok,short,ok", [1, 2, 3], 2)
        self.assertEqual(len({row[5] for row in writes}), 1)
        self.assertIn("errno 5 (Input/output error)", result.stdout)

    def test_release_interruption_retries_only_release(self):
        _, writes = self.failed("tap 0.2 0.3", "ok,error,eintr,ok", [1, 3, 3, 3], 2)
        self.assertEqual(writes[2][1:6], writes[3][1:6])

    def test_failed_second_action_does_not_release_first_action(self):
        result, writes, summary = self.run_cli("tap 0.2 0.3 button home 80 tap 0.1 0.1", "ok,ok,error", 1)
        self.assertEqual([row[1] for row in writes], [1, 3, 4])
        self.assertEqual(summary, (3, 2, 0, 0, 0))
        self.assertNotIn("action 3", result.stdout)
        self.assertNotIn("release queued", result.stdout)

    def test_continuous_eintr_can_be_terminated(self):
        with subprocess.Popen([str(self.binary), "tap", "0.2", "0.3"],
                              stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
                              env=dict(os.environ, MOCK_WRITES="forever-eintr")) as child:
            try:
                with self.assertRaises(subprocess.TimeoutExpired):
                    child.communicate(timeout=0.2)
                child.terminate()
                stdout, stderr = child.communicate(timeout=2)
                self.assertEqual(child.returncode, -signal.SIGTERM, stdout + stderr)
                self.assertIn("forever-eintr", stdout)
                self.assertNotIn("action 1 tap end", stdout)
            finally:
                if child.poll() is None:
                    child.kill()
                    child.wait(timeout=2)

    def test_open_failure_has_no_events(self):
        _, writes, summary = self.run_cli("tap 0.2 0.3", expected=1, MOCK_OPEN_ERROR="1")
        self.assertEqual(writes, [])
        self.assertEqual(summary, (0, 0, 0, 0, 0))

    def test_annsh_spawn_propagates_actual_cli_exit(self):
        for faults, expected in [("", 0), ("error", 1), ("ok,error,ok", 1)]:
            with self.subTest(faults=faults):
                result = subprocess.run(
                    [str(self.spawner), str(self.binary), "tap", "0.2", "0.3"],
                    capture_output=True, text=True, env=dict(os.environ, MOCK_WRITES=faults), timeout=5,
                )
                self.assertEqual(result.returncode, expected, result.stdout + result.stderr)
                self.assertIn("annsh 0| MOCK_END", result.stderr)
                self.assertNotIn("Sanitizer", result.stderr)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, default=SOURCE)
    args, remaining = parser.parse_known_args()
    SOURCE = args.source.resolve()
    unittest.main(argv=[sys.argv[0], *remaining])
