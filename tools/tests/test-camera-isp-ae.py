#!/usr/bin/env python3
"""Host ASan+UBSan tests for camera_isp.c AE convergence, ADRC damping, and readback gating."""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
SERVICES = ROOT / "userland/src/services"

HARNESS = r"""
#include <assert.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "camera_isp.h"

static struct annibale_camera_frame make_back_frame(uint32_t lines, uint32_t gain, uint32_t fll)
{
    struct annibale_camera_frame f;
    memset(&f, 0, sizeof(f));
    f.width = 2560;
    f.height = 1920;
    f.stride = 3200;
    f.black = 64;
    f.exposure_lines = lines;
    f.line_ns = 5333;
    f.gain = gain;
    f.gain_unity = 256;
    f.gain_max = 4096;
    f.frame_lines = fll;
    f.frame_lines_min = 6250;
    f.frame_lines_max = 12500;
    f.margin = 32;
    return f;
}

int main(void)
{
    struct isp isp;
    struct annibale_camera_command cmd;
    struct isp_stats st;

    /* 1. Large bright-to-target jump (>2 EV) uses 0.85 adaptive damping and converges monotonically. */
    isp_init(&isp);
    double scene_light = 0.05; /* bright outdoor scene: initial level ~1243 >> target ~173 */
    uint32_t lines = 6218, gain = 1024, fll = 6250;
    bool first_step_checked = false;
    int steps = 0;
    for (int iter = 0; iter < 12; iter++) {
        struct annibale_camera_frame f = make_back_frame(lines, gain, fll);
        double level = scene_light * (double)lines * gain / 256.0;
        memset(&st, 0, sizeof(st));
        st.key = level;
        st.bright = level * 1.4;
        if (!isp_ae(&isp, &f, &st, &cmd)) {
            break;
        }
        if (!first_step_checked) {
            /* With ratio=0.25 and damp=0.85, new exposure is ~0.308x of old (faster than 0.7 -> 0.379x). */
            double old_ev = (double)lines * gain / 256.0;
            double new_ev = (double)cmd.a * cmd.b / 256.0;
            assert(new_ev < old_ev * 0.34);
            first_step_checked = true;
        }
        /* Simulate 2-frame readback lag: pending returns false while frame still has old exposure. */
        assert(isp_ae_pending(&isp, &f));
        assert(!isp_ae(&isp, &f, &st, &cmd));
        lines = cmd.a;
        gain = cmd.b;
        fll = cmd.c ? cmd.c : 6250;
        steps++;
    }
    assert(first_step_checked && steps > 0 && steps <= 5);

    /* 2. Backlit highlight scene: ADRC drc ramps smoothly via EMA instead of jumping to 2.0 in 1 frame. */
    isp_init(&isp);
    struct annibale_camera_frame bf = make_back_frame(3000, 256, 6250);
    memset(&st, 0, sizeof(st));
    st.key = 60.0;
    st.bright = 920.0;
    float drc0 = isp.drc;
    assert(isp_ae(&isp, &bf, &st, &cmd));
    float drc1 = isp.drc;
    assert(drc1 > drc0 && drc1 < 1.35f); /* smoothed EMA step, not instant jump to 2.0 */
    for (int i = 0; i < 20; i++) {
        isp.ae_wait = 0;
        (void)isp_ae(&isp, &bf, &st, &cmd);
    }
    assert(isp.drc >= 1.90f && isp.drc <= 2.01f);

    /* 3. Extreme dark scene: ramps analog gain -> frame_lines extension -> digital gain <= 4x. */
    isp_init(&isp);
    struct annibale_camera_frame df = make_back_frame(6218, 4096, 6250);
    memset(&st, 0, sizeof(st));
    st.key = 10.0;
    st.bright = 20.0;
    assert(isp_ae(&isp, &df, &st, &cmd));
    assert(cmd.c > 6250 && cmd.c <= 12500);
    assert(cmd.b == 4096);
    assert(isp.digital >= 1.0f && isp.digital <= 4.0f);

    puts("camera_isp AE convergence and ADRC damping checks passed");
    return 0;
}
"""


class CameraIspAeTest(unittest.TestCase):
    def test_ae_convergence_and_adrc(self):
        with tempfile.TemporaryDirectory(prefix="camera-isp-ae-") as tmp:
            d = Path(tmp)
            (d / "harness.c").write_text(HARNESS)
            binary = d / "camera_isp_ae_test"
            subprocess.run([
                "xcrun", "--sdk", "macosx", "clang", "-O1", "-Wall", "-Wextra", "-Werror",
                "-fblocks", "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
                "-I", str(SERVICES),
                str(SERVICES / "camera_isp.c"), str(d / "harness.c"),
                "-lm", "-o", str(binary),
            ], check=True)
            res = subprocess.run([str(binary)], capture_output=True, text=True, timeout=10)
            self.assertEqual(res.returncode, 0, res.stdout + res.stderr)
            self.assertIn("passed", res.stdout)


if __name__ == "__main__":
    unittest.main(verbosity=2)
