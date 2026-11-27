#!/usr/bin/env python3
"""Host ASan+UBSan lifecycle and mutation tests for Mesa 0031 heap/cache changes."""

import argparse
import os
import re
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[4]
MESA = ROOT / "hackios-xnu-kernel/out/ios26-resume/mesa-arm64e/mesa-26.2.5"
BUILD = ROOT / "hackios-xnu-kernel/out/ios26-resume/mesa-arm64e/build"

HARNESS_C = r"""
#define _DARWIN_C_SOURCE
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "freedreno_drmif.h"
#include "freedreno_priv.h"

uint64_t os_page_size = 16384;

static time_t g_fake_sec = 100;
int test_clock_gettime(clockid_t clk_id, struct timespec *tp) {
    (void)clk_id;
    tp->tv_sec = g_fake_sec;
    tp->tv_nsec = 0;
    return 0;
}

int64_t os_time_get_nano(void) {
    return (int64_t)g_fake_sec * 1000000000LL;
}

void mesa_log(unsigned level, const char *tag, const char *fmt, ...) {
    (void)level; (void)tag; (void)fmt;
}

void _simple_mtx_plain_init_once(simple_mtx_t *mtx) {
    pthread_mutex_init((pthread_mutex_t *)&mtx->mtx, NULL);
}
void simple_mtx_init(simple_mtx_t *mtx, int type) {
    (void)type;
    pthread_mutex_init((pthread_mutex_t *)&mtx->mtx, NULL);
}
int mtx_lock(mtx_t *mtx) { return pthread_mutex_lock((pthread_mutex_t *)mtx) == 0 ? thrd_success : thrd_error; }
int mtx_trylock(mtx_t *mtx) { return pthread_mutex_trylock((pthread_mutex_t *)mtx) == 0 ? thrd_success : thrd_busy; }
int mtx_unlock(mtx_t *mtx) { return pthread_mutex_unlock((pthread_mutex_t *)mtx) == 0 ? thrd_success : thrd_error; }
void util_call_once_data_slow(once_flag *flag, util_call_once_data_func func, const void *data) {
    (void)flag;
    func(data);
}

struct hash_entry *_mesa_hash_table_insert(struct hash_table *ht, const void *key, void *data) {
    (void)ht; (void)key; (void)data;
    return NULL;
}
struct hash_entry *_mesa_hash_table_search(const struct hash_table *ht, const void *key) {
    (void)ht; (void)key;
    return NULL;
}
void _mesa_hash_table_remove_key(struct hash_table *ht, const void *key) {
    (void)ht; (void)key;
}

struct fd_fence *fd_fence_ref_locked(struct fd_fence *f) { f->refcnt++; return f; }
void fd_fence_del_locked(struct fd_fence *f) { f->refcnt--; }
void fd_fence_del(struct fd_fence *f) {
    simple_mtx_lock(&fence_lock);
    f->refcnt--;
    simple_mtx_unlock(&fence_lock);
}
void fd_fence_flush(struct fd_fence *f) { (void)f; }
int fd_fence_wait(struct fd_fence *f) { (void)f; return 0; }

int drmIoctl(int fd, unsigned long req, void *arg) { (void)fd; (void)req; (void)arg; return 0; }
int drmPrimeFDToHandle(int fd, int prime_fd, uint32_t *handle) { (void)fd; (void)prime_fd; *handle = 0; return -1; }
int drmPrimeHandleToFD(int fd, uint32_t handle, uint32_t flags, int *prime_fd) { (void)fd; (void)handle; (void)flags; *prime_fd = -1; return -1; }
int annibale_drm_surface(int fd, struct annibale_drm_surface *s) { (void)fd; (void)s; return -1; }

struct mock_bo {
    struct fd_bo base;
    uint32_t alloc_flags_seen;
    void *backing;
};

static uint32_t g_next_handle = 10;
static uint64_t g_next_iova = 0x10000000ull;
static uint32_t g_backend_new_calls = 0;
static uint32_t g_backend_last_flags = 0;
static uint32_t g_backend_last_size = 0;
static uint32_t g_backend_close_calls = 0;
static uint32_t g_map_calls = 0;
static uint32_t g_unmap_calls = 0;

void *annibale_drm_map(int fd, uint32_t handle, uint64_t size) {
    (void)fd; (void)handle;
    g_map_calls++;
    return calloc(1, (size_t)size);
}
void annibale_drm_unmap(int fd, uint32_t handle, void *address) {
    (void)fd; (void)handle;
    g_unmap_calls++;
    free(address);
}

static int mock_offset(struct fd_bo *bo, uint64_t *offset) { *offset = (uint64_t)bo->handle << 32; return 0; }
static int mock_cpu_prep(struct fd_bo *bo, struct fd_pipe *pipe, uint32_t op) { (void)bo; (void)pipe; (void)op; return 0; }
static int mock_madvise(struct fd_bo *bo, int willneed) { (void)bo; return willneed; }
static uint64_t mock_iova(struct fd_bo *bo) { return bo->iova; }
static void mock_set_name(struct fd_bo *bo, const char *fmt, va_list ap) { (void)bo; (void)fmt; (void)ap; }
static void mock_destroy(struct fd_bo *bo) { fd_bo_fini_common(bo); }

static const struct fd_bo_funcs mock_funcs = {
    .offset = mock_offset,
    .map = fd_bo_map_os_mmap,
    .cpu_prep = mock_cpu_prep,
    .madvise = mock_madvise,
    .iova = mock_iova,
    .set_name = mock_set_name,
    .destroy = mock_destroy,
};

static struct fd_bo *mock_bo_new(struct fd_device *dev, uint32_t size, uint32_t flags) {
    struct mock_bo *m = calloc(1, sizeof(*m));
    assert(m);
    g_backend_new_calls++;
    g_backend_last_flags = flags;
    g_backend_last_size = size;
    m->base.size = size;
    m->base.handle = g_next_handle++;
    m->base.iova = g_next_iova;
    g_next_iova += ((uint64_t)size + 4095ull) & ~4095ull;
    m->base.funcs = &mock_funcs;
    fd_bo_init_common(&m->base, dev);
    return &m->base;
}
static void mock_bo_close(struct fd_bo *bo) { (void)bo; g_backend_close_calls++; }
static const struct fd_device_funcs mock_dev_funcs = {
    .bo_new = mock_bo_new,
    .bo_close_handle = mock_bo_close,
};

static struct fd_device *make_test_device(void) {
    struct fd_device *dev = calloc(1, sizeof(*dev));
    assert(dev);
    dev->funcs = &mock_dev_funcs;
    dev->has_cached_coherent = false;
    fd_bo_cache_init(&dev->bo_cache, false, "bo");
    fd_bo_cache_init(&dev->ring_cache, true, "ring");
    dev->ring_heap = fd_bo_heap_new(dev, RING_FLAGS);
    dev->default_heap = fd_bo_heap_new(dev, 0);
    return dev;
}

static void destroy_test_device(struct fd_device *dev) {
    fd_bo_cache_cleanup(&dev->bo_cache, 0);
    fd_bo_cache_cleanup(&dev->ring_cache, 0);
    fd_bo_heap_destroy(dev->ring_heap);
    fd_bo_heap_destroy(dev->default_heap);
    free(dev);
}

static void reset_counters(void) {
    g_backend_new_calls = 0;
    g_backend_last_flags = 0;
    g_backend_last_size = 0;
    g_backend_close_calls = 0;
    g_map_calls = 0;
    g_unmap_calls = 0;
}

int main(void) {
    struct fd_device *dev = make_test_device();
    struct fd_pipe_control ctrl = { .fence = 0 };
    struct fd_pipe pipe = { .dev = dev, .refcnt = 10, .control = &ctrl };
    struct fd_fence f_busy = { .refcnt = 10, .pipe = &pipe, .ufence = 5 };
    struct fd_fence f_idle = { .refcnt = 10, .pipe = &pipe, .ufence = 1 };

    /* 1. ring_heap accepts 0x402 (tex_desc/samp_desc), 0x2 (zero_desc), 0x3 (bindless) when !has_cached_coherent */
    g_fake_sec = 100;
    reset_counters();
    struct fd_bo *t0 = fd_bo_new(dev, 256, FD_BO_GPUREADONLY | FD_BO_HINT_COMMAND, "tex_desc");
    assert(t0 && suballoc_bo(t0));
    assert(g_backend_new_calls == 1 && g_backend_last_size == FD_BO_HEAP_BLOCK_SIZE);
    assert(g_backend_last_flags == (FD_BO_GPUREADONLY | FD_BO_HINT_COMMAND | _FD_BO_HINT_HEAP));
    assert(g_map_calls == 1);
    struct fd_bo *t1 = fd_bo_new(dev, 64, FD_BO_GPUREADONLY | FD_BO_HINT_COMMAND, "samp_desc");
    struct fd_bo *t2 = fd_bo_new(dev, 4096, FD_BO_GPUREADONLY, "zero_desc");
    struct fd_bo *t3 = fd_bo_new(dev, 1024, FD_BO_GPUREADONLY | FD_BO_CACHED_COHERENT, "bindless");
    struct fd_bo *t4 = fd_bo_new(dev, 1024, RING_FLAGS, "ring");
    assert(t1 && suballoc_bo(t1) && t2 && suballoc_bo(t2) && t3 && suballoc_bo(t3) && t4 && suballoc_bo(t4));
    assert(g_backend_new_calls == 1 && g_map_calls == 1);
    fd_bo_del(t0); fd_bo_del(t1); fd_bo_del(t2); fd_bo_del(t3); fd_bo_del(t4);

    /* 2. default_heap accepts 0x4 (FD_BO_NOMAP, 256B aligned even after 64B alloc) while fd_bo_map()==NULL */
    reset_counters();
    struct fd_bo *pad = fd_bo_new(dev, 64, 0, "pad64");
    struct fd_bo *sh0 = fd_bo_new(dev, 768, FD_BO_NOMAP, "ir3");
    assert(pad && suballoc_bo(pad) && sh0 && suballoc_bo(sh0));
    assert(g_backend_new_calls == 1 && g_backend_last_size == FD_BO_HEAP_BLOCK_SIZE && g_map_calls == 1);
    assert((sh0->iova & 255ull) == 0);
    assert(fd_bo_map(sh0) == NULL);
    assert(sh0->prefaulted);
    uint8_t payload[64];
    memset(payload, 0x5a, sizeof(payload));
    fd_bo_upload(sh0, payload, 0, sizeof(payload));
    assert(g_map_calls == 1);
    struct fd_bo *sh1 = fd_bo_new(dev, 768, FD_BO_NOMAP, "lrz");
    assert(sh1 && suballoc_bo(sh1) && (sh1->iova & 255ull) == 0 && g_backend_new_calls == 1);
    fd_bo_del(pad); fd_bo_del(sh0); fd_bo_del(sh1);

    /* 3. FD_BO_NOMAP | FD_BO_HINT_IMAGE (0x204) and FD_BO_SHARED | FD_BO_HINT_BUFFER (0x110) stay out of heap */
    reset_counters();
    struct fd_bo *img = fd_bo_new(dev, 65536, FD_BO_NOMAP | FD_BO_HINT_IMAGE, "img");
    struct fd_bo *shbuf = fd_bo_new(dev, 16384, FD_BO_SHARED | FD_BO_HINT_BUFFER, "mtl");
    assert(img && !suballoc_bo(img) && shbuf && !suballoc_bo(shbuf));
    assert(g_backend_new_calls == 2);
    fd_bo_del(img);
    fd_bo_del(shbuf);

    /* 4. heap_clean skips busy sa_bo head so idle sa_bos return VMA space */
    ctrl.fence = 2; /* f_idle(1) is IDLE, f_busy(5) is BUSY */
    struct fd_bo *sa_busy = fd_bo_new(dev, 65536, FD_BO_NOMAP, "sa_busy");
    assert(sa_busy && suballoc_bo(sa_busy));
    simple_mtx_lock(&fence_lock);
    fd_bo_add_fence(sa_busy, &f_busy);
    simple_mtx_unlock(&fence_lock);
    fd_bo_del(sa_busy);
    struct fd_bo *fill[60];
    for (int i = 0; i < 60; i++) {
        fill[i] = fd_bo_new(dev, 65536, FD_BO_NOMAP, "fill");
        assert(fill[i] && suballoc_bo(fill[i]));
        simple_mtx_lock(&fence_lock);
        fd_bo_add_fence(fill[i], &f_idle);
        simple_mtx_unlock(&fence_lock);
    }
    for (int i = 0; i < 60; i++)
        fd_bo_del(fill[i]);
    uint32_t before_blocks = 0;
    for (int i = 0; i < 256; i++)
        if (dev->default_heap->blocks[i]) before_blocks++;
    struct fd_bo *again[32];
    for (int i = 0; i < 32; i++) {
        again[i] = fd_bo_new(dev, 65536, FD_BO_NOMAP, "again");
        assert(again[i] && suballoc_bo(again[i]));
    }
    uint32_t after_blocks = 0;
    for (int i = 0; i < 256; i++)
        if (dev->default_heap->blocks[i]) after_blocks++;
    assert(after_blocks == before_blocks);
    for (int i = 0; i < 32; i++)
        fd_bo_del(again[i]);

    /* 5. bo_cache stays at 0030 baseline (expires >1 s idle on next free cleanup; busy head stops find_in_bucket) */
    g_fake_sec = 200;
    fd_bo_cache_cleanup(&dev->bo_cache, 0);
    reset_counters();
    struct fd_bo *c0 = fd_bo_new(dev, 65536, FD_BO_NOMAP | FD_BO_HINT_IMAGE, "c0");
    fd_bo_del(c0);
    assert(g_backend_close_calls == 0);
    g_fake_sec = 202; /* >1 s in 0030 bo_cache; next fd_bo_del runs cleanup and expires c0 */
    struct fd_bo *trig = fd_bo_new(dev, 16384, FD_BO_NOMAP | FD_BO_HINT_IMAGE, "trig");
    fd_bo_del(trig);
    assert(g_backend_close_calls == 1);
    reset_counters();
    struct fd_bo *c1 = fd_bo_new(dev, 65536, FD_BO_NOMAP | FD_BO_HINT_IMAGE, "c1");
    assert(c1 != c0 && g_backend_new_calls == 1);
    fd_bo_del(c1);

    destroy_test_device(dev);
    printf("test-fd-bo-cache-reuse: ALL PASS\n");
    return 0;
}
"""


