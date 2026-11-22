#!/usr/bin/env python3
"""compile_targets.py - Parallel incremental build runner for userland targets.

Manages declarative target table, -MMD header dependency discovery,
SHA-256 build caching, multi-core parallel xcrun clang compilation,
codesign ad-hoc signing, PYHID verification, and arm64e architecture gate.
"""

from concurrent.futures import ThreadPoolExecutor, as_completed
import hashlib
import json
import os
from pathlib import Path
import plistlib
import re
import struct
import subprocess
import sys

USERLAND_DIR = Path(__file__).resolve().parent

DEFAULT_DRV = Path("../../hackios-xnu-kernel/tools/userland-driver")
if not (USERLAND_DIR / DEFAULT_DRV).is_dir():
    DEFAULT_DRV = Path("../tools/userland-driver")

if sys.platform == "darwin":
    CLANG_CMD = ["/usr/bin/xcrun", "-sdk", "iphoneos", "clang"]
    BASE_FLAGS = ["-arch", "arm64e", "-miphoneos-version-min=26.0"]
else:
    sdk_path = USERLAND_DIR.parent / "xnu-deps" / "26" / "sdk" / "iPhoneOS.sdk"
    CLANG_CMD = ["xnu-clang"]
    BASE_FLAGS = ["-arch", "arm64e", "-isysroot", str(sdk_path), "-miphoneos-version-min=26.0"]

XCRUN_CLANG = CLANG_CMD
INC_FLAGS = [
    "-Isrc/boot", "-Isrc/inject", "-Isrc/gfx",
    "-Isrc/services", "-Isrc/apps", "-Isrc/tests",
]

