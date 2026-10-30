#!/bin/bash
# Build the user-space side of the bring-up.
#
#   hello            one raw-syscall program; as a RAM disk on its own it boots through mockfs
#   root.apfs        the small image: our init and test programs, etc/rc, and the launchd jobs
#                    (LaunchDaemons/), an APFS container (../tools/apfs/mkapfs.py). It travels in
#                    the payload next to the kernel (the second RAM disk, /dev/md1) and is mounted
#                    on /redwood. Rebuilt every time.
#                      RC=<file>    etc/rc: rc.launchd (default) hands process 1 to the iOS launchd
#                                   after init's checks; rc.ios runs a few iOS programs and halts
#                      JOBS="..."   labels of the stock iOS launchd jobs to load (default: IOS_JOBS)
#                      REST=1       ... and the rest of them, started only when asked for (see mkjobs.py)
#                      WIFID=0      without wifid, iOS's Wi-Fi daemon, which is in by default (see below)
#                      OSLOG="..."  labels of the jobs whose os_log output goes to the console ("all": every
#                                   one of JOBS; default: only the window server, which says little)
#                      INJECT="..." the programs started with a library of ours, "LIBRARY NAME..." per line
#                                   (default: src/inject.c in backboardd, src/inject_sb.c in SpringBoard,
#                                   src/inject_media.c in mediaserverd);
#                                   init hands the list to the kernel (etc/inject.conf -> sysctl kern.exec_inject)
#                      STOREFRONT=1 ... and src/inject_store.c inside appstored: the storefront answered,
#                                   and a bag request watched for (an experiment; see that file)
#                      EXTRA="..."  more programs for /redwood/bin (files on this machine): one-off
#                                   diagnostics, which a file given as RC_JOBS can then run
#                      EXTRA_SHARE="..." directories on this machine to copy whole into /redwood/share,
#                                   each under its own name: the data a one-off diagnostic reads, when it
#                                   needs more than a program (tools/air/intrincheck.py --emit writes such
#                                   a directory). This goes into the boot volume, which every recipe
#                                   carries and which the flash recipe puts in RAM, so its size is paid
#                                   for at every boot -- a few megabytes, not more
#                      RC_JOBS=<file> what the job com.redwood.demo runs: rc.jobs (default), or
#                                   rc.jobs.unlock, which also swipes the lock screen away (for the phone,
#                                   where nothing else can touch the screen yet).
#                                   A job list cannot assume a shell. iOS 16.5's own root has none at all
#                                   -- /bin is df and ps, and nothing else -- so a line beginning /bin/sh
#                                   fails with "No such file or directory" and status 127 on a flash-root
#                                   boot. The /var/jb/bin/sh that rc.jobs.jb and rc.jobs.sileo run is the
#                                   package manager's prefix, which only a JB=1 image carries. And init
#                                   runs these lines itself (src/init.c, run_command): words split on
#                                   blanks, no quoting, no variable assignment, no redirection beyond a
#                                   trailing ">> FILE" and "&". So a program that needs a value passed to
#                                   it needs an option for it, not an environment variable -- which is why
#                                   air_run grew --buffer.
#                      GL=1         with an OpenGL ES implementation for the window server: Mesa
#                                   (tools/mk-mesa.sh fetches and builds it) behind src/eagl.c, as
#                                   lib/libOSMesa.8.dylib (19 MB, Mesa 26.2.5 + rebase layer; see
#                                   docs/analysis/mesa-rebase-2026-10.md 9) and lib/libredwood-gl.dylib. Where the
#                                   kernel has started the phone's GPU, CoreAnimation then draws with its
#                                   OpenGL ES renderer, by that GPU (boot-arg gpu=1); anywhere else -- QEMU,
#                                   no boot-arg, no firmware in ../fw-gpu -- the library is not even loaded
#                                   and nothing changes (src/inject.c, opengl_es()).
#                                   GL=test: only the library and bin/gl_test (a triangle, src/gl_test.c;
#                                   RC_JOBS=rc.jobs.gltest runs it); the window server stays as it is.
#                      ADSP=1       with the audio DSP's firmware from ../fw-audio in fw/ (20 MB; for the
#                                   kernel's adsp_step=N, bsd/dev/redwood_adsp.c, which runs before the root is
#                                   mounted). Not beside GL=1: the two do not fit the 64 MB the loader takes with
#                                   the kernel. adsp=1 needs none of it: the system image has the firmware in /fw.
#   system.apfs      the system image, the root filesystem (the resident RAM disk, /dev/md0), an
#                    APFS container: the part of iOS 16.5 chosen by ../ios165/select-system.py at
#                    its real paths, the dyld shared cache, and src/stage0.c as /sbin/launchd (the
#                    real one is /sbin/launchd.ios). 2.5 GB; upload once per loader session with
#                    `xl ramdisk userland/system.apfs`. Rebuilt when missing, when its inputs
#                    (stage0, the manifest, the cryptex files) are newer, or with SYSTEM=1 (after
#                    changing the recipe below).
#   flashboot.apfs   FLASHBOOT=1: the RAM part of a boot whose root is the iOS volume on the phone's
#                    flash (see below); with CRYPTEXES=flash the boot volume alone, the cryptexes being
#                    on the flash. FLASHBOOT=1 also decides what goes into root.apfs: the job list is
#                    then made for the whole iOS volume (mkjobs.py --root-volume), so it holds daemons
#                    -- fairplayd, identityservicesd, passd ... -- that the system image has not got.
#                    That root.apfs is for a flash boot; a RAM boot wants one built without FLASHBOOT=1,
#                    or launchd will fail to start those few when something asks for them.
#
#   PREBUILT=1       do not compile anything: take our programs from ../prebuilt/userland (this is also
#                    what happens when the cross toolchain is not set up, see ../xnu-deps/setup.sh);
#                    the images are then made of them and of the iOS files alone (python3)
#   CHECK=1          check every image (every container) as it is made: apfsck has to find it clean and
#                    the kernel's apfs_lib has to read every volume back as the directory it was made of
#                    (../tools/apfs/check-image.sh; the system image takes a few minutes more)
#   COMPILE_ONLY=1   compile into obj/ and stage into root/, then stop before any image is made, so
#                    root$V.apfs and system$V.apfs keep whatever they are. For proving the toolchain
#                    works in a container without replacing the image a payload is packed from
set -e
cd "$(dirname "$(readlink -f "$0")")"
source ../xnu-env.sh

CC="xnu-clang -arch arm64 -Os -static -nostdlib -ffreestanding -fno-stack-protector -fno-builtin -Wall -Wextra"
LD="-Wl,-e,_start -Wl,-pagezero_size,0x100000000"
DYN="xnu-clang -arch arm64 -isysroot ${THEOS:-$HOME/theos}/sdks/iPhoneOS16.5.sdk -O1"
# ldid signs a program with its entitlements. The build container has it in theos's toolchain but not
# on PATH, and the iOS 26 branch below calls it three times without a guard, so name it here when PATH
# has none. The entitlement check further down reads LDID out of the environment, hence the export;
# when neither has one, LDID stays unset and every call fails as "ldid: not found", which the guarded
# ones report and the 26 ones stop the build on.
if [ -z "$LDID" ] && ! command -v ldid >/dev/null 2>&1; then
	_ldid="${THEOS:-$HOME/theos}/toolchain/linux/iphone/bin/ldid"
	if [ -x "$_ldid" ]; then
		LDID=$_ldid
		export LDID
	fi
	unset _ldid
fi
# the iOS release the images are made of: ../ios165 (16.5, 20F66, iPhone X), or IOS=../ios26 for 26.6.2
# (23G90, iPhone 17), which ../ios26/fetch.sh lays out the same way; DSC is the cache's file name, and
# the images of 26 are root26.apfs and system26.apfs, beside those of 16.5
IOS=${IOS:-../ios165}
case $IOS in
*ios26) CACHE=$IOS/23G90__iPhone18,3 DSC=dyld_shared_cache_arm64e V=26 ;;
*) CACHE=$IOS/20F66__iPhone10,3_6 DSC=dyld_shared_cache_arm64 V= ;;
esac
# ios165/rootfs and ios165/cryptex must be this copy's own directories of hard links, the way
# tools/new-workspace.sh makes them. A symlink to another copy's tree makes sysroot a symlink too, because
# cp -a does not follow one, and then every rm -rf and mv below edits the shared extraction in place. That
# happened on 19 October 2026: 491 LaunchDaemons, NanoLaunchDaemons, LaunchAngels, xpc/launchd.plist and
# private/var went out of the main copy's tree, and only the hard links in the other copies still held them.
for d in rootfs cryptex cryptex-app; do
	[ -L $IOS/$d ] || continue
	echo "build.sh: ios165/$d is a symlink to $(readlink $IOS/$d)" >&2
	echo "build.sh: it has to be a directory of this copy's own: rm it and cp -al the original over," >&2
	echo "build.sh: as tools/new-workspace.sh does. Building against a link damages the shared tree." >&2
	exit 1
