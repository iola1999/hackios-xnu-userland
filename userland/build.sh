#!/bin/bash
# Build the user-space side of the bring-up for iOS 26.6.2 (23G90, iPhone 17).
#
#   hello            one raw-syscall program; as a RAM disk on its own it boots through mockfs
#   root.apfs        the small image: our init and test programs, etc/rc, and the launchd jobs
#                    (LaunchDaemons/), an APFS container (../tools/apfs/mkapfs.py). It travels in
#                    the payload next to the kernel (the second RAM disk, /dev/md1) and is mounted
#                    on /annibale. Rebuilt every time.
#                      RC=<file>    etc/rc: rc.launchd (default) hands process 1 to the iOS launchd
#                                   after init's checks; rc.ios runs a few iOS programs and halts
#                      JOBS="..."   labels of the stock iOS launchd jobs to load (default: IOS_JOBS)
#                      REST=1       ... and the rest of them, started only when asked for (see mkjobs.py)
#                      WIFID=0      without wifid, iOS's Wi-Fi daemon, which is in by default (see below)
#                      OSLOG="..."  labels of the jobs whose os_log output goes to the console
#                      INJECT="..." the programs started with a library of ours, "LIBRARY NAME..." per line
#                      STOREFRONT=1 ... and src/inject_store.c inside appstored
#                      EXTRA="..."  more programs for /annibale/bin (files on this machine)
#                      EXTRA_SHARE="..." directories on this machine to copy whole into /annibale/share
#                      RC_JOBS=<file> what the job com.annibale.demo runs: rc.jobs (default)
#                      GL=1         with an OpenGL ES implementation for the window server
#                      ADSP=1       with the audio DSP's firmware from ../fw-audio in fw/
#   system26.apfs    the system image, the root filesystem (the resident RAM disk, /dev/md0)
#   flashboot.apfs   FLASHBOOT=1: the small boot volume for a boot whose root is the iOS volume on flash
#   CHECK=1          check every image (every container) as it is made
#   COMPILE_ONLY=1   compile into obj/ and stage into root/, then stop before any image is made
#   FLASHBOOT_ONLY=1 with FLASHBOOT=1 CRYPTEXES=flash, compile stage0 and build a K90 boot volume
set -e
cd "$(dirname "$(readlink -f "$0")")"
for _d in "$PWD/../../hackios-xnu-kernel/xnu-deps/host/bin" "$PWD/../xnu-deps/host/bin"; do
	[ -d "$_d" ] && PATH="$_d:$PATH"
done
unset _d
export PATH
DRV=../../hackios-xnu-kernel/tools/userland-driver
[ -d "$DRV" ] || DRV=../tools/userland-driver

CC="xnu-clang -arch arm64 -Os -static -nostdlib -ffreestanding -fno-stack-protector -fno-builtin -Wall -Wextra"
LD="-Wl,-e,_start -Wl,-pagezero_size,0x100000000"
SRC_INC="-Isrc/boot -Isrc/inject -Isrc/gfx -Isrc/services -Isrc/apps -Isrc/tests -I$DRV"

sign_adhoc() {
	target="$1"
	[ -f "$target" ] || return 0
	if [ "$(uname)" = Darwin ]; then
		/usr/bin/codesign --force --sign - "$target" 2>/dev/null || codesign --force --sign - "$target"
	else
		${LDID:-ldid} -S "$target"
	fi
}