TARGETS = [
    # 1. input26-service & system shims
    {
        "name": "input26-service",
        "type": "exe",
        "sources": ["src/services/input26_service.c"],
        "flags": ["-Os", "-Wall", "-Wextra", "-Werror", "-framework", "CoreFoundation", "-framework", "IOKit"],
        "entitlements": "src/services/input26_service.entitlements",
        "check_hid": True,
    },
    {
        "name": "libannibale-sb26.dylib",
        "type": "dylib",
        "install_name": "/annibale/lib/libannibale-sb26.dylib",
        "sources": [
            "src/inject/inject_sb26.c", "src/inject/inject_sb26_layers.c",
            "src/inject/inject_sb26_rate.c", "src/inject/inject_posterboard.c",
        ],
        "flags": ["-Os", "-Wall", "-Wextra", "-Werror", "-framework", "CoreFoundation", "-framework", "Metal", "-lobjc", "-lMobileGestalt"],
    },
    {
        "name": "libannibale-sbmetal-experiment.dylib",
        "type": "dylib",
        "install_name": "/annibale/lib/libannibale-sbmetal-experiment.dylib",
        "sources": ["src/inject/sb_metal_experiment.m"],
        "flags": ["-Os", "-fno-objc-arc", "-Wall", "-Wextra", "-Werror", "-framework", "Foundation", "-framework", "Metal", "-framework", "UIKit", "-framework", "IOKit", "-lobjc"],
    },

    # 2. General dynamic executables & diagnostics
    {"name": "hello_dyn", "type": "exe", "sources": ["src/tests/hello_dyn.c"], "flags": ["-O1"]},
    {"name": "svc_test", "type": "exe", "sources": ["src/tests/svc_test.c"], "flags": ["-O1", "-framework", "CoreFoundation"]},
    {"name": "surf_test", "type": "exe", "sources": ["src/tests/surf_test.c"], "flags": ["-O1", "-Wno-four-char-constants", "-framework", "CoreFoundation", "-framework", "IOSurface"]},
    {
        "name": "drawable_test",
        "type": "exe",
        "sources": ["src/tests/drawable_test.c", "{DRV}/annibaledrawable.m"],
        "flags": ["-O1", "-fno-objc-arc", "-Wno-four-char-constants", "-framework", "CoreFoundation", "-framework", "IOSurface", "-framework", "IOKit"],
    },
    {"name": "anntouch", "type": "exe", "sources": ["src/apps/anntouch.c"], "flags": ["-O1"]},
    {"name": "annsh", "type": "exe", "sources": ["src/apps/annsh.c"], "flags": ["-O1"]},
    {"name": "annsig", "type": "exe", "sources": ["src/apps/annsig.m"], "flags": ["-O1"]},
    {"name": "framewatch", "type": "exe", "sources": ["src/apps/framewatch.m"], "flags": ["-O1"]},
    {"name": "lsprobe", "type": "exe", "sources": ["src/apps/lsprobe.m"], "flags": ["-O1", "-framework", "Foundation"]},
    {"name": "sound_test", "type": "exe", "sources": ["src/tests/sound_test.c"], "flags": ["-O1", "-framework", "CoreFoundation", "-framework", "AudioToolbox", "-framework", "AVFoundation", "-lobjc"]},
    {"name": "mg_test", "type": "exe", "sources": ["src/tests/mg_test.c"], "flags": ["-O1", "-framework", "CoreFoundation", "-lMobileGestalt"]},
    {"name": "torch_test", "type": "exe", "sources": ["src/tests/torch_test.c"], "flags": ["-O1", "-framework", "CoreFoundation", "-framework", "AVFoundation", "-lobjc"]},
    {"name": "camera_test", "type": "exe", "sources": ["src/tests/camera_test.c", "src/services/camera_isp.c"], "flags": ["-O2"]},
    {
        "name": "camera_avtest",
        "type": "exe",
        "sources": ["src/tests/camera_avtest.m"],
        "flags": ["-O1", "-fobjc-arc", "-framework", "Foundation", "-framework", "AVFoundation"],
        "entitlements": "src/tests/camera_avtest.entitlements",
    },
    {
        "name": "record_test",
        "type": "exe",
        "sources": ["src/tests/record_test.c"],
        "flags": ["-O1", "-framework", "CoreFoundation", "-framework", "AVFoundation", "-lobjc"],
        "entitlements": "src/tests/record_test.entitlements",
    },
    {"name": "cpu_test", "type": "exe", "sources": ["src/tests/cpu_test.c"], "flags": ["-O1"]},
    {"name": "cow_test", "type": "exe", "sources": ["src/tests/cow_test.c"], "flags": ["-O1", "-framework", "CoreFoundation", "-framework", "IOSurface"]},
    {"name": "cg_test", "type": "exe", "sources": ["src/tests/cg_test.c"], "flags": ["-O1", "-framework", "CoreFoundation", "-framework", "CoreGraphics", "-framework", "IOSurface"]},
    {
        "name": "metal_test",
        "type": "exe",
        "sources": ["src/tests/metal_test.m"],
        "flags": ["-O1", "-fobjc-arc", "-Wno-deprecated-declarations", "-framework", "Foundation", "-framework", "CoreGraphics", "-framework", "CoreImage", "-framework", "IOKit", "-framework", "Metal", "-framework", "OpenGLES", "-lMobileGestalt"],
    },
    {"name": "metal_dev", "type": "exe", "sources": ["src/tests/metal_dev.m"], "flags": ["-O1", "-framework", "Foundation", "-framework", "IOKit", "-framework", "Metal", "-lobjc"]},
    {"name": "metal_stubdev", "type": "exe", "sources": ["src/tests/metal_stubdev.m"], "flags": ["-O1", "-framework", "Foundation", "-framework", "IOKit", "-framework", "Metal", "-lobjc"]},
    {
        "name": "libannibalemetal.dylib",
        "type": "dylib",
        "install_name": "/annibale/lib/libannibalemetal.dylib",
        "sources": ["{DRV}/annibalemetal.m", "{DRV}/annibaledrawable.m"],
        "flags": ["-O1", "-fno-objc-arc", "-framework", "Foundation", "-framework", "IOKit", "-framework", "Metal", "-lobjc"],
    },
    {"name": "metal_drv_test", "type": "exe", "sources": ["src/tests/metal_drv_test.m"], "flags": ["-O1", "-framework", "Foundation", "-framework", "IOKit", "-framework", "Metal", "-lobjc"]},
    {
        "name": "AnnibaleTriangle",
        "type": "exe",
        "sources": ["src/tests/triangle_app.m"],
        "flags": ["-O1", "-framework", "Foundation", "-framework", "UIKit", "-framework", "QuartzCore", "-framework", "Metal", "-framework", "IOSurface", "-framework", "IOKit", "-lobjc"],
        "entitlements": "src/tests/triangle_app.entitlements",
    },
    {
        "name": "metal_caps",
        "type": "exe",
        "sources": ["src/tests/metal_caps.m"],
        "flags": ["-O1", "-framework", "Foundation", "-framework", "IOKit", "-framework", "Metal", "-lobjc", "-framework", "MetalFX"],
    },
    {"name": "metal_props", "type": "exe", "sources": ["src/tests/metal_props.c"], "flags": ["-O1", "-framework", "CoreFoundation", "-framework", "IOKit"]},
    {"name": "shm_test", "type": "exe", "sources": ["src/tests/shm_test.c"], "flags": ["-O1"]},
    {"name": "apfs_mmap_test", "type": "exe", "sources": ["src/tests/apfs_mmap_test.c"], "flags": ["-O1"]},
    {"name": "vol_ls", "type": "exe", "sources": ["src/apps/vol_ls.c"], "flags": ["-O1"]},
    {"name": "snap_test", "type": "exe", "sources": ["src/tests/snap_test.c"], "flags": ["-O1", "-framework", "CoreFoundation", "-framework", "CoreGraphics", "-framework", "ImageIO"]},
    {"name": "font_test", "type": "exe", "sources": ["src/tests/font_test.c"], "flags": ["-O1", "-framework", "CoreFoundation", "-framework", "CoreGraphics", "-framework", "CoreText"]},
    {"name": "net_test", "type": "exe", "sources": ["src/tests/net_test.m"], "flags": ["-O1", "-fobjc-arc", "-framework", "Foundation", "-framework", "SystemConfiguration"]},
    {
        "name": "store_test",
        "type": "exe",
        "sources": ["src/tests/store_test.m"],
        "flags": ["-O1", "-fobjc-arc", "-framework", "Foundation", "-framework", "Accounts"],
        "entitlements": "src/tests/store_test.entitlements",
    },
    {
        "name": "battery_test",
        "type": "exe",
        "sources": ["src/tests/battery_test.c"],
        "flags": ["-O1", "-framework", "CoreFoundation", "-framework", "IOKit", "-lsqlite3"],
        "entitlements": "src/tests/battery_test.entitlements",
    },
    {
        "name": "keychain_test",
        "type": "exe",
        "sources": ["src/tests/keychain_test.c"],
        "flags": ["-O1", "-framework", "CoreFoundation", "-framework", "Security"],
        "entitlements": "src/tests/keychain_test.entitlements",
    },
    {
        "name": "attn_test",
        "type": "exe",
        "sources": ["src/tests/attn_test.c"],
        "flags": ["-O1", "-framework", "CoreFoundation"],
        "entitlements": "src/tests/attn_test.entitlements",
    },
    {
        "name": "wifipw_test",
        "type": "exe",
        "sources": ["src/tests/wifipw_test.c"],
        "flags": ["-O1", "-framework", "CoreFoundation", "-framework", "Security"],
        "entitlements": "src/tests/wifipw_test.entitlements",
    },

    # 3. Dynamic libraries & bundles
    {
        "name": "libannibale.dylib",
        "type": "dylib",
        "install_name": "/annibale/lib/libannibale.dylib",
        "sources": ["src/inject/inject.c"],
        "flags": ["-O1", "-framework", "CoreFoundation", "-framework", "IOKit", "-lobjc"],
    },
    {
        "name": "libannibale-tweaks.dylib",
        "type": "dylib",
        "install_name": "/annibale/lib/libannibale-tweaks.dylib",
        "sources": ["src/inject/inject_tweaks.c"],
        "flags": ["-O1", "-framework", "CoreFoundation", "-lobjc"],
    },
    {
        "name": "libannibale-app.dylib",
        "type": "dylib",
        "install_name": "/annibale/lib/libannibale-app.dylib",
        "sources": ["src/inject/inject_app.c"],
        "flags": ["-O1", "-lobjc"],
    },
    {
        "name": "libannibale-media.dylib",
        "type": "dylib",
        "install_name": "/annibale/lib/libannibale-media.dylib",
        "sources": ["src/inject/inject_media.c", "src/services/camera_device.c", "src/services/camera_isp.c"],
        "flags": ["-O2", "-framework", "CoreFoundation", "-framework", "CoreMedia", "-framework", "CoreVideo", "-framework", "IOSurface", "-lobjc"],
    },
    {
        "name": "libannibale-vt.dylib",
        "type": "dylib",
        "install_name": "/annibale/lib/libannibale-vt.dylib",
        "sources": ["src/inject/videotoolbox_iris.c"],
        "flags": ["-Os", "-Wno-four-char-constants", "-framework", "CoreFoundation", "-framework", "CoreMedia", "-framework", "CoreVideo", "-framework", "VideoToolbox", "-framework", "IOSurface"],
    },
    {
        "name": "video_decode_test",
        "type": "exe",
        "sources": ["src/tests/video_decode_test.m"],
        "flags": ["-O1", "-fobjc-arc", "-framework", "Foundation", "-framework", "AVFoundation", "-framework", "CoreMedia", "-framework", "CoreVideo", "-framework", "VideoToolbox"],
    },
    {
        "name": "video_player_test",
        "type": "exe",
        "sources": ["src/tests/video_player_test.m"],
        "flags": ["-O1", "-fobjc-arc", "-framework", "Foundation", "-framework", "AVFoundation", "-framework", "CoreMedia", "-framework", "CoreVideo"],
    },
    {
        "name": "video_combined_test",
        "type": "exe",
        "sources": ["src/tests/video_combined_test.m"],
        "flags": ["-O1", "-fno-objc-arc", "-framework", "Foundation", "-framework", "Metal", "-framework", "CoreVideo", "-framework", "IOSurface", "-lobjc"],
    },
    {
        "name": "libannibale-store.dylib",
        "type": "dylib",
        "install_name": "/annibale/lib/libannibale-store.dylib",
        "sources": ["src/inject/inject_store.c"],
        "flags": ["-O2", "-framework", "CoreFoundation", "-lobjc", "-lc++abi"],
    },
    {
        "name": "libannibale-fairplay.dylib",
        "type": "dylib",
        "install_name": "/annibale/lib/libannibale-fairplay.dylib",
        "sources": ["src/inject/inject_fairplay.c"],
        "flags": ["-O2", "-framework", "CoreFoundation", "-framework", "IOKit", "-framework", "Security", "-lMobileGestalt"],
    },
    {
        "name": "AnnibaleAudio",
        "type": "bundle",
        "sources": ["src/services/audio_plugin.c"],
        "flags": ["-O1", "-framework", "CoreFoundation"],
    },
    {
        "name": "AnnibaleMetal",
        "type": "bundle",
        "sources": ["{DRV}/annibalemetal.m", "{DRV}/annibaledrawable.m"],
        "flags": ["-O1", "-fno-objc-arc", "-framework", "Foundation", "-framework", "IOKit", "-framework", "Metal", "-lobjc"],
    },
]


