# Kodi on Samsung Tizen TV (WebAssembly)

An experimental build of the WASM port packaged as a Tizen web application (`.wgt`).
It has been run on one TV only: a 2025 model on Tizen 9.0 (Chromium 120).

## What works

* GUI, remote control, audio through the browser audio worklet.
* HTTP(S) through the browser's `fetch()` (`xbmc/platform/wasm/network`).
* H.264, HEVC, VP8, VP9 and AV1 decoded by the TV through the Tizen WASM Player
  (`xbmc/cores/VideoPlayer/MediaPipelineTizen.cpp`), with AAC, MP3, AC-3, E-AC-3 or
  Opus audio. Other video codecs are decoded in software, which is too slow for HD.
* HLS, read through Kodi's file layer instead of FFmpeg's network code.
* One binary add-on linked into the executable (see below).

## What does not

* SMB, NFS, FTP and anything else that needs a socket; the embedded web server.
* Python add-ons that open their own connections (`urllib`, `requests`).
* Audio the TV player cannot decode (DTS, MP2, TrueHD, ...) next to hardware decoded
  video: the video plays without sound.
* `User-Agent`, `Cookie` and `Referer` request headers, which a browser does not let a
  page set.

## Building

Build the dependencies as described in [README.WASM.md](../../../docs/README.WASM.md)
(Emscripten 5.0.6), then configure and build Kodi with the toolchain file that step
installs:

```
cmake <kodi> -DCMAKE_TOOLCHAIN_FILE=<depends>/wasm32-unknown-emscripten-release/share/Toolchain.cmake \
             -DCMAKE_BUILD_TYPE=Release -DCORE_PLATFORM_NAME=wasm
make -j$(getconf _NPROCESSORS_ONLN)
```

### Binary add-ons

A browser cannot load a shared library, so a binary add-on has to be linked in at build
time. Pass the add-on's source directory, the libraries it needs from the depends
prefix and the definitions its own build would set:

```
-DWASM_STATIC_ADDONS=<path>/pvr.iptvsimple
-DWASM_STATIC_ADDON_LIBRARIES=pugixml
-DWASM_STATIC_ADDON_DEFINITIONS=IPTV_VERSION=22.6.5
```

The libraries have to be built for `wasm32-unknown-emscripten` and installed into the
depends prefix first. Only one add-on can be linked this way for now.

## Packaging

```
tools/wasm/tizen/package.sh <build-dir> [<security-profile>]
```

Without a security profile this writes an unsigned `Kodi.wgt`. A TV only installs a
widget signed with a Samsung certificate that lists that TV, so everyone has to sign
it themselves: create the certificate with the Tizen Studio Certificate Manager and
pass the profile name. Then install it with `tizen install -n Kodi.wgt -t <tv-name>`.

## Debugging

`sdb shell 0 debug KodiWasmTv.Kodi` starts the app and prints a port; the Chrome
DevTools protocol is then served at `http://<tv-ip>:<port>`, and Kodi logs to its
console. A built-in command can be run from there:

```
Module.ccall('kodi_wasm_execute_builtin', null, ['string'], ['PlayMedia(https://...)'])
```
