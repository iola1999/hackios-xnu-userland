#!/usr/bin/env python3
"""The corpus report: how far every module of every .metallib gets.

This is the project's progress meter (docs/air.md section 5). Each module goes
through stages, and each module is counted under the first one it fails:

  container   found in the .metallib and unwrapped
  reader      our bitcode reader read it (and agrees with llvm-dis: that is
              checked separately, by tools/air/reader-check.sh)
  translate   air_to_nir produced a shader and nir_validate passed
  ir3         ir3 compiled it for the Adreno without error

With --ir3 on a 26.x host the last column is dropped: upstream deleted the
standalone `ir3_cmdline` (986530d231, 25.3+) and left no equivalent, so the
table stops at translate and stderr says why -- the same downgrade as the
SKIPPED line in discardcheck.py, accounted for in
docs/analysis/mesa-rebase-2026-10.md 10.3.

Nothing here says a shader is correct: that needs the CPU reference and a run
on the GPU. A module that reaches ir3 has compiled, which is not the same
thing, and the report never claims otherwise.

usage: corpus.py [--ir3] [--json FILE] [--md FILE] FILE.metallib...
       corpus.py [--ir3] ... --list FILE      (one library path per line)
"""
import argparse
import collections
import json
import os
import subprocess
import sys
import tempfile

import air_env

ROOT = air_env.ROOT
CHIP_ID = air_env.DEFAULT_CHIP_ID
resolve_chip_id = air_env.resolve_chip_id
AIR2NIR = air_env.air2nir_path()
IR3 = air_env.ir3_path()


# The mains of the other tools: each one is linked into its own binary and into
# nothing else, so a change to it cannot make a different tool stale. Treating
# every file of tools/mesa/air as a dependency of every binary refused a run
# whose numbers were perfectly current -- which costs more than the guard
# saves, because the next refusal gets worked around rather than read.
TOOL_MAINS = {"air2nir.c": "air2nir", "air_run.c": "air_run",
              # the library the Metal driver links and its test (air_metal.h):
              # in no binary whose numbers this script reports, so a change to
              # them makes none of those stale.
              "air_metal.c": "libannibaleair", "air_metal.h": "libannibaleair",
              "air_metal_test.c": "air_metal_test"}


def refuse_if_stale(prog, what):
    """A tool older than the frontend it contains is measuring the past.

    ir3_compiler is not in the host build's default target, so a plain
    `ninja -C build/mesa-host/build` leaves it as it was while air2nir beside
    it is rebuilt. That produced a column of numbers from the old frontend and
    a translate column from the new one: on 20 October the two read 315 and
    313, and the missing two looked like modules ir3 could not compile. They
    compiled perfectly; the compiler was six hours old. Both columns came from
    the same table and only one of them was current, which is the worst case --
    a stale tool whose output is plausible beside a fresh one.

    So the age is checked rather than assumed, against every source of the
    frontend, and the run is refused instead of warned: a number nobody can
    trust is worse than no number.
    """
    if not os.path.exists(prog):
        return
    src = os.path.join(ROOT, "tools/mesa/air")
    newest, name = 0.0, None
    for f in os.listdir(src):
        if not f.endswith((".c", ".h")):
            continue
        if TOOL_MAINS.get(f) not in (None, os.path.basename(prog)):
            continue      # another tool's main: not linked into this binary
        t = os.path.getmtime(os.path.join(src, f))
        if t > newest:
            newest, name = t, f
    if newest and os.path.getmtime(prog) < newest:
        print("corpus.py: %s is older than tools/mesa/air/%s, so its column would be "
              "the previous frontend's. Build it: ninja -C build/mesa-host/build %s"
              % (what, name, os.path.relpath(prog, os.path.join(ROOT, "build/mesa-host/build"))),
              file=sys.stderr)
        sys.exit(1)


