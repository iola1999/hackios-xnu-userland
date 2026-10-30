#!/usr/bin/env python3
"""Read the refused AIR modules back out of a boot log.

usage: airfail-extract.py OUT_DIR [LOG|RUN_DIR]...

libredwoodmetal prints a render pipeline it could not build, and redwoodmetal.m's air_fail_dump
prints the module that was refused with it, as base64 between `AIRFAIL begin` and `AIRFAIL end`
lines. Those lines are the only road a module has out of a running daemon: the loader keeps the
console and the harness snapshots it every 20 s, so what the phone held is here.

Each module is verified before it is written -- the byte count and the FNV-1a of the begin line
have to come out of the decoded payload -- because the loader's ring drops the oldest megabyte and
a block can be cut in half, and because two processes sharing one console can interleave. A block
that does not verify is reported and not written; a half-written module is worse than a missing
one, since the frontend translates whatever bytes it is given.

Arguments are log files or run directories; a directory contributes every console snapshot the
harness saved (s*.log, log-*.bin and the teardown files), which overlap by design, so a module
seen in two snapshots is written once.

The .metallib files this writes are what the host translator takes:
    build/air_to_nir_test <module>.metallib      (or the draw sweep, docs/air.md)
"""
from pathlib import Path
import base64
import binascii
import json
import re
import sys

BEGIN = re.compile(r"AIRFAIL begin (\S+) (\S+) (\d+) bytes fnv=([0-9a-f]{8})")
PAYLOAD = re.compile(r"AIRFAIL ([A-Za-z0-9+/=]+)\s*$")
END = re.compile(r"AIRFAIL end (\S+) (\S+)")
OVER_CAP = re.compile(r"AIRFAIL over-cap (\S+) (\S+) (\d+) bytes, the cap is (\d+)")


def fnv1a(data):
    hash = 2166136261
    for byte in data:
        hash = ((hash ^ byte) * 16777619) & 0xFFFFFFFF
    return hash


def console_logs(arguments):
    """Every file named on the command line, expanding a run directory to its console snapshots.

    The pattern is signals.py's: s*.log alone would take system-upload.log, which is the fastboot
    transcript and not the console.
    """
    names = re.compile(r"(log(-\d+)?\.bin|(s\d+|returned|hung-held|running|device(-\d+)?)\.log)")
    paths = []
    for argument in arguments:
        path = Path(argument)
        if path.is_dir():
            paths += sorted(p for p in path.rglob("*") if p.is_file() and names.fullmatch(p.name))
        elif path.is_file():
            paths.append(path)
        else:
            sys.exit(f"{argument} is neither a file nor a directory")
    return paths


def blocks(paths):
    """The AIRFAIL blocks in these snapshots, in the order the snapshots were taken."""
    open_block = None
    for path in paths:
        # One console line can carry a prefix of its own (redwoodmetal[backboardd 11]:) and a
        # snapshot can be cut mid-line, so errors='replace' and a search rather than a match.
        for line in path.read_bytes().decode("utf-8", errors="replace").split("\n"):
            if "AIRFAIL" not in line:
                continue
            began = BEGIN.search(line)
            if began:
                # A begin inside an open block means the end was lost to the ring; drop the
                # partial rather than let its payload join the next module's.
                if open_block:
                    open_block["partial"] = True
                    yield open_block
                open_block = {"stage": began.group(1), "name": began.group(2),
                              "length": int(began.group(3)), "fnv": began.group(4),
                              "payload": [], "source": path.name, "partial": False}
                continue
            ended = END.search(line)
            if ended and open_block:
                open_block["partial"] = ended.groups() != (open_block["stage"], open_block["name"])
                yield open_block
                open_block = None
                continue
            payload = PAYLOAD.search(line)
            if payload and open_block:
                open_block["payload"].append(payload.group(1))
    if open_block:
        open_block["partial"] = True
        yield open_block


def label(block):
    """A file name from the stage and the Metal function name, which carries a bundle prefix."""
    tail = re.sub(r"[^A-Za-z0-9_-]", "_", block["name"].rsplit(".", 1)[-1])
    return f"{block['stage']}-{tail}-{block['fnv']}.metallib"


def decode(block):
    """The module's bytes, or None with the reason in 'problem'."""
    if block["partial"]:
        block["problem"] = "the block has no matching end: the loader's ring dropped part of it"
        return None
    try:
        data = base64.b64decode("".join(block["payload"]), validate=True)
    except (binascii.Error, ValueError) as error:
        block["problem"] = f"the base64 does not decode: {error}"
        return None
    if len(data) != block["length"]:
        block["problem"] = f"{len(data)} bytes decoded, the begin line says {block['length']}"
        return None
    if f"{fnv1a(data):08x}" != block["fnv"]:
        block["problem"] = f"the FNV-1a of the decoded bytes is {fnv1a(data):08x}, not {block['fnv']}"
        return None
    return data


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__.split("\n")[2])
    out = Path(sys.argv[1])
    out.mkdir(parents=True, exist_ok=True)
    paths = console_logs(sys.argv[2:])
    written, rejected, over_cap, seen = [], [], [], set()

    for path in paths:
        for line in path.read_bytes().decode("utf-8", errors="replace").split("\n"):
            found = OVER_CAP.search(line)
            if found and found.groups() not in over_cap:
                over_cap.append(found.groups())

    for block in blocks(paths):
        key = (block["stage"], block["name"], block["fnv"])
        if key in seen:
            continue
        seen.add(key)
        data = decode(block)
        if data is None:
            rejected.append({k: block[k] for k in ("stage", "name", "length", "fnv", "source")}
                            | {"problem": block["problem"]})
            continue
        target = out / label(block)
        target.write_bytes(data)
        written.append({"file": str(target), "stage": block["stage"], "name": block["name"],
                        "bytes": len(data), "fnv": block["fnv"], "source": block["source"]})

    print(json.dumps({"snapshots": len(paths), "written": written, "rejected": rejected,
                      "over_cap": [{"stage": s, "name": n, "bytes": int(b), "cap": int(c)}
                                   for s, n, b, c in over_cap]}, indent=2))
    if not written:
        sys.exit(1)


if __name__ == "__main__":
    main()
