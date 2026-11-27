#!/usr/bin/env python3
"""Host ASan+UBSan test for the 3-slot still RAW ring pool and decoupled delivery queue."""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
SERVICES = ROOT / "userland/src/services"

HARNESS = r"""
#include <assert.h>
#include <Block.h>
#include <CoreFoundation/CoreFoundation.h>
#include <CoreMedia/CoreMedia.h>
#include <CoreVideo/CoreVideo.h>
#include <dispatch/dispatch.h>
#include <mach/mach_time.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "annibale_camera.h"

#define OUTPUTS 1
#define STILL_POOL_SLOTS 3
#define CAM_STREAM_ACTIVE 1
#define CAM_STREAM_STOPPED 2
static inline void say(const char *fmt, ...) { (void)fmt; }

typedef void (^pixel_buffer_handler)(CVPixelBufferRef, CMTime);

struct isp { int dummy; void *tuning; };
struct shot_env { float lux, cct; bool redeye, flash_lit, has_torch; int orient; };
struct output { const char *name; void *handler; uint64_t delivered; };
struct still_slot { uint8_t *buf; uint32_t cap; bool busy; };
struct stream {
    pthread_mutex_t lock;
    dispatch_queue_t still_queue;
    dispatch_queue_t still_deliver_queue;
    struct still_slot still_pool[STILL_POOL_SLOTS];
    struct output out[OUTPUTS];
    volatile int state;
    int torch;
    bool still_redeye_wanted, flash_lit;
    float last_lux, last_cct;
    uint64_t stills_given;
};

static atomic_uint renders_done;
static atomic_uint deliveries_done;
static atomic_uint slot_free_during_handler;
static atomic_uint slow_handler_us;
static uint8_t rendered_tags[16];
static uint8_t delivered_tags[16];

static int still_exif_orientation(struct stream *s) { (void)s; return 1; }
static CFDictionaryRef colour_attachments(void) {
    static CFDictionaryRef empty;
    if (!empty) empty = CFDictionaryCreate(NULL, NULL, NULL, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    return empty;
}
static void attach_metadata(struct stream *s, struct output *o, CVPixelBufferRef pb, CFDictionaryRef md,
    const struct isp *isp, const struct shot_env *env) {
    (void)s; (void)o; (void)pb; (void)md; (void)isp; (void)env;
}
static CVPixelBufferRef picture_buffer(struct output *o, uint32_t *pf, uint32_t *w, uint32_t *h) {
    (void)o;
    *pf = kCVPixelFormatType_32BGRA;
    *w = 16; *h = 16;
    CVPixelBufferRef pb = NULL;
    assert(CVPixelBufferCreate(NULL, 16, 16, *pf, NULL, &pb) == kCVReturnSuccess);
    return pb;
}
static void render_picture(struct output *o, CVPixelBufferRef pb, uint32_t pf, uint32_t w, uint32_t h,
    const struct isp *isp, const struct annibale_camera_frame *f, const uint8_t *raw, bool still) {
    (void)o; (void)pf; (void)w; (void)h; (void)isp; (void)still;
    assert(f->sequence < 16);
    rendered_tags[atomic_fetch_add(&renders_done, 1)] = raw[0];
    CVPixelBufferLockBaseAddress(pb, 0);
    ((uint8_t *)CVPixelBufferGetBaseAddress(pb))[0] = raw[0];
    CVPixelBufferUnlockBaseAddress(pb, 0);
}

#include "still-prod.inc"

int main(void) {
    struct stream s;
    memset(&s, 0, sizeof(s));
    pthread_mutex_init(&s.lock, NULL);
    s.state = CAM_STREAM_ACTIVE;
    s.torch = -1;
    s.out[0].name = "StillImage";
    dispatch_queue_attr_t attr = dispatch_queue_attr_make_with_qos_class(DISPATCH_QUEUE_SERIAL,
        QOS_CLASS_USER_INITIATED, 0);
    s.still_queue = dispatch_queue_create("test-still", attr);
    s.still_deliver_queue = dispatch_queue_create("test-still-deliver", attr);

    struct stream *sp = &s;
    pixel_buffer_handler h = Block_copy(^(CVPixelBufferRef pb, CMTime pts) {
        (void)pts;
        /* Verify at least one RAW slot is already free while handler runs (released before delivery). */
        pthread_mutex_lock(&sp->lock);
        bool any_free = false;
        for (int i = 0; i < STILL_POOL_SLOTS; i++) {
            if (!sp->still_pool[i].busy) any_free = true;
        }
        pthread_mutex_unlock(&sp->lock);
        if (any_free) atomic_fetch_add(&slot_free_during_handler, 1);

        uint32_t us = atomic_load(&slow_handler_us);
        if (us) usleep(us);
        CVPixelBufferLockBaseAddress(pb, kCVPixelBufferLock_ReadOnly);
        uint8_t tag = ((const uint8_t *)CVPixelBufferGetBaseAddress(pb))[0];
        CVPixelBufferUnlockBaseAddress(pb, kCVPixelBufferLock_ReadOnly);
        delivered_tags[atomic_fetch_add(&deliveries_done, 1)] = tag;
    });
    s.out[0].handler = (void *)h;

    const uint32_t frame_bytes = (uint32_t)sizeof(struct annibale_camera_frame) + 64u;
    uint8_t *frame = calloc(1, frame_bytes);
    assert(frame != NULL);
    struct isp isp = { 1, NULL };

    /* 1. Saturation check: 3 slots succeed, 4th returns NULL cleanly without leaking. */
    struct still_job *held[3];
    pthread_mutex_lock(&s.lock);
    for (int i = 0; i < 3; i++) {
        held[i] = still_prepare(&s, &s.out[0], &isp, frame_bytes, NULL);
        assert(held[i] != NULL && held[i]->slot_idx == i && s.still_pool[i].busy);
    }
    assert(still_prepare(&s, &s.out[0], &isp, frame_bytes, NULL) == NULL);
    pthread_mutex_unlock(&s.lock);
    for (int i = 0; i < 3; i++) {
        still_free(held[i]);
        assert(!s.still_pool[i].busy);
    }

    /* 2. Burst of 6 photos with slow delivery handler: RAW slots return before handler ends. */
    atomic_store(&slow_handler_us, 8000u);
    for (uint32_t seq = 0; seq < 6; seq++) {
        struct annibale_camera_frame *f = (struct annibale_camera_frame *)frame;
        f->header_bytes = sizeof(*f);
        f->sequence = seq;
        f->time = 1000u + seq;
        frame[sizeof(*f)] = (uint8_t)(0x40u + seq);

        struct still_job *j = NULL;
        for (int tries = 0; tries < 50 && j == NULL; tries++) {
            pthread_mutex_lock(&s.lock);
            j = still_prepare(&s, &s.out[0], &isp, frame_bytes, NULL);
            pthread_mutex_unlock(&s.lock);
            if (j == NULL) usleep(1000);
        }
        assert(j != NULL);
        /* Lock is released before still_dispatch() performs RAW memcpy. */
        assert(pthread_mutex_trylock(&s.lock) == 0);
        pthread_mutex_unlock(&s.lock);
        still_dispatch(&s, j, frame);
    }
    dispatch_sync(s.still_queue, ^{});
    dispatch_sync(s.still_deliver_queue, ^{});
    assert(atomic_load(&renders_done) == 6);
    assert(atomic_load(&deliveries_done) == 6);
    assert(atomic_load(&slot_free_during_handler) == 6);
    for (uint32_t i = 0; i < 6; i++) {
        assert(rendered_tags[i] == (uint8_t)(0x40u + i));
        assert(delivered_tags[i] == (uint8_t)(0x40u + i));
    }
    for (int i = 0; i < STILL_POOL_SLOTS; i++) {
        assert(!s.still_pool[i].busy);
    }

    /* 3. Stop-mid-flight: queued jobs drop and release all slots cleanly. */
    pthread_mutex_lock(&s.lock);
    struct still_job *j0 = still_prepare(&s, &s.out[0], &isp, frame_bytes, NULL);
    struct still_job *j1 = still_prepare(&s, &s.out[0], &isp, frame_bytes, NULL);
    s.state = CAM_STREAM_STOPPED;
    pthread_mutex_unlock(&s.lock);
    assert(j0 != NULL && j1 != NULL);
    still_dispatch(&s, j0, frame);
    still_dispatch(&s, j1, frame);
    dispatch_sync(s.still_queue, ^{});
    dispatch_sync(s.still_deliver_queue, ^{});
    assert(atomic_load(&deliveries_done) == 6);
    for (int i = 0; i < STILL_POOL_SLOTS; i++) {
        assert(!s.still_pool[i].busy);
        free(s.still_pool[i].buf);
    }
    Block_release(h);
    dispatch_release(s.still_queue);
    dispatch_release(s.still_deliver_queue);
    free(frame);
    puts("still RAW ring pool and decoupled delivery checks passed");
    return 0;
}
"""


class StillPoolTest(unittest.TestCase):
    def test_pool_burst_and_decoupled_delivery(self):
        stream_src = (SERVICES / "camera_device_stream.inc").read_text()
        start = stream_src.index("struct still_job {")
        end = stream_src.index('#include "camera_device_worker.inc"')
        prod = stream_src[start:end]

        with tempfile.TemporaryDirectory(prefix="still-pool-test-") as tmp:
            d = Path(tmp)
            (d / "still-prod.inc").write_text(prod)
            (d / "harness.c").write_text(HARNESS)
            binary = d / "still_pool_test"
            subprocess.run([
                "xcrun", "--sdk", "macosx", "clang", "-O1", "-Wall", "-Wextra", "-Werror",
                "-fblocks", "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
                "-I", str(SERVICES), "-I", str(d),
                "-framework", "CoreFoundation", "-framework", "CoreMedia", "-framework", "CoreVideo",
                str(d / "harness.c"), "-o", str(binary),
            ], check=True)
            res = subprocess.run([str(binary)], capture_output=True, text=True, timeout=10)
            self.assertEqual(res.returncode, 0, res.stdout + res.stderr)
            self.assertIn("passed", res.stdout)


if __name__ == "__main__":
    unittest.main(verbosity=2)