def read_report(paths, tempdir=None):
    """air2nir --report, which forks per module so one crash is one line."""
    rows = []
    skipped_archives = []
    owns_temp = tempdir is None
    if owns_temp:
        ctx = tempfile.TemporaryDirectory(prefix="air-corpus-")
        tempdir = ctx.__enter__()
    else:
        ctx = None
    try:
        for idx, path in enumerate(paths):
            lib_label = os.path.join(os.path.basename(os.path.dirname(path)),
                                     os.path.basename(path))
            target, kind = air_env.materialize_metallib(path, tempdir, "slice_%d" % idx)
            if target is None:
                skipped_archives.append((lib_label, kind))
                continue
            out = subprocess.run([AIR2NIR, "--report", target], capture_output=True, text=True)
            for line in out.stdout.splitlines():
                if not line.startswith("module="):
                    continue
                row = dict(kv.split("=", 1) for kv in line.split("\t") if "=" in kv)
                # Two of the corpus libraries are called default.metallib, so keep parent dir.
                row["library"] = lib_label
                row["path"] = target
                row["source_path"] = path
                calls_raw = row.get("calls", "").split("module=")[0]
                row["calls"] = [c for c in calls_raw.split(",") if c]
                rows.append(row)
    finally:
        if ctx is not None:
            ctx.__exit__(None, None, None)
    return rows, skipped_archives


def run_ir3(rows, chip_id):
    """ir3 on every module that translated, one process each."""
    for row in rows:
        if row.get("translate") != "ok":
            continue
        out = subprocess.run([IR3, "--chip-id", chip_id, row["path"], row["module"]],
                             capture_output=True, text=True, timeout=300)
        if out.returncode == 0:
            row["ir3"] = "ok"
            for line in out.stdout.splitlines():
                # "; FRAG prog 0/0: 5 instr, 0 nops, ..."
                if " prog " in line and " instr," in line:
                    row["ir3_instr"] = int(line.split(":")[1].split()[0])
                    break
        else:
            row["ir3"] = "fail"
            err = (out.stderr or out.stdout).strip().splitlines()
            row["ir3_error"] = err[-1][:200] if err else "no message"


def reason(row):
    if row.get("crash"):
        return "crashed: " + row["crash"]
    if row.get("reader") == "skip":
        return None
    if row.get("reader") != "ok":
        return row.get("error", "reader failed")
    if row.get("translate") != "ok":
        return row.get("error", "translation failed")
    if row.get("ir3") == "fail":
        return "ir3: " + row.get("ir3_error", "")
    return None


