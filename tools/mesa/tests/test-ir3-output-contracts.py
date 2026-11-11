#!/usr/bin/env python3
"""Build and run the old/fixed ir3 output tests without a GPU or shared build."""

import argparse
import hashlib
import importlib.util
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tarfile


VERSION = "26.2.5"
ARCHIVE_SHA256 = "794b3d1729075dae7ebd47c7283b7ca0cdb5dec668f7f11cc0a5d82f6138a0a5"
TARGET = "src/freedreno/ir3/ir3_output_contracts"
REGISTRATION = """
executable('ir3_output_contracts', 'tests/output-contracts.c',
  link_with: libfreedreno_ir3,
  dependencies: [idep_mesautil, idep_nir],
  include_directories: [inc_freedreno, inc_include, inc_src])
"""


def run(command, work, env, log_name, expected=0, stdin=None):
    log = work / log_name
    print(f"Running {command[0]} -> {log.name}", flush=True)
    with log.open("w") as output:
        result = subprocess.run(command, cwd=work, env=env, stdin=stdin,
                                stdout=output, stderr=subprocess.STDOUT)
    if result.returncode != expected:
        raise RuntimeError(f"{log}: exit {result.returncode}, expected {expected}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--archive", required=True, type=Path,
                        help="local mesa-26.2.5.tar.xz (hash checked)")
    parser.add_argument("--work-dir", required=True, type=Path,
                        help="new directory for isolated source, build and logs")
    parser.add_argument("--jobs", type=int, default=4)
    args = parser.parse_args()
    if args.jobs < 1:
        parser.error("--jobs must be positive")
    if sys.version_info < (3, 12):
        parser.error("Python 3.12 or later is required")

    for module in ("mako", "yaml", "packaging", "mesonbuild"):
        if importlib.util.find_spec(module) is None:
            parser.error(f"the current Python needs {module}")
    env = os.environ.copy()
    env["PATH"] = str(Path(sys.executable).parent) + os.pathsep + env["PATH"]
    # Compiler debugging options and shader overrides change the tested passes.
    for key in ("IR3_SHADER_DEBUG", "IR3_SHADER_OVERRIDE_PATH", "NIR_DEBUG"):
        env.pop(key, None)
    for program in ("ninja", "bison", "flex", "patch", "pkg-config"):
        if shutil.which(program, path=env["PATH"]) is None:
            parser.error(f"{program} must be available in PATH")

    archive = args.archive.resolve(strict=True)
    with archive.open("rb") as source:
        digest = hashlib.file_digest(source, "sha256").hexdigest()
    if digest != ARCHIVE_SHA256:
        parser.error("archive SHA-256 differs from the pinned Mesa 26.2.5 release")
    work = args.work_dir.resolve()
    work.mkdir(parents=True, exist_ok=False)
    with tarfile.open(archive) as source:
        source.extractall(work, filter="data")
    mesa = work / f"mesa-{VERSION}"
    tests = Path(__file__).resolve().parent
    shutil.copyfile(tests / "ir3-output-contracts.c",
                    mesa / "src/freedreno/ir3/tests/output-contracts.c")
    with (mesa / "src/freedreno/ir3/meson.build").open("a") as build_file:
        build_file.write(REGISTRATION)

    setup = [sys.executable, "-m", "mesonbuild.mesonmain", "setup",
             "build", str(mesa), "--wrap-mode=nofallback",
             "-Dplatforms=", "-Dgallium-drivers=softpipe", "-Dvulkan-drivers=",
             "-Dtools=freedreno", "-Dglx=disabled", "-Degl=disabled",
             "-Dllvm=disabled", "-Dlibunwind=disabled", "-Dbuild-tests=false",
             "-Dvideo-codecs=", "-Dgallium-va=disabled", "-Dglvnd=disabled"]
    run(setup, work, env, "setup.log")
    build = ["ninja", "-C", "build", "-j", str(args.jobs), TARGET]
    run(build, work, env, "baseline-build.log")
    executable = str(work / "build" / TARGET)
    run([executable], work, env, "baseline.log", expected=1)

    patch = tests.parent / "rebase-26/0018-ir3-alias-relative-and-fsat.patch"
    with patch.open() as source:
        run(["patch", "--batch", "--forward", "-p1", "-d", str(mesa)],
            work, env, "patch.log", stdin=source)
    run(build, work, env, "fixed-build.log")
    run([executable], work, env, "fixed.log")
    print(f"PASS: baseline tests fail and patched tests pass. Logs: {work}")


if __name__ == "__main__":
    try:
        main()
    except (OSError, RuntimeError, tarfile.TarError) as error:
        sys.exit(str(error))
