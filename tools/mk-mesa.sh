#!/bin/bash
# Build Mesa for the iOS userland: the OpenGL ES library behind userland/src/eagl.c (see there and
# userland/src/gl_osmesa.c). Mesa is fetched (a pinned release), patched with tools/mesa/*.patch and
# built with the cross compiler of the kernel (xnu-deps/host/bin/xnu-clang, see xnu-deps/setup.sh)
# against the iOS SDK that userland/build.sh uses; nothing of it is in the repository.
#
#   DRIVERS="softpipe,freedreno"
#                        the gallium drivers to build. softpipe: a rasterizer in plain C; it needs no
#                        LLVM and nothing of the machine but its processor. freedreno: the Adreno's;
#                        it talks to the kernel's GPU driver (the service RedwoodGPU) through
#                        tools/mesa/redwood-drm, which stands in for libdrm (tools/mesa/freedreno-darwin.patch
#                        makes Mesa build it where there is no DRM). The library takes the GPU when the
#                        kernel has one and the environment does not say OSMESA_SOFTWARE=1.
#
# What comes out, in xnu-deps/build/mesa/out:
#   libOSMesa.8.dylib    everything in one library (install name /redwood/lib/libOSMesa.8.dylib): the
#                        OpenGL (ES) entry points gl*, the compiler of the shading language, the drivers,
#                        and the off-screen interface that hands out contexts
#   exported-gl.txt      the gl* functions it exports, and
#   gl.xml               their prototypes (the Khronos registry as Mesa has it): for userland/mkeagl.py
# and with AIR=1, the two things the Metal subsystem needs (docs/air.md, docs/metal.md 10.12):
#   air_run              the test program: a Metal shader translated, drawn or dispatched, read back
#   libredwoodair.dylib  the same path as a library (tools/mesa/air/air_metal.h), install name
#                        /redwood/lib/libredwoodair.dylib, which is what userland's Metal driver
#                        links against to compile a function Metal hands it
# Needs on this machine: bison, flex, python3 with venv (Mako and a pinned Meson are installed into a
# private environment), llvm-ar, and the iOS cross toolchain -- $THEOS/toolchain/linux/iphone (cctools'
# ld64 and install_name_tool, x86_64 binaries that run under qemu-user or Rosetta on an arm64 host) plus
# $THEOS/sdks/iPhoneOS16.5.sdk. All of it is baked into the hackios-dev image (docker/Dockerfile pins
# the theos commit and hashes the tools), so `docker/dev.sh` is the supported way to run this.
#
# This build is NOT reproducible byte for byte, so do not compare one of its outputs with another
# by hash. Measured on 20 October: two builds from the same source tree, the second with
# build-softpipe-freedreno deleted first, gave different libOSMesa.8.dylib and air_run -- same size
# to the byte, 9 132 774 of 14 855 048 bytes different in the library and 4 171 752 of 9 789 824 in
# air_run, with 5 of the 6 named text symbols at new addresses. So the layout moves between builds.
# What moves it was not established; it is not source filenames, which -Db_ndebug=true compiles out.
#
# This is Mesa's property and not the build system's, which is worth knowing before anyone
# distrusts every hash here: the userland programs reproduce exactly. prebuilt/userland/init built
# in one working copy came out byte for byte identical to the same source built in another, at a
# different time -- so there a hash does compare builds, and only for Mesa it does not.
#
# The practical consequence: a hash answers whether this is the same FILE, never whether it holds
# the same CODE, and the size answers neither -- the one-line ir3 rounding patch
# (tools/mesa/ir3-round-default.patch) produced a library of exactly the same 14 855 048 bytes as
# the build without it. To tell whether a patch is in a build, read the source in
# xnu-deps/build/mesa/mesa-$VERSION, which is where this script unpacks and patches it.
set -e
VERSION=${VERSION:-25.0.7}
SHA256=${SHA256:-592272df3cf01e85e7db300c449df5061092574d099da275d19e97ef0510f8a6}
# The 26.x experiment baseline (docs/analysis/mesa-rebase-2026-10.md section 6): the fork's patches survive
# 26.2.5 with an adaptation layer, so the trial runs as
#   VERSION=26.2.5 SHA256=794b3d1729075dae7ebd47c7283b7ca0cdb5dec668f7f11cc0a5d82f6138a0a5 REBASE=26 tools/mk-mesa.sh
# REBASE=26 lays tools/mesa/rebase-26/*.patch over the rest -- a subdirectory, so the glob below misses it
# on purpose: the layer only belongs on 26.x, never on 25.0.7.
REBASE=${REBASE:-}
ROOT=$(cd "$(dirname "$(readlink -f "$0")")/.." && pwd)
source "$ROOT/xnu-env.sh"
B=$ROOT/xnu-deps/build/mesa
SDK=$THEOS/sdks/iPhoneOS16.5.sdk
CC=$ROOT/xnu-deps/host/bin/xnu-clang
# xnu-clang links through $ROOT/xnu-deps/host/bin/ld64 -- that exact path is where the kernel build
# looks for the linker, and nothing but this script puts it there: an untracked symlink onto the
# theos toolchain, which is why the container image, not the repository, is its source of truth.
LD64=$ROOT/xnu-deps/host/bin/ld64
[ -e "$LD64" ] || ln -sfn "$THEOS/toolchain/linux/iphone/bin/ld" "$LD64"
# the C++ name of the same wrapper, which xnu-deps/setup.sh also makes (line: link xnu-clang
# host/bin/xnu-clang++); Mesa needs the C++ compiler, and mesa does not run setup.sh
[ -e "$ROOT/xnu-deps/host/bin/xnu-clang++" ] || ln -s xnu-clang "$ROOT/xnu-deps/host/bin/xnu-clang++"
# the same toolchain holds install_name_tool; the bare name is only right on macOS, and it was
# never on the PATH of the old containers, so resolve it rather than assume it
INTOOL=$THEOS/toolchain/linux/iphone/bin/install_name_tool
[ -x "$INTOOL" ] || INTOOL=install_name_tool
mkdir -p "$B/out"
cd "$B"

