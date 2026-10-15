#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Builds Kodi for Samsung Tizen TVs from scratch: the Emscripten SDK, Kodi's
# dependencies, the PVR add-ons of pvr-addons.txt and InputStream Adaptive, Kodi
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
JSONCPP_VERSION=1.9.8
HDHOMERUN_REVISION=7c54382fb681d03888b469033e50bebaf4ce6bce
# Translations of the add-ons to ship besides English; all of them if empty.
ADDON_LANGUAGES=${ADDON_LANGUAGES-"cs_cz;sk_sk;de_de;pl_pl;fr_fr;es_es;it_it"}
# The decrypter for the browser's DRM, built into InputStream Adaptive.
ADAPTIVE_EXTRA=$KODI/tools/wasm/tizen/inputstream.adaptive
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
  git -C "$WORK/addons-src/inputstream.adaptive" apply "$ADAPTIVE_EXTRA/eme-decrypter.patch"
fi

if [ ! -f "$TARGET/lib/libjsoncpp.a" ]; then
  mkdir -p "$WORK/tarballs" "$WORK/addons-src/jsoncpp-src" "$WORK/addons-src/build-jsoncpp"
  curl -fL -o "$WORK/tarballs/jsoncpp-$JSONCPP_VERSION.tar.gz" \
       "https://github.com/open-source-parsers/jsoncpp/archive/refs/tags/$JSONCPP_VERSION.tar.gz"
  tar -xf "$WORK/tarballs/jsoncpp-$JSONCPP_VERSION.tar.gz" -C "$WORK/addons-src/jsoncpp-src" \
      --strip-components=1
  cmake -S "$WORK/addons-src/jsoncpp-src" -B "$WORK/addons-src/build-jsoncpp" \
        -DCMAKE_TOOLCHAIN_FILE="$TARGET/share/Toolchain.cmake" -DCMAKE_BUILD_TYPE=Release \
        -DBUILD_SHARED_LIBS=OFF -DBUILD_STATIC_LIBS=ON -DBUILD_OBJECT_LIBS=OFF \
        -DJSONCPP_WITH_TESTS=OFF -DJSONCPP_WITH_POST_BUILD_UNITTEST=OFF \
        -DJSONCPP_WITH_EXAMPLE=OFF -DCMAKE_INSTALL_PREFIX="$TARGET"
  make -C "$WORK/addons-src/build-jsoncpp" -j"$JOBS" install
fi

PVR_DIRS=()
while read -r id url revision; do
  case $id in ''|\#*) continue ;; esac
  if [ ! -d "$WORK/addons-src/pvr/$id" ]; then
    git clone "$url" "$WORK/addons-src/pvr/$id"
    git -C "$WORK/addons-src/pvr/$id" checkout -q "$revision"
    # Changes the add-on needs here, from tools/wasm/tizen/addons/<id>.
    for patch in "$KODI/tools/wasm/tizen/addons/$id"/*.patch; do
      [ -f "$patch" ] && git -C "$WORK/addons-src/pvr/$id" apply "$patch"
    done
  fi
  PVR_DIRS+=("$WORK/addons-src/pvr/$id")
done < "$KODI/tools/wasm/tizen/pvr-addons.txt"

# libhdhomerun has no build system of its own; pvr.hdhomerun brings one.
if [ ! -f "$TARGET/lib/libhdhomerun.a" ]; then
  mkdir -p "$WORK/tarballs" "$WORK/addons-src/hdhomerun-src" "$WORK/addons-src/build-hdhomerun"
  curl -fL -o "$WORK/tarballs/libhdhomerun-$HDHOMERUN_REVISION.tar.gz" \
       "https://github.com/Silicondust/libhdhomerun/archive/$HDHOMERUN_REVISION.tar.gz"
  tar -xf "$WORK/tarballs/libhdhomerun-$HDHOMERUN_REVISION.tar.gz" \
      -C "$WORK/addons-src/hdhomerun-src" --strip-components=1
  cp "$WORK/addons-src/pvr/pvr.hdhomerun/depends/common/hdhomerun/CMakeLists.txt" \
     "$WORK/addons-src/hdhomerun-src/"
  cmake -S "$WORK/addons-src/hdhomerun-src" -B "$WORK/addons-src/build-hdhomerun" \
        -DCMAKE_TOOLCHAIN_FILE="$TARGET/share/Toolchain.cmake" -DCMAKE_BUILD_TYPE=Release \
        -DBUILD_SHARED_LIBS=OFF -DCMAKE_INSTALL_PREFIX="$TARGET"
  make -C "$WORK/addons-src/build-hdhomerun" -j"$JOBS" install
fi

mapfile -t PVR_OPTIONS < <("$KODI/tools/wasm/tizen/static_addons.py" "$TARGET" "${PVR_DIRS[@]}")
# The last line lists the add-ons; InputStream Adaptive joins them.
PVR_ADDONS=${PVR_OPTIONS[-1]#-DWASM_STATIC_ADDONS=}
unset 'PVR_OPTIONS[-1]'

cmake -S "$KODI" -B "$WORK/build" \
      -DCMAKE_TOOLCHAIN_FILE="$TARGET/share/Toolchain.cmake" \
      -DCMAKE_BUILD_TYPE=Release -DCORE_PLATFORM_NAME=wasm \
      "${PVR_OPTIONS[@]}" \
      -DWASM_STATIC_ADDONS="$PVR_ADDONS;$WORK/addons-src/inputstream.adaptive" \
      -DWASM_STATIC_ADDON_LANGUAGES="$ADDON_LANGUAGES" \
      -DWASM_STATIC_ADDON_SOURCES_inputstream.adaptive="src/*.cpp;lib/mpegts/mpegts/*.cpp;lib/webm_parser/src/*.cc;$ADAPTIVE_EXTRA/src/*.cpp" \
      -DWASM_STATIC_ADDON_EXCLUDE_inputstream.adaptive="/src/test/|/decrypters/widevine" \
      -DWASM_STATIC_ADDON_INCLUDES_inputstream.adaptive="lib/mpegts;lib/webm_parser/include;lib/webm_parser;$ADAPTIVE_EXTRA/src" \
      -DWASM_STATIC_ADDON_LIBRARIES_inputstream.adaptive="ap4;pugixml" \
      -DWASM_STATIC_ADDON_DEFINITIONS_inputstream.adaptive="__STDC_FORMAT_MACROS" \
      -DWASM_STATIC_ADDON_TEMPLATE_inputstream.adaptive="DECRYPTERPATH=special://home/cdm"
make -C "$WORK/build" -j"$JOBS"

"$KODI/tools/wasm/tizen/package.sh" "$WORK/build" $PROFILE