def short_reason(r):
    """Group reasons so the table ranks causes, not instances."""
    if r is None:
        return None
    r = r.split(" (block ")[0]
    if r.startswith("intrinsic: air."):
        name = r.split("intrinsic: ")[1].split("(")[0].strip()
        # air.sample_texture_2d.v4f16 -> air.sample_texture_2d
        base = name.split(".")
        if len(base) > 2:
            name = ".".join(base[:2]) if base[1] in ("atomic", "wg") else ".".join(base[:2])
        return "intrinsic: " + name
    return r


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("libs", nargs="*")
    # The system's corpus is five files and fits on a command line. A game's
    # does not: RE Engine compiles every shader variant into a .metallib of its
    # own and one pak holds 7 879 of them, which is past what argv takes, so
    # the list can come from a file (one path per line) instead.
    ap.add_argument("--list", help="file of library paths, one per line")
    ap.add_argument("--ir3", action="store_true", help="also compile through ir3")
    ap.add_argument("--chip-id", default=CHIP_ID,
                    help="Adreno chip ID or alias (default: %s / a830)" % CHIP_ID)
    ap.add_argument("--json")
    ap.add_argument("--md")
    args = ap.parse_args()
    chip_id = resolve_chip_id(args.chip_id)

    libs = list(args.libs)
    if args.list:
        with open(args.list) as f:
            libs += [ln.strip() for ln in f if ln.strip()]
    if not libs:
        ap.error("no libraries: give paths, or --list FILE")

    with tempfile.TemporaryDirectory(prefix="air-corpus-") as tempdir:
        rows, skipped_archives = read_report(libs, tempdir)
        if not rows and not skipped_archives:
            sys.exit("corpus.py: no modules; is %s built?" % AIR2NIR)
        ir3_state = "off"
        if args.ir3:
            if os.path.exists(IR3):
                refuse_if_stale(IR3, "ir3_compiler")
                run_ir3(rows, chip_id)
                ir3_state = "ok"
            else:
                # 26.x has no offline compiler: keep ir3 separate as SKIPPED rather than passed.
                print("corpus.py: no offline ir3 compiler at %s: ir3 is SKIPPED (not counted as passed)" % IR3,
                      file=sys.stderr)
                ir3_state = "skipped"

    total = len(rows)
    stages = collections.OrderedDict()
    stages["container"] = total
    stages["reader"] = sum(1 for r in rows if r.get("reader") == "ok")
    stages["translate"] = sum(1 for r in rows if r.get("translate") == "ok")
    skipped = {}
    if ir3_state == "ok":
        stages["ir3"] = sum(1 for r in rows if r.get("ir3") == "ok")
    elif ir3_state == "skipped":
        stages["ir3"] = 0
        skipped["ir3"] = stages["translate"]
    if skipped_archives:
        skipped["archives"] = [lbl for lbl, _ in skipped_archives]

    def ir3_cell(rs):
        if ir3_state == "ok":
            return str(sum(1 for r in rs if r.get("ir3") == "ok"))
        if ir3_state == "skipped":
            return "SKIPPED (%d)" % sum(1 for r in rs if r.get("translate") == "ok")
        return "-"

    by_stage = collections.Counter(r.get("stage", "?") for r in rows)
    ok_by_stage = collections.Counter(r.get("stage", "?") for r in rows
                                      if r.get("translate") == "ok")
    reasons = collections.Counter(short_reason(reason(r)) for r in rows
                                  if reason(r) is not None)
    intrinsics = collections.Counter(c for r in rows for c in r["calls"])
    no_intrinsics = [r for r in rows if not r["calls"]]

    out = []
    out.append("# AIR corpus report\n")
    out.append("What every module of the system's Metal libraries does when put through the\n"
               "frontend. Generated by `tools/air/corpus.py`; see docs/air.md section 5 for\n"
               "what the stages mean. Target chip ID: `%s`. **Reaching ir3 means it compiled,\n"
               "not that it is correct**: correctness needs the CPU reference and a run on the GPU.\n"
               % chip_id)
    out.append("## Stages\n")
    out.append("| stage | modules | of %d |" % total)
    out.append("| --- | --- | --- |")
    for name, n in stages.items():
        if name == "ir3" and ir3_state == "skipped":
            out.append("| %s | %d | 0%% (SKIPPED: %d) |" % (name, n, skipped["ir3"]))
        else:
            pct = (100.0 * n / total) if total else 0.0
            out.append("| %s | %d | %.0f%% |" % (name, n, pct))
    out.append("")

    if skipped_archives:
        out.append("## Skipped non-AIR libraries\n")
        out.append("| library | status |")
        out.append("| --- | --- |")
        for lbl, kind in skipped_archives:
            out.append("| %s | SKIPPED (%s) |" % (lbl, kind))
        out.append("")

    out.append("## By stage of the shader\n")
    out.append("| what the module is | modules | translate | ir3 |")
    out.append("| --- | --- | --- | --- |")
    for st in sorted(by_stage):
        rs = [r for r in rows if r.get("stage", "?") == st]
        out.append("| %s | %d | %d | %s |" % (st, by_stage[st], ok_by_stage[st], ir3_cell(rs)))
    out.append("")

    # The container's TYPE tag and the module's own metadata are two
    # independent statements of what a function is; if they ever disagree, one
    # of the two readings is wrong and the report says so rather than picking.
    same = {"vertex": "vertex", "fragment": "fragment", "kernel": "kernel",
            "visible": "visible"}
    mismatch = [r for r in rows
                if r.get("container") in same and r.get("stage")
                and same[r["container"]] != r.get("stage")]
    out.append("The container's TYPE tag agrees with the module's own metadata on %d of %d; "
               "%d disagree.\n" % (total - len(mismatch), total, len(mismatch)))
    for r in mismatch[:20]:
        out.append("- `%s`: container says %s, metadata says %s" %
                   (r["module"], r["container"], r["stage"]))
    if mismatch:
        out.append("")

    libnames = sorted({r["library"] for r in rows})
    out.append("## By library\n")
    # A row per library is the report for a handful of system libraries and
    # noise for thousands of single-function ones; past that the count is the
    # only thing the table would say that the stage table does not.
    if len(libnames) <= 24:
        out.append("| library | modules | translate | ir3 |")
        out.append("| --- | --- | --- | --- |")
        for lib in libnames:
            rs = [r for r in rows if r["library"] == lib]
            out.append("| %s | %d | %d | %s |" % (
                lib, len(rs), sum(1 for r in rs if r.get("translate") == "ok"),
                ir3_cell(rs)))
    else:
        out.append("%d libraries, %.2f modules each on average; not listed one by one.\n"
                   % (len(libnames), (float(total) / len(libnames)) if libnames else 0.0))
    out.append("")

    out.append("## Modules that call no `air.*` at all\n")
    no_pct = (100.0 * len(no_intrinsics) / total) if total else 0.0
    out.append("%d of %d (%.0f%%), of which %d translate and %s compile through ir3.\n" % (
        len(no_intrinsics), total, no_pct,
        sum(1 for r in no_intrinsics if r.get("translate") == "ok"),
        ir3_cell(no_intrinsics)))

    if reasons:
        out.append("## What is not handled yet, by how many modules it stops\n")
        out.append("| modules | where it stops |")
        out.append("| --- | --- |")
        for why, n in reasons.most_common():
            out.append("| %d | %s |" % (n, why.replace("|", "\\|")))
        out.append("")

    # Translated, and not exactly as asked. A refusal is counted above and a
    # clean translation is counted in the stage table; this is the third thing,
    # which used to have nowhere to be counted and therefore went unsaid.
    notes = collections.Counter()
    for r in rows:
        for note in (r.get("notes") or "").split(";"):
            if note:
                notes[note] += 1
    if notes:
        out.append("## What was translated with a caveat\n")
        out.append("A module here was accepted and something other than exactly what it "
                   "asked for was done. Not a refusal and not a warning about this "
                   "frontend's own code: a thing the hardware cannot do exactly, where "
                   "doing something else is still correct.\n")
        out.append("| modules | what was asked, and what was done |")
        out.append("| --- | --- |")
        for note, n in notes.most_common():
            out.append("| %d | %s |" % (n, note.replace("|", "\\|")))
        out.append("")

    if intrinsics:
        out.append("## The `air.*` functions the corpus calls\n")
        out.append("%d distinct names in %d modules.\n" % (
            len(intrinsics), sum(1 for r in rows if r["calls"])))
        out.append("| calls | name |")
        out.append("| --- | --- |")
        for name, n in intrinsics.most_common():
            out.append("| %d | `%s` |" % (n, name))
        out.append("")

    text = "\n".join(out)
    if args.md:
        with open(args.md, "w") as f:
            f.write(text)
    else:
        print(text)
    if args.json:
        with open(args.json, "w") as f:
            json.dump({"chip_id": chip_id, "total": total, "stages": stages,
                       "skipped": skipped, "rows": rows}, f, indent=1)

    parts = []
    for k, v in stages.items():
        if k == "ir3" and ir3_state == "skipped":
            parts.append("ir3 %d/%d (SKIPPED %d)" % (v, total, skipped["ir3"]))
        else:
            parts.append("%s %d/%d" % (k, v, total))
    if skipped_archives:
        parts.append("archives SKIPPED %d" % len(skipped_archives))
    print("corpus.py (chip %s): %s" % (chip_id, ", ".join(parts)), file=sys.stderr)


if __name__ == "__main__":
    main()
