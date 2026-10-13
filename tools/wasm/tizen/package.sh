#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Packs a Kodi WASM build into a Tizen TV widget (.wgt).
#
# Usage: package.sh <build-dir> [<security-profile>]
#
# With a security profile the widget is signed by the Tizen Studio CLI (`tizen`
# must be on PATH or TIZEN_CLI must point at it). Without one an unsigned
# archive is written, which a TV refuses to install.

set -e

BUILD_DIR=${1:?usage: package.sh <build-dir> [<security-profile>]}
PROFILE=$2
SRC_DIR=$(cd "$(dirname "$0")" && pwd)
KODI_DIR=$(cd "$SRC_DIR/../../.." && pwd)
BUILD_DIR=$(cd "$BUILD_DIR" && pwd)
STAGE_DIR=$BUILD_DIR/tizen-wgt
TIZEN_CLI=${TIZEN_CLI:-tizen}

for f in kodi.js kodi.wasm kodi.data; do
  [ -f "$BUILD_DIR/$f" ] || { echo "error: $BUILD_DIR/$f is missing, build Kodi first" >&2; exit 1; }
done

VERSION=$(awk '$1 == "VERSION_MAJOR" { major = $2 } $1 == "VERSION_MINOR" { minor = $2 }
               END { print major "." minor ".0" }' "$KODI_DIR/version.txt")

rm -rf "$STAGE_DIR"
mkdir -p "$STAGE_DIR/media"
# Everything emcc emits next to kodi.js: the module, its data and the worker bootstraps.
cp "$BUILD_DIR"/kodi.js "$BUILD_DIR"/kodi.wasm "$BUILD_DIR"/kodi.data "$STAGE_DIR/"
for f in "$BUILD_DIR"/kodi.*.js; do
  [ -f "$f" ] && cp "$f" "$STAGE_DIR/"
done
cp "$KODI_DIR/tools/wasm/kodi.html" "$STAGE_DIR/index.html"
cp "$KODI_DIR/media/splash.jpg" "$STAGE_DIR/media/"
cp "$KODI_DIR/media/icon256x256.png" "$STAGE_DIR/icon.png"
sed "s/@APP_VERSION@/$VERSION/" "$SRC_DIR/config.xml" > "$STAGE_DIR/config.xml"

OUTPUT=$BUILD_DIR/Kodi.wgt
rm -f "$OUTPUT"
if [ -n "$PROFILE" ]; then
  "$TIZEN_CLI" package -t wgt -s "$PROFILE" -o "$BUILD_DIR" -- "$STAGE_DIR"
  echo "Signed widget: $OUTPUT"
else
  (cd "$STAGE_DIR" && zip -q -r "$OUTPUT" .)
  echo "Unsigned widget: $OUTPUT (sign it with a security profile to install it on a TV)"
fi
