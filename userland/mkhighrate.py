#!/usr/bin/env python3
"""mkhighrate.py SYSROOT: CADisableMinimumFrameDurationOnPhone = YES in the Info.plist of every application
of the system image being made (/Applications, /System/Library/CoreServices). build.sh runs it with
HIGHRATE=1.

QuartzCore holds a process to 60 frames a second on a phone unless its bundle carries this key, but on
23G90 it reads the key only while its high frame rate restriction is on (inject_sb26_rate.c prints that
state as "restriction" and "plist_key"). None of the 280 applications of 23G90 carries it. On 16.5 the
key was what took the home screen and the applications past 60 on a 120 Hz panel once CoreAnimation
knew the panel's rate (oosexgang/hakios-xnu-kernel c7b5f9a).

SYSROOT is hard links of the extracted system (build.sh: cp -al): each plist is written as a new file
in place of its link, so the extraction stays as it came out of the IPSW. The format (binary or XML)
and the mode are kept; owner and group come from system.manifest when the image is made.
"""
import glob
import os
import plistlib
import sys

KEY = 'CADisableMinimumFrameDurationOnPhone'

root = sys.argv[1]
changed = already = 0
for path in sorted(glob.glob(os.path.join(root, 'Applications', '*.app', 'Info.plist')) +
                   glob.glob(os.path.join(root, 'System', 'Library', 'CoreServices', '*.app', 'Info.plist'))):
    with open(path, 'rb') as f:
        data = f.read()
    info = plistlib.loads(data)
    if info.get(KEY) is True:
        already += 1
        continue
    info[KEY] = True
    mode = os.stat(path).st_mode & 0o7777
    os.unlink(path)
    with open(path, 'wb') as f:
        plistlib.dump(info, f, fmt=plistlib.FMT_BINARY if data.startswith(b'bplist') else plistlib.FMT_XML)
    os.chmod(path, mode)
    changed += 1
print(f'mkhighrate.py: {KEY} set in {changed} applications ({already} had it already)')
