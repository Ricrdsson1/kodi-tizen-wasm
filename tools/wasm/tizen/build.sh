#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Builds Kodi for Samsung Tizen TVs from scratch: the Emscripten SDK, Kodi's
# dependencies, the IPTV Simple Client and InputStream Adaptive add-ons, Kodi
# itself and the widget.
#
# Usage: build.sh <work-dir> [<security-profile>]
#
# Everything that is downloaded or built goes into <work-dir>; steps whose result
# is already there are skipped, so the script can be run again after a failure.
# Needs a Linux host with git, curl, zip, make, a C/C++ compiler, autoconf,
# automake, libtool, pkg-config and Python 3.10 or newer as python3.

set -euo pipefail

WORK=${1:?usage: build.sh <work-dir> [<security-profile>]}
PROFILE=${2:-}
KODI=$(cd "$(dirname "$0")/../../.." && pwd)
JOBS=${JOBS:-$(getconf _NPROCESSORS_ONLN)}

EMSDK_VERSION=5.0.6
PUGIXML_VERSION=1.16
IPTVSIMPLE_REVISION=6d334f6dfa16bbb4c0209ee50bb70f16bccb13bf
IPTVSIMPLE_VERSION=22.6.5
BENTO4_VERSION=1.6.0-641-6-Piers
ADAPTIVE_REVISION=84bdb3581521ccbd5df1bee24eb2c2b4c9499932

python3 -c 'import sys; sys.exit(sys.version_info < (3, 10))' ||
  { echo "error: python3 has to be 3.10 or newer" >&2; exit 1; }

mkdir -p "$WORK"
WORK=$(cd "$WORK" && pwd)
DEPENDS=$WORK/depends
TARGET=$DEPENDS/wasm32-unknown-emscripten-release
NATIVE=$DEPENDS/x86_64-linux-gnu-native

if [ ! -x "$WORK/emsdk/upstream/emscripten/emcc" ]; then
  [ -d "$WORK/emsdk" ] || git clone https://github.com/emscripten-core/emsdk.git "$WORK/emsdk"
  "$WORK/emsdk/emsdk" install $EMSDK_VERSION
  "$WORK/emsdk/emsdk" activate $EMSDK_VERSION
fi
export EMSDK_QUIET=1
source "$WORK/emsdk/emsdk_env.sh"
# emcc runs under the python3 the dependencies build, which has no CA bundle of its
# own; without this, Emscripten port downloads fail certificate verification.
export SSL_CERT_FILE=${SSL_CERT_FILE:-/etc/ssl/certs/ca-certificates.crt}

if [ ! -f "$KODI/tools/depends/target/.installed-wasm32-unknown-emscripten-release" ]; then
  (
    cd "$KODI/tools/depends"
    ./bootstrap
    ./configure --host=wasm32-unknown-emscripten --with-platform=wasm --disable-debug \
                --prefix="$DEPENDS" --with-tarballs="$WORK/tarballs"
    make -j"$JOBS"
  )
fi
export PATH="$NATIVE/bin:$PATH"

if [ ! -f "$TARGET/lib/libpugixml.a" ]; then
  mkdir -p "$WORK/tarballs" "$WORK/addons-src/build-pugixml"
  curl -fL -o "$WORK/tarballs/pugixml-$PUGIXML_VERSION.tar.gz" \
       "https://github.com/zeux/pugixml/releases/download/v$PUGIXML_VERSION/pugixml-$PUGIXML_VERSION.tar.gz"
  tar -xf "$WORK/tarballs/pugixml-$PUGIXML_VERSION.tar.gz" -C "$WORK/addons-src"
  cmake -S "$WORK/addons-src/pugixml-$PUGIXML_VERSION" -B "$WORK/addons-src/build-pugixml" \
        -DCMAKE_TOOLCHAIN_FILE="$TARGET/share/Toolchain.cmake" -DCMAKE_BUILD_TYPE=Release \
        -DBUILD_SHARED_LIBS=OFF -DCMAKE_INSTALL_PREFIX="$TARGET"
  make -C "$WORK/addons-src/build-pugixml" -j"$JOBS" install
fi

if [ ! -f "$TARGET/lib/libap4.a" ]; then
  mkdir -p "$WORK/tarballs" "$WORK/addons-src/build-bento4"
  curl -fL -o "$WORK/tarballs/bento4-$BENTO4_VERSION.tar.gz" \
       "https://github.com/xbmc/Bento4/archive/refs/tags/$BENTO4_VERSION.tar.gz"
  tar -xf "$WORK/tarballs/bento4-$BENTO4_VERSION.tar.gz" -C "$WORK/addons-src"
  cmake -S "$WORK/addons-src/Bento4-$BENTO4_VERSION" -B "$WORK/addons-src/build-bento4" \
        -DCMAKE_TOOLCHAIN_FILE="$TARGET/share/Toolchain.cmake" -DCMAKE_BUILD_TYPE=Release \
        -DBUILD_APPS=OFF -DBUILD_SHARED_LIBS=OFF -DCMAKE_INSTALL_PREFIX="$TARGET"
  make -C "$WORK/addons-src/build-bento4" -j"$JOBS" install
fi

if [ ! -d "$WORK/addons-src/inputstream.adaptive" ]; then
  git clone https://github.com/xbmc/inputstream.adaptive "$WORK/addons-src/inputstream.adaptive"
  git -C "$WORK/addons-src/inputstream.adaptive" checkout -q $ADAPTIVE_REVISION
fi

if [ ! -d "$WORK/addons-src/pvr.iptvsimple" ]; then
  git clone https://github.com/kodi-pvr/pvr.iptvsimple "$WORK/addons-src/pvr.iptvsimple"
  git -C "$WORK/addons-src/pvr.iptvsimple" checkout -q $IPTVSIMPLE_REVISION
fi

cmake -S "$KODI" -B "$WORK/build" \
      -DCMAKE_TOOLCHAIN_FILE="$TARGET/share/Toolchain.cmake" \
      -DCMAKE_BUILD_TYPE=Release -DCORE_PLATFORM_NAME=wasm \
      -DWASM_STATIC_ADDONS="$WORK/addons-src/pvr.iptvsimple;$WORK/addons-src/inputstream.adaptive" \
      -DWASM_STATIC_ADDON_SOURCES_inputstream.adaptive="src/*.cpp;lib/mpegts/mpegts/*.cpp;lib/webm_parser/src/*.cc" \
      -DWASM_STATIC_ADDON_EXCLUDE_inputstream.adaptive="/src/test/|/decrypters/widevine" \
      -DWASM_STATIC_ADDON_INCLUDES_inputstream.adaptive="lib/mpegts;lib/webm_parser/include;lib/webm_parser" \
      -DWASM_STATIC_ADDON_LIBRARIES_inputstream.adaptive="ap4;pugixml" \
      -DWASM_STATIC_ADDON_DEFINITIONS_inputstream.adaptive="__STDC_FORMAT_MACROS;TARGET_DARWIN_EMBEDDED" \
      -DWASM_STATIC_ADDON_LIBRARIES_pvr.iptvsimple=pugixml \
      -DWASM_STATIC_ADDON_DEFINITIONS_pvr.iptvsimple=IPTV_VERSION=$IPTVSIMPLE_VERSION
make -C "$WORK/build" -j"$JOBS"

"$KODI/tools/wasm/tizen/package.sh" "$WORK/build" $PROFILE
