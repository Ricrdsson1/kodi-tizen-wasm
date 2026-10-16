# Kodi on Samsung Tizen TV (WebAssembly)

An experimental build of the WASM port packaged as a Tizen web application (`.wgt`).
It has been run on one TV only: a 2025 model on Tizen 9.0 (Chromium 120).

It needs Tizen 6.5 or newer, which Samsung ships on its TVs from 2022 on. Emscripten
targets Chromium 85, the browser of Tizen 6.5, and Kodi runs multithreaded WebAssembly
and plays audio through an AudioWorklet. Video is decoded through the Tizen WASM
Player, which TVs before 2021 (Tizen 6.0) do not have at all.

## What works

* GUI, remote control, audio through the browser audio worklet.
* HTTP(S) through the browser's `fetch()` (`xbmc/platform/wasm/network`).
* H.264, HEVC, VP8, VP9 and AV1 decoded by the TV through the Tizen WASM Player
  (`xbmc/cores/VideoPlayer/MediaPipelineTizen.cpp`). Other video codecs are decoded in
  software, which is too slow for HD.
* AAC, MP3, AC-3, E-AC-3 and Opus audio decoded by the TV. Other audio, such as DTS,
  TrueHD, FLAC, Vorbis, MP2 or PCM, is decoded by Kodi and handed to the TV as PCM, with
  up to 8 channels.
* HLS, read through Kodi's file layer instead of FFmpeg's network code.
* Unencrypted DASH through InputStream Adaptive, including changes of quality while
  playing.
