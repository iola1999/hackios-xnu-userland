#!/usr/bin/env python3
"""Write complete volume metadata and validate iOS 26 extraction provenance.

Commands: manifest LISTING OUTPUT; skeleton LISTING; fingerprint IMAGE; check DIRECTORY.
          prepare-empty LISTING SOURCE DESTINATION.
The manifest includes all entries from the original image. Staged application
metadata is also emitted at /Applications for the synthesized volume's copies.
"""
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import stat
import sys

HERE = Path(__file__).resolve().parent
FORMAT = 'full-ios26-v1'


def digest(path):
    h = hashlib.sha256()
    with open(path, 'rb') as source:
        for block in iter(lambda: source.read(1 << 20), b''):
            h.update(block)
    return h.hexdigest()


def preparation():
    return {name: digest(HERE / name) for name in ['full-system.py', 'fetch.sh']}


def entries(listing):
    entry = {}
    with open(listing, errors='strict') as source:
        for raw in source:
            line = raw.rstrip('\n')
            if not line:
                if 'Path' in entry and 'Mode' in entry:
                    yield entry
                entry = {}
            elif ' =' in line:
                key, _, value = line.partition(' =')
                entry[key] = value[1:] if value.startswith(' ') else value
    if 'Path' in entry and 'Mode' in entry:
        yield entry


def mode_bits(text):
    kinds = {'-': stat.S_IFREG, 'd': stat.S_IFDIR, 'l': stat.S_IFLNK}
    if len(text) != 10 or text[0] not in kinds:
        raise ValueError(f'unsupported image mode: {text!r}')
    mode = kinds[text[0]]
    for i, ch in enumerate(text[1:]):
        if ch in 'rwxst':
            mode |= 0o400 >> i
        if ch in 'sS' and i == 2:
            mode |= stat.S_ISUID
        if ch in 'sS' and i == 5:
            mode |= stat.S_ISGID
        if ch in 'tT' and i == 8:
            mode |= stat.S_ISVTX
    return mode


def manifest(listing, output):
    with open(output, 'w') as dest:
        for entry in entries(listing):
            path = entry['Path']
            target = entry.get('Symbolic Link', '')
            if path.startswith('/') or '..' in PurePosixPath(path).parts or any(c in path + target for c in '\t\n'):
                raise ValueError(f'invalid manifest path: {path!r}')
            mode = mode_bits(entry['Mode'])
            fields = (mode, int(entry.get('User ID') or 0), int(entry.get('Group ID') or 0))
            paths = [path]
            prefix = 'private/var/staged_system_apps/'
            if path.startswith(prefix):
                paths.append('Applications/' + path[len(prefix):])
            for name in paths:
                dest.write(f'{fields[0]:o}\t{fields[1]}\t{fields[2]}\t{name}\t{target}\n')


def skeleton(listing):
    directories, links = {}, []
    for entry in entries(listing):
        path = entry['Path']
        if not path.startswith('private/var/') or path.startswith(('private/var/staged_system_apps', 'private/var/MobileAsset/')):
            continue
        mode = mode_bits(entry['Mode'])
        if stat.S_ISDIR(mode):
            directories['/' + path] = (mode & 0o7777, int(entry.get('User ID') or 0), int(entry.get('Group ID') or 0))
        elif stat.S_ISLNK(mode):
            links.append((entry.get('Symbolic Link', ''), '/' + path))
    extra = [('/private/var/db/uuidtext', 0o755, 272, 272),
             ('/private/var/db/diagnostics/timesync', 0o755, 272, 272)]
    extra += [('/private/var/containers' + suffix, 0o755, 0, 0)
              for suffix in ['', '/Data', '/Shared', '/Bundle']]
    extra += [('/private/var/mobile/Containers' + suffix, 0o755, 501, 501)
              for suffix in ['', '/Data', '/Shared']]
    extra += [(f'/private/var/{user}/Library/{name}', mode, uid, uid)
              for user, mode, uid in [('mobile', 0o755, 501), ('root', 0o700, 0)]
              for name in ['Caches', 'Preferences', 'Logs']]
    for path, mode, uid, gid in extra:
        directories.setdefault(path, (mode, uid, gid))
    for path, (mode, uid, gid) in sorted(directories.items()):
        print(f'd\t{mode:o}\t{uid}\t{gid}\t{path}')
    for target, path in links:
        print(f'l\t{target}\t{path}')
    print('l\t/usr/share/zoneinfo\t/private/var/db/timezone/zoneinfo')


def prepare_empty(listing, source, destination):
    """Precreate unreadable empty files so rsync can compare their size and mtime."""
    for entry in entries(listing):
        mode = mode_bits(entry['Mode'])
        if not stat.S_ISREG(mode) or mode & stat.S_IRUSR:
            continue
        path = PurePosixPath(entry['Path'])
        if path.is_absolute() or '..' in path.parts:
            raise ValueError(f'invalid image path: {path}')
        node = (Path(source) / path).lstat()
        if int(entry['Size']) != 0 or node.st_size != 0 or not stat.S_ISREG(node.st_mode):
            raise ValueError(f'cannot precreate unreadable nonempty or mismatched file: {path}')
        target = Path(destination) / path
        target.parent.mkdir(parents=True, exist_ok=True)
        with target.open('xb'):
            pass
        os.utime(target, ns=(node.st_atime_ns, node.st_mtime_ns))
        target.chmod(stat.S_IMODE(node.st_mode))


def check(directory):
    root = Path(directory)
    for name in ['rootfs', 'cryptex', 'cryptex-app']:
        record = json.loads((root / ('.done-' + name)).read_text())
        if record.get('format') != FORMAT or record.get('preparation') != preparation():
            raise ValueError(f'{name}: preparation changed; run ios26/fetch.sh')
        if not (root / name).is_dir() or not record.get('image_sha256') or record.get('image_bytes', 0) <= 0:
            raise ValueError(f'{name}: incomplete extraction')
    if not (root / 'system.manifest').is_file():
        raise ValueError('system.manifest is missing')


def main():
    command, *args = sys.argv[1:]
    if command == 'manifest' and len(args) == 2:
        manifest(*args)
    elif command == 'skeleton' and len(args) == 1:
        skeleton(*args)
    elif command == 'prepare-empty' and len(args) == 3:
        prepare_empty(*args)
    elif command == 'fingerprint' and len(args) == 1:
        image = Path(args[0])
        print(json.dumps(dict(format=FORMAT, preparation=preparation(),
                              image_bytes=image.stat().st_size, image_sha256=digest(image)), sort_keys=True))
    elif command == 'check' and len(args) == 1:
        check(*args)
    else:
        raise ValueError(__doc__)


if __name__ == '__main__':
    try:
        main()
    except (OSError, ValueError) as error:
        sys.exit(str(error))