# The sources are the archive's with tools/mesa/*.patch on top. When the patches change, a tree that was
# patched with the earlier ones is of no use (and builds without a word into a library that does not match
# the rest): the sources are taken from the archive again and everything is built anew, about a minute.
REBASE_PATCHES=""
if [ "$REBASE" = 26 ]; then REBASE_PATCHES="$ROOT"/tools/mesa/rebase-26/*.patch; fi
PATCHES=$(cat "$ROOT"/tools/mesa/*.patch $REBASE_PATCHES | sha256sum | cut -d' ' -f1)
if [ ! -d mesa-$VERSION ] || [ "$(cat mesa-$VERSION/.redwood-patches 2>/dev/null)" != "$PATCHES" ]; then
	[ -f mesa-$VERSION.tar.xz ] || curl -sL --fail -o mesa-$VERSION.tar.xz https://archive.mesa3d.org/mesa-$VERSION.tar.xz
	echo "$SHA256  mesa-$VERSION.tar.xz" | sha256sum -c --quiet
	rm -rf mesa-$VERSION build-*
	tar xJf mesa-$VERSION.tar.xz
	# On 26.x some hunks of the old patches fail or skip, which is expected: 0003-restore-* re-establishes
	# what they carried (tools/mesa/rebase-26/README.md). Only there, so a 25.0.7 build still stops on a
	# patch that no longer fits.
	for p in "$ROOT"/tools/mesa/*.patch; do patch -s --no-backup-if-mismatch -d mesa-$VERSION -p1 < "$p" || [ "$REBASE" = 26 ]; done
	if [ "$REBASE" = 26 ]; then for p in "$ROOT"/tools/mesa/rebase-26/*.patch; do patch -s --no-backup-if-mismatch -d mesa-$VERSION -p1 < "$p"; done; fi
	echo "$PATCHES" > mesa-$VERSION/.redwood-patches
fi
# ours, kept in the repository: always the current ones
rm -rf mesa-$VERSION/src/redwood-drm
cp -r "$ROOT/tools/mesa/redwood-drm" mesa-$VERSION/src/redwood-drm
# the AIR frontend (docs/air.md): the Metal runtime will call it in this
# library's own process, and tools/mesa/air-frontend.patch is the meson that
# reaches it. Copied rather than linked, because this tree is cross-built.
rm -rf mesa-$VERSION/src/compiler/air
cp -r "$ROOT/tools/mesa/air" mesa-$VERSION/src/compiler/air
if [ ! -x venv/bin/python3 ]; then
	python3 -m venv venv
	# meson is pinned, not apt's: 26.x meson.build requires >= 1.4 and Ubuntu noble ships 1.3,
	# and a pinned builder is what makes the cross build reproducible across machines
	# (docs/analysis/mesa-rebase-2026-10.md section 9).
	venv/bin/pip -q install mako pyyaml packaging "meson==1.12.1"
fi
export PATH=$B/venv/bin:$PATH

# the compiler is the kernel's wrapper: it names the target (arm64-apple-ios) and the linker (ld64)
cat > cross.ini <<INI
[binaries]
c = ['$CC', '-arch', 'arm64', '-isysroot', '$SDK']
cpp = ['$CC++', '-arch', 'arm64', '-isysroot', '$SDK', '-fuse-cxa-atexit']
objc = ['$CC', '-arch', 'arm64', '-isysroot', '$SDK']
ar = 'llvm-ar'
ranlib = 'llvm-ranlib'
strip = 'llvm-strip'
pkg-config = 'false'

[host_machine]
system = 'darwin'
subsystem = 'ios'
cpu_family = 'aarch64'
cpu = 'arm64'
endian = 'little'

[properties]
needs_exe_wrapper = true
INI
DRIVERS=${DRIVERS:-softpipe,freedreno}
BUILD=build-${DRIVERS//,/-}
# 26.x dropped the gallium state trackers vdpau and xa, so those options only exist up to 25.x;
# meson hard-errors on an unknown -D, which means the flag list has to follow the version.
LEGACY_TRACKERS="-Dgallium-vdpau=disabled -Dgallium-xa=disabled"
case "$VERSION" in
	1.*|2[0-5].*) ;;
	*) LEGACY_TRACKERS="" ;;
esac
[ -f $BUILD/build.ninja ] || meson setup $BUILD mesa-$VERSION --cross-file cross.ini --wrap-mode=nofallback \
	--prefix=/redwood --libdir=lib --buildtype=release -Db_ndebug=true \
	-Dplatforms= -Dgallium-drivers="$DRIVERS" -Dfreedreno-kmds=msm -Dvulkan-drivers= -Dglx=disabled -Degl=disabled -Dgbm=disabled \
	-Dosmesa=true -Dllvm=disabled -Dshared-glapi=enabled -Dgles1=disabled -Dgles2=enabled -Dopengl=true \
	-Dzstd=disabled -Dxmlconfig=disabled -Dexpat=disabled -Dshader-cache=disabled -Dvalgrind=disabled \
	-Dlibunwind=disabled -Dglvnd=disabled -Dvideo-codecs= -Dtools= -Dgallium-va=disabled \
	$LEGACY_TRACKERS -Dlmsensors=disabled -Dbuild-tests=false > meson-setup.log
ninja -C $BUILD > ninja.log
# air_run draws with a translated Metal shader and reads the pixels back; it is
# the only way to find out whether the Adreno takes work that did not come from
# the window server. Built only when asked, since it is a test program.
if [ "${AIR:-}" = 1 ]; then
	ninja -C $BUILD src/gallium/drivers/freedreno/air_run >> ninja.log
	cp $BUILD/src/gallium/drivers/freedreno/air_run out/air_run
	echo "mk-mesa.sh: $B/out/air_run ($(stat -c %s out/air_run) bytes)"
	# and the same path as a library, which is not a test program: the Metal driver links
	# against it to compile the bitcode Metal hands a function. Stripped and named the way
	# libOSMesa is below -- -x keeps the global symbols, which here are the rwa_* of
	# tools/mesa/air/air_metal.h, and the install name has to be the path on the device
	# because meson's own is @rpath.
	ninja -C $BUILD src/gallium/drivers/freedreno/libredwoodair.dylib >> ninja.log
	llvm-strip -x -o out/libredwoodair.dylib $BUILD/src/gallium/drivers/freedreno/libredwoodair.dylib
	"$INTOOL" -id /redwood/lib/libredwoodair.dylib out/libredwoodair.dylib
	echo "mk-mesa.sh: $B/out/libredwoodair.dylib ($(stat -c %s out/libredwoodair.dylib) bytes," \
		"$(llvm-nm -gU out/libredwoodair.dylib | grep -c ' _rwa_') rwa_* functions)"
	# what this library was built from, so userland/build.sh can tell that it is older than the
	# sources and come back here instead of shipping it beside a driver that disagrees with it;
	# byte order (LC_ALL=C), no locale collation -- see the note on built-from below
	find "$ROOT"/tools/mesa/air -maxdepth 1 -type f -print | LC_ALL=C sort | xargs cat | sha256sum > out/built-from-air
fi

llvm-strip -x -o out/libOSMesa.8.dylib $BUILD/src/gallium/targets/osmesa/libOSMesa.8.dylib
"$INTOOL" -id /redwood/lib/libOSMesa.8.dylib out/libOSMesa.8.dylib
llvm-nm -gU out/libOSMesa.8.dylib | awk '$3 ~ /^_gl/ { print substr($3, 2) }' > out/exported-gl.txt
# 26.x moved the Khronos registry inside the tree (src/mapi/glapi/registry, then src/mesa/glapi/glapi/registry)
glxml=mesa-$VERSION/src/mapi/glapi/registry/gl.xml
[ -f "$glxml" ] || glxml=mesa-$VERSION/src/mesa/glapi/glapi/registry/gl.xml
cp "$glxml" out/gl.xml
# what this library was built from, so userland/build.sh can tell that it is older than the
# sources and come back here instead of shipping it beside a driver that disagrees with it.
# Hashed in byte order (LC_ALL=C) and NOT by a glob: the locale collation of the shell that runs
# this decided the old hash, so the same tree gave different stamps on a macOS host and in the
# container, and 23 October lost a build to that. The list also covers the rebase layer and names
# the baseline, so a rebuild of another VERSION cannot leave the previous stamp standing.
# userland/build.sh computes the same expression -- both sides here and there, or not at all.
{ { find "$ROOT"/tools/mesa -maxdepth 1 -type f -name '*.patch' -print
	[ "$REBASE" = 26 ] && find "$ROOT"/tools/mesa/rebase-26 -maxdepth 1 -type f -name '*.patch' -print
	find "$ROOT"/tools/mesa/redwood-drm -maxdepth 1 -type f -print; } |
	LC_ALL=C sort | xargs cat
	echo "mesa-$VERSION REBASE=$REBASE"; } | sha256sum > out/built-from
echo "mk-mesa.sh: Mesa $VERSION ($DRIVERS): $B/out/libOSMesa.8.dylib, $(stat -c %s out/libOSMesa.8.dylib) bytes," \
	"$(wc -l < out/exported-gl.txt) gl* functions"
