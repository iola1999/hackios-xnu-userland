#!/usr/bin/env python3
"""The launchd cache for iOS 26: mkcache.py APPLE_CACHE JOBS_DIR OUT

Outside the restore environment iOS 26's launchd takes its jobs from /System/Library/xpc/launchd.plist,
which the boot task launchd_cache_loader hands to it, and reads no LaunchDaemons directory. The cache
is a dictionary: "LaunchDaemons" maps each job plist's path to its contents; the other keys
(AppExtensions, Symlinks, SystemLibraryTreeState ...) are taken over from Apple's cache unchanged.
Its LaunchDaemons are replaced here by the jobs of JOBS_DIR (mkjobs.py's output), under the path
they have once stage0 binds JOBS_DIR onto /System/Library/LaunchDaemons. The cache is unsigned;
launchd_cache_loader passes it on without checking only with the boot-arg launchd_unsecure_cache.
"""
import os, plistlib, sys

apple, jobs, out = sys.argv[1:]
with open(apple, 'rb') as f:
    cache = plistlib.load(f)
cache['LaunchDaemons'] = {}
for name in sorted(os.listdir(jobs)):
    if name.endswith('.plist'):
        with open(os.path.join(jobs, name), 'rb') as f:
            cache['LaunchDaemons'][f'/System/Library/LaunchDaemons/{name}'] = plistlib.load(f)
with open(out, 'wb') as f:
    plistlib.dump(cache, f, fmt=plistlib.FMT_BINARY)
print(f'mkcache.py: {len(cache["LaunchDaemons"])} jobs in {out}')