def sha256_file(path: Path) -> str:
    """Compute SHA-256 digest of file."""
    h = hashlib.sha256()
    try:
        with open(path, "rb") as f:
            while chunk := f.read(65536):
                h.update(chunk)
        return h.hexdigest()
    except OSError:
        return ""


def parse_depfile(dep_path: Path) -> list[Path]:
    """Parse Makefile syntax depfile generated by clang -MMD."""
    if not dep_path.is_file():
        return []
    try:
        content = dep_path.read_text(errors="replace").replace("\\\n", " ")
        deps = []
        for line in content.splitlines():
            line = line.strip()
            if not line:
                continue
            parts = line.split(":", 1)
            tokens = parts[1].split() if len(parts) >= 2 else line.split()
            for token in tokens:
                p = Path(token)
                if not p.is_absolute():
                    p = USERLAND_DIR / p
                p = p.resolve()
                # Skip system framework headers for fast caching; track project headers
                if p.is_file() and not str(p).startswith("/Applications/Xcode.app") and not str(p).startswith("/Library/Developer/CommandLineTools"):
                    deps.append(p)
        return sorted(set(deps))
    except Exception:
        return []


def verify_hid_entitlement(path: Path) -> None:
    """PYHID check: ensure input26-service has HID event-dispatch entitlement."""
    if sys.platform == "darwin":
        subprocess.run(["/usr/bin/codesign", "--verify", "--strict", str(path)], check=True)
        res = subprocess.run(
            ["/usr/bin/codesign", "-d", "--entitlements", ":-", str(path)],
            capture_output=True,
            check=True,
        )
    else:
        ldid = os.environ.get("LDID", "ldid")
        res = subprocess.run(
            [ldid, "-e", str(path)],
            capture_output=True,
            check=True,
        )
    entitlements = plistlib.loads(res.stdout)
    if entitlements.get("com.apple.private.hid.client.event-dispatch") is not True:
        raise SystemExit(f"compile_targets.py: {path.name} requires the HID event-dispatch entitlement")