done
# The stock iOS jobs given to launchd, roughly in the order they were brought up.
IOS_JOBS=${JOBS-"
	com.apple.notifyd com.apple.cfprefsd.xpc.daemon com.apple.cfprefsd.xpc.daemon.system com.apple.logd
	com.apple.syslogd com.apple.distnoted.xpc.daemon com.apple.diagnosticd com.apple.fseventsd
	com.apple.containermanagerd com.apple.containermanagerd.system com.apple.runningboardd
	com.apple.backboardd com.apple.mobile.usermanagerd com.apple.mobilegestalt.xpc com.apple.lsd
	com.apple.powerd com.apple.analyticsd com.apple.logd_helper
	com.apple.iconservices.iconservicesagent com.apple.pluginkit.pkd
	com.apple.SpringBoard
"}
if [ "$V" = 26 ] && [ "${JOBS+x}" != x ]; then
	# iOS 26 publishes SystemStatus through its separate daemon.
	IOS_JOBS="$IOS_JOBS com.apple.systemstatusd"
fi
# iOS's own Wi-Fi daemon, for Settings to have a Wi-Fi to show: the kernel answers it in
# bsd/dev/redwood_apple80211.c. With the radio (wifi=1) Settings then shows Wi-Fi on, the network joined and the
# networks the radio hears. NOT FINISHED: a network cannot be joined from Settings yet.
# It is in by default now, because it costs nothing where there is no radio at all: in QEMU it comes up, talks
# to itself about country codes and sits there -- 114 processes started against 119 without it, no crash, never
# restarted -- and it answers what the rest of the system keeps asking for anyway. com.apple.wifi.manager, which
# wifid registers itself rather than declaring in its plist, fell from 30 failed lookups a session to 3,
# com.apple.private.corewifi-xpc from 1 to none, and the session's whole failed-lookup count from 331 to 305.
# (Count a QEMU run log with its duplicate lines removed: run-ios.sh prints launchd's log twice.)
# WIFID=0 leaves it out.
[ "${WIFID-1}" != 0 ] && IOS_JOBS="$IOS_JOBS com.apple.wifid"
# The programs (file names) started with a library of ours inside them: "LIBRARY NAME..." per line
# (src/inject.c is libredwood.dylib, src/inject_sb.c is libredwood-sb.dylib, src/inject_media.c is
# libredwood-media.dylib).
# What Sileo runs from /bin (see the package manager's prefix below): the system image's /bin, or the
# boot volume's bin, bound over the iOS volume's /bin when that is the root (src/init.c, flash_root).
SILEO_BIN="chmod cp mkdir mv rm"
# (libredwood-tweaks is the tweak loader of the package manager's prefix, src/inject_tweaks.c: in
# SpringBoard the library above opens it, here it is named for the programs that have no library of
# ours -- Settings, where a tweak's own pane belongs. Add a program's name to have tweaks in it.)
if [ "$V" = 26 ]; then
	# The default injection libraries use 20F66 private layouts.
	INJECT=${INJECT-"/redwood/lib/libredwood-sb26.dylib SpringBoard"}
fi
INJECT=${INJECT-"/redwood/lib/libredwood.dylib backboardd containermanagerd containermanagerd_system
/redwood/lib/libredwood-sb.dylib SpringBoard
/redwood/lib/libredwood-media.dylib mediaserverd
/redwood/lib/libredwood-app.dylib Sileo
/redwood/lib/libredwood-tweaks.dylib Preferences"}
# STOREFRONT=1: and a library of ours inside appstored as well (src/inject_store.c). It is not in the
# list above because it changes what the daemon believes: AMS's account is made to answer with the
# storefront Apple's bag server returned for this device, which is how the App Store is asked whether
# a storefront is the only thing it is missing. It also writes down whether a bag request is ever
# built at all. For an experiment, not for an image anybody else boots.
[ -n "$STOREFRONT" ] && INJECT="$INJECT
/redwood/lib/libredwood-store.dylib appstored AppStore
/redwood/lib/libredwood-fairplay.dylib fairplayd.H2"
mkdir -p obj

if [ -z "$PREBUILT" ] && ! command -v ld64 >/dev/null; then PREBUILT=1; fi
if [ -n "$PREBUILT" ]; then
	echo "build.sh: not compiling, using the programs in prebuilt/userland"
	for f in ../prebuilt/userland/*; do
		# keep a file that did not change (and its time stamp): system.apfs depends on stage0;
		# and one built here after the prebuilt copy was made (cp -p keeps that copy's time stamp)
		o=obj/$(basename "$f")
		cmp -s "$f" "$o" || [ "$o" -nt "$f" ] || cp -p "$f" obj/
	done
	cp obj/hello hello
else
	$CC $LD -o hello src/hello.c
	$CC -Ilibmini -c libmini/mini.c -o obj/mini.o
	$CC -c libmini/start.S -o obj/start.o
	for prog in init child stage0 var_store; do
		$CC -Ilibmini $LD -o obj/$prog.new src/$prog.c obj/mini.o obj/start.o
		# keep the old file (and its time stamp) when nothing changed: system.apfs depends on stage0
		cmp -s obj/$prog.new obj/$prog && rm obj/$prog.new || mv obj/$prog.new obj/$prog
	done
fi

# The iOS 26 input process uses the checked 23G90 HID callback ABI.
if [ "$V" = 26 ]; then
	if [ -z "$PREBUILT" ]; then
		if [ "$(uname -s)" = Darwin ]; then
			/usr/bin/xcrun -sdk iphoneos clang -arch arm64e -miphoneos-version-min=26.0 \
				-Os -Wall -Wextra -Werror -framework CoreFoundation -framework IOKit \
				src/input26_service.c -o obj/input26-service
			/usr/bin/codesign --force --sign - --entitlements src/input26_service.entitlements obj/input26-service
			/usr/bin/xcrun -sdk iphoneos clang -arch arm64e -miphoneos-version-min=26.0 \
				-dynamiclib -install_name /redwood/lib/libredwood-sb26.dylib -Os -Wall -Wextra -Werror \
				-framework CoreFoundation -framework Metal -lobjc src/inject_sb26.c -o obj/libredwood-sb26.dylib
			/usr/bin/codesign --force --sign - obj/libredwood-sb26.dylib
			# Metal device registration in SpringBoard and backboardd, and the compression
			# preference the driver needs set before QuartzCore picks a format: it imports
			# linear IOSurfaces only.
			/usr/bin/xcrun -sdk iphoneos clang -arch arm64e -miphoneos-version-min=26.0 \
				-dynamiclib -install_name /redwood/lib/libredwood-sbmetal-experiment.dylib \
				-Os -fno-objc-arc -Wall -Wextra -Werror \
				-framework Foundation -framework Metal -framework UIKit -framework IOKit -lobjc \
				src/sb_metal_experiment.m -o obj/libredwood-sbmetal-experiment.dylib
			/usr/bin/codesign --force --sign - obj/libredwood-sbmetal-experiment.dylib
		else
			# The iOS 26 SDK is a symlink farm into the build host's Xcode SDK, so a container
			# that does not carry that same absolute path sees no frameworks at all and clang
			# answers "CoreFoundation/CoreFoundation.h not found", which reads like a broken SDK
			# instead of a missing mount. Say which of the two it is before a compile is spent.
			[ -f ../xnu-deps/26/sdk/iPhoneOS.sdk/System/Library/Frameworks/CoreFoundation.framework/Headers/CoreFoundation.h ] || {
				echo "build.sh: the iOS 26 SDK has no frameworks in this environment" >&2
				echo "build.sh: its System is $(readlink ../xnu-deps/26/sdk/iPhoneOS.sdk/System 2>/dev/null || echo 'not a symlink')" >&2
				echo "build.sh: that is a path of the build host; mount it into the container at the same absolute path, read only" >&2
				exit 1
			}
			XNU_SDK_VERSION=26.0 xnu-clang -arch arm64e -isysroot ../xnu-deps/26/sdk/iPhoneOS.sdk -miphoneos-version-min=26.0 \
				-Os -framework CoreFoundation -framework IOKit src/input26_service.c -o obj/input26-service
			${LDID:-ldid} -Ssrc/input26_service.entitlements obj/input26-service
			XNU_SDK_VERSION=26.0 xnu-clang -arch arm64e -isysroot ../xnu-deps/26/sdk/iPhoneOS.sdk \
				-dynamiclib -install_name /redwood/lib/libredwood-sb26.dylib -Os \
				-framework CoreFoundation -framework Metal -lobjc src/inject_sb26.c -o obj/libredwood-sb26.dylib
			${LDID:-ldid} -S obj/libredwood-sb26.dylib
			XNU_SDK_VERSION=26.0 xnu-clang -arch arm64e -isysroot ../xnu-deps/26/sdk/iPhoneOS.sdk \
				-dynamiclib -install_name /redwood/lib/libredwood-sbmetal-experiment.dylib \
				-Os -fno-objc-arc -framework Foundation -framework Metal -framework UIKit -framework IOKit -lobjc \
				src/sb_metal_experiment.m -o obj/libredwood-sbmetal-experiment.dylib
			${LDID:-ldid} -S obj/libredwood-sbmetal-experiment.dylib
		fi
	fi
	[ -f obj/input26-service ] || { echo "build.sh: iOS 26 requires obj/input26-service" >&2; exit 1; }
	[ -f obj/libredwood-sb26.dylib ] || { echo "build.sh: iOS 26 requires obj/libredwood-sb26.dylib" >&2; exit 1; }
	python3 - <<'PYHID'
import plistlib, subprocess, sys
if sys.platform == 'darwin':
    subprocess.run(['/usr/bin/codesign', '--verify', '--strict', 'obj/input26-service'], check=True)
    command = ['/usr/bin/codesign', '-d', '--entitlements', ':-', 'obj/input26-service']
else:
    import os
    command = [os.environ.get('LDID', 'ldid'), '-e', 'obj/input26-service']
result = subprocess.run(command, check=True, stdout=subprocess.PIPE)
if plistlib.loads(result.stdout).get('com.apple.private.hid.client.event-dispatch') is not True:
    raise SystemExit('build.sh: input26-service requires the HID event-dispatch entitlement')
PYHID
fi

# --- the small image ------------------------------------------------------------------------------
rm -rf root
mkdir -p root/sbin root/bin root/etc root/share root/LaunchDaemons
cp obj/init root/sbin/init
cp obj/child root/bin/child
# The data volume into the flash and back (src/var_store.c), and the job that saves it when a run
# ends: the program and its job together, away from the iOS job list below, which several working
# copies are editing at once.
[ -f obj/var_store ] && cp obj/var_store root/bin/
cp com.redwood.varstore.plist root/LaunchDaemons/
if [ "$V" = 26 ]; then
	cp obj/input26-service root/bin/
	cp com.redwood.input26.plist root/LaunchDaemons/
fi
echo "Darwin on redwood: image built $(date '+%Y-%m-%d %H:%M')" > root/etc/motd
# 300000 bytes, byte i = (i*31 + (i>>8)) & 0xff; init verifies it through read() and mmap()
python3 -c "import sys; sys.stdout.buffer.write(bytes((i*31 + (i>>8)) & 0xff for i in range(300000)))" > root/share/pattern.bin
# EXTRA_SHARE: whole directories of data for a one-off diagnostic, each under its own name. Here and
# not in root/bin, because what goes in is not a program: a generated test suite is modules and the
# bytes they read, and the job list runs it by its own run.sh. -L so that a directory assembled out
# of symbolic links arrives as files, since the image is made from this tree and not from the links'
# targets.
for d in $EXTRA_SHARE; do
	[ -d "$d" ] || { echo "build.sh: EXTRA_SHARE: $d is not a directory"; exit 1; }
	cp -rL "$d" root/share/
done
if [ -d $IOS/rootfs/usr/lib ]; then
	if [ -z "$PREBUILT" ]; then
		# ordinary dynamically linked programs of our own, built against the iOS SDK
		$DYN -o obj/hello_dyn src/hello_dyn.c
		$DYN -framework CoreFoundation -o obj/svc_test src/svc_test.c
		$DYN -Wno-four-char-constants -framework CoreFoundation -framework IOSurface -o obj/surf_test src/surf_test.c
		# an IOSurface of ours imported as a texture of the GPU, written through one view
		# and read through the other (src/drawable_test.c): the check that tells a texture
		# over the surface's own pages from a texture over a copy of them. It goes through
		# src/redwooddrawable.m, which is the bridge under test, and dlopens the compiler
		# library the way the driver does.
		$DYN -Wno-four-char-constants -framework CoreFoundation -framework IOSurface \
			-framework IOKit -o obj/drawable_test src/drawable_test.c src/redwooddrawable.m
		$DYN -o obj/rwtouch src/rwtouch.c
		# stop and continue named daemons, so that one round carries both halves of an
		# experiment without rebuilding the image (src/rwsig.m)
		$DYN -o obj/rwsig src/rwsig.m
		# wait for the compositor to stop putting frames on the panel and ask for a thread
		# dump at that instant (src/framewatch.m). rwsig freezes a process and asks where it
		# sits; framewatch waits for the system to stop by itself, which is what the round
		# with 17 frames did.
		$DYN -o obj/framewatch src/framewatch.m
		# what LaunchServices and pluginkit answer on this boot (src/lsprobe.m)
		$DYN -framework Foundation -o obj/lsprobe src/lsprobe.m
		$DYN -framework CoreFoundation -framework AudioToolbox -framework AVFoundation -lobjc -o obj/sound_test src/sound_test.c
		$DYN -framework CoreFoundation -lMobileGestalt -o obj/mg_test src/mg_test.c
		$DYN -framework CoreFoundation -framework AVFoundation -lobjc -o obj/torch_test src/torch_test.c
		$DYN -O2 -o obj/camera_test src/camera_test.c src/camera_isp.c
		$DYN -fobjc-arc -framework Foundation -framework AVFoundation -o obj/camera_avtest src/camera_avtest.m
		# (the microphone as an application records: allowed to, by an entitlement, if ldid is there)
		$DYN -framework CoreFoundation -framework AVFoundation -lobjc -o obj/record_test src/record_test.c &&
			{ ${LDID:-ldid} -Ssrc/record_test.entitlements obj/record_test 2>/dev/null ||
			  echo "build.sh: no ldid: record_test is unsigned, and the system may not let it record"; }
		$DYN -o obj/cpu_test src/cpu_test.c
		$DYN -framework CoreFoundation -framework IOSurface -o obj/cow_test src/cow_test.c
		$DYN -framework CoreFoundation -framework CoreGraphics -framework IOSurface -o obj/cg_test src/cg_test.c
		# what iOS here believes about Metal, and what CoreImage does without it (src/metal_test.m);
		# OpenGLES for the CIContext made over an EAGLContext, which is deprecated and the point
		$DYN -fobjc-arc -Wno-deprecated-declarations -framework Foundation -framework CoreGraphics \
			-framework CoreImage -framework IOKit -framework Metal -framework OpenGLES \
			-lMobileGestalt -o obj/metal_test src/metal_test.m
		# the first question of subsystem (2): can a subclass of the private _MTLDevice be
		# made here, and what does its initWithAcceleratorPort: do (src/metal_dev.m). No ARC:
		# it calls objc_msgSend by hand and allocates a class at runtime.
		$DYN -framework Foundation -framework IOKit -framework Metal -lobjc \
			-o obj/metal_dev src/metal_dev.m
		# a device object that implements nothing and logs what Apple's own init asks it
		# for (src/metal_stubdev.m); same reasons for no ARC and for -lobjc
		$DYN -framework Foundation -framework IOKit -framework Metal -lobjc \
			-o obj/metal_stubdev src/metal_stubdev.m
		# the device object of the driver itself, as the library a bundle would hold
		# (src/redwoodmetal.m). It links against Metal's private _OBJC_CLASS_$__MTLDevice,
		# which is bindable because IOGPU.framework imports the same symbol; -lobjc for
		# class_addProtocol and no ARC because it calls objc_msgSend by hand.
		# (with src/redwooddrawable.m beside it: the IOSurface a CAMetalLayer hands a
		# device, as a texture. A file of its own because it answers CoreAnimation and
		# not Metal, and the two meet in one method -- docs/metal.md 10.46.)
		$DYN -dynamiclib -install_name /redwood/lib/libredwoodmetal.dylib \
			-framework Foundation -framework IOKit -framework Metal -lobjc \
			-o obj/libredwoodmetal.dylib src/redwoodmetal.m src/redwooddrawable.m
		# and the program that walks Metal's own steps 4 and 5 against it
		$DYN -framework Foundation -framework IOKit -framework Metal -lobjc \
			-o obj/metal_drv_test src/metal_drv_test.m
		# An application of this port's own: a window, a CAMetalLayer, a drawable and a
		# triangle in it (src/triangle_app.m). UIKit because that is the point -- every
		# Metal result so far is from inside one of Apple's daemons, and this is an
		# ordinary bundle in /Applications that SpringBoard launches; IOSurface because
		# it reads the drawable's own surface back to say whether the texture WAS those
		# pages; no ARC and -lobjc because it sends -initWithAcceleratorPort: by hand,
		# the way src/metal_drv_test.m does. The bundle is assembled further down, where
		# a PREBUILT tree can assemble it too.
		# (signed, if ldid is there, for the entitlements an application has and a
		# /redwood/bin program does not: see src/triangle_app.entitlements, which says
		# why none of them is needed to RUN. Signed here rather than in the bundle, so
		# that the copy in /redwood/bin carries the same signature.)
		$DYN -framework Foundation -framework UIKit -framework QuartzCore \
			-framework Metal -framework IOSurface -framework IOKit -lobjc \
			-o obj/RedwoodTriangle src/triangle_app.m &&
			{ ${LDID:-ldid} -Ssrc/triangle_app.entitlements obj/RedwoodTriangle 2>/dev/null ||
			  echo "build.sh: no ldid: RedwoodTriangle is unsigned, which still runs (AMFI here accepts everything) but holds no entitlements"; }
		# what the device ANSWERS when an application asks what it supports
		# (src/metal_caps.m). It takes the device from MTLCreateSystemDefaultDevice() so
		# that the ivars behind Apple's inherited answers are filled by the registration
		# Metal itself ran; no ARC and -lobjc because most of what it asks is MTLDeviceSPI,
		# whose header this port has not got, so every send is an objc_msgSend by hand. MetalFX
		# because the acceptance test's own capability gate is +supportsDevice: on its two
		# scaler descriptors, and that gate can be measured without the game.
		$DYN -framework Foundation -framework IOKit -framework Metal -lobjc \
			-framework MetalFX -o obj/metal_caps src/metal_caps.m
		# ... and the switch that steps 1 and 2 need: it puts MetalPluginName and
		# MetalPluginClassName on the IOAcceleratorES service (src/metal_props.c). IOKit and
		# CoreFoundation only -- a program that flips this switch must not itself ask Metal,
		# whose conditions are asserts. It needs the kernel booted with metal=1.
		$DYN -framework CoreFoundation -framework IOKit -o obj/metal_props src/metal_props.c
		$DYN -o obj/shm_test src/shm_test.c
		$DYN -o obj/apfs_mmap_test src/apfs_mmap_test.c
		$DYN -o obj/vol_ls src/vol_ls.c
		$DYN -framework CoreFoundation -framework CoreGraphics -framework ImageIO -o obj/snap_test src/snap_test.c
		$DYN -framework CoreFoundation -framework CoreGraphics -framework CoreText -o obj/font_test src/font_test.c
		$DYN -fobjc-arc -framework Foundation -framework SystemConfiguration -o obj/net_test src/net_test.m
		# (the account database answers only a client with the entitlements its own daemons hold: src/store_test.m)
		$DYN -fobjc-arc -framework Foundation -framework Accounts -o obj/store_test src/store_test.m &&
			{ ${LDID:-ldid} -Ssrc/store_test.entitlements obj/store_test 2>/dev/null ||
			  echo "build.sh: no ldid: store_test is unsigned, and accountsd will not show it the accounts"; }
		# (with an entitlement for "battery_test open URL", if ldid is there: src/battery_test.c)
		$DYN -framework CoreFoundation -framework IOKit -lsqlite3 -o obj/battery_test src/battery_test.c &&
			{ ${LDID:-ldid} -Ssrc/battery_test.entitlements obj/battery_test 2>/dev/null ||
			  echo "build.sh: no ldid: battery_test is unsigned, and SpringBoard will not open URLs for it"; }
		# (the keychain answers only a program that has a keychain access group: signed with one, if ldid is there)
		$DYN -framework CoreFoundation -framework Security -o obj/keychain_test src/keychain_test.c &&
			{ ${LDID:-ldid} -Ssrc/keychain_test.entitlements obj/keychain_test 2>/dev/null ||
			  echo "build.sh: no ldid: keychain_test is unsigned, and the keychain will refuse it (-34018)"; }
		# (and wifid's way of keeping a network's password, by wifid's name: src/wifipw_test.c)
		$DYN -framework CoreFoundation -framework Security -o obj/wifipw_test src/wifipw_test.c &&
			{ ${LDID:-ldid} -Ssrc/wifipw_test.entitlements obj/wifipw_test 2>/dev/null ||
			  echo "build.sh: no ldid: wifipw_test is unsigned, and the keychain will refuse it (-34018)"; }
		# our library for inside stock iOS programs (the jobs in INJECT are started with it)
		$DYN -dynamiclib -install_name /redwood/lib/libredwood.dylib -framework CoreFoundation -framework IOKit -lobjc \
			-o obj/libredwood.dylib src/inject.c
		$DYN -dynamiclib -install_name /redwood/lib/libredwood-sb.dylib -framework CoreFoundation -framework IOKit -lobjc \
			-o obj/libredwood-sb.dylib src/inject_sb.c
		# ... the tweak loader of the package manager's prefix (src/inject_tweaks.c): it goes into a
		# program either as the one library the kernel inserts or opened by a library of ours
		$DYN -dynamiclib -install_name /redwood/lib/libredwood-tweaks.dylib -framework CoreFoundation -lobjc \
			-o obj/libredwood-tweaks.dylib src/inject_tweaks.c
		# ... and inside an application, to say what killed it (src/inject_app.c)
		$DYN -dynamiclib -install_name /redwood/lib/libredwood-app.dylib -lobjc \
			-o obj/libredwood-app.dylib src/inject_app.c
		$DYN -O2 -dynamiclib -install_name /redwood/lib/libredwood-media.dylib -framework CoreFoundation -framework CoreMedia \
			-framework CoreVideo -framework IOSurface -lobjc \
			-o obj/libredwood-media.dylib src/inject_media.c src/camera_device.c src/camera_isp.c
		# ... and inside appstored (STOREFRONT=1), which answers with a storefront and watches for a bag
		# request being built (src/inject_store.c)
		# (-lc++abi for __cxa_throw: every C++ exception thrown in the process is watched, which is how
		#  the jetpack parser's rejections are seen -- a throw somebody catches logs nothing of itself)
		$DYN -O2 -dynamiclib -install_name /redwood/lib/libredwood-store.dylib -framework CoreFoundation -lobjc \
			-lc++abi -o obj/libredwood-store.dylib src/inject_store.c
		# ... and inside fairplayd.H2, which starts and leaves at once: what it looks for, and where it
		# gives up (src/inject_fairplay.c; watches only, replaces nothing)
		$DYN -O2 -dynamiclib -install_name /redwood/lib/libredwood-fairplay.dylib -framework CoreFoundation \
			-framework IOKit -framework Security -lMobileGestalt \
			-o obj/libredwood-fairplay.dylib src/inject_fairplay.c
		# the output device of the audio server: a plug-in for CoreAudio's HAL
		$DYN -bundle -framework CoreFoundation -o obj/RedwoodAudio src/audio_plugin.c
		# The same driver again as a loadable bundle, which is the form Metal's own
		# discovery wants: step 3 builds the path /System/Library/Extensions/<name>.bundle
		# and opens it with NSBundle, and -[NSBundle classNamed:] then tests that the class
		# came out of that bundle (0x1811e0dac). The dylib above cannot serve as the
		# executable: dyld uniques images by install_name, so a process that already has
		# /redwood/lib/libredwoodmetal.dylib would bind the bundle to it and fail that test.
		$DYN -bundle -framework Foundation -framework IOKit -framework Metal -lobjc \
			-o obj/RedwoodMetal src/redwoodmetal.m src/redwooddrawable.m
		if [ -n "$GL" ]; then
			# the OpenGL ES library, and the table of functions that hands CoreAnimation's calls to it
			# (Mesa says beside the library what it was built from: one from other patches is built again)
			M=../xnu-deps/build/mesa/out
			# The baseline is 26.2.5 with the rebase layer (docs/analysis/mesa-rebase-2026-10.md 6 and 9),
			# and the stamp below says so: pinning VERSION/REBASE into it is what stops a rebuild this
			# branch starts from coming out as a 25.x library beside a 26.x air frontend.
			MESA_BUILD="REBASE=26 VERSION=26.2.5 SHA256=794b3d1729075dae7ebd47c7283b7ca0cdb5dec668f7f11cc0a5d82f6138a0a5"
			# both stamps are computed by tools/mesa-stamp.sh, the same file mk-mesa.sh writes them
			# with -- one text on both sides, so a drift is not possible. Why the list covers the
			# rebase layer, names the baseline and sorts in byte order is written down there.
			. ../tools/mesa-stamp.sh
			MESA_FROM=$(mesa_from_stamp .. 26.2.5 26)
			[ "$(cat $M/built-from 2>/dev/null)" = "$MESA_FROM" ] || env $MESA_BUILD ../tools/mk-mesa.sh
			# ... and the compiler library separately, because the hash above covers the PATCHES
			# and the library is built from tools/mesa/air/, which they do not mention. Two boots
			# of 22 October carried a libredwoodair.dylib three hours older than the driver beside
			# it while rwa_render_shape had grown IN THE MIDDLE -- the image was the mismatch the
			# contract warns about, and nothing in it said so. Silence is what made it expensive:
			# the library is stripped, so the driver found the symbols it wanted and read a stride
			# where a pixel format was.
			[ "$(cat $M/built-from-air 2>/dev/null)" = "$(mesa_air_stamp ..)" ] || {
				echo "build.sh: tools/mesa/air/ has changed since libredwoodair.dylib was built"
				env AIR=1 $MESA_BUILD ../tools/mk-mesa.sh
			}
			./mkeagl.py src/eagl_slots.txt $M/gl.xml $M/exported-gl.txt src/eagl.c > obj/eagl_gen.h
			$DYN -dynamiclib -install_name /redwood/lib/libredwood-gl.dylib -Iobj -framework CoreFoundation \
				-framework IOKit -framework IOSurface -lobjc $M/libOSMesa.8.dylib -o obj/libredwood-gl.dylib \
				src/eagl.c src/gl_osmesa.c
			$DYN -framework CoreFoundation -framework IOKit $M/libOSMesa.8.dylib -o obj/gl_test src/gl_test.c
			# ... and what this GPU makes of an unsigned integer turned into a float (src/u2f_test.c)
			$DYN $M/libOSMesa.8.dylib -o obj/u2f_test src/u2f_test.c
			cp $M/libOSMesa.8.dylib obj/
			# air_run draws with a Metal shader translated by the AIR frontend
			# and reads the pixels back (docs/air.md). It is built inside the
			# Mesa build, because it needs Gallium itself and not the gl*
			# entry points this library exports; AIR=1 tools/mk-mesa.sh makes
			# it, and it is copied in when it is there.
			[ -f $M/air_run ] && cp $M/air_run obj/air_run
			# The compiler the Metal driver asks for: AIR in, a dispatch on the
			# Adreno out (tools/mesa/air/air_metal.h). It is copied whether or not
			# GL is wanted, because src/redwoodmetal.m dlopens it by that path --
			# and its absence is a sentence the driver prints, not a crash.
			[ -f $M/libredwoodair.dylib ] && cp $M/libredwoodair.dylib obj/
		fi
	fi
	# (drawable_test is built in the same block as surf_test, so it is copied with it)
	# (prebuilt/userland does not ship every program -- e.g. drawable_test -- so copy what is there)
	# lsprobe is in the list below and the list copies what is there, so a probe whose source changed
	# after the last compile packs the old one without a word and the round then reports numbers the
	# current source does not produce: the shape of every "changed but not in the image" case in
	# docs/plan/k90auto-run-protocol.md. Required only where its compile above runs, because
	# prebuilt/userland ships no lsprobe and PREBUILT=1 promises nothing it does not carry.
	if [ -z "$PREBUILT" ]; then
		[ -f obj/lsprobe ] || { echo "build.sh: obj/lsprobe missing" >&2; exit 1; }
		[ obj/lsprobe -nt src/lsprobe.m ] || {
			echo "build.sh: obj/lsprobe is older than src/lsprobe.m; build it (IOS=$IOS) before staging" >&2
			exit 1
		}
	fi
	for t in hello_dyn svc_test surf_test drawable_test rwtouch rwsig framewatch lsprobe mg_test; do
		if [ -f obj/$t ]; then cp obj/$t root/bin/; fi
	done
	[ -f obj/cpu_test ] && cp obj/cpu_test root/bin/        # (a prebuilt/ from before this program has none)
	[ -f obj/cow_test ] && cp obj/cow_test root/bin/
	[ -f obj/cg_test ] && cp obj/cg_test root/bin/
	[ -f obj/metal_dev ] && cp obj/metal_dev root/bin/
	[ -f obj/metal_stubdev ] && cp obj/metal_stubdev root/bin/
	[ -f obj/metal_drv_test ] && cp obj/metal_drv_test root/bin/
	[ -f obj/metal_caps ] && cp obj/metal_caps root/bin/
	[ -f obj/metal_props ] && cp obj/metal_props root/bin/
	[ -f obj/shm_test ] && cp obj/shm_test root/bin/
	[ -f obj/apfs_mmap_test ] && cp obj/apfs_mmap_test root/bin/
	[ -f obj/vol_ls ] && cp obj/vol_ls root/bin/
	[ -f obj/snap_test ] && cp obj/snap_test root/bin/
	[ -f obj/font_test ] && cp obj/font_test root/bin/
	[ -f obj/metal_test ] && cp obj/metal_test root/bin/
	[ -f obj/net_test ] && cp obj/net_test root/bin/
	[ -f obj/store_test ] && cp obj/store_test root/bin/
	[ -f obj/battery_test ] && cp obj/battery_test root/bin/
	[ -f obj/keychain_test ] && cp obj/keychain_test root/bin/
	[ -f obj/wifipw_test ] && cp obj/wifipw_test root/bin/
	[ -f obj/sound_test ] && cp obj/sound_test root/bin/
	[ -f obj/torch_test ] && cp obj/torch_test root/bin/
	[ -f obj/camera_test ] && cp obj/camera_test root/bin/
	[ -f obj/camera_avtest ] && cp obj/camera_avtest root/bin/
	[ -f obj/record_test ] && cp obj/record_test root/bin/
	# The same binary as the application below, under /redwood/bin, for its --layer mode:
	# a CAMetalLayer with no window and no UIKit, driven once. That mode cannot say whether
	# a frame reaches the panel, and it answers the two questions that come before it -- does
	# the driver work outside Apple's daemons, and does nextDrawable give a texture -- in a
	# process whose output is certainly in the log, because the job list runs it. An
	# application launched by SpringBoard is a process this file cannot promise that about.
	[ -f obj/RedwoodTriangle ] && cp obj/RedwoodTriangle root/bin/triangle_app
	for f in $EXTRA; do
		if [ "$V" = 26 ] && [ "$(basename "$f")" = input26-service ]; then
			echo "build.sh: EXTRA cannot replace the validated input26-service" >&2
			exit 1
		fi
		cp "$f" root/bin/
	done
	#
	# ... and the application bundle itself, staged in obj/ and copied into
	# sysroot/Applications by the system image below.
	#
	# A PLAIN DIRECTORY OF REAL FILES, and that is not a style: a bundle made of symbolic
	# links is refused by LaunchServices ("Unable to register"), and the registrar that
	# walks /Applications at boot passes over mount points. So the icons are generated here
	# rather than linked from anywhere, and the plist and the executable are copied.
	if [ -f obj/RedwoodTriangle ]; then
		rm -rf obj/RedwoodTriangle.app
		mkdir -p obj/RedwoodTriangle.app
		cp obj/RedwoodTriangle obj/RedwoodTriangle.app/
		cp src/triangle_app.plist obj/RedwoodTriangle.app/Info.plist
		printf 'APPL????' > obj/RedwoodTriangle.app/PkgInfo
		# The icon is the picture the application draws: an orange triangle over the
		# lower left half of a blue square, by the same rule the shader covers a pixel
		# by ((x+0.5)/w < (y+0.5)/h). So the home screen states the prediction, and an
		# icon that comes out with its halves the other way round says the arithmetic in
		# this one line disagrees with the arithmetic in src/triangle_app.m. No alpha
		# channel: iOS masks an icon itself and an icon with alpha comes out wrong.
		python3 - obj/RedwoodTriangle.app <<-'PY'
		import struct, sys, zlib
		out = sys.argv[1]
		for size, scale in ((120, 2), (180, 3)):
		    rows = bytearray()
		    for y in range(size):
		        rows.append(0)                        # PNG filter type: none
		        for x in range(size):
		            inside = (2 * x + 1) * size < (2 * y + 1) * size
		            rows += bytes((255, 128, 0) if inside else (0, 0, 255))
		    def chunk(kind, body):
		        return (struct.pack('>I', len(body)) + kind + body +
		                struct.pack('>I', zlib.crc32(kind + body)))
		    png = (b'\x89PNG\r\n\x1a\n' +
		           chunk(b'IHDR', struct.pack('>IIBBBBB', size, size, 8, 2, 0, 0, 0)) +
		           chunk(b'IDAT', zlib.compress(bytes(rows), 9)) + chunk(b'IEND', b''))
		    open('%s/AppIcon60x60@%dx.png' % (out, scale), 'wb').write(png)
		PY
		# Unsigned, which is enough: AMFI in this kernel accepts every signature and
		# holds no trust cache (libkern/amfi/miniamfi.c), and the kernel is built without
		# config_enforce_signed_code. What a signature would still buy is entitlements,
		# and this application asks the system for nothing that needs one.
		echo "  RedwoodTriangle.app: $(du -sk obj/RedwoodTriangle.app | cut -f1) KB for /Applications"
	fi
	# /dev and /private/var exist so that this image can also be the root on its own:
	# devfs is mounted on /dev, and init puts a ramfs on /private/var
	mkdir -p root/dev root/private/var
	mkdir -p root/lib
	cp obj/libredwood.dylib obj/libredwood-sb.dylib root/lib/
	if [ "$V" = 26 ]; then
		cp obj/libredwood-sb26.dylib root/lib/
		# A prebuilt tree may not ship it; the inject.conf loop below drops an entry whose library
		# is absent, so an image that names it has to be built here.
		if [ -f obj/libredwood-sbmetal-experiment.dylib ]; then cp obj/libredwood-sbmetal-experiment.dylib root/lib/; fi
	fi
	[ -f obj/libredwood-media.dylib ] && cp obj/libredwood-media.dylib root/lib/     # (as with cpu_test)
	[ -f obj/libredwood-store.dylib ] && cp obj/libredwood-store.dylib root/lib/
	[ -f obj/libredwood-fairplay.dylib ] && cp obj/libredwood-fairplay.dylib root/lib/
	[ -f obj/libredwood-app.dylib ] && cp obj/libredwood-app.dylib root/lib/
	[ -f obj/libredwood-tweaks.dylib ] && cp obj/libredwood-tweaks.dylib root/lib/
	# The build above sits behind `-d $IOS/rootfs/usr/lib` and `-z $PREBUILT`, this copy does
	# not: with IOS at its default of ../ios165, which has no rootfs here, the compile is
	# skipped and the image gets whatever object was left from an earlier run. That is how a
	# driver built on 23 October reached a 28 October image (docs/plan/k90auto-run-protocol.md).
	[ -f obj/libredwoodmetal.dylib ] || { echo "build.sh: obj/libredwoodmetal.dylib missing"; exit 1; }
	[ obj/libredwoodmetal.dylib -nt src/redwoodmetal.m ] || {
		echo "build.sh: obj/libredwoodmetal.dylib is older than src/redwoodmetal.m; build it (IOS=../ios26) before staging"
		exit 1
	}
	cp obj/libredwoodmetal.dylib root/lib/
	# The AIR library comes from the Mesa build rather than from this script, so its architecture is
	# not decided here -- and dyld refuses an arm64 slice in an arm64e process outright, which leaves
	# src/redwoodmetal.m without a compiler: no shader is ever translated, the GPU takes no
	# submission, and the desktop ran at 9-11 frames a second where the same image with an arm64e
	# library reached 60. Only the kernel log says any of that, so the build stops here instead.
	# AIR_DYLIB names another library to install.
	air_lib=${AIR_DYLIB:-obj/libredwoodair.dylib}
	if [ -f "$air_lib" ]; then
		air_want=arm64; [ "$V" = 26 ] && air_want=arm64e
		case " $(lipo -archs "$air_lib" 2>/dev/null) " in
		*" $air_want "*) ;;
		*)
			echo "build.sh: $air_lib is '$(lipo -archs "$air_lib" 2>/dev/null)', and this image's processes load $air_want" >&2
			echo "build.sh: point AIR_DYLIB at a $air_want build of tools/mesa/air/" >&2
			exit 1
			;;
		esac
		cp "$air_lib" root/lib/libredwoodair.dylib
	fi
	if [ -f obj/RedwoodAudio ]; then
		mkdir -p root/lib/RedwoodAudio.driver
		cp obj/RedwoodAudio root/lib/RedwoodAudio.driver/
		cp src/audio_plugin.plist root/lib/RedwoodAudio.driver/Info.plist
	fi
	# The Metal plug-in bundle, flat and unsigned -- which is enough here: AMFI in this
	# kernel is a stand-in that accepts every signature (libkern/amfi/miniamfi.c), and
	# Apple's own code already loads the unsigned RedwoodAudio.driver above. The system image
	# now links this bundle into /System/Library/Extensions as well (see below), but the two
	# properties that would make Metal look for it are still nobody's by default: only a boot
	# with metal=1 lets a process put them on the service (docs/metal.md 10.31), because 10.14
	# measured what a half-made device does to every process that asks Metal for one.
	if [ -f obj/RedwoodMetal ]; then
		mkdir -p root/lib/RedwoodMetal.bundle
		cp obj/RedwoodMetal root/lib/RedwoodMetal.bundle/
		cp src/metal_plugin.plist root/lib/RedwoodMetal.bundle/Info.plist
	fi
	if [ -n "$GL" ]; then
		cp obj/libOSMesa.8.dylib root/lib/
		cp obj/gl_test root/bin/
		if [ -f obj/u2f_test ]; then cp obj/u2f_test root/bin/; fi   # an "if": see the inject.conf loop below
		[ -f obj/air_run ] && cp obj/air_run root/bin/
		[ "$GL" = test ] || cp obj/libredwood-gl.dylib root/lib/
	fi
	# a program is not started with a library that is not there: it would not start at all
	echo "$INJECT" | while read -r library names; do
		# a /redwood/lib library only if it was actually built; any other path (e.g. a tweak loader
		# in /var/jb) as it is, since it lives on the prefix and is there at run time
		case "$library" in
		"") ;;
		# an "if" and not "&& echo": a last line whose library is absent would make the loop,
		# and with it the pipeline and the whole build under set -e, fail -- which is what a tree
		# with only prebuilt/userland in it does, since not every injection library is shipped there
		/redwood/lib/*) if [ -f "root/lib/${library#/redwood/lib/}" ]; then echo "$library $names"; fi ;;
		*) echo "$library $names" ;;
		esac
	done > root/etc/inject.conf
	if [ "$V" = 26 ]; then
		# The kernel input queue has one reader; the older backboardd library also reads it.
		python3 - <<'PYINPUT'
from pathlib import Path
for line in Path('root/etc/inject.conf').read_text().splitlines():
    fields = line.split()
    if fields and Path(fields[0]).name == 'libredwood.dylib' and 'backboardd' in fields[1:]:
        raise SystemExit('build.sh: input26-service conflicts with libredwood.dylib in backboardd')
PYINPUT
	fi
	cp $IOS/var.skel root/etc/var.skel              # the directories init creates in /private/var
	# time zones: the template's links go in a circle until the time zone daemon has run (/usr/share/zoneinfo
	# points back into /var); point them at the data that ships with the system, and use this machine's zone
	zone=$( { timedatectl show -p Timezone --value 2>/dev/null || readlink /etc/localtime 2>/dev/null | sed -E 's|.*/zoneinfo/||'; } || true ); zone=${zone:-UTC}
	tmp=$(mktemp) && sed -e 's|^l\t/usr/share/zoneinfo\t/private/var/db/timezone/zoneinfo$|l\t/usr/share/zoneinfo.default\t/private/var/db/timezone/zoneinfo|' \
		-e "s|^l\t/var/db/timezone/zoneinfo/[^\t]*\t/private/var/db/timezone/localtime\$|l\t/var/db/timezone/zoneinfo/$zone\t/private/var/db/timezone/localtime|" root/etc/var.skel >"$tmp" && cat "$tmp" >root/etc/var.skel && rm -f "$tmp"
	# ... and the files of ours it puts there (var/): those under mobile/ belong to the user mobile
	cp -r var root/var
	(cd var && find . -type f | sed 's|^\./||' | sort) | while read -r f; do
		case $f in mobile/*) owner=501 ;; *) owner=0 ;; esac
		printf 'f\t644\t%s\t%s\t/private/var/%s\t/var/%s\n' $owner $owner "$f" "$f"
	done >> root/etc/var.skel
	# mobile's own temporary directory, $HOME/tmp, which iOS's template does not have: an application of
	# mobile's without a data container of its own keeps its temporary files there -- Sileo, registered from
	# the prefix with the root on the flash, downloads every package and index there, and without it each
	# download ended in "No such file or directory"
	printf 'd\t700\t501\t501\t/private/var/mobile/tmp\n' >> root/etc/var.skel
	# installd runs (the biomed round logged it being demoted as a stuck process) and has nowhere to
	# put what it builds: the install records live under mobile/Library/MobileInstallation and the
	# home screen layout under mobile/Library/SpringBoard, and the data volume template carries
	# neither directory. Without them there is nothing to write into and no records are ever created.
	cat var.skel.extra >> root/etc/var.skel
	# iOS 26's usermanagerd aborts ("Daemon failed to load persona manifest.") when there is no
	# /private/var/keybags/persona.kb; a manifest of the current format (version 1) with no users
	# lets it start, with no personas
	if [ "$V" = 26 ]; then
		# This template initializes a new RAM data volume; restore_var overlays saved data afterward.
		python3 - "$IOS/rootfs/System/Library/CoreServices/SystemVersion.plist" <<'PY'
import pathlib, plistlib, sys
build = plistlib.loads(pathlib.Path(sys.argv[1]).read_bytes())["ProductBuildVersion"]
path = pathlib.Path("root/var/mobile/Library/Preferences/com.apple.migration.plist")
record = plistlib.loads(path.read_bytes())
record["LastSystemVersion"] = build
record["DMLastMigrationResults"]["buildVersion"] = build
path.write_bytes(plistlib.dumps(record, fmt=plistlib.FMT_BINARY))
PY
		mkdir -p root/var/keybags
		python3 -c 'import plistlib, sys; sys.stdout.buffer.write(plistlib.dumps({"UsePersonaManifestVersion": 1,
			"UsePersonaGenerationID": 1, "UserPersonaDictionary": {}}, fmt=plistlib.FMT_BINARY))' > root/var/keybags/persona.kb
		printf 'd\t700\t0\t0\t/private/var/keybags\n' >> root/etc/var.skel
		printf 'f\t600\t0\t0\t/private/var/keybags/persona.kb\t/var/keybags/persona.kb\n' >> root/etc/var.skel
		# The current platform has no ExclaveOS SILManager service. Use QuartzCore's non-Medina path.
		mkdir -p root/var/mobile/Library/Preferences
		python3 -c 'import plistlib, sys; sys.stdout.buffer.write(plistlib.dumps({"CAEnableMedina": False},
			fmt=plistlib.FMT_BINARY))' > root/var/mobile/Library/Preferences/com.apple.coreanimation.plist
		printf 'f\t644\t501\t501\t/private/var/mobile/Library/Preferences/com.apple.coreanimation.plist\t/var/mobile/Library/Preferences/com.apple.coreanimation.plist\n' >> root/etc/var.skel
	fi
	# A key of ours for root, when there is one: ssh/authorized_keys, never in the repository (like
	# fw-*). dropbear from the package manager's prefix reads it and lets us in over the phone's
	# Wi-Fi, which saves rebuilding an image for every question one wants to ask a running iOS. It
	# refuses a key whose directory anyone but its owner may write, hence the 700.
	# SHARE=1: no keys in an image that is going to somebody else. ssh_host_ed25519_key is a
	# private key, and it would be leaving this machine inside a 2.5 GB container
	if [ -z "$SHARE" ] && [ -f ../ssh/authorized_keys ]; then
		mkdir -p root/var/root/.ssh
		printf 'd\t700\t0\t0\t/private/var/root/.ssh\n' >> root/etc/var.skel
		# the key that lets us in, and the host's own key beside it: made here and not on the
		# phone, because ssh-keygen asks who uid 0 is and gets "No user exists for uid 0" --
		# it asks through libiosexec, which reads a database the bootstrap does not ship (see
		# init.c, where it is now built at every boot); the key is made here all the same,
		# since nothing should depend on the order of those two
		for k in authorized_keys ssh_host_ed25519_key ssh_host_ed25519_key.pub; do
			[ -f ../ssh/$k ] || continue
			cp ../ssh/$k root/var/root/.ssh/$k
			printf 'f\t600\t0\t0\t/private/var/root/.ssh/%s\t/var/root/.ssh/%s\n' $k $k >> root/etc/var.skel
		done
	fi
	cp ${RC:-rc.launchd} root/etc/rc
	cp ${RC_JOBS:-rc.jobs} root/etc/rc.jobs
	# QUIET=1: launchd's own standard error goes nowhere (src/init.c). It says a few thousand
	# lines about its own business in the first minutes and pushes everything else out of the
	# kernel's log; the daemons keep their own standard error on the console all the same.
	rm -f root/etc/quiet
	[ -n "$QUIET" ] && : > root/etc/quiet
	# the jobs the iOS launchd will see (/System/Library/LaunchDaemons is a symlink to this directory).
	# With FLASHBOOT=1 the root will be Apple's whole volume on the flash, not the part of it that fits in
	# the system image, so mkjobs.py judges "is this daemon's program there" by the listing of the volume
	# and not by what select-system.py extracted here (--root-volume; seven jobs turn on the difference,
	# fairplayd among them).
	cp com.redwood.demo.plist com.redwood.wifi.plist root/LaunchDaemons/
	VOLUME=""
	[ -n "$FLASHBOOT" ] && [ -f $IOS/rootfs.slt ] && VOLUME="--root-volume $IOS/rootfs.slt"
	[ -n "$IOS_JOBS" ] && OSLOG=${OSLOG-com.apple.backboardd} ./mkjobs.py ${REST:+--rest} $VOLUME $IOS/rootfs root/LaunchDaemons $IOS_JOBS
fi
# firmware of the phone's own hardware, where somebody has pulled it from the phone's Android into ../fw-gpu
# (it is the vendor's and never in the repository): the kernel's GPU bring-up, bsd/dev/redwood_adreno.c, finds it in fw/
if [ -d ../fw-gpu ]; then
	mkdir -p root/fw
	cp ../fw-gpu/* root/fw/
fi
# ... and the image of the Wi-Fi DSP, from /vendor/firmware_mnt/image of the same Android into ../fw-wifi
# (bsd/dev/redwood_wifi.c; 7.6 MB), with what the radio's firmware is sent once it runs: this phone's
# board data and the regulatory database (image/qca6750 there; 60 and 24 KB)
if [ -f ../fw-wifi/wpss.mdt ]; then
	mkdir -p root/fw
	install -m 0644 ../fw-wifi/wpss.mdt ../fw-wifi/wpss.b0[2-9] root/fw/
	for f in bd_m20gl.elf regdb_xiaomi.bin; do
		[ -f ../fw-wifi/qca6750/$f ] && install -m 0644 ../fw-wifi/qca6750/$f root/fw/
	done
	# the network the radio is to join: its name on the first line, its passphrase on the second. Yours, and
	# so not in the repository; it goes where the firmware goes and the kernel reads it there (wifi_join=1)
	rm -f root/fw/network
	# SHARE=1: leave it out, for an image that is going to somebody else -- it is a passphrase
	if [ -z "$SHARE" ] && [ -f ../fw-wifi/network ]; then install -m 0600 ../fw-wifi/network root/fw/network; fi
fi
# ... and the vibration motor's waveforms, the phone's /vendor/firmware/aw8697_haptic.bin, from ../fw-haptic
# (bsd/dev/redwood_haptic.c, haptic=3; 3.6 KB, the vendor's and not in the repository either)
if [ -f ../fw-haptic/aw8697_haptic.bin ]; then
	mkdir -p root/fw
	install -m 0644 ../fw-haptic/aw8697_haptic.bin root/fw/
fi
# ... and the cameras' register tables (bsd/dev/redwood_camera.c, cam=2 and up), made by tools/camsensor.py out of
# the sensors' module files of Android's camera stack, the phone's /vendor/lib64/camera/com.qti.sensormodule.
# redwood_<maker>_<sensor>_<camera>.bin, in ../fw-camera (the vendor's: neither they nor the tables are in the
# repository): camera-<sensor>.tab
for module in ../fw-camera/com.qti.sensormodule.redwood_*.bin; do
	[ -f "$module" ] || continue
	sensor=$(basename "$module" .bin | sed 's/^com\.qti\.sensormodule\.redwood_[a-z]*_\([a-z0-9]*\)_.*/\1/')
	mkdir -p root/fw
	../tools/camsensor.py "$module" "root/fw/camera-$sensor.tab" >/dev/null
done
# ... and their colour tuning (libredwood-media's processor, src/camera_isp.c), made by tools/camtuning.py out of the
# cameras' tuning files beside them, com.qti.tuned.redwood_<maker>_<sensor>_<camera>.bin: camera-<sensor>.isp
for tuned in ../fw-camera/com.qti.tuned.redwood_*.bin; do
	[ -f "$tuned" ] || continue
	sensor=$(basename "$tuned" .bin | sed 's/^com\.qti\.tuned\.redwood_[a-z]*_\([a-z0-9]*\)_.*/\1/')
	mkdir -p root/fw
	../tools/camtuning.py "$tuned" "root/fw/camera-$sensor.isp" >/dev/null
done
# ... and with ADSP=1 the image of the audio DSP, from the same place into ../fw-audio (bsd/dev/redwood_adsp.c):
# adsp.mdt and adsp.b02 to adsp.b36, 20 MB. Only when asked for: beside GL=1 the payload would be more than the 64 MB
# the loader takes.
if [ -n "$ADSP" ] && [ -f ../fw-audio/adsp.mdt ]; then
	mkdir -p root/fw
	install -m 0644 ../fw-audio/adsp.mdt ../fw-audio/adsp.b0[2-9] ../fw-audio/adsp.b[1-9][0-9] root/fw/
fi
# Firmware that is not there is otherwise silent at build time and speaks only much later, deep in a
# device log -- "adreno: THE GPU IS NOT STARTED: the small image has no fw/a660_gmu.bin" cost a phone
# slot on 19 October 2026. It is the vendor's and never in the repository, so a copy made with bare
# git worktree add has none of it, and neither has one whose tools/new-workspace.sh predates the fw-*
# loop there. Say what went in and what was not there to go in; what each one is for is above.
fwhave= fwnone=
for d in gpu wifi haptic camera audio; do
	if [ -d ../fw-$d ]; then fwhave="$fwhave $d"; else fwnone="$fwnone $d"; fi
done
echo "build.sh: fw/: $(ls -1 root/fw 2>/dev/null | wc -l) files from ../fw-*:${fwhave:- none present}${fwnone:+, absent:$fwnone}"
# The images are what somebody would be handed to run this on a second phone, and two of the things
# build.sh puts in them are secrets: the Wi-Fi network's name and passphrase, and the host's own
# private key for ssh. Both are one small file inside 2.5 GB, and nobody would notice either leave.
secrets=
[ -f root/fw/network ] && secrets="$secrets fw/network (the name and passphrase of your Wi-Fi)"
[ -f root/var/root/.ssh/ssh_host_ed25519_key ] &&
	secrets="$secrets /var/root/.ssh/ssh_host_ed25519_key (a private key)"
if [ -n "$secrets" ]; then
	echo "build.sh: this image carries:$secrets"
	echo "build.sh: do not pass it on as it is; SHARE=1 builds the same image without them."
fi
# When the iOS volume on the phone's flash is to become the root (boot-arg root_apfs=ufs_darwin,
# with flashboot.apfs resident; src/init.c, flash_root): an empty directory to hide iOS's
# NanoLaunchDaemons and LaunchAngels with,
# xpc without launchd.plist (launchd would read that list of all of iOS's jobs instead of
# LaunchDaemons) and the audio server's plug-in directory with only our device in it.
mkdir -p root/empty root/xpc root/hal
for f in extensions.plist extensions.plist.sig; do
	[ -f $IOS/rootfs/System/Library/xpc/$f ] && cp $IOS/rootfs/System/Library/xpc/$f root/xpc/
done
# iOS 26 outside the restore environment: launchd's jobs come from the cache in xpc, made of ours
# (mkcache.py); stage0 binds root/xpc onto /System/Library/xpc
if [ "$V" = 26 ]; then
	cp $IOS/rootfs/System/Library/xpc/foreground_user_endpoints.plist root/xpc/
	./mkcache.py $IOS/rootfs/System/Library/xpc/launchd.plist root/LaunchDaemons root/xpc/launchd.plist
	# and it panics on a job whose program is not a platform binary; the kernel takes an ad hoc
	# signed program for one (xnu/files/bsd/kern/kern_minimac.c). macOS's own codesign: the one of
	# the kernel build's host tools (xnu-deps/host/bin) does nothing.
	for f in root/sbin/* root/bin/*; do
		if file -b "$f" | grep -q Mach-O && /usr/bin/codesign -dv "$f" 2>&1 | grep -q 'not signed at all'; then
			/usr/bin/codesign -s - "$f"
		fi
	done
fi
[ -d root/lib/RedwoodAudio.driver ] && ln -s /redwood/lib/RedwoodAudio.driver root/hal/RedwoodAudio.driver
# The 26 image runs where the dyld shared cache has an arm64e slice and no other (ios26/23G90__iPhone18,3
# holds dyld_shared_cache_arm64e alone), so every program in it that reaches dyld has to be an arm64e
# slice, and one Apple's clang made: a slice whose cpusubtype lacks the LIB64 bit comes from a compiler
# whose __has_feature(ptrauth) is 0, where ptrauth_strip() is the identity, so src/inject_sb26.c would
# take a signed pointer for an address. The programs of our own that never reach dyld (init, child,
# stage0: -static -nostdlib) are left alone, and so is everything that is not a Mach-O.
# Checked here, after every copy into root/ and before any image is made, because a program that
# cannot load looks exactly like one that can in the image and in the log a round reads.
# The refusal states only what this gate measured: each file's cputype and cpusubtype, and the
# requirement the gate applies. That a rejected program cannot load in the 26 image follows from
# the image carrying an arm64e-only shared cache; that consequence has not been measured on the
# device, and one log line showing a rejected program produce output would falsify it. Both the
# consequence and its falsifier are written up in the doc the refusal names.
if [ "$V" = 26 ]; then
	python3 - <<'PYARCH'
import os, struct, sys
MH_MAGIC_64, FAT_MAGIC, FAT_CIGAM = 0xfeedfacf, 0xcafebabe, 0xbebafeca
CPU_TYPE_ARM64, ARM64E, LIB64 = 0x0100000c, 2, 0x80000000
MH_EXECUTE, MH_DYLIB, MH_BUNDLE, LC_LOAD_DYLINKER = 2, 6, 8, 0xe
WANT = ARM64E | LIB64
bad = []
for dirpath, dirnames, filenames in os.walk('root'):
    for name in sorted(filenames):
        path = os.path.join(dirpath, name)
        if os.path.islink(path):
            continue
        with open(path, 'rb') as f:
            head = f.read(32)
        if len(head) < 4:
            continue
        magic, = struct.unpack('<I', head[:4])
        if magic in (FAT_MAGIC, FAT_CIGAM):
            bad.append((path, None, 'is a fat binary, and this gate requires one thin arm64e slice'))
            continue
        if magic != MH_MAGIC_64 or len(head) < 32:
            continue
        cputype, cpusubtype, filetype, ncmds = struct.unpack('<IIII', head[4:20])
        if filetype in (MH_DYLIB, MH_BUNDLE):
            dynamic = True
        elif filetype == MH_EXECUTE and cputype == CPU_TYPE_ARM64:
            dynamic = False
            with open(path, 'rb') as f:
                f.seek(32)
                for _ in range(ncmds):
                    command = f.read(8)
                    if len(command) < 8:
                        break
                    cmd, size = struct.unpack('<II', command)
                    if cmd == LC_LOAD_DYLINKER:
                        dynamic = True
                        break
                    f.seek(size - 8, 1)
        else:
            continue
        if not dynamic:
            continue
        if cputype != CPU_TYPE_ARM64:
            bad.append((path, cpusubtype, 'is cputype %#x, and this gate requires %#x (arm64e)'
                        % (cputype, CPU_TYPE_ARM64)))
        elif cpusubtype != WANT:
            what = 'is arm64e without the LIB64 bit' if cpusubtype & ~LIB64 == ARM64E else 'is an arm64 slice'
            bad.append((path, cpusubtype, '%s (cpusubtype %#x), and this gate requires %#x'
                        % (what, cpusubtype, WANT)))
for path, _, why in bad:
    print('build.sh: %s %s' % (path, why), file=sys.stderr)
if bad:
    print('build.sh: %d of the programs staged for this image are not arm64e with the LIB64 bit'
          % len(bad), file=sys.stderr)
    print('build.sh: the 26 line has to be built on a host with Apple clang; a container clang has no ptrauth', file=sys.stderr)
    print('build.sh: why this image needs arm64e, and what would falsify it: docs/progress/2026-10-28-userland-container-build-fix.md', file=sys.stderr)
    raise SystemExit(1)
PYARCH
fi
# COMPILE_ONLY=1 stops here: obj/ holds this run's programs and root/ is staged from them, and no
# image is written. root$V.apfs is what a payload gets packed from, so a run whose only job is to
# prove the toolchain works has to leave it alone; the system image below is the same risk at 4.9 GB.
if [ -n "$COMPILE_ONLY" ]; then
	echo "build.sh: COMPILE_ONLY=1, stopping before the images: obj/ is built, root/ is staged, no apfs was written"
	exit 0
fi
# the image: zlib where it saves room (a small file whole in its attribute, as Apple keeps them); fw/
# as it is, which the kernel's drivers use where it lies in memory (bsd/dev/redwood_image.c)
python3 ../tools/apfs/mkapfs.py -z --plain fw -o root$V.apfs redwood=root | tail -1
[ -z "$CHECK" ] || ../tools/apfs/check-image.sh root$V.apfs redwood=root

# --- the RAM part of a boot off the flash: FLASHBOOT=1 -> flashboot.apfs --------------------------------
# With the iOS volume on the phone's flash as the root, all that has to come from RAM is what is not
# on it -- the two cryptexes, OS with the dyld shared cache and App -- and what starts the boot: three
# APFS containers back to back in the one resident image. The first, the boot volume, is the root the
# kernel starts from (rd=md0): stage0 as /sbin/launchd mounts the small image on /redwood and runs its
# init, which with boot-arg root_apfs=DEV mounts the volume on /flash, makes it the root and moves the
# boot volume to the volume's /private/preboot, then mounts the second container -- the OS cryptex -- on
# the boot volume's empty Cryptexes/OS and the third -- Apple's App cryptex as it is -- on Cryptexes/App:
# /private/preboot/Cryptexes/OS and App, where the volume's own /System/Cryptexes/OS and App point and
# where iOS grafts the cryptexes (src/init.c, flash_root; the kernel's apfs takes "/dev/md0,c=1" and
# "c=2", and bsd/vm/vm_unix.c takes a shared cache from Cryptexes/OS). CRYPTEX_IMAGE=FILE:
# Apple's own image of the cryptex as it is instead of this one remade (2.6 GB against 1 GB: too much
# for the phone's memory beside iOS, not for QEMU's). Upload it with `xl ramdisk userland/flashboot.apfs`.
# CRYPTEXES=flash: the boot volume alone (20 MB) -- the cryptexes are Apple's images as they are on the
# flash, in darwin right after the iOS volume (../tools/darwin-write.py put them there), and init mounts
# them from there, the same containers 1 and 2 of the root's own device.
if [ -n "$FLASHBOOT" ] && [ -f $CACHE/$DSC ]; then
	rm -rf flashboot flashboot.os
	mkdir -p flashboot/sbin flashboot/redwood flashboot/dev flashboot/flash flashboot/Cryptexes/OS flashboot/Cryptexes/App
	cp obj/stage0 flashboot/sbin/launchd
	# the OS cryptex, and the shared cache in it, only where this image will carry it:
	# with CRYPTEXES=flash the cryptexes are read from the phone's flash and this tree is
	# built and thrown away unused. The guard ends with the cache: everything after it goes
	# into the boot volume, which every recipe carries, flash cryptexes included
	if [ "$CRYPTEXES" != flash ]; then
		if [ -d $IOS/cryptex ]; then cp -al $IOS/cryptex flashboot.os; else mkdir -p flashboot.os; fi
		D=flashboot.os/System/Library/Caches/com.apple.dyld
		mkdir -p $D
		for f in $CACHE/dyld_shared_cache_arm64*; do
			case "$f" in *.symbols) continue ;; esac
			# -f: a cryptex fetched with the cache already in it would otherwise stop the build
			ln -f "$f" $D/
		done
	fi
	# the audio DSP's firmware and the speaker amplifiers' configuration, as in the system image's /fw: the kernel
	# reads the firmware as soon as there is a root (this image, adsp=1), the configuration maybe later -- by then
	# from the iOS volume's /fw, which src/init.c (flash_root) binds to this
	if [ -f ../fw-audio/adsp.mdt ]; then
		mkdir -p flashboot/fw
		cp ../fw-audio/adsp.mdt ../fw-audio/adsp.b0[2-9] ../fw-audio/adsp.b[1-9][0-9] flashboot/fw/
	fi
	if [ -f ../fw-audio/aw882xx_acf.bin ]; then
		mkdir -p flashboot/fw
		cp ../fw-audio/aw882xx_acf.bin flashboot/fw/
	fi
	# the package manager's prefix, when tools/mk-jb.sh has fetched one: /jb of the boot volume, which is
	# /private/preboot/jb once the iOS volume is the root -- the volume where a rootless jailbreak keeps its
	# Procursus on an iPhone as well; init grafts it onto /var/jb (src/init.c, mount_writable)
	if [ -d ../jb/root/var/jb ]; then
		cp -a ../jb/root/var/jb flashboot/jb
		mkdir -p flashboot/jb-rw        # where init mounts the prefix's writable copy (src/init.c)
		# and what Sileo runs from /bin, for init to put over the iOS volume's /bin (see the system image below)
		mkdir -p flashboot/jb-bin
		for c in $SILEO_BIN; do
			ln -s /var/jb/usr/bin/$c flashboot/jb-bin/$c
		done
		echo "  the package manager's prefix: $(du -sh flashboot/jb | cut -f1) as /jb of the boot volume"
	fi
	python3 ../tools/apfs/mkapfs.py -z --plain fw -o flashboot.boot.apfs Boot=flashboot | tail -1
	[ -z "$CHECK" ] || ../tools/apfs/check-image.sh flashboot.boot.apfs Boot=flashboot
	# the third container: Apple's App cryptex as it is (10 MB; fetch.sh), for the boot volume's Cryptexes/App
	APP=""
	[ -f $IOS/cryptex-app.dmg ] && APP=$IOS/cryptex-app.dmg
	if [ "$CRYPTEXES" = flash ]; then
		mv flashboot.boot.apfs flashboot.apfs
	elif [ -n "$CRYPTEX_IMAGE" ]; then
		cat flashboot.boot.apfs "$CRYPTEX_IMAGE" $APP > flashboot.apfs
	else
		python3 ../tools/apfs/mkapfs.py -z -o flashboot.os.apfs OS=flashboot.os | tail -1
		[ -z "$CHECK" ] || ../tools/apfs/check-image.sh flashboot.os.apfs OS=flashboot.os
		cat flashboot.boot.apfs flashboot.os.apfs $APP > flashboot.apfs
		rm -f flashboot.os.apfs
	fi
	rm -rf flashboot flashboot.os flashboot.boot.apfs
	echo "flashboot.apfs: $(( $(stat -c %s flashboot.apfs) / 1048576 )) MB"
fi

# --- the system image -----------------------------------------------------------------------------
if [ -f $IOS/system.manifest ] && [ -f $CACHE/$DSC ] &&
	{ [ ! -f system$V.apfs ] || [ obj/stage0 -nt system$V.apfs ] || [ $IOS/system.manifest -nt system$V.apfs ] ||
	  [ $IOS/cryptex -nt system$V.apfs ] || [ $IOS/cryptex-app -nt system$V.apfs ] || [ -n "$SYSTEM" ]; }; then
	# what ios165/select-system.py has chosen since ios165/rootfs was extracted is not in it, and would not be
	# in the image either (so it was once with the audio plug-ins and the system's sounds: iOS found no
	# speaker and played nothing, though our device was there and the kernel's side worked)
	if [ -f $IOS/.done-rootfs ] && [ $IOS/select-system.py -nt $IOS/.done-rootfs ]; then
		echo "build.sh: ios165/select-system.py has changed since ios165/rootfs was extracted: run KEEP=1 ios165/fetch.sh" >&2
		exit 1
	fi
	rm -rf sysroot
	cp -al $IOS/rootfs sysroot                      # hard links: no copies of the 0.5 GB
	# launchd must load our choice of jobs, not all 500 that iOS has
	rm -rf sysroot/System/Library/LaunchDaemons sysroot/System/Library/NanoLaunchDaemons \
		sysroot/System/Library/LaunchAngels sysroot/System/Library/xpc/launchd.plist
	if [ "$V" = 26 ]; then
		mkdir sysroot/System/Library/LaunchDaemons    # stage0 binds /redwood/LaunchDaemons onto it (src/stage0.c)
	else
		ln -s /redwood/LaunchDaemons sysroot/System/Library/LaunchDaemons
	fi
	mkdir -p sysroot/redwood sysroot/sbin
	# where the kernel mounts the iOS root container off the phone's flash (boot-arg apfs_volume=):
	# iOS has no /System/Volumes, and nothing can make a directory in this image once it is built
	mkdir -p sysroot/System/Volumes/iOS
	# the applications iOS installs on its first boot are system applications here (see select-system.py)
	if [ -d sysroot/private/var/staged_system_apps ]; then
		mv sysroot/private/var/staged_system_apps/* sysroot/Applications/
		rm -rf sysroot/private/var
	fi
	mv sysroot/sbin/launchd sysroot/sbin/launchd.ios
	cp obj/stage0 sysroot/sbin/launchd
	# the audio DSP's firmware, the phone's own from ../fw-audio (never in the repository), where the kernel reads
	# it once the root is mounted (bsd/dev/redwood_adsp.c, adsp=1): 20 MB that do not fit beside GL=1 in the small image
	if [ -f ../fw-audio/adsp.mdt ]; then
		mkdir -p sysroot/fw
		cp ../fw-audio/adsp.mdt ../fw-audio/adsp.b0[2-9] ../fw-audio/adsp.b[1-9][0-9] sysroot/fw/
	fi
	# ... and the speaker amplifiers' configuration, the phone's /vendor/etc/aw882xx_acf.bin (bsd/dev/redwood_speaker.c,
	# for sound=3): Android's own register values, which are not in the repository either
	if [ -f ../fw-audio/aw882xx_acf.bin ]; then
		mkdir -p sysroot/fw
		cp ../fw-audio/aw882xx_acf.bin sysroot/fw/
	fi
	# the package manager's prefix, when tools/mk-jb.sh has fetched one: the Procursus bootstrap with
	# Sileo in it, as /jb. It cannot live at /var/jb in the image -- /private/var is a memory
	# filesystem made afresh at every boot -- so init copies it there (src/init.c, copy_tree).
	if [ -d ../jb/root/var/jb ]; then
		rm -rf sysroot/jb
		cp -a ../jb/root/var/jb sysroot/jb
		# the applications of the prefix are system applications here: LaunchServices builds its
		# database afresh at every boot by walking /Applications, which is how their icons appear
		for app in sysroot/jb/Applications/*.app; do
			[ -d "$app" ] && cp -a "$app" sysroot/Applications/
		done
		# Sileo moves, copies and removes files as root with /bin/mv, /bin/cp, /bin/rm, /bin/mkdir and
		# /bin/chmod -- where a jailbreak with a writable root keeps them -- and iOS's /bin has df and ps
		# alone: every one of those spawns failed with nothing said, and the package it had downloaded never
		# reached apt's archives ("Unable to fetch some archives"). The prefix's own programs, by those names.
		for c in $SILEO_BIN; do
			ln -sf /var/jb/usr/bin/$c sysroot/bin/$c
		done
		echo "  the package manager's prefix: $(du -sh sysroot/jb | cut -f1) as /jb (init puts it at /var/jb)"
	fi
	# An application of this port's own, registered the same free way the prefix's are: a plain
	# directory in /Applications, which the LaunchServices database is rebuilt from at every boot
	# by walking that directory -- no installd, no container, no uicache. cp -a and not a link,
	# because a bundle of symbolic links is refused ("Unable to register") and the boot-time
	# registrar passes over mount points.
	#
	# This is in the SYSTEM image and not the small one, so a change to it costs a 2.5 GB rebuild
	# and a re-upload: SYSTEM=1 is what makes this line run again.
	if [ -d obj/RedwoodTriangle.app ]; then
		cp -a obj/RedwoodTriangle.app sysroot/Applications/
		echo "  RedwoodTriangle.app in /Applications (com.redwood.triangle, scheme redwoodtriangle:)"
	fi
	# Applications from outside this tree: every .app in ../apps, which is kept out of the
	# repository the way ../fw-* and ../jb are, so that nothing of somebody else's package can
	# reach it. They go into /Applications beside the port's own on exactly the same terms --
	# a plain directory, which LaunchServices finds by walking that folder at every boot.
	#
	# cp -aL and not cp -a: a bundle made of symbolic links is refused ("Unable to register"),
	# so whatever a link points at is copied as the file itself.
	for app in ../apps/*.app; do
		[ -d "$app" ] || continue
		cp -aL "$app" sysroot/Applications/
		echo "  ${app##*/} in /Applications ($(du -sm "$app" | cut -f1) MB from ../apps)"
	done
	# our audio device, where the audio server looks for such plug-ins (and for no others: Apple's are left out)
	mkdir -p sysroot/System/Library/Audio/Plug-Ins/HAL
	ln -s /redwood/lib/RedwoodAudio.driver sysroot/System/Library/Audio/Plug-Ins/HAL/RedwoodAudio.driver
	# ... and our Metal plug-in, at the one path Metal's own discovery can open: step 3 composes
	# "/System/Library/Extensions/" + MetalPluginName + ".bundle" and opens it with NSBundle
	# (docs/metal.md 1). Same shape as the audio plug-in above -- a link into /redwood/lib, where
	# the bundle itself travels in the small image -- because this directory is iOS's own and
	# nothing can be added to it after the image is built.
	#
	# This link is inert on a boot that does not ask for Metal: nothing composes that path until
	# MetalPluginName is on the IOAcceleratorES service, and only boot-arg metal=1 lets a process
	# put it there (iokit/Kernel/MiniIOMobileFramebuffer.cpp). So it is not behind a switch of its
	# own -- a switch here would cost a 2.5 GB system.apfs rebuild and a re-upload every time it
	# was flipped, and buy nothing a missing boot-arg does not already buy.
	mkdir -p sysroot/System/Library/Extensions
	ln -s /redwood/lib/RedwoodMetal.bundle sysroot/System/Library/Extensions/RedwoodMetal.bundle
	# the OS cryptex: on a device a separate volume grafted at /System/Cryptexes/OS. It holds the dyld
	# shared cache (dyld looks there first) and a few libraries and resources outside the cache.
	mkdir -p sysroot/System/Cryptexes
	if [ -d $IOS/cryptex ]; then cp -al $IOS/cryptex sysroot/System/Cryptexes/OS; fi
	# the App cryptex, the same way: Safari's daemons (eight launchd jobs run them from there), its settings and
	# the bundles around it, which the root's links reach (System/Library/PreferenceBundles/MobileSafariSettings.bundle...)
	if [ -d $IOS/cryptex-app ]; then cp -al $IOS/cryptex-app sysroot/System/Cryptexes/App; fi
	D=sysroot/System/Cryptexes/OS/System/Library/Caches/com.apple.dyld
	mkdir -p $D
	for f in $CACHE/dyld_shared_cache_arm64*; do
		case "$f" in *.symbols) continue ;; esac     # symbolication data, never mapped
		# -f: a cryptex fetched with the cache already in it would otherwise stop the build
		ln -f "$f" $D/
	done
	# zlib where it saves room, as Apple keeps the root (a small file whole in its attribute): 5.4 GB of
	# files in 2.5 GB, what fits into the phone's RAM beside iOS
	python3 ../tools/apfs/mkapfs.py -z -o system$V.apfs System=sysroot,manifest=$IOS/system.manifest | tail -1
	[ -z "$CHECK" ] || ../tools/apfs/check-image.sh system$V.apfs System=sysroot,manifest=$IOS/system.manifest
	rm -rf sysroot
fi
