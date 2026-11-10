#!/bin/bash
# Builds the toolchain of a native Kodi for Samsung Tizen TVs (ARMv7 with NEON, softfp,
# glibc 2.30): a sysroot made of the Tizen 8.0 TV rootstrap of Tizen Studio plus the
# headers and libraries it lacks, and a GCC 13 cross compiler against it. The GCC 9.2 of
# Tizen Studio is too old for Kodi's C++20.
#
# Usage: build.sh <prefix> [x86]
#   <prefix>/sysroot                    the sysroot
#   <prefix>/arm-tizen-linux-gnueabi    the toolchain, for tools/depends' --with-toolchain
# TIZEN_STUDIO (default ~/tizen-studio) has to have the TIZEN-8.0 native app development
# package installed.
#
# With x86 it builds for the TV emulator of Tizen Studio instead (i686, glibc 2.24), from
# the Tizen 5.5 emulator rootstrap in ROOTSTRAP, e.g. mobile-5.5-emulator.core of
# https://download.tizen.org/sdk/tizenstudio/official/binary/, so that Kodi also runs on
# the older Tizen of the emulator: <prefix>/i686-tizen-linux-gnu.
set -euo pipefail

HERE=$(cd "$(dirname "$0")" && pwd)
PREFIX=$(mkdir -p "$1" && cd "$1" && pwd)
ARCH=${2:-arm}
TIZEN_STUDIO=${TIZEN_STUDIO:-$HOME/tizen-studio}
case $ARCH in
  arm)
    ROOTSTRAP=$TIZEN_STUDIO/platforms/tizen-8.0/tizen/rootstraps/tizen-8.0-device.core
    TARGET=arm-tizen-linux-gnueabi
    GLIBC_VERSION=2.30
    ARCH_FLAGS=(--with-arch=armv7-a --with-float=softfp --with-fpu=neon --with-mode=arm)
    ;;
  x86)
    ROOTSTRAP=${ROOTSTRAP:?ROOTSTRAP has to name a Tizen 5.5 emulator rootstrap}
    TARGET=i686-tizen-linux-gnu
    GLIBC_VERSION=2.24
    ARCH_FLAGS=(--with-arch=i686 --with-tune=generic)
    ;;
  *)
    echo "Unknown architecture $ARCH" >&2
    exit 1
    ;;
esac
TOOLCHAIN=$PREFIX/$TARGET
SYSROOT=$PREFIX/sysroot
SRC=$PREFIX/src
JOBS=$(nproc)

BINUTILS=binutils-2.42
GCC=gcc-13.3.0
GMP=gmp-6.3.0
MPFR=mpfr-4.2.1
MPC=mpc-1.3.1
PULSEAUDIO=pulseaudio-15.0
KHRONOS_EGL=https://raw.githubusercontent.com/KhronosGroup/EGL-Registry/main/api/EGL
KHRONOS_GLES3=https://raw.githubusercontent.com/KhronosGroup/OpenGL-Registry/main/api/GLES3

fetch() {
  [ -f "$SRC/$(basename "$1")" ] || curl -fL --retry 3 -o "$SRC/$(basename "$1")" "$1"
}

mkdir -p "$SRC"
fetch https://ftp.gnu.org/gnu/binutils/$BINUTILS.tar.xz
fetch https://ftp.gnu.org/gnu/gcc/$GCC/$GCC.tar.xz
fetch https://ftp.gnu.org/gnu/gmp/$GMP.tar.xz
fetch https://ftp.gnu.org/gnu/mpfr/$MPFR.tar.xz
fetch https://ftp.gnu.org/gnu/mpc/$MPC.tar.gz
fetch https://freedesktop.org/software/pulseaudio/releases/$PULSEAUDIO.tar.xz

