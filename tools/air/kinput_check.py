"""--check LOG verification helper for kinput.py."""
import re

def bind(ns):
    for k in ("KERNELS", "wrap"):
        globals()[k] = ns[k]

# ---------------------------------------------------------------------------
# --check LOG: what the device did, against what the CPU did
#
# The bytes above are an input. These are the output the same input produced on
# the CPU, where the module runs as Apple's own LLVM IR -- air2nir --ll printed
# it, the air64 triple and the address spaces were dropped, the air.* calls got
# host bodies, and a driver bound sixteen zeroed 4096-byte buffers and ran a
# 4x4 grid one thread at a time. All eleven returned there.
#
# So this table is a SECOND REFERENCE and not a specification: it says what
# Apple's own instructions compute from these bytes, with our translation and
# the hardware both out of the picture. A device run that disagrees with it is
# therefore a statement about our translation or about the Adreno -- which is
# the whole point of writing it down -- and a device run that agrees proves
# both only for the words named here.
#
# Each entry is a list of (slot, byte offset, hex) that MUST differ afterwards
# and hold exactly those bytes, plus `partial` when the kernel also changes
# words this table does not predict, and `texels` for the two that write a
# texture instead of a buffer. An empty list means NOTHING may change, which
# for three of these kernels is the measurement (the association branch they
# must not take writes 900 KiB out of bounds).
#
# Reproducing the reference is a shell script's worth of work and is written
# down in docs/air.md section 5; it is deliberately not a repo tool, because it
# carries Apple's modules as text.
AFTER = {
    "MetalFaceMask_AddSeedPoint":      dict(bufs=[(3, 28, "5a")]),
    "MetalFaceMask_ConnectSpans":      dict(bufs=[]),
    "MetalFaceMask_PropagateParents":  dict(bufs=[]),
    "MetalFaceMask_ResolveAssociations": dict(bufs=[], weak="its loop is never entered, so "
                                             "agreeing here is writing nothing"),
    "MetalFaceMask_DrawSpans":         dict(bufs=[], texels=(64, 1.0)),
    "MetalFaceMask_DrawEye":           dict(bufs=[], texels=(64, 1.0)),
    "MetalFaceMask_GenerateToothMask": dict(bufs=[(4, 0, "ffffffffffffffff")]),
    # Sixteen threads claim span indices through one atomic counter, so what lands in
    # buffer 4 depends on the order they get them in: the CPU's 0c 00 0d 00 0e 00 0f 00
    # is one legal outcome and not the only one. It is therefore not required here.
    "MetalFaceMask_GenerateMask":      dict(bufs=[(3, 8, "10")], partial=True,
                                            weak="the span indices come from an atomic "
                                            "counter, so buffer 4 is order-dependent and no "
                                            "byte of it can be required"),
    "fbs_bistochastize_iter":          dict(bufs=[(3, 0, "3acd133f" * 4)]),
    # The reduction word is deliberately NOT here. Both kernels end with
    # `if (tid == 0)` summing all 128 entries of a threadgroup array and one
    # atomic_fetch_add, and `tid` is Metal's scalar thread_position_in_threadgroup
    # -- the x component, not the flattened index. Under air_run's 4x4x1 group
    # four threads have tid == 0, so four of them run the whole reduction and
    # each adds its own copy; only x = 0..3 ever index the array, so 4 of the 128
    # entries are written and 124 are whatever was left in threadgroup memory.
    # The word is therefore 4 x (the 4 entries + that leftover), which is a
    # property of the dispatch. It is not a reference the CPU can state either:
    # the simulator runs the sixteen threads one after another and every thread
    # with gid == 0 re-zeroes the counter before adding, so only the last of the
    # four adds survives -- walk the same sixteen threads with x in the outer
    # loop instead and the same bytes give 0x00040000 instead of 0x00100000.
    # docs/air.md section 5 has the four measurements this is read off.
    "fbs_pcg_init":                    dict(bufs=[(6, 2, "80bf"), (6, 6, "80bf"),
                                                  (6, 10, "80bf"), (6, 14, "80bf"),
                                                  (7, 2, "80be"), (7, 6, "80be"),
                                                  (7, 10, "80be"), (7, 14, "80be")],
                                            partial=True,
                                            weak="lnew_out_buf word 0 cannot be an equality "
                                            "under this dispatch: four threads of the 4x4x1 "
                                            "group have tid == 0 and each adds a sum over "
                                            "128 threadgroup entries of which the dispatch "
                                            "writes 4. 0x00400000 is 4 x (4 x 0x00040000), "
                                            "and the CPU's 0x00100000 is one add of the same "
                                            "sum because its threads ran one at a time. A "
                                            "128x1x1 group makes it the equality 0x00100000"),
    "fbs_pcg_iter1":                   dict(bufs=[(5, 2, "c03f"), (5, 6, "c03f"),
                                                  (5, 10, "c03f"), (5, 14, "c03f")],
                                            partial=True,
                                            weak="alpha_out_buf word 0 cannot be an equality "
                                            "for the same reason as fbs_pcg_init's "
                                            "lnew_out_buf: 0x00c00000 in one boot is 4 x the "
                                            "sum of its 4 written entries, and 0xe3800000 in "
                                            "another is 4 x (that sum + 0x38800000 left in "
                                            "an entry nobody wrote)"),
}