def sign_binary(path: Path, ent_path: Path = None) -> None:
    """Sign a Mach-O binary with ad-hoc signature and optional entitlements."""
    if sys.platform == "darwin":
        cmd = ["/usr/bin/codesign", "--force", "--sign", "-"]
        if ent_path:
            cmd += ["--entitlements", str(ent_path)]
        cmd.append(str(path))
        proc = subprocess.run(cmd, cwd=USERLAND_DIR, capture_output=True, text=True)
        if proc.returncode != 0:
            print(f"Error codesigning {path.name}:\n{proc.stderr}", file=sys.stderr)
            raise SystemExit(1)
    else:
        ldid = os.environ.get("LDID", "ldid")
        if ent_path:
            cmd = [ldid, f"-S{ent_path}", str(path)]
        else:
            cmd = [ldid, "-S", str(path)]
        proc = subprocess.run(cmd, cwd=USERLAND_DIR, capture_output=True, text=True)
        if proc.returncode != 0:
            print(f"Error codesigning {path.name} with ldid:\n{proc.stderr}", file=sys.stderr)
            raise SystemExit(1)


def is_binary_signed(path: Path) -> bool:
    """Check if a Mach-O binary has a valid code signature."""
    if not path.is_file():
        return False
    if sys.platform == "darwin":
        proc = subprocess.run(["/usr/bin/codesign", "-dv", str(path)], capture_output=True, text=True)
        return proc.returncode == 0
    else:
        try:
            data = path.read_bytes()
            if len(data) < 32:
                return False
            magic = struct.unpack_from("<I", data, 0)[0]
            if magic not in (0xfeedfacf, 0xfeedface):
                return False
            ncmds = struct.unpack_from("<I", data, 16)[0]
            offset = 32 if magic == 0xfeedfacf else 28
            for _ in range(ncmds):
                cmd, cmdsize = struct.unpack_from("<II", data, offset)
                if cmd == 0x1d:  # LC_CODE_SIGNATURE
                    return True
                offset += cmdsize
            return False
        except Exception:
            return False


