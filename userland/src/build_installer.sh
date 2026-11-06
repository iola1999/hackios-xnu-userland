#!/bin/sh
# Build script for RedwoodInstaller.app (IPA 管理器)
set -e

HERE="$(cd "$(dirname "$0")" && pwd)"
USERLAND_DIR="$(cd "$HERE/.." && pwd)"
OBJ_DIR="$USERLAND_DIR/obj"
APP_DIR="$OBJ_DIR/RedwoodInstaller.app"

mkdir -p "$OBJ_DIR"
rm -rf "$APP_DIR"
mkdir -p "$APP_DIR"

echo "build_installer.sh: compiling RedwoodInstaller binary..."
if [ "$(uname -s)" = Darwin ]; then
	/usr/bin/xcrun -sdk iphoneos clang -arch arm64e -miphoneos-version-min=26.0 -Os -fobjc-arc \
		-Wno-deprecated-declarations -Wno-unused-command-line-argument \
		-framework UIKit -framework Foundation -framework CoreGraphics -lobjc \
		"$HERE/installer_app.m" -o "$APP_DIR/RedwoodInstaller"
else
	${CC:-xnu-clang} -arch arm64e -miphoneos-version-min=26.0 -Os -fobjc-arc \
		-Wno-deprecated-declarations \
		-framework UIKit -framework Foundation -framework CoreGraphics -lobjc \
		"$HERE/installer_app.m" -o "$APP_DIR/RedwoodInstaller"
fi

cp "$HERE/installer_app.plist" "$APP_DIR/Info.plist"
printf 'APPL????' > "$APP_DIR/PkgInfo"

echo "build_installer.sh: generating AppIcon assets..."
python3 - "$APP_DIR" <<-'PY'
import struct, sys, zlib

out = sys.argv[1]

def make_icon_png(size):
    rows = bytearray()
    center = size / 2.0
    margin = size * 0.22
    bw = max(2.0, size * 0.045)
    box_top = size * 0.42
    box_bot = size - margin
    box_left = margin
    box_right = size - margin
    arrow_w = max(1.5, size * 0.05)

    for y in range(size):
        rows.append(0)
        t = y / float(size)
        bg_r = int(16 * (1 - t) + 10 * t)
        bg_g = int(175 * (1 - t) + 135 * t)
        bg_b = int(135 * (1 - t) + 105 * t)

        for x in range(size):
            r, g, b = bg_r, bg_g, bg_b

            # Container box outline
            is_box_bottom = (box_bot - bw <= y <= box_bot) and (box_left <= x <= box_right)
            is_box_left = (box_top <= y <= box_bot) and (box_left <= x <= box_left + bw)
            is_box_right = (box_top <= y <= box_bot) and (box_right - bw <= x <= box_right)
            is_box_top = (box_top <= y <= box_top + bw) and ((box_left <= x <= box_left + bw * 2.0) or (box_right - bw * 2.0 <= x <= box_right))

            # Vertical arrow shaft
            shaft_top = size * 0.20
            shaft_bot = size * 0.54
            is_shaft = (shaft_top <= y <= shaft_bot) and (abs(x - center) <= arrow_w)

            # Arrow tip pointing down
            tip_h = size * 0.16
            dy = y - shaft_bot
            dx = abs(x - center)
            is_tip = (0 <= dy <= tip_h) and (dx <= (tip_h - dy) + arrow_w * 0.6)

            if is_box_bottom or is_box_left or is_box_right or is_box_top or is_shaft or is_tip:
                r, g, b = 255, 255, 255

            rows.extend((r, g, b))

    def chunk(kind, body):
        return struct.pack('>I', len(body)) + kind + body + struct.pack('>I', zlib.crc32(kind + body))

    return (b'\x89PNG\r\n\x1a\n' +
            chunk(b'IHDR', struct.pack('>IIBBBBB', size, size, 8, 2, 0, 0, 0)) +
            chunk(b'IDAT', zlib.compress(bytes(rows), 9)) +
            chunk(b'IEND', b''))

# Generate icons for all standard iOS icon dimensions
size_map = {
    'AppIcon60x60@3x.png': 180,
    'AppIcon60x60@2x.png': 120,
    'AppIcon60x60.png': 60,
    'AppIcon40x40@3x.png': 120,
    'AppIcon40x40@2x.png': 80,
    'AppIcon40x40.png': 40,
    'AppIcon29x29@3x.png': 87,
    'AppIcon29x29@2x.png': 58,
    'AppIcon29x29.png': 29,
    'AppIcon76x76@2x~ipad.png': 152,
    'AppIcon76x76~ipad.png': 76,
    'AppIcon83.5x83.5@2x~ipad.png': 167,
    'AppIcon.png': 180,
    'AppIcon@2x.png': 120,
    'AppIcon@3x.png': 180,
    'icon.png': 180,
    'icon@2x.png': 120,
    'icon@3x.png': 180,
}

cache = {}
for filename, sz in size_map.items():
    if sz not in cache:
        cache[sz] = make_icon_png(sz)
    with open(f'{out}/{filename}', 'wb') as f:
        f.write(cache[sz])
PY

echo "build_installer.sh: signing bundle with entitlements..."
if [ "$(uname -s)" = Darwin ]; then
	/usr/bin/codesign --force --sign - --entitlements "$HERE/installer_app.entitlements" "$APP_DIR/RedwoodInstaller"
	/usr/bin/codesign --force --sign - --entitlements "$HERE/installer_app.entitlements" "$APP_DIR"
else
	${LDID:-ldid} -S"$HERE/installer_app.entitlements" "$APP_DIR/RedwoodInstaller"
fi

echo "build_installer.sh: successfully built RedwoodInstaller.app ($(du -sk "$APP_DIR" | cut -f1) KB)"
