"""Whole-library --draw-all sweep comparison helper for metalcheck.py."""
import os
import re
import subprocess
import sys

def bind(ns):
    for k in (
        "AIR2NIR", "IOS", "NEW_SCHEME", "ROOT", "TEST_KEYS_ON_SIX",
        "TIMEOUT", "buflist", "host_pixel", "log_text", "status_scheme",
    ):
        globals()[k] = ns[k]

# --- the whole-library draw sweep ----------------------------------------------
#
# --draw-all draws every fragment shader of a library through the driver and prints
# one line each. Each of those shaders has a reference: air_run draws them all on
# softpipe. So the sweep can be compared shader by shader -- with one correction
# that is not optional.
#
# A shader whose result depends on a varying or on its position is kept apart from
# one whose result does not, and the reason has changed twice. It was read as the two
# back ends orienting the picture differently (docs/metal.md 10.26): the device
# printed at pixel (0,0) the texel softpipe printed at (0,3). That was wrong, and
# 10.32 is how: every varying was FLAT on the Adreno, because finalize_for_gallium
# called nir_lower_io without nir_lower_io_use_interpolated_input_intrinsics while
# softpipe inferred smooth interpolation from the variable. A flat varying puts the
# provoking vertex's texel under pixel (0,0) -- the same four numbers a mirrored
# picture would put there -- and one pixel cannot tell those apart. With the flag in,
# all sixteen pixels of that draw match the texture texel for texel
# (logs/090905-run.log).
#
# So these rows are no longer expected to differ at all. They are still counted
# separately, for two reasons: a log made before that fix still has them, and the
# orientation question itself is not closed -- Metal requires clip y = +1 in memory
# row 0 and our viewport puts clip y = -1 there (10.28), a one-line change that is
# written down and deliberately not landed. When it lands, these are the rows that
# move, and a row that can move for a known reason should not sit inside a count of
# agreements.
# The trailing group holds EVERY " tN w w w w" the line carries, and getting that
# wrong was the whole of 816462e's 65 uncompared rows. The pattern there ended in
# `((?: t\d (?:\d+ ?){4})*)`, whose inner `\d+ ?` eats the space in front of the
# next "t"; the outer `*` then stops after one repetition, and -- because the group
# is the last thing in the pattern -- nothing afterwards fails, so nothing forces
# the engine to backtrack and the match is accepted with t0 alone. The device had
# been printing all three attachments since 816462e and this read one of them, which
# is the same mistake that commit's own message reports fixing on the device side.
# Hence `\s+` before each "t" and `(?:\s+\d+)`, which cannot cross it.
RE_DA = re.compile(r"drvtest: drawall (\S+)\s+status (\d+)((?:\s+t\d(?:\s+\d+){4})*)")
RE_DA_T = re.compile(r"t(\d)((?:\s+\d+){4})")
# A second reader of the same line, written to share nothing with the first: the
# whole rest of the line, and the "tN" markers counted in it. The two must agree
# on how many attachments the line carries. That disagreement is the only thing
# that would have caught 816462e's mistake on its first row -- the sweep was
# internally consistent while reading one attachment of three, and nothing in the
# numbers it printed could show it. One regex for the structure and one for the
# count, and a mismatch is this tool's failure and said so.
RE_DA_LINE = re.compile(r"drvtest: drawall (\S+)\s+status (\d+)(.*)$", re.M)
RE_DA_LOOSE = re.compile(r"(?<![0-9A-Za-z_])t\d(?=\s|$)")
# The two outcomes that are NOT a "status" line, and so do not match RE_DA at all.
# Both were invisible to this tool until they were looked for: `no pipeline` has
# always been printed and has always been zero, and `no texture` arrived with the
# per-shader texture binding. A row the sweep printed and this did not read is the
# failure mode the whole ledger exists against -- the sweep would have said 271
# shaders and this would have counted 265 without a word.
RE_DA_NOPIPE = re.compile(r"drvtest: drawall (\S+)\s+no pipeline: (.*)$", re.M)
RE_DA_NOTEX = re.compile(r"drvtest: drawall (\S+)\s+no texture: (.*)$", re.M)
# What was actually bound, at the end of a drawn row: "tex 0=2:125 1=7:123", as
# slot=MTLTextureType:MTLPixelFormat. A row of a table is a measurement only if
# what it was measured against is in it.
RE_DA_TEX = re.compile(r"(?<![0-9A-Za-z_])tex((?:\s+\d+=\d+:\d+)+)")
RE_DA_TEX_ONE = re.compile(r"(\d+)=(\d+):(\d+)")
# The sweep's own summary, read as a LIST of outcomes rather than as a fixed
# number of them. It had three, it has four, and the fourth appeared while this
# tool was being written -- so an unknown phrase with a non-zero count beside it
# is a failure here and not something to skip past. The parenthesis differs
# between the two shapes too (the newer one names how many shaders asked for a
# texture and got it), so everything after the list is optional.
RE_DA_TOTAL = re.compile(r"drvtest: drawall: ([^(\n]+?)\s*"
                         r"\(of the (\d+) fragment shaders from index (\d+)"
                         r"(?:; (\d+) declared a texture and were bound what they "
                         r"declared)?\)")