# The canary of userland/rc.jobs.kinput, and what a healthy boot's dump of it
# looks like: six words and no others, measured on softpipe and on the device.
# After one hung dispatch this port's GPU stops making writes visible without
# saying so, and the canary is the only thing in a log that distinguishes "the
# kernel wrote nothing" from "nothing could write any more".
CANARY = "MetalFaceMask_InitMinMax"
CANARY_WORDS = 6


class Run(object):
    """One air_run job as a boot log records it."""

    def __init__(self, kernel, line):
        self.kernel = kernel
        self.line = line
        self.finished = True
        self.changed = {}      # slot -> {word: (before, after)}
        self.texels = None
        self.patched = {}      # slot -> bytes the --buffer flags put there
        self.fill = None
        self.cut = False       # the loader dropped the log under this run


JOB_RE = re.compile(r"^air_run: (\S+) on .*, as a compute kernel")
PATCH_RE = re.compile(r"^air_run: buffer at index (\d+), (\d+) of its (\d+) bytes from --buffer")
FILL_RE = re.compile(r"^air_run: dispatching .* filled with (.*)$")
BUF_RE = re.compile(r"^air_run: device buffer (\d+) after the dispatch")
WORD_RE = re.compile(r"^  changed \[\s*(\d+)\] 0x([0-9a-f]{8}) -> 0x([0-9a-f]{8})")
TEX_RE = re.compile(r"^air_run: image (\d+) after the dispatch")
TEXEL_RE = re.compile(r"^\s*\((\d+),\s*(\d+)(?:,\s*\d+)?\)\s+(\S+)")


CUT_RE = re.compile(r"the log between the first .* was dropped")


def read_log(path):
    """Every air_run compute job in a boot log, in the order it ran.

    The loader keeps the first 256 KB of a boot's log and the last 1 MB and
    drops what is between, leaving one marker where it did. A run the marker
    falls inside printed more than this log holds, and reading its silence as a
    result is how a harness limit gets reported as a difference -- so the run
    it interrupts is marked, and so is every kernel that is missing while a cut
    exists, because the missing one may be in the dropped middle.
    """
    runs, cur, slot, cut = [], None, None, False
    for line in open(path, errors="replace"):
        line = line.replace("\0", "").rstrip("\n")
        if CUT_RE.search(line):
            cut = True
            if cur is not None:
                cur.cut = True
            continue
        m = JOB_RE.match(line)
        if m:
            cur = Run(m.group(1).split("/")[-1], line)
            runs.append(cur)
            slot = None
            continue
        if cur is None:
            continue
        m = FILL_RE.match(line)
        if m:
            cur.fill = m.group(1)
        m = PATCH_RE.match(line)
        if m:
            cur.patched[int(m.group(1))] = int(m.group(2))
        m = BUF_RE.match(line)
        if m:
            slot = int(m.group(1))
            cur.changed.setdefault(slot, {})
            continue
        m = WORD_RE.match(line)
        if m and slot is not None:
            cur.changed[slot][int(m.group(1))] = (int(m.group(2), 16), int(m.group(3), 16))
            continue
        if "the dispatch did not finish" in line or "THE GPU DID NOT FINISH" in line:
            cur.finished = False
        m = TEX_RE.match(line)
        if m:
            cur.texels = []
            continue
        if cur.texels is not None:
            m = TEXEL_RE.match(line)
            if m:
                cur.texels.append(m.group(3))
    return runs, cut


