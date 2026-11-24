#!/bin/bash
# Native CPU rasterizers for the K90 AIR frontend. Metal device diagnostics in the
# iOS Mesa patches call into the on-device driver, so apply only the NIR patches.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
VERSION=26.2.5
SHA=794b3d1729075dae7ebd47c7283b7ca0cdb5dec668f7f11cc0a5d82f6138a0a5
B=${AIR_HOST_DIR:-"$ROOT/build/mesa-host-replay"}
mkdir -p "$B"
B=$(cd "$B" && pwd)
archive=${MESA_ARCHIVE:-"$ROOT/../hackios-xnu-kernel/xnu-deps/build/mesa/mesa-$VERSION.tar.xz"}
if [ ! -f "$archive" ] && [ -z "${MESA_ARCHIVE:-}" ]; then
    archive="$B/mesa-$VERSION.tar.xz"
    if [ ! -f "$archive" ]; then
        curl -fL "https://archive.mesa3d.org/mesa-$VERSION.tar.xz" -o "$archive.part"
        mv "$archive.part" "$archive"
    fi
fi
if ! command -v llvm-config >/dev/null && command -v brew >/dev/null; then
    export PATH="$(brew --prefix llvm)/bin:$PATH"
fi
command -v llvm-config >/dev/null || { echo "Add LLVM's bin directory to PATH" >&2; exit 1; }
actual=$(shasum -a 256 "$archive")
[ "${actual%% *}" = "$SHA" ] || { echo "Incorrect Mesa source archive: $archive" >&2; exit 1; }

patches=("$ROOT/tools/mesa/air-frontend.patch" "$ROOT/tools/mesa/rebase-26/"*"-nir-"*.patch)
stamp=$(cat "$0" "${patches[@]}" | shasum -a 256 | cut -c1-16)
src="$B/mesa-$VERSION-$stamp"
if [ ! -d "$src" ]; then
    staging=$(mktemp -d "$B/prepare.XXXXXX")
    trap 'rm -rf "$staging"' EXIT
    tar -xJf "$archive" -C "$staging"
    tree="$staging/mesa-$VERSION"
    for p in "${patches[@]}"; do
        patch -s -d "$tree" -p1 < "$p"
    done
    ln -s "$ROOT/tools/mesa/air" "$tree/src/compiler/air"
    ln -s "$ROOT/tools/air/air_vertex.c" "$tree/src/gallium/air_vertex.c"
    cat >> "$tree/src/gallium/meson.build" <<'MESON'

foreach tool, source : {'air_run': '../compiler/air/air_run.c', 'air_vertex': 'air_vertex.c'}
executable(tool, source,
  include_directories : [inc_include, inc_src, inc_mesa, inc_gallium, inc_gallium_aux,
                         inc_gallium_winsys, inc_gallium_winsys_sw, inc_gallium_drivers],
  dependencies : [dep_m, idep_air, idep_mesautil, driver_swrast],
  link_with : [libgallium, libgalliumvl_stub, libws_null], build_by_default : false)
endforeach
MESON
    mv "$tree" "$src"
fi
build="$B/build-$stamp"
if [ ! -f "$build/build.ninja" ]; then
    meson setup "$build" "$src" --buildtype=release --wrap-mode=nofallback \
        -Dplatforms= -Dgallium-drivers=softpipe,llvmpipe -Dvulkan-drivers= -Dtools= \
        -Dglx=disabled -Degl=disabled -Dllvm=enabled -Dlibunwind=disabled \
        -Dvideo-codecs= -Dgallium-va=disabled -Dglvnd=disabled
fi
ninja -C "$build" src/compiler/air/air2nir src/gallium/air_run src/gallium/air_vertex
ln -sfn "$build/src/compiler/air/air2nir" "$B/air2nir"
ln -sfn "$build/src/gallium/air_run" "$B/air_run"
ln -sfn "$build/src/gallium/air_vertex" "$B/air_vertex"
if [ "$(uname -s)" = Darwin ]; then
    xcrun clang -fobjc-arc -framework Foundation -framework Metal \
        "$ROOT/tools/air/vertex-metal.m" -o "$B/vertex-metal"
fi
echo "Host tools built in $B"
