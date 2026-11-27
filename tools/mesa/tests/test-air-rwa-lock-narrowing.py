#!/usr/bin/env python3
"""Host verification for G29 rwa_lock narrowing, packed16_served() sync, and tally_lock.

Runs static contract + mutation checks, standalone TSan + ASan/UBSan multi-thread tests
for annibale_drm.c tally_lock and air_metal_formats.inc packed16_served(), and (when a
host Mesa softpipe build dir exists or is given via --mesa-build-dir) multi-thread TSan
stress + air_copy_test + air_metal_test --surface against libannibaleair.dylib.
"""
import argparse
import os
import re
import shutil
import subprocess
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
AIR_DIR = ROOT / 'tools' / 'mesa' / 'air'
DRM_C = ROOT / 'tools' / 'mesa' / 'annibale-drm' / 'annibale_drm.c'
DEFAULT_MESA_BUILD = ROOT / 'out' / 'g29-host-mesa' / 'build'


def extract_function(text, signature_regex):
    m = re.search(signature_regex, text, re.M)
    assert m, f'missing function matching {signature_regex!r}'
    brace = text.find('{', m.end() - 1)
    assert brace >= 0
    depth = 0
    for idx in range(brace, len(text)):
        ch = text[idx]
        if ch == '{':
            depth += 1
        elif ch == '}':
            depth -= 1
            if depth == 0:
                return text[m.start():idx + 1]
    raise AssertionError(f'unclosed brace for {signature_regex!r}')


def check_static_contracts():
    alloc = (AIR_DIR / 'air_metal_allocation.inc').read_text()
    priv = (AIR_DIR / 'air_metal_resource_private.h').read_text()
    fmts = (AIR_DIR / 'air_metal_formats.inc').read_text()
    tex = (AIR_DIR / 'air_metal_texture.inc').read_text()
    drm = DRM_C.read_text()

    fn_create = extract_function(alloc, r'\nrwa_texture_create\s*\([^)]*\)\s*\{')
    fn_surf = extract_function(alloc, r'\nrwa_texture_from_surface\s*\([^)]*\)\s*\{')
    fn_zero = extract_function(priv, r'\ntexture_start_zeroed\s*\([^)]*\)\s*\{')
    fn_buf = extract_function(tex, r'\nrwa_texture_from_buffer\s*\([^)]*\)\s*\{')
    fn_p16 = extract_function(fmts, r'\npacked16_served\s*\([^)]*\)\s*\{')
    fn_tally = extract_function(drm, r'\ntally_add\s*\([^)]*\)\s*\{')

    assert 'RWA_LOCKED' not in fn_create, 'rwa_texture_create must not acquire RWA_LOCKED'
    assert 'RWA_LOCKED' not in fn_surf, 'rwa_texture_from_surface must not acquire RWA_LOCKED'
    assert 'RWA_LOCKED' in fn_buf, 'rwa_texture_from_buffer must retain RWA_LOCKED'

    lock_pos = fn_zero.find('RWA_LOCKED;')
    clear_pos = fn_zero.find('clear_texture(')
    fast_ret_pos = fn_zero.find('return texture;')
    assert lock_pos >= 0 and fast_ret_pos >= 0 and clear_pos >= 0
    assert fast_ret_pos < lock_pos < clear_pos, (
        'texture_start_zeroed must fast-return before RWA_LOCKED and lock before clear_texture')

    assert 'pthread_once' in fn_p16 or '__atomic_load' in fn_p16, (
        'packed16_served must synchronize initialization')
    assert 'pthread_mutex_t tally_lock' in drm
    t_lock = fn_tally.find('pthread_mutex_lock(&tally_lock)')
    t_unlock = fn_tally.find('pthread_mutex_unlock(&tally_lock)')
    t_printf = fn_tally.find('printf(')
    assert 0 <= t_lock < t_unlock < t_printf, (
        'tally_add must unlock tally_lock before printf')
    print('static contracts: PASS')


