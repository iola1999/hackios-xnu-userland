#!/usr/bin/env python3
"""Host ASan+UBSan test for annsh bg built-in argument parsing, non-blocking spawn, and reaping."""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
ANNSH = ROOT / "userland/src/apps/annsh.c"

HARNESS = r"""
#include <assert.h>
#include <errno.h>
#include <signal.h>
#include <spawn.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;
static char last_out[256];

static void out(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(last_out, sizeof(last_out), fmt, ap);
    va_end(ap);
}

static uint64_t now_ms(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (uint64_t)tv.tv_sec * 1000u + (uint64_t)tv.tv_usec / 1000u;
}

#include "bg-prod.inc"

static int dispatch_bg(char *line)
{
    char *argv[32];
    int argc = 0;
    for (char *w = strtok(line, " \t"); w != NULL && argc < 31; w = strtok(NULL, " \t"))
        argv[argc++] = w;
    argv[argc] = NULL;
    if (argc < 2) {
        out("bg: usage: bg PROGRAM [ARG...]");
        return 2;
    }
    return do_bg(argv + 1);
}

int main(void)
{
    /* 1. Missing program argument returns 2 with usage message. */
    char c1[] = "bg";
    assert(dispatch_bg(c1) == 2);
    assert(strstr(last_out, "usage:") != NULL);

    /* 2. Missing executable returns 127 without leaking a slot. */
    char c2[] = "bg /nonexistent_annsh_bg_binary";
    assert(dispatch_bg(c2) == 127);
    assert(bg_count == 0);

    /* 3. Background sleep returns immediately (<250 ms) with bg pid=PID. */
    char c3[] = "bg /bin/sleep 2";
    uint64_t t0 = now_ms();
    assert(dispatch_bg(c3) == 0);
    uint64_t dt = now_ms() - t0;
    assert(dt < 250);
    assert(strncmp(last_out, "bg pid=", 7) == 0);
    pid_t sleep_pid = (pid_t)atoi(last_out + 7);
    assert(sleep_pid > 0 && bg_count == 1 && bg_pids[0] == sleep_pid);

    /* 4. Short-lived background process is reaped cleanly by reap_bg(), while sleep_pid stays tracked. */
    char c4[] = "bg /usr/bin/true";
    assert(dispatch_bg(c4) == 0);
    assert(bg_count == 2);
    for (int i = 0; i < 50 && bg_count > 1; i++) {
        usleep(10000);
        reap_bg();
    }
    assert(bg_count == 1 && bg_pids[0] == sleep_pid);

    /* 5. Clean up sleep_pid and verify reap_bg() drains bg_count to 0. */
    kill(sleep_pid, SIGTERM);
    for (int i = 0; i < 50 && bg_count > 0; i++) {
        usleep(10000);
        reap_bg();
    }
    assert(bg_count == 0);
    puts("annsh bg built-in checks passed");
    return 0;
}
"""


class AnnshBgTest(unittest.TestCase):
    def test_bg_builtin(self):
        src = ANNSH.read_text()
        start = src.index("#define ANNSH_MAX_BG")
        end = src.index("static int\nrun(char *text)")
        prod = src[start:end]

        with tempfile.TemporaryDirectory(prefix="annsh-bg-test-") as tmp:
            d = Path(tmp)
            (d / "bg-prod.inc").write_text(prod)
            (d / "harness.c").write_text(HARNESS)
            binary = d / "annsh_bg_test"
            subprocess.run([
                "xcrun", "--sdk", "macosx", "clang", "-O1", "-Wall", "-Wextra", "-Werror",
                "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
                "-I", str(d), str(d / "harness.c"), "-o", str(binary),
            ], check=True)
            res = subprocess.run([str(binary)], capture_output=True, text=True, timeout=10)
            self.assertEqual(res.returncode, 0, res.stdout + res.stderr)
            self.assertIn("passed", res.stdout)


if __name__ == "__main__":
    unittest.main(verbosity=2)