RE_DA_ITEM = re.compile(r"^(\d+) (.+)$")
# A third reader, dumber than both: every per-shader line of the sweep, whatever
# it goes on to say. The summary line is "drvtest: drawall: ..." with a colon, so
# it is not one of these.
#
# It exists because of a hole the other checks did not close. Blinded to the "no
# texture" rows, this tool read 13 of the sweep's 19 shaders, found its own ledger
# six short, and blamed the loader's cut -- the arithmetic added up and the
# sentence was false. A cut REMOVES lines; an outcome nobody taught this to read
# leaves the line sitting in the log. Counting the lines tells those two apart,
# and nothing else here does.
RE_DA_ANY = re.compile(r"drvtest: drawall (\S+)")
# phrase -> which of this section's columns the sweep is counting
DA_OUTCOMES = {
    "drew": "drew",
    "refused at the draw": "refused",
    "got no pipeline": "nopipe",
    "could not be given the textures they declare": "notex",
}


# MTLTextureType and MTLPixelFormat as names, read out of Apple's own headers --
# the ones userland/build.sh builds the test against, or the copy in xnu-deps.
# Not a table written here from memory: the numbers in the log are whatever that
# SDK says they are, and a name this invented would be a caption on somebody
# else's picture. Where no header can be read the number is printed bare.
_MTL_ENUMS = {}


def mtl_name(kind, value):
    """"MTLTextureType2D" for (texture, 2), or "2" when no header says so."""
    if not _MTL_ENUMS:
        sdks = [os.path.expanduser("~/theos/sdks/iPhoneOS16.5.sdk"),
                os.path.join(ROOT, "xnu-deps/sdk/iPhoneOS16.5.xnu.sdk")]
        if os.environ.get("THEOS"):
            sdks.insert(0, os.path.join(os.environ["THEOS"], "sdks/iPhoneOS16.5.sdk"))
        for which, header in (("texture", "MTLTexture.h"), ("format", "MTLPixelFormat.h")):
            _MTL_ENUMS[which] = {}
            for sdk in sdks:
                path = os.path.join(
                    sdk, "System/Library/Frameworks/Metal.framework/Headers", header)
                if not os.path.exists(path):
                    continue
                try:
                    text = open(path, errors="replace").read()
                except OSError:
                    continue
                for name, num in re.findall(
                        r"^\s+(MTL(?:TextureType|PixelFormat)\w+)\s*=\s*(\d+)\s*,",
                        text, re.M):
                    _MTL_ENUMS[which].setdefault(int(num), name)
                break
    return _MTL_ENUMS.get(kind, {}).get(value, str(value))