def want_bytes(entry):
    """The (slot, byte) -> value the CPU reference asks for.

    Byte and not word granularity, because the reference's spans are not all
    word-aligned: a `ushort out_index` into a float array leaves two bytes
    written and two untouched inside one word, and a word comparison would
    either refuse those rows or invent the two bytes it does not know.
    """
    want = {}
    for slot, off, hexs in entry.get("bufs", []):
        for i, byte in enumerate(bytes.fromhex(hexs)):
            want[(slot, off + i)] = byte
    return want


def check_run(run, entry):
    """AGREE / DIFFER for one job, with the reason in words.

    The log prints every changed word as `old -> new`, so the expected new word
    is the old one with the reference's bytes laid into it. That is what makes a
    byte-level expectation checkable against a word-level dump at all.
    """
    if not run.finished:
        return "DID NOT FINISH", ["the dispatch did not end, so nothing below it is evidence"]
    if run.cut:
        return "LOG CUT", ["the loader dropped the log inside this run, so what it printed "
                           "after its bindings is not here to compare"]

    want = want_bytes(entry)
    notes, bad = [], False
    covered = set()

    for slot in sorted(run.changed):
        for w in sorted(run.changed[slot]):
            before, after = run.changed[slot][w]
            expect, touched = before, False
            for k in range(4):
                key = (slot, 4 * w + k)
                if key in want:
                    expect = (expect & ~(0xff << (8 * k))) | (want[key] << (8 * k))
                    covered.add(key)
                    touched = True
            if after == expect:
                continue
            if touched:
                notes.append("buffer %d word %d is 0x%08x, the CPU's bytes make it 0x%08x"
                             % (slot, w, after, expect))
                bad = True
            elif entry.get("partial"):
                notes.append("buffer %d word %d changed to 0x%08x, which this reference does "
                             "not predict" % (slot, w, after))
            else:
                notes.append("buffer %d word %d changed to 0x%08x and the CPU left it alone"
                             % (slot, w, after))
                bad = True

    missed = sorted(k for k in want if k not in covered)
    if missed:
        for slot, off in missed[:8]:
            notes.append("buffer %d byte %d should have become 0x%02x, and the word holding "
                         "it did not change at all" % (slot, off, want[(slot, off)]))
        if len(missed) > 8:
            notes.append("and %d more bytes the CPU wrote and this run did not"
                         % (len(missed) - 8))
        bad = True

    if entry.get("texels"):
        n, value = entry["texels"]
        if run.texels is None:
            notes.append("no texture was read back, and the CPU wrote %d texels of %g"
                         % (n, value))
            bad = True
        else:
            notes.append("a texture came back with %d texels printed; the CPU wrote %d of %g "
                         "(air_run prints a corner, so this is not a count)"
                         % (len(run.texels), n, value))
    if entry.get("weak"):
        notes.append("and this row is weak evidence whatever it says: %s" % entry["weak"])

    if bad:
        return "DIFFER", notes
    if not want and not entry.get("texels"):
        return "AGREE (nothing changed, as the CPU)", notes or ["not one word differs"]
    if entry.get("partial"):
        return "AGREE so far", notes
    return "AGREE", notes or ["every byte the CPU wrote, and no word it did not"]


