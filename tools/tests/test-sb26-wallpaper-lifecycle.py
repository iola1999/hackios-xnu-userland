#!/usr/bin/env python3
"""Host unit test for inject_sb26.c CoverSheet wallpaper suspend/resume lifecycle."""
import os
import re
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
SRC = os.path.join(ROOT, "userland/src/inject/inject_sb26.c")


def extract_lifecycle_block(text: str) -> str:
    start = text.index("static int sb26_wallpaper_suspended = -1;")
    end = text.index("static void\nsb26_reset_idle_timer(const char *reason)")
    return text[start:end]


HARNESS = r"""
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef void *id;
typedef void *Class;
typedef const char *SEL;
typedef int BOOL;
typedef const char *CFStringRef;
typedef int CFStringEncoding;
#define YES 1
#define NO 0
#define nil NULL
#define NOTIFY_TOKEN_INVALID (-1)
#define NOTIFY_STATUS_OK 0
#define NSEC_PER_MSEC 1000000LL
#define DISPATCH_TIME_NOW 0
#define kCFAllocatorDefault NULL
#define kCFStringEncodingUTF8 0

static int g_is_main = 1;
static int g_cover_sheet_presented = 0;
static uint64_t g_notify_state = 99;
static int g_suspend_posts = 0;
static int g_resume_posts = 0;
static int g_observer_count = 0;
static char g_observer_names[8][64];
typedef void (^test_block_t)(void);
typedef void (^test_notif_block_t)(id);
static test_notif_block_t g_observer_blocks[8];

static test_block_t g_pending_timers[16];
static int g_pending_timer_count = 0;

static int pthread_main_np(void) { return g_is_main; }
static int notify_register_check(const char *name, int *out_token) {
    assert(strcmp(name, "com.annibale.wallpaper.suspend") == 0);
    *out_token = 42;
    return NOTIFY_STATUS_OK;
}
static int notify_set_state(int token, uint64_t state) {
    assert(token == 42);
    g_notify_state = state;
    return NOTIFY_STATUS_OK;
}
static int notify_post(const char *name) {
    if (strcmp(name, "com.annibale.wallpaper.suspend") == 0) g_suspend_posts++;
    else if (strcmp(name, "com.annibale.wallpaper.resume") == 0) g_resume_posts++;
    else assert(0 && "unexpected notification name");
    return NOTIFY_STATUS_OK;
}
static uint64_t dispatch_time(uint64_t when, int64_t delta) { return when + (uint64_t)delta; }
static void *dispatch_get_main_queue(void) { return (void *)1; }
static void dispatch_after(uint64_t when, void *q, test_block_t block) {
    (void)when; (void)q;
    assert(g_pending_timer_count < 16);
    g_pending_timers[g_pending_timer_count++] = Block_copy(block);
}
static void dispatch_async(void *q, test_block_t block) {
    (void)q;
    block();
}

static Class objc_getClass(const char *name) { return (Class)name; }
static SEL sel_registerName(const char *name) { return name; }
static void *class_getClassMethod(Class c, SEL s) { (void)c; (void)s; return (void *)1; }
static Class object_getClass(id o) { return (Class)o; }
static int class_respondsToSelector(Class c, SEL s) { (void)c; (void)s; return 1; }
static CFStringRef CFStringCreateWithCString(void *alloc, const char *s, CFStringEncoding enc) {
    (void)alloc; (void)enc; return s;
}
static void CFRelease(CFStringRef s) { (void)s; }

static id fake_msg_class(Class c, SEL s) {
    (void)s;
    return (id)c;
}
static BOOL fake_msg_bool(id o, SEL s) {
    (void)o;
    assert(strcmp(s, "isPresented") == 0);
    return g_cover_sheet_presented ? YES : NO;
}
static id fake_msg_observe(id center, SEL s, id name, id obj, id queue, test_notif_block_t block) {
    (void)center; (void)s; (void)obj; (void)queue;
    assert(g_observer_count < 8);
    strncpy(g_observer_names[g_observer_count], (const char *)name, 63);
    g_observer_blocks[g_observer_count] = Block_copy(block);
    g_observer_count++;
    return (id)1;
}

/* Intercept the three exact cast patterns via helper functions for clean host testing */
static void fire_all_timers(void) {
    int n = g_pending_timer_count;
    g_pending_timer_count = 0;
    for (int i = 0; i < n; i++) {
        g_pending_timers[i]();
        Block_release(g_pending_timers[i]);
    }
}

static void fire_notification(const char *name) {
    int fired = 0;
    for (int i = 0; i < g_observer_count; i++) {
        if (strcmp(g_observer_names[i], name) == 0) {
            g_observer_blocks[i](NULL);
            fired++;
        }
    }
    assert(fired == 1);
}
"""