def tex_bound(rest):
    """The "tex ..." suffix of a drawn row as [(slot, type name, format name)]."""
    m = RE_DA_TEX.search(rest or "")
    if not m:
        return []
    return [(int(slot), mtl_name("texture", int(t)), mtl_name("format", int(f)))
            for slot, t, f in RE_DA_TEX_ONE.findall(m.group(1))]


def position_dependent(lib, name):
    """Does this shader read a varying or its own position? (air2nir --ll)"""
    try:
        out = subprocess.run([AIR2NIR, "--ll", lib, name], capture_output=True,
                             timeout=TIMEOUT).stdout.decode(errors="replace")
    except (subprocess.TimeoutExpired, OSError):
        return None
    return ('air.fragment_input' in out or 'air.position' in out
            or 'air.vertex_output' in out)


def sweep_library(path):
    """The library the sweep drew, as the test's own line names it, under ios26/rootfs."""
    raw = open(path, "rb").read()
    text = raw.decode("utf-8", errors="replace").replace("\x00", "")
    m = re.search(r"drvtest: (\S+\.metallib), \d+ bytes", text)
    if not m:
        return None
    return os.path.join(IOS, "rootfs") + m.group(1)


# The sweep's own reason table. Ten reasons were already named on the dispatch
# side (ROWS) and the sweep had none: it collected a sentence per shader and
# printed four of them, so "65 could not be compared" was a number with no parts.
# Each of these is a different statement, and the point of keeping them apart is
# that some are this harness's business and some are not.
DRAW_NOREF = [
    ("nopixel", "the log carries no pixel for it, which is a hole in the log and not a "
                "result (the loader drops the middle of a long log)"),
    ("twice", "the log reports it twice with two different pictures, which is not "
              "something to pick between"),
    ("nocolour", "the shader writes no colour attachment this harness reads, so there is "
                 "nothing to compare -- and the device's attachments all hold the clear "
                 "value, which agrees with that and proves nothing"),
    ("multisample", "the shader samples a multisample texture and neither harness binds "
                    "one, so it is not run rather than run against the wrong texture"),
    ("beyond", "air_run drew into a render target the device's render pass does not bind, "
               "so the device never held this shader's output"),
    ("ntt", "softpipe refuses the shader (nir_to_tgsi/tgsi_ureg). That is softpipe's "
            "limit; the device is the only reference these have"),
    ("notex", "the driver could not be given the texture the shader declares, so it was "
              "not drawn rather than drawn against the wrong texture -- the sentence is "
              "the measurement"),
    ("nopipe", "the driver made no pipeline for it, so nothing was drawn and this is not "
               "a result about the shader"),
    ("hostfail", "the host reference could not be produced"),
]
# Of these, `twice` and `hostfail` are something going wrong here. The rest are
# measurements: a property of the shader, of the device test's render pass, or of
# softpipe, each of which stays until something is built. `nopixel` is the
# loader's, not ours -- it keeps the first 256 KB of a boot's log and the last
# 1 MB -- and the dispatch side's equivalent row (`nolines`) is kept out of BAD
# for the same reason: a cut log must not turn check.sh red.
DRAW_BAD = ("twice", "hostfail")


