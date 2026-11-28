#!/usr/bin/env python3
"""Host ASan+UBSan unit tests for rear wide OVX8000 PDAF EEPROM parser and disparity estimator."""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
SERVICES = ROOT / "userland/src/services"

HARNESS = r"""
#include <assert.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define _IOR(g, n, t) 0
#define _IOWR(g, n, t) 0
#include "annibale_camera.h"

static void say(const char *fmt, ...) { (void)fmt; }

/* Mock paged EEPROM reader for camera_pdaf.inc */
static uint8_t mock_eeprom[0x1840];
static size_t mock_eeprom_len;

static size_t
camera_native_read_eeprom(int device, uint8_t *out, size_t max_bytes)
{
    (void)device;
    size_t n = mock_eeprom_len < max_bytes ? mock_eeprom_len : max_bytes;
    if (n > 0) memcpy(out, mock_eeprom, n);
    return n;
}

#include "camera_pdaf.inc"

static void
wr_u16_be(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)((v >> 8) & 0xffu);
    p[1] = (uint8_t)(v & 0xffu);
}

static void
build_valid_eeprom(uint8_t *ee, int16_t dcc_q4, uint16_t gl, uint16_t gr)
{
    memset(ee, 0, 0x1840);
    ee[0x0001] = 0x10;
    wr_u16_be(ee + 0x002d, 676); /* macro (0x02a4) */
    wr_u16_be(ee + 0x0031, 325); /* infinity (0x0145) */
    wr_u16_be(ee + 0x0eed, 10);
    wr_u16_be(ee + 0x0eef, 17);
    wr_u16_be(ee + 0x0ef1, 13);
    for (uint32_t i = 0; i < 17 * 13; i++) {
        wr_u16_be(ee + 0x0ef3 + i * 2, gl);
        wr_u16_be(ee + 0x10ad + i * 2, gr);
        wr_u16_be(ee + 0x1399 + i * 2, gl);
        wr_u16_be(ee + 0x1553 + i * 2, gr);
    }
    for (uint32_t i = 0; i < 8 * 6; i++) {
        wr_u16_be(ee + 0x126d + i * 2, (uint16_t)dcc_q4);
        wr_u16_be(ee + 0x1713 + i * 2, (uint16_t)dcc_q4);
    }
}

static void
pack_raw10_row(uint8_t *dst, const uint16_t *px, uint32_t width)
{
    for (uint32_t x = 0; x < width; x += 4) {
        uint16_t p0 = px[x + 0] & 0x3ffu;
        uint16_t p1 = px[x + 1] & 0x3ffu;
        uint16_t p2 = px[x + 2] & 0x3ffu;
        uint16_t p3 = px[x + 3] & 0x3ffu;
        uint8_t *g = dst + (x / 4) * 5;
        g[0] = (uint8_t)(p0 >> 2);
        g[1] = (uint8_t)(p1 >> 2);
        g[2] = (uint8_t)(p2 >> 2);
        g[3] = (uint8_t)(p3 >> 2);
        g[4] = (uint8_t)((p0 & 3u) | ((p1 & 3u) << 2) | ((p2 & 3u) << 4) | ((p3 & 3u) << 6));
    }
}

static double
synth_pattern(double x)
{
    /* Multi-frequency textured edge pattern (no ambiguity within +-16 px) */
    double e1 = 220.0 * exp(-pow((x - 520.0) / 7.5, 2.0));
    double e2 = 180.0 * exp(-pow((x - 640.0) / 5.0, 2.0));
    double e3 = 200.0 * exp(-pow((x - 740.0) / 9.0, 2.0));
    double bg = 180.0 + 90.0 * sin(x * 0.037) + 60.0 * cos(x * 0.083);
    double v = bg + e1 + e2 + e3;
    return v < 0.0 ? 0.0 : v > 900.0 ? 900.0 : v;
}

static void
fill_pd_frame(uint8_t *pd_raw, double shift_r_minus_l, double gain_l_div, double gain_r_div, bool flat)
{
    uint16_t line[2560];
    for (uint32_t y = 0; y < 480; y++) {
        for (uint32_t p = 0; p < 1280; p++) {
            double lv = flat ? 8.0 : synth_pattern((double)p) / gain_l_div;
            double rv = flat ? 8.0 : synth_pattern((double)p - shift_r_minus_l) / gain_r_div;
            int li = (int)(lv + 0.5) + PDAF_BLACK_LEVEL;
            int ri = (int)(rv + 0.5) + PDAF_BLACK_LEVEL;
            if (li > 1023) li = 1023;
            if (ri > 1023) ri = 1023;
            line[p * 2 + 0] = (uint16_t)li;
            line[p * 2 + 1] = (uint16_t)ri;
        }
        pack_raw10_row(pd_raw + (size_t)y * 3200, line, 2560);
    }
}

int
main(void)
{
    uint8_t ee[0x1840];
    struct pdaf_cal cal;

    /* 1. Valid Big-Endian EEPROM parsing (matching b5r67 hardware values) */
    build_valid_eeprom(ee, 0x0289 /* 40.5625 DAC/px in Q4 */, 256, 256);
    assert(pdaf_parse_eeprom(&cal, ee, sizeof(ee)));
    assert(cal.valid && cal.dac_macro == 676 && cal.dac_inf == 325);
    assert(fabsf(cal.dcc_h[0] - 40.5625f) < 1e-5f);

    /* 2. Rejection cases: bad vendor, bad grid, truncated, zero DCC, macro==inf */
    ee[0x0001] = 0x11;
    assert(!pdaf_parse_eeprom(&cal, ee, sizeof(ee)));
    build_valid_eeprom(ee, 256, 256, 256);
    wr_u16_be(ee + 0x0eef, 16);
    assert(!pdaf_parse_eeprom(&cal, ee, sizeof(ee)));
    build_valid_eeprom(ee, 256, 256, 256);
    assert(!pdaf_parse_eeprom(&cal, ee, 0x1000));
    build_valid_eeprom(ee, 0, 256, 256);
    assert(!pdaf_parse_eeprom(&cal, ee, sizeof(ee)));
    build_valid_eeprom(ee, 256, 256, 256);
    wr_u16_be(ee + 0x0031, 676);
    assert(!pdaf_parse_eeprom(&cal, ee, sizeof(ee)));

    /* 3. Paged load helper */
    build_valid_eeprom(mock_eeprom, 320 /* 20.0 DAC/px */, 320, 256);
    mock_eeprom_len = sizeof(mock_eeprom);
    pdaf_load_eeprom(0, &cal);
    assert(cal.valid && fabsf(cal.dcc_h[3 * 8 + 4] - 20.0f) < 1e-5f);

    /* 4. Disparity + GainMap + DCC accuracy tests */
    uint8_t *pd_raw = calloc(1, PDAF_MAX_BYTES);
    assert(pd_raw != NULL);
    struct annibale_camera_frame f;
    memset(&f, 0, sizeof(f));
    f.pd_width = 2560; f.pd_height = 480; f.pd_stride = 3200;
    f.pd_bytes = PDAF_MAX_BYTES; f.pd_flags = ANNIBALE_CAM_PD_VALID;
    const float win[4] = { 0.3f, 0.3f, 0.7f, 0.7f };
    struct pdaf_result res;

    /* +3.25 px shift with asymmetric GainMap (L=320/256=1.25x, R=1.0x), dcc=20, scaleX=2 -> delta=-130 */
    fill_pd_frame(pd_raw, 3.25, 320.0 / 256.0, 1.0, false);
    assert(pdaf_measure(&cal, &f, pd_raw, win, 400, &res));
    assert(fabsf(res.disparity - 3.25f) < 0.15f);
    assert(res.confidence >= PDAF_CONF_MIN);
    assert(res.target_dac >= 264 && res.target_dac <= 276);

    /* -2.31 px shift (matching b5r68 at DAC 275) with dcc=20, scaleX=2 -> delta=+92.4 -> target ~367 */
    fill_pd_frame(pd_raw, -2.31, 320.0 / 256.0, 1.0, false);
    assert(pdaf_measure(&cal, &f, pd_raw, win, 275, &res));
    assert(fabsf(res.disparity - (-2.31f)) < 0.15f);
    assert(res.target_dac >= 361 && res.target_dac <= 373);

    /* Zero shift -> converged */
    fill_pd_frame(pd_raw, 0.0, 320.0 / 256.0, 1.0, false);
    assert(pdaf_measure(&cal, &f, pd_raw, win, 350, &res));
    assert(fabsf(res.disparity) < 0.1f && fabsf(res.delta_dac) < 2.0f);

    /* Clamp to [AF_NEAR, AF_FAR] */
    fill_pd_frame(pd_raw, -10.0, 320.0 / 256.0, 1.0, false);
    assert(pdaf_measure(&cal, &f, pd_raw, win, 650, &res));
    assert(res.target_dac == PDAF_AF_FAR);

    /* Flat / dark scene returns false for CDAF fallback */
    fill_pd_frame(pd_raw, 2.0, 1.0, 1.0, true);
    assert(!pdaf_measure(&cal, &f, pd_raw, win, 300, &res));

    /* Missing PD_VALID flag returns false */
    f.pd_flags = 0;
    fill_pd_frame(pd_raw, 3.0, 320.0 / 256.0, 1.0, false);
    assert(!pdaf_measure(&cal, &f, pd_raw, win, 300, &res));

    free(pd_raw);
    printf("PASS: test-camera-pdaf (all assertions passed)\n");
    return 0;
}
"""


def main():
    with tempfile.TemporaryDirectory(prefix="camera-pdaf-") as tmp:
        d = Path(tmp)
        src = d / "harness.c"
        exe = d / "harness"
        src.write_text(HARNESS)
        cmd = [
            "clang", "-std=c11", "-O1", "-Wall", "-Wextra", "-Werror",
            "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
            "-I", str(SERVICES), str(src), "-lm", "-o", str(exe),
        ]
        subprocess.run(cmd, check=True)
        res = subprocess.run([str(exe)], check=True, capture_output=True, text=True)
        print(res.stdout.strip())


if __name__ == "__main__":
    main()