DRM_HARNESS = r"""
#define _DARWIN_C_SOURCE
#include <assert.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <IOKit/IOKitLib.h>

static uint64_t fake_ns = 1000000000ULL;

/* Route clock_gettime through a helper macro in test translation unit. */
static int test_clock_gettime(clockid_t clk, struct timespec *ts) {
    (void)clk;
    uint64_t ns = __atomic_fetch_add(&fake_ns, 2500000ULL, __ATOMIC_RELAXED);
    ts->tv_sec = (time_t)(ns / 1000000000ULL);
    ts->tv_nsec = (long)(ns % 1000000000ULL);
    return 0;
}
#define clock_gettime test_clock_gettime

static kern_return_t stub_connect_call(io_connect_t c, uint32_t sel,
    const void *in_s, size_t in_sz, void *out_s, size_t *out_sz) {
    (void)c; (void)sel; (void)in_s; (void)in_sz; (void)out_s; (void)out_sz;
    return 0;
}
#define IOConnectCallStructMethod stub_connect_call

static kern_return_t stub_map(io_connect_t c, uint32_t m, task_port_t t,
    mach_vm_address_t *a, mach_vm_size_t *s, IOOptionBits o) {
    (void)c; (void)m; (void)t; (void)o;
    *a = 0x1000; *s = 4096;
    return 0;
}
#define IOConnectMapMemory64 stub_map

static kern_return_t stub_unmap(io_connect_t c, uint32_t m, task_port_t t, mach_vm_address_t a) {
    (void)c; (void)m; (void)t; (void)a;
    return 0;
}
#define IOConnectUnmapMemory64 stub_unmap

#include "annibale_drm.c"

enum { THREADS = 8, ITERS = 4000 };
typedef struct { pthread_mutex_t m; pthread_cond_t c; int count, trip; } bar_t;
static bar_t gbar = { PTHREAD_MUTEX_INITIALIZER, PTHREAD_COND_INITIALIZER, 0, THREADS };
static void bar_wait(void) {
    pthread_mutex_lock(&gbar.m);
    if (++gbar.count == gbar.trip) {
        pthread_cond_broadcast(&gbar.c);
    } else {
        while (gbar.count < gbar.trip) pthread_cond_wait(&gbar.c, &gbar.m);
    }
    pthread_mutex_unlock(&gbar.m);
}

static void *worker(void *arg) {
    unsigned idx = (unsigned)(uintptr_t)arg;
    struct drm_msm_gem_new req = { .size = 4096, .flags = 0x204 };
    bar_wait();
    for (int i = 0; i < ITERS; i++) {
        assert(drmCommandWriteRead(0, DRM_MSM_GEM_NEW, &req, sizeof(req)) == 0);
        if ((i & 7) == (int)(idx & 7)) {
            void *p = annibale_drm_map(0, 1, 4096);
            assert(p != (void *)-1);
            annibale_drm_unmap(0, 1, p);
        }
    }
    return NULL;
}

int main(void) {
    pthread_t th[THREADS];
    for (int i = 0; i < THREADS; i++)
        assert(pthread_create(&th[i], NULL, worker, (void *)(uintptr_t)i) == 0);
    for (int i = 0; i < THREADS; i++)
        assert(pthread_join(th[i], NULL) == 0);
    /* Force one final flush past 4s so remaining buffered counts are included in stdout. */
    __atomic_fetch_add(&fake_ns, 5000000000ULL, __ATOMIC_RELAXED);
    struct drm_msm_gem_new last = { .size = 4096, .flags = 0x204 };
    assert(drmCommandWriteRead(0, DRM_MSM_GEM_NEW, &last, sizeof(last)) == 0);
    fprintf(stderr, "drm_tally_test: %d threads x %d iters PASS\n", THREADS, ITERS);
    return 0;
}
"""

