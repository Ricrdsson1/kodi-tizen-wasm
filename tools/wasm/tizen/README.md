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
* Unencrypted DASH through InputStream Adaptive, including changes of quality while
  playing.
* Widevine protected streams, experimentally and not watchable yet: a decrypter added
  to InputStream Adaptive (`tools/wasm/tizen/inputstream.adaptive`) runs the license
  exchange through the browser's Encrypted Media Extensions, and the TV decrypts the
  packets. Audio plays, but the picture stutters and jumps as soon as it is encrypted,
  although the packets decrypt correctly (checked against the Axinom test vectors).
  The TV refuses `ElementaryMediaTrack.setMediaKeySession()` for Widevine and
  PlayReady alike, so the keys are attached to the media element instead. The TV also
  fails when the keys of a stream are spread over several sessions, so a stream whose
  key came with an earlier license shares that session, and when encrypted audio
  follows clear audio, so the clear lead of encrypted audio is passed as encrypted
  packets without encrypted bytes.
* Binary add-ons linked into the executable (see below).

## What does not

* SMB, NFS, FTP and anything else that needs a socket; the embedded web server.
* Python add-ons that open their own connections (`urllib`, `requests`).
* Audio the TV player cannot decode (DTS, MP2, TrueHD, ...) next to hardware decoded
  video: the video plays without sound.
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