def check(path):
    runs, cut = read_log(path)
    if not runs:
        print("kinput: %s holds no air_run compute job at all" % path)
        return 1

    # A log from before air_run printed the words a kernel changed carries no
    # such line at all, and then every dump reads as "wrote nothing" -- including
    # the canary's, which would put the boundary at job 1 and call a whole good
    # run unmeasured. Refuse to read it rather than report that.
    if not any(r.changed and any(w for w in r.changed.values()) for r in runs):
        print("kinput: %s was written before air_run printed the words a dispatch "
              "changed, so there is nothing here to compare. Re-run the list with a "
              "current air_run." % path)
        return 1

    # the canary first: everything after its last healthy dump is unmeasured, and
    # saying so before the verdicts keeps a wedged boot from reading as a result
    canaries = [i for i, r in enumerate(runs) if r.kernel == CANARY]
    boundary = len(runs)
    for i in canaries:
        words = sum(len(w) for w in runs[i].changed.values())
        if not runs[i].finished or words != CANARY_WORDS:
            boundary = i
            break
    print("kinput --check %s" % path)
    print("  %d air_run jobs, %d of them the canary" % (len(runs), len(canaries)))
    # What a DIFFER may be read as, printed where the verdicts are and not only
    # in this file's header. The reference has been re-derived with the grid
    # air_run actually dispatches -- 4x4x1, a scalar thread_position_in_grid
    # being x, so the ids are 0..3 with four threads each -- after a first
    # version of it got thread_position_in_threadgroup wrong for the two pcg
    # kernels. So a difference here is no longer about the two references
    # disagreeing on what a thread is. What it can still be about is the
    # compiler: two bugs in ir3 answer for every difference in this table as of
    # logs/065057-run.log, and a run with tools/mesa/ir3-ssbo-subdword.patch
    # and ir3-shift-split.patch in is the one that says so.
    print("  the reference is the same grid air_run dispatches (4x4x1, scalar ids 0..3),")
    print("  so a difference here is not the two references disagreeing about threads. It")
    print("  can still be the compiler: see tools/mesa/ir3-ssbo-subdword.patch and")
    print("  ir3-shift-split.patch, and whether this log was made with them in.")
    if boundary < len(runs):
        print("  the canary stopped writing at job %d of %d (%s): NOTHING from there on "
              "is evidence about a kernel" % (boundary + 1, len(runs), runs[boundary].kernel))
    elif canaries:
        print("  the canary wrote its %d words every one of its %d turns, so the GPU was "
              "answering for the whole run" % (CANARY_WORDS, len(canaries)))

    ledger, verdicts = {}, {}
    print()
    for i, run in enumerate(runs):
        if run.kernel == CANARY:
            continue
        entry = AFTER.get(run.kernel)
        if entry is None:
            verdict, notes = "NOT IN THE REFERENCE", ["this kernel has no CPU run written down"]
        elif i > boundary:
            verdict, notes = "UNMEASURED", ["it ran after the canary stopped writing"]
        else:
            verdict, notes = check_run(run, entry)
        # A kernel run twice in one boot is one row, not two: the list runs
        # ConnectSpans with its input and again with zeros, and counting both
        # made the ledger add to twelve out of eleven. Two runs that disagree
        # are their own verdict, because then the kernel is not deterministic
        # here and neither number is a measurement.
        if run.kernel in verdicts and verdicts[run.kernel] != verdict:
            ledger[verdicts[run.kernel]] -= 1
            verdict = "TWICE, AND NOT THE SAME"
            notes.append("an earlier run of it in this log said %s"
                         % verdicts[run.kernel])
        elif run.kernel in verdicts:
            print("  %-34s %s (again, same verdict)" % (run.kernel, verdict))
            for n in notes:
                print("        %s" % wrap(n, 8))
            continue
        verdicts[run.kernel] = verdict
        ledger[verdict] = ledger.get(verdict, 0) + 1
        print("  %-34s %s" % (run.kernel, verdict))
        if run.fill and "ZEROS" not in run.fill and entry is not None:
            print("        the fill was %s -- these bytes were written for zeros"
                  % run.fill.split(" (")[0])
        for n in notes:
            print("        %s" % wrap(n, 8))

    print()
    missing = [n for n, _b in KERNELS if not any(r.kernel == n for r in runs)]
    print("  ledger, which has to add up to the %d kernels this file knows:" % len(KERNELS))
    for verdict in sorted(ledger):
        print("    %3d %s" % (ledger[verdict], verdict))
    if missing:
        print("    %3d not in this log at all%s: %s"
              % (len(missing),
                 ", and this log has a cut, so they may be in the dropped middle" if cut
                 else "", ", ".join(missing)))
    total = sum(ledger.values()) + len(missing)
    extra = [r.kernel for r in runs if r.kernel != CANARY and r.kernel not in dict(KERNELS)]
    if extra:
        print("    (%d jobs in the log are not of this group: %s)"
              % (len(extra), ", ".join(sorted(set(extra)))))
    print("    %3d total" % total)
    if total != len(KERNELS):
        print("  kinput: the ledger does NOT add up, so read it as broken, not as a result")
        return 1
    return 0