# 1. Sysroot: the rootstrap, with the EGL, GLES 3 and PulseAudio headers it lacks
if [ ! -f "$SYSROOT/.rootstrap" ]; then
  [ -d "$ROOTSTRAP" ] || { echo "No rootstrap in $ROOTSTRAP" >&2; exit 1; }
  rm -rf "$SYSROOT"
  cp -a "$ROOTSTRAP" "$SYSROOT"
  mkdir -p "$SYSROOT/usr/include/EGL" "$SYSROOT/usr/include/GLES3" "$SYSROOT/usr/include/pulse"
  for h in egl.h eglext.h eglplatform.h; do
    curl -fL --retry 3 -o "$SYSROOT/usr/include/EGL/$h" "$KHRONOS_EGL/$h"
  done
  for h in gl3.h gl31.h gl32.h gl3platform.h; do
    curl -fL --retry 3 -o "$SYSROOT/usr/include/GLES3/$h" "$KHRONOS_GLES3/$h"
  done
  tar -C "$SRC" -xf "$SRC/$PULSEAUDIO.tar.xz"
  cp "$SRC/$PULSEAUDIO"/src/pulse/*.h "$SYSROOT/usr/include/pulse/"
  rm -f "$SYSROOT"/usr/include/pulse/{internal,client-conf,client-conf-x11,fork-detect}.h
  sed -e 's/@PA_MAJORMINOR@/15.0/; s/@PACKAGE_VERSION@/15.0/; s/@PA_API_VERSION@/12/' \
      -e 's/@PA_PROTOCOL_VERSION@/35/; s/@PA_MAJOR@/15/; s/@PA_MINOR@/0/; s/@PA_MICRO@/0/' \
      "$SRC/$PULSEAUDIO/src/pulse/version.h.in" > "$SYSROOT/usr/include/pulse/version.h"
  touch "$SYSROOT/.rootstrap"
fi

# 2. Cross compiler
if [ ! -x "$TOOLCHAIN/bin/$TARGET-g++" ]; then
  for archive in $BINUTILS.tar.xz $GCC.tar.xz $GMP.tar.xz $MPFR.tar.xz $MPC.tar.gz; do
    [ -d "$SRC/${archive%.tar.*}" ] || tar -C "$SRC" -xf "$SRC/$archive"
  done
  ln -sfn "../$GMP" "$SRC/$GCC/gmp"
  ln -sfn "../$MPFR" "$SRC/$GCC/mpfr"
  ln -sfn "../$MPC" "$SRC/$GCC/mpc"

  mkdir -p "$PREFIX/build/binutils" "$PREFIX/build/gcc"
  (cd "$PREFIX/build/binutils" &&
   "$SRC/$BINUTILS/configure" --target=$TARGET --prefix="$TOOLCHAIN" --with-sysroot="$SYSROOT" \
     --disable-nls --disable-werror --enable-plugins --disable-gdb --disable-gprofng &&
   make -j"$JOBS" && make install)
  (cd "$PREFIX/build/gcc" && PATH=$TOOLCHAIN/bin:$PATH &&
   "$SRC/$GCC/configure" --target=$TARGET --prefix="$TOOLCHAIN" --with-sysroot="$SYSROOT" \
     --with-glibc-version=$GLIBC_VERSION --enable-languages=c,c++ "${ARCH_FLAGS[@]}" \
     --disable-multilib --disable-nls --disable-bootstrap \
     --enable-threads=posix --enable-__cxa_atexit --enable-shared --enable-linker-build-id \
     --disable-libsanitizer --disable-libquadmath --disable-libssp --disable-libgomp \
     --disable-libvtv --disable-libitm --enable-libstdcxx-time=yes &&
   make -j"$JOBS" && make install)

  # Kodi links libstdc++ statically, as a TV has an older one. Without the shared one,
  # libtool cannot link it in either.
  mkdir -p "$TOOLCHAIN/$TARGET/lib/shared-disabled"
  mv "$TOOLCHAIN/$TARGET"/lib/libstdc++.so* "$TOOLCHAIN/$TARGET"/lib/libstdc++.la \
     "$TOOLCHAIN/$TARGET/lib/shared-disabled/"
fi

# 3. Libraries of the TV that the rootstrap lacks: stubs with the functions Kodi may call,
#    to link against. The TV's own libraries are used at run time.
LIBDIR=$SYSROOT/usr/lib
for symbols in "$HERE"/stubs/*.symbols; do
  soname=$(basename "$symbols" .symbols)
  [ -f "$LIBDIR/$soname" ] && continue
  stub=$PREFIX/build/stubs/${soname%%.so*}
  mkdir -p "$(dirname "$stub")"
  grep -v '^[#@]' "$symbols" | sed 's/.*/void &(void) {}/' > "$stub.c"
  version=$(sed -n 's/^@version //p' "$symbols")
  versionflags=()
  if [ -n "$version" ]; then
    echo "$version { global: *; };" > "$stub.map"
    versionflags=(-Wl,--version-script="$stub.map")
  fi
  "$TOOLCHAIN/bin/$TARGET-gcc" -shared -fPIC -nostdlib -Wl,-soname,"$soname" "${versionflags[@]}" \
    -o "$LIBDIR/$soname" "$stub.c"
  ln -sfn "$soname" "$LIBDIR/${soname%%.so*}.so"
done
# What the libraries of the TV need in turn, for the linker to check them
cp -n "$TOOLCHAIN/$TARGET/lib/libgcc_s.so.1" "$SYSROOT/lib/"

# tools/depends looks for the sysroot next to the toolchain, as on webOS
ln -sfn "$SYSROOT" "$TOOLCHAIN/$TARGET/sysroot"
echo "Toolchain: $TOOLCHAIN"