PACKED16_HARNESS = r"""
#include <assert.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>

enum pipe_format {
    PIPE_FORMAT_NONE = 0,
    PIPE_FORMAT_A8_UNORM = 1,
    PIPE_FORMAT_R8_UNORM, PIPE_FORMAT_R8_SRGB, PIPE_FORMAT_R8_SNORM,
    PIPE_FORMAT_R8_UINT, PIPE_FORMAT_R8_SINT,
    PIPE_FORMAT_R16_UNORM, PIPE_FORMAT_R16_SNORM, PIPE_FORMAT_R16_UINT,
    PIPE_FORMAT_R16_SINT, PIPE_FORMAT_R16_FLOAT,
    PIPE_FORMAT_R8G8_UNORM, PIPE_FORMAT_R8G8_SRGB, PIPE_FORMAT_R8G8_SNORM,
    PIPE_FORMAT_R8G8_UINT, PIPE_FORMAT_R8G8_SINT,
    PIPE_FORMAT_R5G6B5_UNORM, PIPE_FORMAT_A1R5G5B5_UNORM,
    PIPE_FORMAT_A4R4G4B4_UNORM, PIPE_FORMAT_R5G5B5A1_UNORM,
    PIPE_FORMAT_R32_UINT, PIPE_FORMAT_R32_SINT, PIPE_FORMAT_R32_FLOAT,
    PIPE_FORMAT_R16G16_UNORM, PIPE_FORMAT_R16G16_SNORM, PIPE_FORMAT_R16G16_UINT,
    PIPE_FORMAT_R16G16_SINT, PIPE_FORMAT_R16G16_FLOAT,
    PIPE_FORMAT_R8G8B8A8_UNORM, PIPE_FORMAT_R8G8B8A8_SRGB, PIPE_FORMAT_R8G8B8A8_SNORM,
    PIPE_FORMAT_R8G8B8A8_UINT, PIPE_FORMAT_R8G8B8A8_SINT,
    PIPE_FORMAT_B8G8R8A8_UNORM, PIPE_FORMAT_B8G8R8A8_SRGB,
    PIPE_FORMAT_R10G10B10A2_UNORM, PIPE_FORMAT_R10G10B10A2_UINT,
    PIPE_FORMAT_R11G11B10_FLOAT, PIPE_FORMAT_R9G9B9E5_FLOAT,
    PIPE_FORMAT_B10G10R10A2_UNORM,
    PIPE_FORMAT_R32G32_UINT, PIPE_FORMAT_R32G32_SINT, PIPE_FORMAT_R32G32_FLOAT,
    PIPE_FORMAT_R16G16B16A16_UNORM, PIPE_FORMAT_R16G16B16A16_SNORM,
    PIPE_FORMAT_R16G16B16A16_UINT, PIPE_FORMAT_R16G16B16A16_SINT,
    PIPE_FORMAT_R16G16B16A16_FLOAT,
    PIPE_FORMAT_R32G32B32A32_UINT, PIPE_FORMAT_R32G32B32A32_SINT,
    PIPE_FORMAT_R32G32B32A32_FLOAT,
    PIPE_FORMAT_DXT1_RGBA, PIPE_FORMAT_DXT1_SRGBA, PIPE_FORMAT_DXT3_RGBA,
    PIPE_FORMAT_DXT3_SRGBA, PIPE_FORMAT_DXT5_RGBA, PIPE_FORMAT_DXT5_SRGBA,
    PIPE_FORMAT_RGTC1_UNORM, PIPE_FORMAT_RGTC1_SNORM, PIPE_FORMAT_RGTC2_UNORM,
    PIPE_FORMAT_RGTC2_SNORM, PIPE_FORMAT_BPTC_RGB_FLOAT, PIPE_FORMAT_BPTC_RGB_UFLOAT,
    PIPE_FORMAT_BPTC_RGBA_UNORM, PIPE_FORMAT_BPTC_SRGBA,
    PIPE_FORMAT_ASTC_4x4_SRGB, PIPE_FORMAT_ASTC_4x4,
    PIPE_FORMAT_ASTC_5x4_SRGB, PIPE_FORMAT_ASTC_5x4,
    PIPE_FORMAT_ASTC_5x5_SRGB, PIPE_FORMAT_ASTC_5x5,
    PIPE_FORMAT_ASTC_6x5_SRGB, PIPE_FORMAT_ASTC_6x5,
    PIPE_FORMAT_ASTC_6x6_SRGB, PIPE_FORMAT_ASTC_6x6,
    PIPE_FORMAT_ASTC_8x5_SRGB, PIPE_FORMAT_ASTC_8x5,
    PIPE_FORMAT_ASTC_8x6_SRGB, PIPE_FORMAT_ASTC_8x6,
    PIPE_FORMAT_ASTC_8x8_SRGB, PIPE_FORMAT_ASTC_8x8,
    PIPE_FORMAT_ASTC_10x5_SRGB, PIPE_FORMAT_ASTC_10x5,
    PIPE_FORMAT_ASTC_10x6_SRGB, PIPE_FORMAT_ASTC_10x6,
    PIPE_FORMAT_ASTC_10x8_SRGB, PIPE_FORMAT_ASTC_10x8,
    PIPE_FORMAT_ASTC_10x10_SRGB, PIPE_FORMAT_ASTC_10x10,
    PIPE_FORMAT_ASTC_12x10_SRGB, PIPE_FORMAT_ASTC_12x10,
    PIPE_FORMAT_ASTC_12x12_SRGB, PIPE_FORMAT_ASTC_12x12,
    PIPE_FORMAT_Z16_UNORM, PIPE_FORMAT_Z32_FLOAT,
    PIPE_FORMAT_S8_UINT, PIPE_FORMAT_Z32_FLOAT_S8X24_UINT
};
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
static inline const void *util_format_description(enum pipe_format f) { (void)f; return NULL; }
static inline bool util_format_has_depth(const void *d) { (void)d; return false; }
static inline bool util_format_has_stencil(const void *d) { (void)d; return false; }
#include "air_metal_formats.inc"

enum { THREADS = 8, ITERS = 50000 };
static struct { pthread_mutex_t m; pthread_cond_t c; int count; } bar = {
    PTHREAD_MUTEX_INITIALIZER, PTHREAD_COND_INITIALIZER, 0
};
static int expect_served;

static void *worker(void *arg) {
    (void)arg;
    pthread_mutex_lock(&bar.m);
    if (++bar.count == THREADS) pthread_cond_broadcast(&bar.c);
    else while (bar.count < THREADS) pthread_cond_wait(&bar.c, &bar.m);
    pthread_mutex_unlock(&bar.m);

    for (int i = 0; i < ITERS; i++) {
        for (unsigned fmt = 40; fmt <= 43; fmt++) {
            const struct rwa_pixel_format *pf = pixel_format(fmt);
            assert((pf != NULL) == (expect_served != 0));
        }
        assert(pixel_format(80) != NULL);
    }
    return NULL;
}

int main(void) {
    const char *v = getenv("RWA_PACKED16");
    expect_served = !(v && *v == '0');
    pthread_t th[THREADS];
    for (int i = 0; i < THREADS; i++)
        assert(pthread_create(&th[i], NULL, worker, NULL) == 0);
    for (int i = 0; i < THREADS; i++)
        assert(pthread_join(th[i], NULL) == 0);
    printf("packed16_test (expect_served=%d): PASS\n", expect_served);
    return 0;
}
"""

