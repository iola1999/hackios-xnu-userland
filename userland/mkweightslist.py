#!/usr/bin/env python3
"""Generate weights.list, the manifest init binds the espresso weights by.

usage: python3 mkweightslist.py [COPY-MAP-JSON] [E5-BUILD-DIR]

COPY-MAP-JSON is the batch's deploy/copy-map-final.json (default: the batch 5 evidence path
relative to this checkout) and E5-BUILD-DIR the harness output with md7/jit.bundle and
md8/jit.bundle. The output is userland/weights.list, one tab-separated line per entry:

  f SIZE SRC TARGET SHA256   a single file: bind SRC onto TARGET, SIZE bytes expected
  d SIZE SRC TARGET -        a directory tree: bind SRC onto TARGET, SIZE = total bytes of the tree

SRC is relative to the staging tree /private/var/annibale-weights and mirrors the target's
position inside its framework (framework name without ".framework", then the rest of the path),
so the staging tree reads like the frameworks it feeds. TARGET is the absolute framework path.
The two mubb entries deploy the jit compile products under the universal.bundle name, as the
harness report recommends (e5-compile-harness.md §6): fast steady state, small memory.
"""
import json
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
BATCH = HERE.parent.parent / "out/goal-1124/batch5"


def framework_rel(target):
    """Staging-relative source path of a target inside SomeFramework.framework."""
    for prefix in ("System/Library/Frameworks/", "System/Library/PrivateFrameworks/"):
        if target.startswith(prefix):
            rest = target[len(prefix):]
            framework, _, inner = rest.partition(".framework/")
            if not inner:
                raise SystemExit(f"mkweightslist.py: {target} has nothing inside its framework")
            return f"{framework}/{inner}"
    raise SystemExit(f"mkweightslist.py: {target} is not inside a framework")


def tree_bytes(path):
    return sum(f.stat().st_size for f in Path(path).rglob("*") if f.is_file())


def main():
    copy_map = Path(sys.argv[1]) if len(sys.argv) > 1 else BATCH / "deploy/copy-map-final.json"
    e5_build = Path(sys.argv[2]) if len(sys.argv) > 2 else BATCH / "e5-build"
    entries = json.loads(copy_map.read_text())
    lines = [
        "# the espresso weights and mubb backbones mediaanalysisd's models load, bound into the",
        "# frameworks of the read-only system volume; see mkweightslist.py and",
        "# out/goal-1124/batch5/weights-bind-impl.md. init reads this as etc/weights.list.",
        "# f: size, staging source (in /private/var/annibale-weights), target, sha256 of the source file.",
        "# d: total bytes of the tree, staging source, target.",
    ]
    longest = 0
    total = 0
    for e in entries:
        rel = framework_rel(e["dst"])
        target = "/" + e["dst"]
        longest = max(longest, len(target.rsplit("/", 1)[-1].encode()))
        total += e["size"]
        lines.append(f'f\t{e["size"]}\t{rel}\t{target}\t{e["sha256"]}')
    for md in ("md7", "md8"):
        tree = e5_build / md / "jit.bundle"
        if not tree.is_dir():
            raise SystemExit(f"mkweightslist.py: {tree} is missing (the jit compile products)")
        size = tree_bytes(tree)
        total += size
        rel = f"MediaAnalysis/mubb_{md}.bundle/universal.bundle"
        target = f"/System/Library/PrivateFrameworks/MediaAnalysis.framework/mubb_{md}.bundle/universal.bundle"
        lines.append(f"d\t{size}\t{rel}\t{target}\t-")
    out = HERE / "weights.list"
    out.write_text("\n".join(lines) + "\n")
    n_files = sum(1 for line in lines if line.startswith("f\t"))
    print(f"mkweightslist.py: {out.name}: {n_files} files, 2 directory trees, "
          f"{total / 1e6:.1f} MB; longest target name {longest} bytes")
    if longest > 255:
        print("mkweightslist.py: a target name is over 255 bytes and cannot be a synthetic entry")
        return 1
    if longest > 96:
        print(f"mkweightslist.py: note: {longest} bytes needs the kernel with APFS_SYNTH_NAMELEN 256")
    return 0


if __name__ == "__main__":
    sys.exit(main())
