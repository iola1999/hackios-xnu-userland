"""Shared Adreno target defaults and host tool discovery for tools/air scripts."""
import os
import shutil
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
TOOLS_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
if TOOLS_DIR not in sys.path:
    sys.path.insert(0, TOOLS_DIR)

import metallib  # noqa: E402

A830_CHIP_ID = "0x44050001"
A635_CHIP_ID = "0x007506030500"
CHIP_ID_ALIASES = {"a830": A830_CHIP_ID, "a635": A635_CHIP_ID}
# Default to K90's Adreno 830 (gen8_0_1); override via AIR_CHIP_ID or --chip-id.
DEFAULT_CHIP_ID = os.environ.get("AIR_CHIP_ID", A830_CHIP_ID)


def resolve_chip_id(value):
    """Expand a short target alias (a830, a635) or pass through a hex chip ID."""
    return CHIP_ID_ALIASES.get(value.lower(), value)


def find_tool(env_name, candidates):
    """Return an explicit environment override or the first existing host tool binary."""
    env = os.environ.get(env_name)
    if env:
        return env
    for path in candidates:
        if path and os.path.exists(path):
            return path
    return candidates[0]


def air2nir_path():
    return find_tool("AIR2NIR", [
        os.path.join(ROOT, "build/mesa-host/build/src/compiler/air/air2nir"),
        os.path.join(ROOT, "build/mesa-host-replay/air2nir"),
        os.path.join(os.path.dirname(ROOT), "out/mesa-host-build/air2nir"),
    ])


def air_run_path():
    return find_tool("AIR_RUN", [
        os.path.join(ROOT, "build/mesa-host/build/src/gallium/drivers/freedreno/air_run"),
        os.path.join(ROOT, "build/mesa-host-replay/air_run"),
    ])


def ir3_path():
    return find_tool("IR3", [
        os.path.join(ROOT, "build/mesa-host/build/src/gallium/drivers/freedreno/ir3_compiler"),
    ])


def llvm_as_path():
    return find_tool("LLVM_AS", [
        shutil.which("llvm-as") or "",
        "/opt/homebrew/opt/llvm/bin/llvm-as",
        "/usr/local/opt/llvm/bin/llvm-as",
    ])


def materialize_metallib(path, tempdir, tag="slice"):
    """Extract an MTLB slice from fat containers and skip Mach-O precompiled archives."""
    with open(path, "rb") as f:
        magic = f.read(4)
        if magic in (b"\xcf\xfa\xed\xfe", b"\xfe\xed\xfa\xcf"):
            return None, "precompiled-bin"
        if magic != b"\xca\xfe\xba\xbe":
            return path, "raw"
        f.seek(0)
        raw = f.read()
    unwrapped, kind = metallib.unwrap_fat_metallib(raw)
    if kind != "fat":
        return None, kind
    out_path = os.path.join(tempdir, "%s.metallib" % tag)
    with open(out_path, "wb") as out_f:
        out_f.write(unwrapped)
    return out_path, "fat"