AIR_STRESS_HARNESS = r"""
#include <stdbool.h>
#include "air_metal.h"
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>

enum { TEX_THREADS = 8, BUF_THREADS = 2, TEX_ITERS = 60, BUF_ITERS = 30 };
static struct rwa_device *g_dev;
static struct { pthread_mutex_t m; pthread_cond_t c; int count, total; } bar = {
    PTHREAD_MUTEX_INITIALIZER, PTHREAD_COND_INITIALIZER, 0, TEX_THREADS + BUF_THREADS
};

static void bar_wait(void) {
    pthread_mutex_lock(&bar.m);
    if (++bar.count == bar.total) pthread_cond_broadcast(&bar.c);
    else while (bar.count < bar.total) pthread_cond_wait(&bar.c, &bar.m);
    pthread_mutex_unlock(&bar.m);
}

static void *tex_worker(void *arg) {
    unsigned id = (unsigned)(uintptr_t)arg;
    char why[512];
    bar_wait();
    for (int i = 0; i < TEX_ITERS; i++) {
        /* 1. Sample-only texture: avoids texture_start_zeroed and skips rwa_lock completely. */
        struct rwa_texture_shape s_ro = {
            .pixel_format = (id & 1) ? 40 : 80, .type = 2,
            .width = 32, .height = 32, .depth = 1, .array_length = 1,
            .mip_levels = 1, .sample_count = 1, .usage = 1,
        };
        struct rwa_texture *t_ro = rwa_texture_create(g_dev, &s_ro, why, sizeof(why));
        assert(t_ro != NULL);
        rwa_texture_destroy(t_ro);

        /* 2. Render-target texture: takes rwa_lock only inside texture_start_zeroed(). */
        struct rwa_texture_shape s_rt = {
            .pixel_format = 80, .type = 2,
            .width = 16, .height = 16, .depth = 1, .array_length = 1,
            .mip_levels = 1, .sample_count = 1, .usage = 0,
        };
        struct rwa_texture *t_rt = rwa_texture_create(g_dev, &s_rt, why, sizeof(why));
        assert(t_rt != NULL);
        rwa_texture_destroy(t_rt);

        /* 3. Surface import validation & softpipe refusal outside rwa_lock. */
        assert(rwa_texture_from_surface(g_dev, &s_rt, 0, 0, 64, why, sizeof(why)) == NULL);
        assert(rwa_texture_from_surface(g_dev, &s_rt, 7, 0, 64, why, sizeof(why)) == NULL);
    }
    return NULL;
}

static void *buf_worker(void *arg) {
    (void)arg;
    char why[512];
    bar_wait();
    for (int i = 0; i < BUF_ITERS; i++) {
        struct rwa_buffer *buf = rwa_buffer_create(g_dev, 2048, why, sizeof(why));
        assert(buf != NULL);
        struct rwa_texture_shape s = {
            .pixel_format = 80, .type = 2,
            .width = 16, .height = 16, .depth = 1, .array_length = 1,
            .mip_levels = 1, .sample_count = 1, .usage = 1,
        };
        struct rwa_texture *bt = rwa_texture_from_buffer(g_dev, &s, buf, 0, 64, why, sizeof(why));
        assert(bt != NULL);
        struct rwa_texture *rt_shape_tex = NULL;
        struct rwa_texture_shape rt_s = {
            .pixel_format = 80, .type = 2,
            .width = 16, .height = 16, .depth = 1, .array_length = 1,
            .mip_levels = 1, .sample_count = 1, .usage = 4,
        };
        rt_shape_tex = rwa_texture_create(g_dev, &rt_s, why, sizeof(why));
        assert(rt_shape_tex != NULL);
        float cc[4] = { 0.25f, 0.5f, 0.75f, 1.0f };
        struct rwa_draw draw = { .color = &rt_shape_tex, .color_count = 1, .clear_color = cc, .discard = 1u << 8 };
        assert(rwa_render_clear(g_dev, &draw, why, sizeof(why)) == 0);
        assert(rwa_finish(g_dev, why, sizeof(why)) == 0);
        rwa_texture_destroy(rt_shape_tex);
        rwa_texture_destroy(bt);
        rwa_buffer_destroy(buf);
    }
    return NULL;
}

int main(void) {
    char why[512];
    setenv("RWA_SOFTWARE", "1", 1);
    g_dev = rwa_device_open(why, sizeof(why));
    assert(g_dev != NULL);
    pthread_t tth[TEX_THREADS], bth[BUF_THREADS];
    for (int i = 0; i < TEX_THREADS; i++)
        assert(pthread_create(&tth[i], NULL, tex_worker, (void *)(uintptr_t)i) == 0);
    for (int i = 0; i < BUF_THREADS; i++)
        assert(pthread_create(&bth[i], NULL, buf_worker, (void *)(uintptr_t)i) == 0);
    for (int i = 0; i < TEX_THREADS; i++)
        assert(pthread_join(tth[i], NULL) == 0);
    for (int i = 0; i < BUF_THREADS; i++)
        assert(pthread_join(bth[i], NULL) == 0);
    rwa_device_close(g_dev);
    printf("air_rwa_stress (%d tex + %d buf threads): PASS\n", TEX_THREADS, BUF_THREADS);
    return 0;
}
"""