def drawall_section(path, library_hint):
    """The sweep's rows. True when nothing in them failed; no sweep in the log passes."""
    text = log_text(path)
    completed = status_scheme(path)[0]
    status_rows = RE_DA.findall(text)
    notex_rows = RE_DA_NOTEX.findall(text)
    nopipe_rows = RE_DA_NOPIPE.findall(text)
    if not status_rows and not notex_rows and not nopipe_rows:
        return True

    lib = library_hint
    if lib is None or not os.path.exists(lib):
        print("metalcheck: the sweep's library is not under ios26/rootfs, so no reference "
              "can be produced for it")
        return True

    # The loose reader of each line, and the whole rest of it (for the "tex" suffix).
    loose, rest_of = {}, {}
    for name, status, rest in RE_DA_LINE.findall(text):
        loose.setdefault(name, len(RE_DA_LOOSE.findall(rest)))
        rest_of.setdefault(name, rest)

    # One shader is one row, keyed on the name, over ALL THREE kinds of line the
    # sweep prints. The loader's heartbeat reprints the console's tail, so a line
    # can appear twice: three of the five slices of 816462e held one such echo each
    # and the sweep counted 274 rows for 271 shaders. Two copies that say the same
    # thing are one row; two that disagree are a row of their own.
    order, byname, twice = [], {}, []
    echoes = 0

    def add(name, key):
        nonlocal echoes
        if name not in byname:
            order.append(name)
            byname[name] = key
        elif byname[name] == key:
            echoes += 1
        elif name not in twice:
            twice.append(name)

    for name, status, pixel in status_rows:
        add(name, ("status", status, " ".join((pixel or "").split())))
    for name, why in notex_rows:
        add(name, ("notex", why.strip()))
    for name, why in nopipe_rows:
        add(name, ("nopipe", why.strip()))

    agree, differ, flip, refused = [], [], [], []
    mismatched = []
    noref = {key: [] for key, _ in DRAW_NOREF}
    drew_rows = 0
    # How much a comparison of one attachment can say. These shaders write a
    # constant out of a uniform into some of three targets, and two targets often
    # hold the same constant -- so for some rows the right attachment and the wrong
    # one carry the same four words, and such a row would agree even if the two
    # sides' indices were off. Counted rather than assumed away.
    flat = 0
    # Which attachment each comparison was about. Printed because it is the number
    # that was silently 0 for all of them, and a column of zeros here is the shape
    # of this going wrong again.
    on_target = {}
    # What the device bound, for the rows that declare a texture at all.
    bound = {}
    with_tex_rows = set()
    for name in order:
        key = byname[name]
        if name in twice:
            noref["twice"].append((name, "two different outcomes under one name"))
            continue
        if key[0] in ("notex", "nopipe"):
            noref[key[0]].append((name, key[1]))
            continue
        status, pixel = key[1], key[2]
        if status != str(completed):
            refused.append(name)
            continue
        drew_rows += 1
        # counted for every DRAWN row, compared or not, because that is what the
        # summary's "declared a texture and were bound what they declared" counts
        if tex_bound(rest_of.get(name, "")):
            with_tex_rows.add(name)
        # every attachment the device read, by target
        got = {int(t): [int(w) for w in words.split()]
               for t, words in RE_DA_T.findall(pixel or "")}
        if loose.get(name, len(got)) != len(got):
            mismatched.append((name, loose[name], len(got)))
        if not got:
            noref["nopixel"].append(
                (name, "completed (status %d) and no attachment on the line" % completed))
            continue
        # Which attachment to compare is the reference's answer: air_run prints the
        # lowest colour attachment the shader declares and says which one that is.
        target, host, row, why = host_pixel(lib, name)
        if host is None:
            if row == "nocolour":
                why += "; the device's attachment(s) %s hold %s" % (
                    buflist(got), "the clear value"
                    if not any(any(w) for w in got.values()) else "non-zero bytes")
            noref[row].append((name, why))
            continue
        if target not in got:
            noref["beyond"].append(
                (name, "air_run drew into render target %d and the line carries "
                       "attachment(s) %s" % (target, buflist(got))))
            continue
        if len({tuple(w) for w in got.values()}) == 1:
            flat += 1
        on_target[target] = on_target.get(target, 0) + 1
        tex = tex_bound(rest_of.get(name, ""))
        if tex:
            bound[name] = (target, tex)
        device = got[target]
        if device == host:
            agree.append(name)
        elif position_dependent(lib, name):
            flip.append(name)
        else:
            differ.append((name, target, device, host))

    # --- the sweep's own account of the same run -------------------------------
    #
    # Read as a LIST of outcomes and not as a fixed number of them. It had three,
    # it has four, and the fourth arrived while this was being written: six shaders
    # the driver will not bind a multisample texture for now print "no texture"
    # instead of a status, so a tool counting only status lines would have read 265
    # rows where the sweep saw 271 and said nothing. An outcome this does not know
    # is therefore a failure here, not a column to leave out.
    total = RE_DA_TOTAL.search(text)
    said, unknown, shown_total = {}, [], []
    library_total, slice_from, with_textures = None, None, None
    if total:
        for item in total.group(1).split(", "):
            m = RE_DA_ITEM.match(item.strip())
            if not m:
                unknown.append(item.strip())
                continue
            n, phrase = int(m.group(1)), m.group(2).strip()
            shown_total.append("%d %s" % (n, phrase))
            col = DA_OUTCOMES.get(phrase)
            if col is None:
                unknown.append(item.strip())
            else:
                said[col] = said.get(col, 0) + n
        library_total, slice_from = int(total.group(2)), int(total.group(3))
        with_textures = int(total.group(4)) if total.group(4) else None

    # Lines of the sweep that none of the readers above understood.
    unread = [n for n in dict.fromkeys(RE_DA_ANY.findall(text)) if n not in byname]

    mine = {"drew": drew_rows, "refused": len(refused),
            "nopipe": len(noref["nopipe"]), "notex": len(noref["notex"])}
    # What the sweep says it was asked for, which is the number the ledger has to
    # add to -- not the rows that happen to be in the log. A cut log holds the
    # summary (it is the last line) and not every row above it, and the difference
    # is a loss that must be named rather than quietly divided out.
    asked = sum(said.values()) if said else len(order)
    lost = asked - len(order)

    uncompared = sum(len(v) for v in noref.values())
    print()
    head = "metalcheck: the draw sweep, %d shaders the sweep says it was asked to draw" % asked
    if echoes:
        head += (" (%d line%s of the %d was the loader's heartbeat reprinting an earlier "
                 "one)" % (echoes, "" if echoes == 1 else "s",
                           len(status_rows) + len(notex_rows) + len(nopipe_rows)))
    if shown_total:
        head += " (its own count: %s" % ", ".join(shown_total)
        if library_total is not None:
            head += "; the library holds %d fragment shaders and this slice starts at %d" % (
                library_total, slice_from)
        if with_textures is not None:
            head += "; %d declared a texture and were bound what they declared" % with_textures
        head += ")"
    print(head)
    print("  %3d AGREE with softpipe on the pixel at (0, 0)%s" % (
        len(agree), "" if not on_target else ", compared on attachment " + ", ".join(
            "%d for %d of them" % (t, on_target[t]) for t in sorted(on_target))))
    print("  %3d DIFFER, and they read no varying and no position, so the orientation "
          "cannot explain it" % len(differ))
    print("  %3d differ but read a varying or a position: the two backends orient the "
          "picture differently (10.26, read again in 10.32), so this is not counted "
          "either way" % len(flip))
    print("  %3d the driver did not finish (status not %d)" % (len(refused), completed))
    print("  %3d could not be compared:" % uncompared)
    for key, what in DRAW_NOREF:
        if noref[key]:
            print("      %3d  %s" % (len(noref[key]), what))
    if noref["nopixel"] and completed == NEW_SCHEME[2]:
        # Where every drawn row has no readback, the driver's numbers moved and the test's
        # did not; this is the one row whose cause is in another file, so it says which.
        print("      (%s)" % TEST_KEYS_ON_SIX)
    if lost:
        print("  %3d the sweep counted and this log does not hold: the loader keeps the "
              "first 256 KB of a boot's log and the last 1 MB, and the summary is the "
              "last line, so it can outlive the rows it counts" % lost)
    for name, target, device, host in differ[:8]:
        print("      %-34s target %d: device %s | softpipe %s" % (
            name, target, " ".join("%u" % w for w in device),
            " ".join("%u" % w for w in host)))
    # Every uncompared row by name, not the first four: the reason a row is not
    # compared is the only thing it has to say, and a capped list hid 61 of 65.
    for key, _ in DRAW_NOREF:
        for name, why in sorted(noref[key]):
            print("      %-11s %-34s %s" % (key, name, why))
    if agree or differ or flip:
        print("  (of the %d compared, %d had all of the device's attachments holding the "
              "same four words, so for those the attachment this picked and the ones it "
              "did not would read alike; the other %d would have shown a DIFFER had the "
              "two sides' attachment indices not lined up)"
              % (len(agree) + len(differ) + len(flip), flat,
                 len(agree) + len(differ) + len(flip) - flat))
    # What each compared shader that declares a texture was bound. The 256 that
    # declare none are bound the sweep's own texture and print no suffix, so the
    # absence of a line here is itself the statement. air_run binds its OWN
    # texture and does not print what it bound, so the two sides' texture is not
    # cross-checked by this -- said out loud, because a row that names one side's
    # input and not the other's is half a measurement.
    if bound:
        print("  the %d compared shader%s that declare a texture, and what the device "
              "bound (air_run does not say what it bound, so this is one side only):"
              % (len(bound), "" if len(bound) == 1 else "s"))
        for name in sorted(bound):
            target, tex = bound[name]
            print("      %-34s attachment %d, %s%s" % (
                name, target,
                ", ".join("slot %d %s of %s" % t for t in tex),
                "" if name in agree else "  [NOT an agreement]"))

    ok = True
    if unread:
        print("metalcheck: %d line%s of the sweep this did not read (%s%s): they are in "
              "the log, so they are not the loader's cut -- an outcome of the sweep has "
              "no reason here and every count above is short by them"
              % (len(unread), "" if len(unread) == 1 else "s",
                 " ".join(unread[:6]), " ..." if len(unread) > 6 else ""),
              file=sys.stderr)
        ok = False
    if unknown:
        print("metalcheck: the sweep's summary names %d outcome%s this does not know (%s), "
              "so the ledger below cannot be checked against it"
              % (len(unknown), "" if len(unknown) == 1 else "s", "; ".join(unknown)),
              file=sys.stderr)
        ok = False
    if said and not twice:
        # Every row of the log must be an outcome the summary also counted, and no
        # more of them than the summary counted. With a `twice` row in the log one
        # shader cannot be attributed to a column, so this is not attempted then
        # rather than reported wrongly.
        for col, n in sorted(said.items()):
            if mine[col] > n:
                print("metalcheck: the log holds %d %s rows and the sweep counted %d"
                      % (mine[col], col, n), file=sys.stderr)
                ok = False
        if with_textures is not None and len(with_tex_rows) > with_textures:
            # The summary counts the shaders that declared a texture and were given
            # it, drawn or not; a "tex" suffix is printed only on a drawn row. So
            # the rows may be fewer and must never be more. Checked because this is
            # the fifth number of a line that had three of them a day ago.
            print("metalcheck: %d drawn rows name a bound texture and the sweep counted "
                  "%d shaders that declared one"
                  % (len(with_tex_rows), with_textures), file=sys.stderr)
            ok = False
        if sum(said.values()) - sum(mine.values()) != lost:
            print("metalcheck: the sweep counted %d rows, this read %d, and %d are "
                  "accounted for as lost" % (sum(said.values()), sum(mine.values()), lost),
                  file=sys.stderr)
            ok = False
    if lost < 0:
        print("metalcheck: this read %d rows and the sweep says it drew %d"
              % (len(order), asked), file=sys.stderr)
        ok = False
    if len(agree) + len(differ) + len(flip) + len(refused) + uncompared + lost != asked:
        print("metalcheck: the sweep's rows do not add up", file=sys.stderr)
        ok = False
    if mismatched:
        name, n_loose, structured = mismatched[0]
        print("metalcheck: the line for %s carries %d \"tN\" markers and RE_DA read %d of "
              "them (%d such line%s): the attachments were not read, so every row above "
              "is about a picture this did not look at"
              % (name, n_loose, structured, len(mismatched),
                 "" if len(mismatched) == 1 else "s"), file=sys.stderr)
        ok = False
    bad = [k for k in DRAW_BAD if noref[k]]
    return ok and not differ and not bad