def rewrite_msg_sends(code: str) -> str:
    code = code.replace(
        "((id (*)(Class, SEL))objc_msgSend)(cspm_cls, shared_sel)",
        "fake_msg_class(cspm_cls, shared_sel)",
    )
    code = code.replace(
        "((BOOL (*)(id, SEL))objc_msgSend)(cspm, presented_sel)",
        "fake_msg_bool(cspm, presented_sel)",
    )
    code = code.replace(
        "((id (*)(Class, SEL))objc_msgSend)(nc_cls, sel_registerName(\"defaultCenter\"))",
        "fake_msg_class(nc_cls, sel_registerName(\"defaultCenter\"))",
    )
    code = code.replace(
        "((id (*)(id, SEL, id, id, id, sb26_notif_block_t))objc_msgSend)",
        "fake_msg_observe",
    )
    return code


TESTS = r"""
int main(void) {
    sb26_install_wallpaper_lifecycle();
    assert(g_observer_count == 4);

    /* 1. Unlock schedules delayed suspend; does not suspend synchronously. */
    g_cover_sheet_presented = 0;
    sb26_on_unlocked();
    assert(g_suspend_posts == 0 && g_resume_posts == 0);
    assert(g_pending_timer_count == 1);

    /* 2. Settled home screen fires suspend and sets notify state = 1. */
    fire_all_timers();
    assert(g_suspend_posts == 1 && g_resume_posts == 0);
    assert(g_notify_state == 1);

    /* 3. Pulling down Notification Center resumes immediately and clears notify state = 0. */
    g_cover_sheet_presented = 1;
    fire_notification("SBCoverSheetWillPresentNotification");
    assert(g_resume_posts == 1 && g_suspend_posts == 1);
    assert(g_notify_state == 0);

    /* 4. Duplicate WillPresent is deduplicated. */
    fire_notification("SBLockScreenUIWillPresentNotification");
    assert(g_resume_posts == 1);

    /* 5. Dismissing Notification Center schedules suspend; if cancelled by re-present, no suspend fires. */
    g_cover_sheet_presented = 0;
    fire_notification("SBCoverSheetDidDismissNotification");
    assert(g_suspend_posts == 1 && g_pending_timer_count == 1);
    g_cover_sheet_presented = 1;
    fire_notification("SBCoverSheetWillPresentNotification");
    fire_all_timers();
    assert(g_suspend_posts == 1 && g_notify_state == 0);

    /* 6. Dismissing and letting timer settle suspends again. */
    g_cover_sheet_presented = 0;
    fire_notification("SBLockScreenUIDidDismissNotification");
    fire_all_timers();
    assert(g_suspend_posts == 2 && g_notify_state == 1);

    /* 7. If CoverSheet is presented when timer fires (without WillPresent), guard prevents suspend. */
    fire_notification("SBCoverSheetWillPresentNotification");
    assert(g_resume_posts == 2 && g_notify_state == 0);
    fire_notification("SBCoverSheetDidDismissNotification");
    g_cover_sheet_presented = 1;
    fire_all_timers();
    assert(g_suspend_posts == 2 && g_notify_state == 0);

    printf("PASS: inject_sb26 wallpaper lifecycle suspend/resume state machine verified\n");
    return 0;
}
"""


def main() -> int:
    text = open(SRC, "r", encoding="utf-8").read()
    block = rewrite_msg_sends(extract_lifecycle_block(text))
    assert "sb26_install_wallpaper_lifecycle();" in text, "install_unlock must call sb26_install_wallpaper_lifecycle()"

    src = "#include <Block.h>\n" + HARNESS + "\n" + block + "\n" + TESTS
    with tempfile.TemporaryDirectory() as td:
        cpath = os.path.join(td, "test_sb26_wp.c")
        bpath = os.path.join(td, "test_sb26_wp")
        with open(cpath, "w", encoding="utf-8") as f:
            f.write(src)
        subprocess.check_call([
            "clang", "-fblocks", "-Wall", "-Wextra", "-Werror",
            "-fsanitize=address,undefined", cpath, "-o", bpath,
        ])
        subprocess.check_call([bpath])
    return 0


if __name__ == "__main__":
    sys.exit(main())