def run_build(mesa_dir: Path, build_dir: Path, overrides: dict[str, tuple[str, str]] | None = None) -> int:
    with tempfile.TemporaryDirectory() as td:
        tdir = Path(td)
        drm_copy = tdir / "drm"
        drm_copy.mkdir()
        for fn in ["freedreno_bo.c", "freedreno_bo_cache.c", "freedreno_bo_heap.c", "freedreno_priv.h", "freedreno_drmif.h"]:
            txt = (mesa_dir / "src/freedreno/drm" / fn).read_text()
            if overrides and fn in overrides:
                old, new = overrides[fn]
                assert old in txt, f"mutation target missing in {fn}"
                txt = txt.replace(old, new, 1)
            (drm_copy / fn).write_text(txt)
        (tdir / "harness.c").write_text(HARNESS_C)

        incs = [
            f"-I{drm_copy}",
            f"-I{build_dir}/src/freedreno/drm",
            f"-I{mesa_dir}/src/freedreno/drm",
            f"-I{build_dir}/src/freedreno",
            f"-I{mesa_dir}/src/freedreno",
            f"-I{build_dir}/src/freedreno/registers",
            f"-I{mesa_dir}/src/freedreno/registers",
            f"-I{build_dir}/src/freedreno/registers/adreno",
            f"-I{mesa_dir}/src/freedreno/registers/adreno",
            f"-I{build_dir}/src/freedreno/common",
            f"-I{mesa_dir}/src/freedreno/common",
            f"-I{build_dir}/include",
            f"-I{mesa_dir}/include",
            f"-I{build_dir}/src",
            f"-I{mesa_dir}/src",
            f"-I{mesa_dir}/src/annibale-drm",
            f"-I{build_dir}/src/util",
            f"-I{build_dir}/src/util/format",
        ]
        defs = ["-DHAVE_PTHREAD=1", "-DHAVE_STRUCT_TIMESPEC=1", "-DHAVE_TIMESPEC_GET=1", "-DHAVE_FREEDRENO", "-D__APPLE__"]
        cflags = ["-std=c11", "-Wall", "-Wextra", "-Werror", "-Wno-unused-parameter", "-Wno-missing-field-initializers", "-fsanitize=address,undefined", "-fno-sanitize=vla-bound", *defs, *incs]
        objs = []
        for fn, extra in [
            (drm_copy / "freedreno_bo_cache.c", ["-Dclock_gettime=test_clock_gettime"]),
            (drm_copy / "freedreno_bo_heap.c", []),
            (drm_copy / "freedreno_bo.c", []),
            (mesa_dir / "src/util/vma.c", []),
            (tdir / "harness.c", []),
        ]:
            obj = tdir / f"{fn.stem}.o"
            subprocess.run(["clang", "-c", *cflags, *extra, str(fn), "-o", str(obj)], check=True)
            objs.append(str(obj))
        bin_path = tdir / "test_bin"
        subprocess.run(["clang", "-fsanitize=address,undefined", *objs, "-lpthread", "-o", str(bin_path)], check=True)
        res = subprocess.run([str(bin_path)], capture_output=True, text=True)
        if not overrides:
            sys.stdout.write(res.stdout)
            if res.returncode != 0:
                sys.stderr.write(res.stderr)
        return res.returncode


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--mutations", action="store_true")
    args = ap.parse_args()
    rc = run_build(MESA, BUILD)
    if rc != 0:
        raise SystemExit(f"production check failed with rc={rc}")
    if args.mutations:
        mutations = {
            "ring_heap_skip_readonly": ("freedreno_bo.c", ("} else if ((alloc_flags == ring_alloc_flags) &&", "} else if (0 && (alloc_flags == ring_alloc_flags) &&")),
            "ring_demote_missing": ("freedreno_bo.c", ("if (!dev->has_cached_coherent)\n         ring_alloc_flags &= ~FD_BO_CACHED_COHERENT;", "")),
            "default_heap_skip_nomap": ("freedreno_bo.c", ("((alloc_flags == FD_BO_NOMAP) && !(flags & FD_BO_HINT_IMAGE))", "0")),
            "nomap_align_64": ("freedreno_bo_heap.c", ("(flags & FD_BO_NOMAP) ? MAX2(SUBALLOC_ALIGNMENT, 256u) : SUBALLOC_ALIGNMENT;", "SUBALLOC_ALIGNMENT;")),
            "heap_clean_break": ("freedreno_bo_heap.c", ("if (idle && !sa_idle(bo))\n         continue;", "if (idle && !sa_idle(bo))\n         break;")),
        }
        for name, (fn, repl) in mutations.items():
            mrc = run_build(MESA, BUILD, {fn: repl})
            assert mrc != 0, f"mutation {name} escaped"
        print(f"Mesa 0031 Slice B heap mutations detected: {len(mutations)}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