IOS=${IOS:-../ios26}
CACHE=$IOS/23G90__iPhone18,3
DSC=dyld_shared_cache_arm64e
V=26
if [ -n "$FLASHBOOT_ONLY" ]; then
	[ -n "$FLASHBOOT" ] || { echo "build.sh: FLASHBOOT_ONLY=1 requires FLASHBOOT=1" >&2; exit 1; }
	[ "$CRYPTEXES" = flash ] || { echo "build.sh: FLASHBOOT_ONLY=1 requires CRYPTEXES=flash" >&2; exit 1; }
	boot_tree=$(mktemp -d .flashboot.XXXXXX)
	trap 'rm -rf "$boot_tree"' EXIT
	$CC -Ilibmini -c libmini/mini.c -o "$boot_tree/mini.o"
	$CC -c libmini/start.S -o "$boot_tree/start.o"
	mkdir -p "$boot_tree/root"/{sbin,annibale,dev,flash,Cryptexes/OS,Cryptexes/App}
	$CC -Ilibmini $LD -o "$boot_tree/root/sbin/launchd" src/boot/stage0.c "$boot_tree/mini.o" "$boot_tree/start.o"
	sign_adhoc "$boot_tree/root/sbin/launchd"
	python3 ../tools/apfs/mkapfs.py -z -o flashboot.apfs "Boot=$boot_tree/root"
	[ -z "$CHECK" ] || ../tools/apfs/check-image.sh flashboot.apfs "Boot=$boot_tree/root"
	exit 0
fi

python3 "$IOS/full-system.py" check "$IOS" || exit 1

for d in rootfs cryptex cryptex-app; do
	[ -L $IOS/$d ] || continue
	echo "build.sh: ios26/$d is a symlink to $(readlink $IOS/$d)" >&2
	echo "build.sh: it has to be a directory of this copy's own: rm it and cp -al the original over." >&2
	echo "build.sh: Building against a link damages the shared tree." >&2
	exit 1
done

