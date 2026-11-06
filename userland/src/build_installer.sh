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
# Generate solid emerald green icon with stylized box/arrow symbol in center
for size, scale in ((120, 2), (180, 3)):
    rows = bytearray()
    center = size // 2
    margin = size // 4
    for y in range(size):
        rows.append(0)  # filter type none
        for x in range(size):
            # Emerald green background
            r, g, b = 16, 163, 127
            # Draw a centered square box container
            if margin <= x <= size - margin and margin <= y <= size - margin:
                # White box outline (border width = size // 20)
                bw = max(2, size // 24)
                if x < margin + bw or x > size - margin - bw or y < margin + bw or y > size - margin - bw:
                    r, g, b = 255, 255, 255
                # Downward arrow in center
                elif abs(x - center) <= bw and y >= margin + bw * 2 and y <= center + bw:
                    r, g, b = 255, 255, 255
                elif y > center and y <= size - margin - bw * 2 and abs(x - center) <= (size - margin - bw * 2 - y + bw):
                    r, g, b = 255, 255, 255
            rows += bytes((r, g, b))
    def chunk(kind, body):
        return struct.pack('>I', len(body)) + kind + body + struct.pack('>I', zlib.crc32(kind + body))
    png = (b'\x89PNG\r\n\x1a\n' +
           chunk(b'IHDR', struct.pack('>IIBBBBB', size, size, 8, 2, 0, 0, 0)) +
           chunk(b'IDAT', zlib.compress(bytes(rows), 9)) + chunk(b'IEND', b''))
    with open('%s/AppIcon60x60@%dx.png' % (out, scale), 'wb') as f:
        f.write(png)
PY

echo "build_installer.sh: signing bundle with entitlements..."
if [ "$(uname -s)" = Darwin ]; then
	/usr/bin/codesign --force --sign - --entitlements "$HERE/installer_app.entitlements" "$APP_DIR/RedwoodInstaller"
	/usr/bin/codesign --force --sign - --entitlements "$HERE/installer_app.entitlements" "$APP_DIR"
else
	${LDID:-ldid} -S"$HERE/installer_app.entitlements" "$APP_DIR/RedwoodInstaller"
fi

echo "build_installer.sh: successfully built RedwoodInstaller.app ($(du -sk "$APP_DIR" | cut -f1) KB)"