def ensure_boot_binaries_signed(obj_dir: Path) -> None:
    """Ensure standalone boot binaries (init, child, stage0) in obj/ are ad-hoc signed."""
    boot_binaries = ["init", "child", "stage0"]
    for name in boot_binaries:
        bin_path = obj_dir / name
        if bin_path.is_file() and not is_binary_signed(bin_path):
            sign_binary(bin_path)


def verify_arm64e_arch(obj_dir: Path) -> None:
    """PYARCH check: ensure all dynamic binaries are thin arm64e with LIB64 bit."""
    MH_MAGIC_64, FAT_MAGIC, FAT_CIGAM = 0xfeedfacf, 0xcafebabe, 0xbebafeca
    CPU_TYPE_ARM64, ARM64E, LIB64 = 0x0100000c, 2, 0x80000000
    MH_EXECUTE, MH_DYLIB, MH_BUNDLE, LC_LOAD_DYLINKER = 2, 6, 8, 0xe
    WANT = ARM64E | LIB64
    bad = []

    for item in sorted(obj_dir.iterdir()):
        if not item.is_file() or item.is_symlink():
            continue
        try:
            head = item.read_bytes()[:32]
        except OSError:
            continue
        if len(head) < 4:
            continue
        magic = struct.unpack("<I", head[:4])[0]
        if magic in (FAT_MAGIC, FAT_CIGAM):
            bad.append((item.name, "is a fat binary, and gate requires one thin arm64e slice"))
            continue
        if magic != MH_MAGIC_64 or len(head) < 32:
            continue
        cputype, cpusubtype, filetype, ncmds = struct.unpack("<IIII", head[4:20])
        dynamic = False
        if filetype in (MH_DYLIB, MH_BUNDLE):
            dynamic = True
        elif filetype == MH_EXECUTE and cputype == CPU_TYPE_ARM64:
            with open(item, "rb") as f:
                f.seek(32)
                for _ in range(ncmds):
                    cmd_data = f.read(8)
                    if len(cmd_data) < 8:
                        break
                    cmd_id, cmd_size = struct.unpack("<II", cmd_data)
                    if cmd_id == LC_LOAD_DYLINKER:
                        dynamic = True
                        break
                    f.seek(cmd_size - 8, 1)
        if not dynamic:
            continue
        if cputype != CPU_TYPE_ARM64:
            bad.append((item.name, f"is cputype 0x{cputype:x}, expected 0x{CPU_TYPE_ARM64:x}"))
        elif cpusubtype != WANT:
            bad.append((item.name, f"is cpusubtype 0x{cpusubtype:x}, expected 0x{WANT:x}"))

    if bad:
        for name, reason in bad:
            print(f"compile_targets.py: {name} {reason}", file=sys.stderr)
        raise SystemExit(1)