IOS_JOBS=${JOBS-"
	com.apple.notifyd com.apple.cfprefsd.xpc.daemon com.apple.cfprefsd.xpc.daemon.system com.apple.logd
	com.apple.syslogd com.apple.distnoted.xpc.daemon com.apple.diagnosticd com.apple.fseventsd
	com.apple.containermanagerd com.apple.containermanagerd.system com.apple.runningboardd
	com.apple.backboardd com.apple.mobile.usermanagerd com.apple.mobilegestalt.xpc com.apple.lsd
	com.apple.powerd com.apple.analyticsd com.apple.logd_helper
	com.apple.iconservices.iconservicesagent com.apple.pluginkit.pkd
	com.apple.SpringBoard
"}
if [ "${JOBS+x}" != x ]; then
	IOS_JOBS="$IOS_JOBS com.apple.systemstatusd com.apple.fontservicesd"
fi
[ "${WIFID-1}" != 0 ] && IOS_JOBS="$IOS_JOBS com.apple.wifid"

SILEO_BIN="chmod cp mkdir mv rm"
INJECT=${INJECT-"/annibale/lib/libannibale-sb26.dylib SpringBoard PosterBoard MercuryPosterExtension WidgetRenderer_Default SharingUIService Preferences lsd wifid
/annibale/lib/libannibale-vt.dylib videocodecd mediaserverd com.apple.WebKit.GPU video_decode_test video_player_test"}

[ -n "$STOREFRONT" ] && INJECT="$INJECT
/annibale/lib/libannibale-store.dylib appstored AppStore
/annibale/lib/libannibale-fairplay.dylib fairplayd.H2"

mkdir -p obj obj/.deps

$CC $LD -o hello src/tests/hello.c
sign_adhoc hello
$CC -Ilibmini -c libmini/mini.c -o obj/mini.o
$CC -c libmini/start.S -o obj/start.o
for prog in init child stage0; do
	$CC -Ilibmini $LD -o obj/$prog.new src/boot/$prog.c obj/mini.o obj/start.o
	if cmp -s obj/$prog.new obj/.deps/$prog.raw 2>/dev/null && [ -f obj/$prog ]; then
		rm obj/$prog.new
	else
		cp obj/$prog.new obj/.deps/$prog.raw
		mv obj/$prog.new obj/$prog
		sign_adhoc obj/$prog
	fi
	if [ "$(uname)" = Darwin ]; then
		if /usr/bin/codesign -dv obj/$prog 2>&1 | grep -q 'not signed at all'; then
			sign_adhoc obj/$prog
		fi
	else
		sign_adhoc obj/$prog
	fi
done

if [ "$(uname)" = Darwin ]; then
	DYN="/usr/bin/xcrun -sdk iphoneos clang -arch arm64e -miphoneos-version-min=26.0 -O1 $SRC_INC"
else
	DYN="xnu-clang -arch arm64e -isysroot ../xnu-deps/26/sdk/iPhoneOS.sdk -miphoneos-version-min=26.0 -O1 $SRC_INC"
fi

if [ -d "$IOS/rootfs/usr/lib" ]; then
	python3 compile_targets.py --drv-dir "$DRV"
	if [ -n "$GL" ]; then
		M=../xnu-deps/build/mesa/out
		MESA_BUILD="REBASE=26 VERSION=26.2.5 SHA256=794b3d1729075dae7ebd47c7283b7ca0cdb5dec668f7f11cc0a5d82f6138a0a5"
		. ../tools/mesa-stamp.sh
		MESA_FROM=$(mesa_from_stamp .. 26.2.5 26)
		[ "$(cat $M/built-from 2>/dev/null)" = "$MESA_FROM" ] || env $MESA_BUILD ../tools/mk-mesa.sh
		[ "$(cat $M/built-from-air 2>/dev/null)" = "$(mesa_air_stamp ..)" ] || {
			echo "build.sh: tools/mesa/air/ has changed since libannibaleair.dylib was built"
			env AIR=1 $MESA_BUILD ../tools/mk-mesa.sh
		}
		./mkeagl.py src/gfx/eagl_slots.txt $M/gl.xml $M/exported-gl.txt src/gfx/eagl.c > obj/eagl_gen.h
		$DYN -dynamiclib -install_name /annibale/lib/libannibale-gl.dylib -Iobj -framework CoreFoundation \
			-framework IOKit -framework IOSurface -lobjc $M/libOSMesa.8.dylib -o obj/libannibale-gl.dylib \
			src/gfx/eagl.c src/gfx/gl_osmesa.c
		$DYN -framework CoreFoundation -framework IOKit $M/libOSMesa.8.dylib -o obj/gl_test src/tests/gl_test.c
		$DYN $M/libOSMesa.8.dylib -o obj/u2f_test src/tests/u2f_test.c
		cp $M/libOSMesa.8.dylib obj/
		[ -f $M/air_run ] && cp $M/air_run obj/air_run
		[ -f $M/libannibaleair.dylib ] && cp $M/libannibaleair.dylib obj/
		sign_adhoc obj/libannibale-gl.dylib
		sign_adhoc obj/gl_test
		[ -f obj/u2f_test ] && sign_adhoc obj/u2f_test
	fi
else
	python3 compile_targets.py --drv-dir "$DRV" --minimal
fi

# --- the small image ------------------------------------------------------------------------------
rm -rf root
mkdir -p root/sbin root/bin root/etc root/share root/LaunchDaemons
cp obj/init root/sbin/init
sign_adhoc root/sbin/init
cp obj/child root/bin/child
sign_adhoc root/bin/child
cp obj/input26-service root/bin/
cp com.annibale.input26.plist root/LaunchDaemons/
echo "Darwin on annibale: image built $(date '+%Y-%m-%d %H:%M')" > root/etc/motd
python3 -c "import sys; sys.stdout.buffer.write(bytes((i*31 + (i>>8)) & 0xff for i in range(300000)))" > root/share/pattern.bin

for d in $EXTRA_SHARE; do
	[ -d "$d" ] || { echo "build.sh: EXTRA_SHARE: $d is not a directory"; exit 1; }
	cp -rL "$d" root/share/
done

if [ -d $IOS/rootfs/usr/lib ]; then
	for t in hello_dyn svc_test surf_test drawable_test anntouch annsh annsig framewatch lsprobe mg_test \
		cpu_test cow_test cg_test metal_dev metal_stubdev metal_drv_test ubo_test metal_caps metal_props \
		shm_test apfs_mmap_test vol_ls snap_test font_test metal_test net_test store_test \
		battery_test keychain_test attn_test wifipw_test a80211_test sound_test torch_test camera_test camera_avtest \
		record_test video_decode_test video_player_test video_combined_test; do
		[ -f obj/$t ] && cp obj/$t root/bin/
	done

	[ -f obj/AnnibaleTriangle ] && cp obj/AnnibaleTriangle root/bin/triangle_app
	for f in $EXTRA; do
		if [ "$(basename "$f")" = input26-service ]; then
			echo "build.sh: EXTRA cannot replace the validated input26-service" >&2
			exit 1
		fi
		cp "$f" root/bin/
	done

	if [ -f obj/AnnibaleTriangle ]; then
		rm -rf obj/AnnibaleTriangle.app
		mkdir -p obj/AnnibaleTriangle.app
		cp obj/AnnibaleTriangle obj/AnnibaleTriangle.app/
		cp src/tests/triangle_app.plist obj/AnnibaleTriangle.app/Info.plist
		printf 'APPL????' > obj/AnnibaleTriangle.app/PkgInfo
		python3 - obj/AnnibaleTriangle.app <<-'PY'
		import struct, sys, zlib
		out = sys.argv[1]
		for size, scale in ((120, 2), (180, 3)):
		    rows = bytearray()
		    for y in range(size):
		        rows.append(0)
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
		echo "  AnnibaleTriangle.app: $(du -sk obj/AnnibaleTriangle.app | cut -f1) KB for /Applications"
	fi

	mkdir -p root/dev root/private/var root/lib
	for l in libannibale.dylib libannibale-sb26.dylib libannibale-sbmetal-experiment.dylib \
		libannibale-media.dylib libannibale-vt.dylib libannibale-store.dylib \
		libannibale-fairplay.dylib libannibale-app.dylib libannibale-tweaks.dylib \
		libannibalemetal.dylib; do
		[ -f obj/$l ] && cp obj/$l root/lib/
	done

	air_lib=${AIR_DYLIB:-../out/ios26-resume/bbmetal-device/libannibaleair.dylib}
	if [ -f "$air_lib" ]; then
		case " $(lipo -archs "$air_lib" 2>/dev/null) " in
		*" arm64e "*) ;;
		*)
			echo "build.sh: $air_lib is '$(lipo -archs "$air_lib" 2>/dev/null)', and this image's processes load arm64e" >&2
			echo "build.sh: point AIR_DYLIB at a arm64e build of tools/mesa/air/" >&2
			exit 1
			;;
		esac
		cp "$air_lib" root/lib/libannibaleair.dylib
	fi

	if [ -f obj/AnnibaleAudio ]; then
		mkdir -p root/lib/AnnibaleAudio.driver
		cp obj/AnnibaleAudio root/lib/AnnibaleAudio.driver/
		cp src/services/audio_plugin.plist root/lib/AnnibaleAudio.driver/Info.plist
	fi

	if [ -f obj/AnnibaleMetal ]; then
		mkdir -p root/lib/AnnibaleMetal.bundle
		cp obj/AnnibaleMetal root/lib/AnnibaleMetal.bundle/
		cp src/gfx/metal_plugin.plist root/lib/AnnibaleMetal.bundle/Info.plist
	fi

	if [ -n "$GL" ]; then
		cp obj/libOSMesa.8.dylib root/lib/
		cp obj/gl_test root/bin/
		if [ -f obj/u2f_test ]; then cp obj/u2f_test root/bin/; fi
		[ -f obj/air_run ] && cp obj/air_run root/bin/
		[ "$GL" = test ] || cp obj/libannibale-gl.dylib root/lib/
	fi

	echo "$INJECT" | while read -r library names; do
		case "$library" in
		"") ;;
		/annibale/lib/*) if [ -f "root/lib/${library#/annibale/lib/}" ]; then echo "$library $names"; fi ;;
		*) echo "$library $names" ;;
		esac
	done > root/etc/inject.conf

	python3 - <<'PYINPUT'
from pathlib import Path
for line in Path('root/etc/inject.conf').read_text().splitlines():
    fields = line.split()
    if fields and Path(fields[0]).name == 'libannibale.dylib' and 'backboardd' in fields[1:]:
        raise SystemExit('build.sh: input26-service conflicts with libannibale.dylib in backboardd')
PYINPUT

	cp $IOS/var.skel root/etc/var.skel
	zone=$( { timedatectl show -p Timezone --value 2>/dev/null || readlink /etc/localtime 2>/dev/null | sed -E 's|.*/zoneinfo/||'; } || true ); zone=${zone:-UTC}
	tmp=$(mktemp) && sed -e 's|^l\t/usr/share/zoneinfo\t/private/var/db/timezone/zoneinfo$|l\t/usr/share/zoneinfo.default\t/private/var/db/timezone/zoneinfo|' \
		-e "s|^l\t/var/db/timezone/zoneinfo/[^\t]*\t/private/var/db/timezone/localtime\$|l\t/var/db/timezone/zoneinfo/$zone\t/private/var/db/timezone/localtime|" root/etc/var.skel >"$tmp" && cat "$tmp" >root/etc/var.skel && rm -f "$tmp"
	cp -r var root/var
	(cd var && find . -type f | sed 's|^\./||' | sort) | while read -r f; do
		case $f in mobile/*) owner=501 ;; *) owner=0 ;; esac
		printf 'f\t644\t%s\t%s\t/private/var/%s\t/var/%s\n' $owner $owner "$f" "$f"
	done >> root/etc/var.skel
	printf 'd\t700\t501\t501\t/private/var/mobile/tmp\n' >> root/etc/var.skel
	cat var.skel.extra >> root/etc/var.skel

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
	mkdir -p root/var/mobile/Library/Preferences
	python3 -c 'import plistlib, sys; sys.stdout.buffer.write(plistlib.dumps({"CAEnableMedina": False},
		fmt=plistlib.FMT_BINARY))' > root/var/mobile/Library/Preferences/com.apple.coreanimation.plist
	printf 'f\t644\t501\t501\t/private/var/mobile/Library/Preferences/com.apple.coreanimation.plist\t/var/mobile/Library/Preferences/com.apple.coreanimation.plist\n' >> root/etc/var.skel
	mkdir -p root/var/db/fontconfig
	chmod 0755 root/var/db/fontconfig
	printf 'd\t755\t0\t0\t/private/var/db/fontconfig\n' >> root/etc/var.skel

	if [ -z "$SHARE" ] && [ -f ../ssh/authorized_keys ]; then
		mkdir -p root/var/root/.ssh
		printf 'd\t700\t0\t0\t/private/var/root/.ssh\n' >> root/etc/var.skel
		for k in authorized_keys ssh_host_ed25519_key ssh_host_ed25519_key.pub; do
			[ -f ../ssh/$k ] || continue
			cp ../ssh/$k root/var/root/.ssh/$k
			printf 'f\t600\t0\t0\t/private/var/root/.ssh/%s\t/var/root/.ssh/%s\n' $k $k >> root/etc/var.skel
		done
	fi

	cp ${RC:-rc.launchd} root/etc/rc
	rc_jobs_src=${RC_JOBS:-jobs/rc.jobs}
	[ -f "$rc_jobs_src" ] || [ ! -f "jobs/$rc_jobs_src" ] || rc_jobs_src="jobs/$rc_jobs_src"
	cp "$rc_jobs_src" root/etc/rc.jobs

	rm -f root/etc/quiet
	[ -n "$QUIET" ] && : > root/etc/quiet

	cp com.annibale.demo.plist com.annibale.wifi.plist com.annibale.trollstore.plist root/LaunchDaemons/
	VOLUME=""
	[ -n "$FLASHBOOT" ] && [ -f $IOS/rootfs.slt ] && VOLUME="--root-volume $IOS/rootfs.slt"
	[ -n "$IOS_JOBS" ] && OSLOG=${OSLOG-com.apple.backboardd} ./mkjobs.py ${REST:+--rest} $VOLUME $IOS/rootfs root/LaunchDaemons $IOS_JOBS
fi

# the GPU firmware Qualcomm redistributes is in the repository (firmware-gen8/README.md); ../fw-gpu holds
# the phone's own gen80000_zap.mbn
mkdir -p root/fw
cp ../firmware-gen8/gen80000_aqe.fw ../firmware-gen8/gen80000_gmu.bin ../firmware-gen8/gen80000_sqe.fw \
	../firmware-gen8/LICENSE.qcom.txt ../firmware-gen8/NOTICE.qcom.txt root/fw/
if [ -d ../fw-gpu ]; then
	cp ../fw-gpu/* root/fw/
fi

camera_tools=../../hackios-xnu-kernel/tools
[ -f "$camera_tools/camsensor.py" ] || camera_tools=../tools
if [ ! -f "$camera_tools/camsensor.py" ]; then
	echo "build.sh: missing camera profile exporter: $camera_tools/camsensor.py" >&2
	exit 1
fi
mkdir -p root/fw
for camera_sensor in front wide; do
	case "$camera_sensor" in
		front)
			camera_module=../fw-camera/com.qti.sensormodule.annibale_sunny_ov20b40_front_i.bin
			camera_mode=4
			camera_output=camera-ov20b-front-mode4.tab
			camera_tuned=../fw-camera/com.qti.tuned.annibale_sunny_ov20b40_front_i.bin
			camera_isp=camera-ov20b40.isp
			;;
		wide)
			camera_module=../fw-camera/com.qti.sensormodule.annibale_aac_ovx8000_wide_i.bin
			camera_mode=24
			camera_output=camera-ovx8000-wide-mode24.tab
			camera_tuned=../fw-camera/com.qti.tuned.annibale_aac_ovx8000_wide_i.bin
			camera_isp=camera-ovx8000.isp
			;;
	esac
	if [ ! -f "$camera_module" ]; then
		echo "build.sh: missing original camera sensor module: $camera_module" >&2
		exit 1
	fi
	python3 "$camera_tools/camsensor.py" --profile "$camera_module" "$camera_mode" "root/fw/$camera_output" >/dev/null
	# The tuning's lens shading is laid over the part of the sensor this mode reads.
	if [ -f "$camera_tuned" ]; then
		python3 "$camera_tools/camtuning.py" "$camera_tuned" "root/fw/$camera_isp" "$camera_module" "$camera_mode" >/dev/null
	fi
done
unset camera_module camera_tools camera_sensor camera_mode camera_output camera_tuned camera_isp
python3 ./mkcameraresources.py "$IOS/rootfs/System/Library/PrivateFrameworks/CMCapture.framework/V57" \
	root/lib/CMCaptureResources/V57

if [ -n "$ADSP" ] && [ -f ../fw-audio/adsp.mdt ]; then
	mkdir -p root/fw
	install -m 0644 ../fw-audio/adsp.mdt ../fw-audio/adsp.b0[2-9] ../fw-audio/adsp.b[1-9][0-9] root/fw/
fi
for _iris_fw in ../out/fw-iris/vpu35_4v.mbn ../fw-iris/vpu35_4v.mbn; do
	if [ -f "$_iris_fw" ]; then
		mkdir -p root/fw
		cp "$_iris_fw" root/fw/vpu35_4v.mbn
		break
	fi
done
unset _iris_fw

fwhave= fwnone=
for d in gpu camera audio; do
	if [ -d ../fw-$d ]; then fwhave="$fwhave $d"; else fwnone="$fwnone $d"; fi
done
echo "build.sh: fw/: $(ls -1 root/fw 2>/dev/null | wc -l) files from ../fw-*:${fwhave:- none present}${fwnone:+, absent:$fwnone}"

secrets=
[ -f root/var/root/.ssh/ssh_host_ed25519_key ] &&
	secrets="$secrets /var/root/.ssh/ssh_host_ed25519_key (a private key)"
if [ -n "$secrets" ]; then
	echo "build.sh: this image carries:$secrets"
	echo "build.sh: do not pass it on as it is; SHARE=1 builds the same image without them."
fi

mkdir -p root/empty root/xpc root/hal
for f in extensions.plist extensions.plist.sig; do
	[ -f $IOS/rootfs/System/Library/xpc/$f ] && cp $IOS/rootfs/System/Library/xpc/$f root/xpc/
done
if [ -f ../../TrollStore/build_k90.sh ]; then
	bash ../../TrollStore/build_k90.sh "$PWD/obj" "$PWD/root"
fi

cp $IOS/rootfs/System/Library/xpc/foreground_user_endpoints.plist root/xpc/
./mkcache.py $IOS/rootfs/System/Library/xpc/launchd.plist root/LaunchDaemons root/xpc/launchd.plist
for f in root/sbin/* root/bin/* root/lib/* root/lib/*/*; do
	if [ -f "$f" ] && file -b "$f" | grep -q Mach-O; then
		if [ "$(uname)" = Darwin ]; then
			if /usr/bin/codesign -dv "$f" 2>&1 | grep -q 'not signed at all'; then
				sign_adhoc "$f"
			fi
		else
			sign_adhoc "$f"
		fi
	fi
