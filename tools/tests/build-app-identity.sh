#!/bin/sh
# Build and sign the standalone LS/FBS query tool; no device access.
set -eu

if [ "$#" -ne 1 ]; then
    echo "usage: build-app-identity.sh OUTPUT_FILE" >&2
    exit 2
fi

source_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
output=$1
mkdir -p -- "$(dirname -- "$output")"
entitlements=$(mktemp)
trap 'rm -f "$entitlements"' EXIT HUP INT TERM
cat > "$entitlements" <<'PLIST'
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
    <key>platform-application</key><true/>
    <key>com.apple.private.security.container-required</key><false/>
    <key>com.apple.private.security.no-sandbox</key><true/>
    <key>com.apple.private.coreservices.canmaplsdatabase</key><true/>
    <key>com.apple.private.security.storage.AppBundles</key><true/>
</dict></plist>
PLIST

xcrun --sdk iphoneos clang -arch arm64e -miphoneos-version-min=26.0 \
    -fobjc-arc -Os -Wall -Wextra -Werror -framework Foundation \
    "$source_dir/app-identity.m" -o "$output"
codesign --force --sign - --entitlements "$entitlements" "$output"
codesign --verify --strict "$output"