def build_target(target: dict, obj_dir: Path, drv_dir: Path, cache: dict, force: bool = False) -> tuple[str, bool, dict]:
    """Compile and sign a single target with -MMD dependency discovery and caching."""
    name = target["name"]
    target_type = target["type"]
    out_path = obj_dir / name
    dep_dir = obj_dir / ".deps"
    dep_file = dep_dir / f"{name}.d"

    # Resolve sources
    sources = []
    for s in target["sources"]:
        s_resolved = s.replace("{DRV}", str(drv_dir))
        p = Path(s_resolved)
        if not p.is_absolute():
            p = USERLAND_DIR / p
        p = p.resolve()
        if not p.is_file():
            raise FileNotFoundError(f"Source file not found for {name}: {p}")
        sources.append(p)

    # Entitlements path if any
    ent_path = None
    sign_ent_path = None
    if target.get("entitlements"):
        ent_p = Path(target["entitlements"])
        if not ent_p.is_absolute():
            ent_p = USERLAND_DIR / ent_p
        ent_path = ent_p.resolve()
        if not ent_path.is_file():
            raise FileNotFoundError(f"Entitlements file not found for {name}: {ent_path}")
        ent_text = ent_path.read_text(errors="replace")
        clean_text = re.sub(r"<!--.*?-->", "", ent_text, flags=re.DOTALL).strip()
        sign_ent_path = dep_dir / f"{name}.entitlements"
        sign_ent_path.write_text(clean_text)

    # Build command line(s) and dependency file list
    dep_files = []
    if len(sources) == 1:
        dep_files = [dep_file]
        cmd = list(CLANG_CMD) + list(BASE_FLAGS)
        for inc in INC_FLAGS:
            cmd.append(inc)
        cmd.append(f"-I{drv_dir}")

        if target_type == "dylib":
            cmd += ["-dynamiclib", "-install_name", target["install_name"]]
        elif target_type == "bundle":
            cmd.append("-bundle")

        cmd += target["flags"]
        cmd += ["-MMD", "-MF", str(dep_file), str(sources[0]), "-o", str(out_path)]
        cmd_repr = " ".join(cmd)
        cmd_hash = hashlib.sha256(cmd_repr.encode()).hexdigest()
        compile_cmds = [cmd]
        link_cmd = None
    else:
        obj_files = []
        compile_cmds = []
        for idx, src in enumerate(sources):
            dep_i = dep_dir / f"{name}_{idx}_{src.stem}.d"
            obj_i = dep_dir / f"{name}_{idx}_{src.stem}.o"
            dep_files.append(dep_i)
            obj_files.append(obj_i)
            cmd_i = list(CLANG_CMD) + list(BASE_FLAGS)
            for inc in INC_FLAGS:
                cmd_i.append(inc)
            cmd_i.append(f"-I{drv_dir}")
            cmd_i += target["flags"]
            cmd_i += ["-Wno-unused-command-line-argument", "-MMD", "-MF", str(dep_i), "-c", str(src), "-o", str(obj_i)]
            compile_cmds.append(cmd_i)

        link_cmd = list(CLANG_CMD) + list(BASE_FLAGS)
        for inc in INC_FLAGS:
            link_cmd.append(inc)
        link_cmd.append(f"-I{drv_dir}")
        if target_type == "dylib":
            link_cmd += ["-dynamiclib", "-install_name", target["install_name"]]
        elif target_type == "bundle":
            link_cmd.append("-bundle")
        link_cmd += target["flags"]
        for obj_i in obj_files:
            link_cmd.append(str(obj_i))
        link_cmd += ["-o", str(out_path)]

        cmd_repr = "; ".join([" ".join(c) for c in compile_cmds + [link_cmd]])
        cmd_hash = hashlib.sha256(cmd_repr.encode()).hexdigest()

    # Check cache: genuine recompilation if cache missing or deps altered
    if not force and out_path.is_file():
        if name in cache:
            entry = cache[name]
            if entry.get("cmd_hash") == cmd_hash:
                cached_deps = entry.get("deps", {})
                def _check_dep(p_str: str, exp_hash: str) -> bool:
                    dp = Path(p_str)
                    if not dp.is_absolute():
                        dp = USERLAND_DIR / dp
                    dp = dp.resolve()
                    return dp.is_file() and sha256_file(dp) == exp_hash

                if cached_deps and all(_check_dep(p_str, h) for p_str, h in cached_deps.items()):
                    return (name, False, entry)  # Up to date

    # Compile
    for cmd_i in compile_cmds:
        proc = subprocess.run(cmd_i, cwd=USERLAND_DIR, capture_output=True, text=True)
        if proc.returncode != 0:
            print(f"Error compiling {name}:\n{proc.stderr}", file=sys.stderr)
            raise SystemExit(1)

    if link_cmd:
        proc_link = subprocess.run(link_cmd, cwd=USERLAND_DIR, capture_output=True, text=True)
        if proc_link.returncode != 0:
            print(f"Error linking {name}:\n{proc_link.stderr}", file=sys.stderr)
            raise SystemExit(1)
        merged = "\n".join(df.read_text(errors="replace") for df in dep_files if df.is_file())
        if merged:
            dep_file.write_text(merged)

    # Codesign with strict entitlements validation
    sign_binary(out_path, sign_ent_path)

    if target.get("check_hid"):
        verify_hid_entitlement(out_path)

    # Record dependency hashes for cache
    header_deps = []
    for df in dep_files:
        header_deps.extend(parse_depfile(df))
    all_dep_paths = set(sources) | set(header_deps)
    if ent_path:
        all_dep_paths.add(ent_path)

    dep_hashes = {str(p): sha256_file(p) for p in sorted(all_dep_paths) if p.is_file()}

    cache_entry = {
        "cmd_hash": cmd_hash,
        "deps": dep_hashes,
        "output_sha256": sha256_file(out_path),
    }

    return (name, True, cache_entry)