done
[ -d root/lib/AnnibaleAudio.driver ] && ln -sf /annibale/lib/AnnibaleAudio.driver root/hal/AnnibaleAudio.driver

if [ -n "$COMPILE_ONLY" ]; then
	echo "build.sh: COMPILE_ONLY=1, stopping before the images: obj/ is built, root/ is staged, no apfs was written"
	exit 0
fi

python3 ../tools/apfs/mkapfs.py -z --plain fw -o root$V.apfs annibale=root | tail -1
[ -z "$CHECK" ] || ../tools/apfs/check-image.sh root$V.apfs annibale=root

# --- the RAM part of a boot off the flash: FLASHBOOT=1 -> flashboot.apfs --------------------------------
if [ -n "$FLASHBOOT" ] && [ -f "$CACHE/$DSC" ]; then
	rm -rf flashboot flashboot.os
	mkdir -p flashboot/sbin flashboot/annibale flashboot/dev flashboot/flash flashboot/Cryptexes/OS flashboot/Cryptexes/App
	cp obj/stage0 flashboot/sbin/launchd
	sign_adhoc flashboot/sbin/launchd

	if [ "$CRYPTEXES" != flash ]; then
		if [ -d $IOS/cryptex ]; then cp -al $IOS/cryptex flashboot.os; else mkdir -p flashboot.os; fi
		D=flashboot.os/System/Library/Caches/com.apple.dyld
		mkdir -p $D
		for f in $CACHE/dyld_shared_cache_arm64*; do
			case "$f" in *.symbols) continue ;; esac
			ln -f "$f" $D/
		done
	fi

	if [ -f ../fw-audio/adsp.mdt ]; then
		mkdir -p flashboot/fw
		cp ../fw-audio/adsp.mdt ../fw-audio/adsp.b0[2-9] ../fw-audio/adsp.b[1-9][0-9] flashboot/fw/
	fi
	if [ -f ../fw-audio/aw882xx_acf.bin ]; then
		mkdir -p flashboot/fw
		cp ../fw-audio/aw882xx_acf.bin flashboot/fw/
	fi

	if [ -d ../jb/root/var/jb ]; then
		cp -a ../jb/root/var/jb flashboot/jb
		mkdir -p flashboot/jb-rw
		mkdir -p flashboot/jb-bin
		for c in $SILEO_BIN; do
			ln -sf /var/jb/usr/bin/$c flashboot/jb-bin/$c
		done
		echo "  the package manager's prefix: $(du -sh flashboot/jb | cut -f1) as /jb of the boot volume"
	fi
	python3 ../tools/apfs/mkapfs.py -z --plain fw -o flashboot.boot.apfs Boot=flashboot | tail -1
	[ -z "$CHECK" ] || ../tools/apfs/check-image.sh flashboot.boot.apfs Boot=flashboot

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
	echo "flashboot.apfs: $(( $(stat -c %s flashboot.apfs 2>/dev/null || stat -f %z flashboot.apfs) / 1048576 )) MB"