* DRM protected streams (Widevine, PlayReady, ClearKey), DASH and HLS: an item for
  InputStream Adaptive with DRM settings (`drm_legacy`, `drm` or `license_key`) is played
  by `CWebPlayer` (`xbmc/platform/wasm/player`) instead of VideoPlayer. It hands the
  manifest and the license settings to [Shaka Player](https://github.com/shaka-project/shaka-player)
  in an HTML5 video element under the GUI, so Kodi's OSD and controls stay in use.
  The TV decrypts only in its own media pipeline: packets the Tizen WASM Player gets
  encrypted come out garbled on a Tizen 9 TV, although they decrypt correctly with the
  same key elsewhere, and its tracks refuse a DRM session (`setMediaKeySession()`).
  Kodi's audio settings and subtitle styles do not apply to these streams.
* Binary add-ons linked into the executable (see below).
* Python add-ons that open their own connections (`urllib`, `requests`).

## What does not

* SMB, NFS, FTP and anything else that needs a socket; the embedded web server.
* Passing audio through to an amplifier: the TV gets DTS and TrueHD as PCM.
* `User-Agent`, `Cookie` and `Referer` request headers, which a browser does not let a
  page set.
* Large add-ons, such as most skins other than Confluence: the TV gives the app 80 MB
  of storage, and Kodi keeps all of its files in memory as well. An add-on that does
  not fit is refused before it is downloaded. For the same reason thumbnails are
  cached only until the app is closed.

## Building

`build.sh` does all of the steps below on a Linux host, starting with the Emscripten
SDK, and links in the PVR add-ons and InputStream Adaptive:

```
tools/wasm/tizen/build.sh <work-dir> [<security-profile>]
```

It needs git, curl, zip, make, a C/C++ compiler, autoconf, automake, libtool,
pkg-config and Python 3.10 or newer. The first run downloads and builds for a long
time; a later run skips what is already built.

By hand: build the dependencies as described in [README.WASM.md](../../../docs/README.WASM.md)
(Emscripten 5.0.6), then configure and build Kodi with the toolchain file that step
installs:

```
cmake <kodi> -DCMAKE_TOOLCHAIN_FILE=<depends>/wasm32-unknown-emscripten-release/share/Toolchain.cmake \
             -DCMAKE_BUILD_TYPE=Release -DCORE_PLATFORM_NAME=wasm
make -j$(getconf _NPROCESSORS_ONLN)
```

### Binary add-ons

A browser cannot load a shared library, so binary add-ons have to be linked in at
build time. List their source directories and, per add-on, the libraries it needs
from the depends prefix and the definitions its own build would set:

```
-DWASM_STATIC_ADDONS="<path>/pvr.iptvsimple;<path>/pvr.demo"
-DWASM_STATIC_ADDON_LIBRARIES_pvr.iptvsimple=pugixml
-DWASM_STATIC_ADDON_DEFINITIONS_pvr.iptvsimple=IPTV_VERSION=22.6.5
-DWASM_STATIC_ADDON_LIBRARIES_pvr.demo=tinyxml2
```

The libraries have to be built for `wasm32-unknown-emscripten` and installed into the
depends prefix first. Each add-on is merged with its libraries into one object whose
symbols, apart from the entry points, are made local
(`tools/wasm/localize_symbols.py`), so add-ons do not collide with each other. An
add-on that is not listed in `system/addon-manifest.xml` starts out disabled. Further
per add-on options (source globs, exclusions, include directories) are described in
`cmake/scripts/wasm/ExtraTargets.cmake`; `build.sh` shows them for InputStream Adaptive.

## PVR add-ons

`build.sh` links in the PVR add-ons of `pvr-addons.txt`, at the revisions pinned there,
and `static_addons.py` derives their build options. All of them compile, link and are
found by Kodi; only the IPTV Simple Client is enabled from the start, the others are
enabled under Add-ons > My add-ons > PVR clients.

Changes an add-on needs here are kept as patches in `addons/<id>`, which `build.sh`
applies. The IPTV Simple Client tells a live MPEG-TS stream by its first bytes instead
of asking the server with a HEAD request, which some IPTV servers never answer.

Only the IPTV Simple Client has been used against a real backend. The others are
untested: an add-on that talks to its backend over a protocol of its own depends on the
sockets of the TV, and those for services with DRM (such as Zattoo, Waipu, Teleboy)
on the DRM support described above.

| Add-on | Name | Version |
|---|---|---|
| `pvr.argustv` | ARGUS TV client | 22.3.5 |
| `pvr.demo` | Demo PVR Client | 22.4.5 |
| `pvr.dvblink` | TVMosaic/DVBLink PVR Client | 22.3.2 |
| `pvr.dvbviewer` | DVBViewer Client | 22.3.2 |
| `pvr.filmon` | Filmon Client | 22.2.5 |
| `pvr.freebox` | PVR Freebox TV | 22.2.1 |
| `pvr.hdhomerun` | HDHomeRun Client | 22.2.7 |
| `pvr.hts` | Tvheadend HTSP Client | 22.9.3 |
| `pvr.iptvsimple` | IPTV Simple Client | 22.6.5 |
| `pvr.mediaportal.tvserver` | MediaPortal PVR Client | 22.2.5 |
| `pvr.mythtv` | MythTV PVR Client | 22.3.19 |
| `pvr.nextpvr` | NextPVR PVR Client | 22.6.2 |
| `pvr.njoy` | Njoy N7 PVR Client | 22.2.4 |
| `pvr.octonet` | Digital Devices Octopus NET Client | 22.2.1 |
| `pvr.pctv` | PCTV Systems Client | 22.2.5 |
| `pvr.plutotv` | Pluto.tv PVR Client | 22.3.2 |
| `pvr.sledovanitv.cz` | Sledovanitv.cz Client (unofficial) | 22.6.1 |
| `pvr.stalker` | Stalker Client | 22.2.7 |
| `pvr.teleboy` | Teleboy PVR Client | 22.2.2 |
| `pvr.vbox` | VBox TV Gateway PVR Client | 22.2.5 |
| `pvr.vdr.vnsi` | VDR VNSI Client | 22.3.3 |
| `pvr.vuplus` | Enigma2 Client | 22.3.7 |
| `pvr.waipu` | waipu.tv PVR Client | 22.10.0 |
| `pvr.wmc` | Windows Media Center Client | 22.2.5 |
| `pvr.zattoo` | Zattoo PVR Client | 22.2.3 |

Only the translations named by `ADDON_LANGUAGES` in `build.sh` are shipped with them,
as all of the data is held in memory.

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