def main():
    import argparse
    parser = argparse.ArgumentParser(description="Parallel incremental build runner for userland targets.")
    parser.add_argument("--obj-dir", type=Path, default=USERLAND_DIR / "obj", help="Path to obj output directory.")
    parser.add_argument("--drv-dir", type=Path, default=DEFAULT_DRV, help="Path to userland-driver directory.")
    parser.add_argument("--force", action="store_true", help="Force recompilation of all targets.")
    parser.add_argument("--minimal", action="store_true", help="Only build core system shims and input service (3 targets for V=26).")
    parser.add_argument("-j", "--jobs", type=int, default=min(32, os.cpu_count() or 4), help="Number of worker threads.")
    parser.add_argument("--verbose", "-v", action="store_true", help="Verbose output.")
    args = parser.parse_args()

    obj_dir = args.obj_dir
    if not obj_dir.is_absolute():
        obj_dir = (Path.cwd() / obj_dir).resolve()
    obj_dir.mkdir(parents=True, exist_ok=True)
    dep_dir = obj_dir / ".deps"
    dep_dir.mkdir(parents=True, exist_ok=True)

    cache_file = dep_dir / ".build-cache.json"
    cache = {}
    if not args.force and cache_file.is_file():
        try:
            cache = json.loads(cache_file.read_text())
        except Exception:
            cache = {}

    try:
        if args.drv_dir.is_absolute():
            raw_drv = args.drv_dir
        elif (USERLAND_DIR / args.drv_dir).is_dir():
            raw_drv = USERLAND_DIR / args.drv_dir
        elif (Path.cwd() / args.drv_dir).is_dir():
            raw_drv = Path.cwd() / args.drv_dir
        else:
            raw_drv = USERLAND_DIR / args.drv_dir
        drv_dir = Path(os.path.relpath(raw_drv.resolve(), USERLAND_DIR.resolve()))
    except ValueError:
        drv_dir = args.drv_dir

    target_list = TARGETS
    if args.minimal:
        core_names = {"input26-service", "libannibale-sb26.dylib", "libannibale-sbmetal-experiment.dylib"}
        target_list = [t for t in TARGETS if t["name"] in core_names]

    total_targets = len(target_list)
    compiled_count = 0
    cached_count = 0

    with ThreadPoolExecutor(max_workers=args.jobs) as pool:
        futures = {
            pool.submit(build_target, t, obj_dir, drv_dir, cache, args.force): t["name"]
            for t in target_list
        }
        for fut in as_completed(futures):
            t_name = futures[fut]
            try:
                name, compiled, entry = fut.result()
                if entry:
                    cache[name] = entry
                if compiled:
                    compiled_count += 1
                    if args.verbose:
                        print(f"  [compiled] {name}")
                else:
                    cached_count += 1
                    if args.verbose:
                        print(f"  [cached]   {name}")
            except Exception as e:
                print(f"Target build failed: {t_name}: {e}", file=sys.stderr)
                sys.exit(1)

    # Save cache
    cache_file.write_text(json.dumps(cache, indent=2, sort_keys=True) + "\n")

    # Ensure boot binaries in obj/ are signed
    ensure_boot_binaries_signed(obj_dir)

    # Architecture verification gate
    verify_arm64e_arch(obj_dir)

    print(f"compile_targets.py: {total_targets} targets verified ({compiled_count} compiled, {cached_count} cached up-to-date).")
    return 0


if __name__ == "__main__":
    sys.exit(main())