fi

# --- the system image -----------------------------------------------------------------------------
if [ -f $IOS/system.manifest ] && [ -f $CACHE/$DSC ] &&
	{ [ ! -f system$V.apfs ] || [ obj/stage0 -nt system$V.apfs ] || [ $IOS/system.manifest -nt system$V.apfs ] ||
	  [ $IOS/.done-rootfs -nt system$V.apfs ] || [ $IOS/cryptex -nt system$V.apfs ] ||
	  [ $IOS/cryptex-app -nt system$V.apfs ] || [ -n "$SYSTEM" ]; }; then
	rm -rf sysroot
	cp -al $IOS/rootfs sysroot
	rm -rf sysroot/System/Library/LaunchDaemons sysroot/System/Library/NanoLaunchDaemons \
		sysroot/System/Library/LaunchAngels sysroot/System/Library/xpc/launchd.plist
	mkdir sysroot/System/Library/LaunchDaemons
	mkdir -p sysroot/annibale sysroot/sbin
	mkdir -p sysroot/System/Volumes/iOS
	if [ -d sysroot/private/var/staged_system_apps ]; then
		mv sysroot/private/var/staged_system_apps/* sysroot/Applications/
		rm -rf sysroot/private/var
	fi
	mv sysroot/sbin/launchd sysroot/sbin/launchd.ios
	cp obj/stage0 sysroot/sbin/launchd
	sign_adhoc sysroot/sbin/launchd

	if [ -f ../fw-audio/adsp.mdt ]; then
		mkdir -p sysroot/fw
		cp ../fw-audio/adsp.mdt ../fw-audio/adsp.b0[2-9] ../fw-audio/adsp.b[1-9][0-9] sysroot/fw/
	fi
	if [ -f ../fw-audio/aw882xx_acf.bin ]; then
		mkdir -p sysroot/fw
		cp ../fw-audio/aw882xx_acf.bin sysroot/fw/
	fi

	if [ -d ../jb/root/var/jb ]; then
		rm -rf sysroot/jb
		cp -a ../jb/root/var/jb sysroot/jb
		for app in sysroot/jb/Applications/*.app; do
			[ -d "$app" ] && cp -a "$app" sysroot/Applications/
		done
		for c in $SILEO_BIN; do
			ln -sf /var/jb/usr/bin/$c sysroot/bin/$c
		done
		echo "  the package manager's prefix: $(du -sh sysroot/jb | cut -f1) as /jb (init puts it at /var/jb)"
	fi

	if [ -d obj/AnnibaleTriangle.app ]; then
		cp -a obj/AnnibaleTriangle.app sysroot/Applications/
		echo "  AnnibaleTriangle.app in /Applications (com.annibale.triangle, scheme annibaletriangle:)"
	fi

	for app in ../apps/*.app; do
		[ -d "$app" ] || continue
		cp -aL "$app" sysroot/Applications/
		echo "  ${app##*/} in /Applications ($(du -sm "$app" | cut -f1) MB from ../apps)"
	done

	mkdir -p sysroot/System/Library/Audio/Plug-Ins/HAL
	ln -sf /annibale/lib/AnnibaleAudio.driver sysroot/System/Library/Audio/Plug-Ins/HAL/AnnibaleAudio.driver

	mkdir -p sysroot/System/Library/Extensions
	ln -sf /annibale/lib/AnnibaleMetal.bundle sysroot/System/Library/Extensions/AnnibaleMetal.bundle

	mkdir -p sysroot/System/Cryptexes
	rm -rf sysroot/System/Cryptexes/OS sysroot/System/Cryptexes/App
	if [ -d $IOS/cryptex ]; then cp -al $IOS/cryptex sysroot/System/Cryptexes/OS; fi
	if [ -d $IOS/cryptex-app ]; then cp -al $IOS/cryptex-app sysroot/System/Cryptexes/App; fi
	D=sysroot/System/Cryptexes/OS/System/Library/Caches/com.apple.dyld
	mkdir -p $D
	for f in $CACHE/dyld_shared_cache_arm64*; do
		case "$f" in *.symbols) continue ;; esac
		ln -f "$f" $D/
	done

	find sysroot -type f \( -name '*.i64' -o -name '*.idb' -o -name '*.id[0-2]' -o -name '*.nam' -o -name '*.til' \) \
		-print -delete | sed 's|^sysroot/|build.sh: left out of the system image: /|'
	[ -z "$HIGHRATE" ] || python3 mkhighrate.py sysroot

	python3 ../tools/apfs/mkapfs.py -z -o system$V.apfs System=sysroot,manifest=$IOS/system.manifest | tail -1
	[ -z "$CHECK" ] || ../tools/apfs/check-image.sh system$V.apfs System=sysroot,manifest=$IOS/system.manifest
	rm -rf sysroot
fi