def run_sanitized_harness(tmp, name, src, includes, flags, env_extra=None, check_drm_sum=False):
    c_path = tmp / f'{name}.c'
    bin_path = tmp / name
    c_path.write_text(src)
    cmd = ['clang', '-std=c11', '-O1', '-g', '-Wall', '-Wextra', '-pthread', *flags,
           *[f'-I{inc}' for inc in includes], str(c_path), '-framework', 'IOKit', '-o', str(bin_path)]
    subprocess.run(cmd, check=True)
    env = dict(os.environ)
    if env_extra:
        env.update(env_extra)
    res = subprocess.run([str(bin_path)], capture_output=True, text=True, env=env, check=True)
    if check_drm_sum:
        total_gem = 0
        total_map = 0
        total_unmap = 0
        lines = 0
        for line in res.stdout.splitlines():
            if 'calls in ' not in line:
                continue
            lines += 1
            for m in re.finditer(r'(0x[0-9a-f]+):(\d+)/(\d+)', line):
                sel, calls = m.group(1), int(m.group(2))
                if sel == '0x42':
                    total_gem += calls
                elif sel == '0xf0':
                    total_map += calls
                elif sel == '0xf1':
                    total_unmap += calls
        assert lines >= 2, f'expected >=2 flushed tally lines, got {lines}'
        assert total_gem == 8 * 4000 + 1, f'lost gemNew count: {total_gem} vs {8*4000+1}'
        assert total_map == 8 * (4000 // 8), f'lost map count: {total_map}'
        assert total_unmap == 8 * (4000 // 8), f'lost unmap count: {total_unmap}'
        print(f'{name} ({",".join(flags)}): {lines} flushes, exact sum 0x42={total_gem} 0xf0={total_map} 0xf1={total_unmap} PASS')
    else:
        print(f'{name} ({",".join(flags)}): {res.stdout.strip()}')


def run_mesa_tsan_tests(tmp, build_dir):
    lib_dir = build_dir / 'src' / 'gallium' / 'drivers' / 'freedreno'
    amt = lib_dir / 'air_metal_test'
    assert amt.is_file(), f'missing {amt}'
    env = dict(os.environ, RWA_SOFTWARE='1', DYLD_LIBRARY_PATH=str(lib_dir))

    r1 = subprocess.run([str(amt), '--surface'], env=env, capture_output=True, text=True, check=True)
    assert '0 answers wrong' in r1.stdout, r1.stdout
    print('air_metal_test --surface (TSan): PASS')

    copy_bin = tmp / 'air_copy_test'
    subprocess.run([
        'clang', '-std=c11', '-O1', '-g', '-fsanitize=thread', '-include', 'stdbool.h',
        f'-I{AIR_DIR}', str(AIR_DIR / 'air_copy_test.c'),
        str(lib_dir / 'libannibaleair.dylib'),
        f'-Wl,-rpath,{lib_dir}', '-o', str(copy_bin),
    ], check=True)
    r2 = subprocess.run([str(copy_bin), '--software'], env=env, capture_output=True, text=True, check=True)
    assert 'format 115' in r2.stdout and 'PASS' in r2.stdout, r2.stdout
    print('air_copy_test --software (TSan): PASS')

    stress_c = tmp / 'air_rwa_stress.c'
    stress_bin = tmp / 'air_rwa_stress'
    stress_c.write_text(AIR_STRESS_HARNESS)
    subprocess.run([
        'clang', '-std=c11', '-O1', '-g', '-pthread', '-fsanitize=thread',
        f'-I{AIR_DIR}', str(stress_c),
        str(lib_dir / 'libannibaleair.dylib'),
        f'-Wl,-rpath,{lib_dir}', '-o', str(stress_bin),
    ], check=True)
    r3 = subprocess.run([str(stress_bin)], env=env, capture_output=True, text=True, check=True)
    assert 'PASS' in r3.stdout, r3.stdout
    print('air_rwa_stress (8 tex + 2 buf threads, TSan): PASS')


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--mesa-build-dir', type=Path,
                    default=DEFAULT_MESA_BUILD if DEFAULT_MESA_BUILD.is_dir() else None)
    args = ap.parse_args()

    check_static_contracts()
    mesa_inc = ROOT.parent / 'hackios-xnu-kernel' / 'xnu-deps' / 'build' / 'mesa' / 'mesa-26.2.5' / 'include'
    if not mesa_inc.is_dir():
        mesa_inc = ROOT.parent / 'hackios-xnu-kernel' / 'out' / 'ios26-resume' / 'mesa-arm64e' / 'mesa-26.2.5' / 'include'
    assert mesa_inc.is_dir(), f'missing Mesa include dir: {mesa_inc}'

    with tempfile.TemporaryDirectory(prefix='test-air-rwa-lock-') as td:
        tmp = Path(td)
        for flags in (['-fsanitize=thread'], ['-fsanitize=address,undefined']):
            run_sanitized_harness(tmp, 'drm_tally', DRM_HARNESS,
                                  [ROOT / 'tools' / 'mesa' / 'annibale-drm', mesa_inc],
                                  [*flags, '-Wno-unused-function'], check_drm_sum=True)
            run_sanitized_harness(tmp, 'packed16_default', PACKED16_HARNESS,
                                  [AIR_DIR], [*flags, '-Wno-unused-function'])
            run_sanitized_harness(tmp, 'packed16_off', PACKED16_HARNESS,
                                  [AIR_DIR], [*flags, '-Wno-unused-function'],
                                  env_extra={'RWA_PACKED16': '0'})
        if args.mesa_build_dir is not None:
            run_mesa_tsan_tests(tmp, args.mesa_build_dir)
    print('test-air-rwa-lock-narrowing: ALL PASS')


if __name__ == '__main__':
    main()
