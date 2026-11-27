#!/usr/bin/env python3
"""Verify mkcameraresources.py strips H18 ANE/ISP keys and produces deterministic binary plists."""
from pathlib import Path
import plistlib
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "userland"))
import mkcameraresources

H18_ANE_KEYS = (
    "smartCameraSupported",
    "LensSmudgeDetectionSupported",
    "geometricDistortionCorrection",
    "geometricDistortionCorrectionForSmartCropEnabled",
    "IntelligentDistortionCorrectionSupported",
)
V57_DIR = ROOT / "ios26/rootfs/System/Library/PrivateFrameworks/CMCapture.framework/V57"


def collect_key_values(obj, target_keys, out):
    if isinstance(obj, dict):
        for k, v in obj.items():
            if k in target_keys:
                out.setdefault(k, []).append(v)
            collect_key_values(v, target_keys, out)
    elif isinstance(obj, list):
        for item in obj:
            collect_key_values(item, target_keys, out)


class MkCameraResourcesTest(unittest.TestCase):
    def test_without_isp_features_strips_h18_ane_keys(self):
        sample = {
            "AVCaptureDeviceCommonSettings": {
                "smartCameraSupported": True,
                "LensSmudgeDetectionSupported": True,
                "keepMe": 42,
            },
            "nested": [
                {
                    "geometricDistortionCorrection": True,
                    "geometricDistortionCorrectionForSmartCropEnabled": True,
                    "IntelligentDistortionCorrectionSupported": True,
                    "stillImageDepthDataType": 10,
                    "unchanged": "ok",
                }
            ],
        }
        out = mkcameraresources.without_isp_features(sample)
        self.assertEqual(out["AVCaptureDeviceCommonSettings"]["keepMe"], 42)
        self.assertEqual(out["nested"][0]["unchanged"], "ok")
        self.assertNotIn("stillImageDepthDataType", out["nested"][0])
        found = {}
        collect_key_values(out, H18_ANE_KEYS, found)
        self.assertEqual(set(found.keys()), set(H18_ANE_KEYS))
        for k, vals in found.items():
            self.assertEqual(vals, [False], f"{k} must be False")

    def test_stage_real_v57_deterministic_and_clean(self):
        if not V57_DIR.is_dir():
            self.skipTest("iOS 26 V57 rootfs directory not present locally")
        with tempfile.TemporaryDirectory(prefix="mkcamres-test-") as tmp:
            out1 = Path(tmp) / "v57-a"
            out2 = Path(tmp) / "v57-b"
            mkcameraresources.stage(V57_DIR, out1)
            mkcameraresources.stage(V57_DIR, out2)

            src_files = sorted(p.name for p in V57_DIR.iterdir())
            out_files = sorted(p.name for p in out1.iterdir())
            self.assertEqual(out_files, src_files)

            found = {}
            for name in out_files:
                b1 = (out1 / name).read_bytes()
                b2 = (out2 / name).read_bytes()
                self.assertEqual(b1, b2, f"non-deterministic output for {name}")
                if name.endswith(".plist"):
                    parsed = plistlib.loads(b1)
                    collect_key_values(parsed, H18_ANE_KEYS, found)
                    omitted = {}
                    collect_key_values(parsed, mkcameraresources.OMITTED_KEYS, omitted)
                    self.assertEqual(omitted, {}, f"omitted keys leaked in {name}: {omitted}")
                else:
                    self.assertEqual(b1, (V57_DIR / name).read_bytes())

            self.assertEqual(set(found.keys()), set(H18_ANE_KEYS))
            for k, vals in found.items():
                self.assertTrue(vals and all(v is False for v in vals), f"{k} had non-False values: {vals}")


if __name__ == "__main__":
    unittest.main(verbosity=2)
