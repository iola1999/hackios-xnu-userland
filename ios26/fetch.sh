#!/bin/bash
# Lay out the files of iOS 26.6.2 (23G90, iPhone 17: iPhone18,3) that the system image is made of, the
# way ../userland/build.sh expects them with IOS=../ios26 (the layout of ../ios165/fetch.sh):
#
#   rootfs/                  the chosen part of the root filesystem (../ios165/select-system.py)
#   system.files/.manifest   what was chosen, with owners, modes and symbolic links
#   var.skel                 the skeleton of /private/var (../ios165/mkvarskel.py)
#   cryptex/                 the OS cryptex without the dyld shared cache
#   cryptex-app.dmg, cryptex-app/   the App cryptex as Apple ships it, and its files
#   23G90__iPhone18,3/       the dyld shared cache (arm64e), without its .symbols file
#
# On macOS: the three disk images are attached read-only with hdiutil and the files copied out of the
# volumes, which hand out file contents decompressed. Owners come from 7zz's listing of the root image
# (system.manifest): the attached volumes report every file as the user's own.
#
# Input: IPSW_DIR (default ../out/ipsw/v57/26) holding the IPSW members as
#   os.dmg        the root filesystem (094-96558-090.dmg.aea, decrypted: aea decrypt with the key that
#                 ../../tools/ipsw/aea-key.py reads from the file's own header)
#   systemos.dmg  the OS cryptex (Cryptex1,SystemOS in the BuildManifest)
#   appos.dmg     the App cryptex (094-96441-094.dmg, Cryptex1,AppOS)
# ../../tools/ipsw/fetch.py downloads single members of the IPSW.
# Needs: hdiutil, 7zz (7-Zip 23 or newer), rsync, python3. About 11 GB of disk afterwards.
set -e
cd "$(dirname "$0")"
IPSW_DIR=${IPSW_DIR:-../out/ipsw/v57/26}
CACHE=23G90__iPhone18,3
for f in os.dmg systemos.dmg appos.dmg; do
	[ -f "$IPSW_DIR/$f" ] || { echo "fetch.sh: no $IPSW_DIR/$f" >&2; exit 1; }
done
for tool in hdiutil 7zz rsync python3; do
	command -v $tool >/dev/null || { echo "fetch.sh: $tool is not installed" >&2; exit 1; }
done

MNT=$(mktemp -d /tmp/ios26.XXXXXX)
detach_all() {
	for d in "$MNT"/*; do [ -d "$d" ] && hdiutil detach -quiet "$d" 2>/dev/null; done
	rmdir "$MNT"/* "$MNT" 2>/dev/null || true
}
trap detach_all EXIT
attach() { # attach NAME IMAGE: the image's volume at $MNT/NAME
	mkdir -p "$MNT/$1"
	hdiutil attach -readonly -nobrowse -noautoopen -mountpoint "$MNT/$1" "$IPSW_DIR/$2" >/dev/null
}

# --- the root filesystem --------------------------------------------------------------------------
if [ ! -f .done-rootfs ] || [ select-system.py -nt .done-rootfs ]; then
	echo "fetch.sh: listing the root filesystem image"
	7zz l -slt "$IPSW_DIR/os.dmg" > rootfs.slt
	python3 select-system.py rootfs.slt system.files system.manifest
	python3 ../ios165/mkvarskel.py rootfs.slt > var.skel
	echo "fetch.sh: copying $(wc -l < system.files | tr -d ' ') files"
	attach os os.dmg
	rm -rf rootfs
	rsync -a --files-from=system.files "$MNT/os/" rootfs/
	python3 ../tools/font_trim.py emoji rootfs/System/Library/Fonts/CoreAddition/AppleColorEmoji-160px.ttc \
		rootfs/System/Library/Fonts/CoreAddition/AppleColorEmoji-160px.ttc 40,64,96
	touch .done-rootfs
fi

# --- the OS cryptex and the dyld shared cache -----------------------------------------------------
if [ ! -f .done-cryptex ]; then
	echo "fetch.sh: copying the OS cryptex"
	attach sysos systemos.dmg
	rm -rf cryptex $CACHE
	rsync -a --exclude /System/Library/Caches/com.apple.dyld --exclude /System/DriverKit "$MNT/sysos/" cryptex/
	# the cache's .symbols file (1 GB) is symbolication data, never mapped
	mkdir -p $CACHE
	rsync -a --exclude '*.symbols' "$MNT/sysos/System/Library/Caches/com.apple.dyld/" $CACHE/
	[ -f $CACHE/dyld_shared_cache_arm64e ] || { echo "fetch.sh: no dyld shared cache in the cryptex" >&2; exit 1; }
	touch .done-cryptex
fi

# --- the App cryptex ------------------------------------------------------------------------------
if [ ! -f .done-cryptex-app ]; then
	echo "fetch.sh: copying the App cryptex"
	cp "$IPSW_DIR/appos.dmg" cryptex-app.dmg
	attach app appos.dmg
	rm -rf cryptex-app
	rsync -a "$MNT/app/" cryptex-app/
	touch .done-cryptex-app
fi
echo "fetch.sh: done: $(du -sh rootfs | cut -f1) rootfs, $(du -sh cryptex | cut -f1) cryptex," \
	"$(du -sh cryptex-app | cut -f1) App cryptex, $(du -sh $CACHE | cut -f1) dyld shared cache"
