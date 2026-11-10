![Kodi Logo](resources/banner_slim.png)

# Samsung Tizen TV build guide
This guide builds a native Kodi for Samsung TVs with Tizen, tested with a 2025 model with Tizen 9.0. The host OS was Ubuntu 20.04 (64-bit). Kodi is built with **[Kodi's unified depends build system](../tools/depends/README.md)**, please read it before you proceed.

A TV runs no native apps of third parties, only .NET and web apps, and an app cannot start a program from its package. So Kodi comes with a small .NET app that copies it into the app's data directory and runs it there as a normal Linux program. Kodi shows its GUI on its own Wayland window and plays video with the TV's hardware decoders through Samsung's ESPlayer, sound goes to the TV's PulseAudio.

For a Kodi that runs in the TV's web engine, see **[README.WASM.md](README.WASM.md)**.

## Table of Contents
1. **[Prerequisites](#1-prerequisites)**
2. **[Build the tool chain](#2-build-the-tool-chain)**
3. **[Configure and build tools and dependencies](#3-configure-and-build-tools-and-dependencies)**
4. **[Build binary add-ons](#4-build-binary-add-ons)**
5. **[Build Kodi](#5-build-kodi)**
6. **[Package and install](#6-package-and-install)**
7. **[Development builds](#7-development-builds)**
8. **[Limitations](#8-limitations)**

## 1. Prerequisites
* **[Tizen Studio](https://developer.tizen.org/development/tizen-studio/download)** with the *TIZEN-8.0 Native app. development (CLI)* package, for the TV rootstrap, and the *Samsung Certificate Extension*.
* A Samsung certificate in a security profile of Tizen Studio's Certificate Manager, for the TV's DUID.
* The **[.NET 8 SDK](https://dotnet.microsoft.com/download)**, for the launcher.
* The TV in developer mode, with the IP address of the build machine as host PC, connected with `sdb connect <TV IP>`.
* The packages the depends build needs on the host, see **[README.Linux.md](README.Linux.md)**.

## 2. Build the tool chain
Kodi needs C++20, which the GCC 9.2 of Tizen Studio does not have. The script builds GCC 13 against the TV rootstrap, which it completes with the EGL, OpenGL ES 3 and PulseAudio headers and stub libraries of the TV's EGL and PulseAudio:
```
tools/tizen/toolchain/build.sh $HOME/tizen-toolchain
```

## 3. Configure and build tools and dependencies
```
cd $HOME/kodi/tools/depends
./bootstrap
./configure --host=arm-tizen-linux-gnueabi --with-platform=tizen \
            --with-toolchain=$HOME/tizen-toolchain/arm-tizen-linux-gnueabi \
            --prefix=$HOME/kodi-deps --with-tarballs=$HOME/kodi-tarballs
make -j$(getconf _NPROCESSORS_ONLN)
```

## 4. Build binary add-ons
```
cd $HOME/kodi/tools/depends/target/binary-addons
make ADDONS="inputstream.adaptive inputstream.ffmpegdirect"
```
`make bundle` puts the binary add-ons named in `TIZEN_BUNDLED_ADDONS` (default: these two) into the package; the others come from the add-on repository.

Binary add-ons for a TV are tagged with the platform `tizen-armv7`. Kodi on a TV installs no binary add-ons of other Linux systems, which are built for another ABI and C library; add-ons that are not binary, such as Python ones, it installs whether they are tagged for Tizen or Linux.

### Add-on repository
More binary add-ons are installed from a repository of their own, as the official one has none for Tizen. The script builds the add-ons listed in `tools/tizen/repository/addons.txt`, or those given, into a directory to serve over HTTP(S) at the URL given:
```
STRIP=$HOME/tizen-toolchain/arm-tizen-linux-gnueabi/bin/arm-tizen-linux-gnueabi-strip \
  tools/tizen/repository/build-repository.sh https://example.org/kodi-tizen $HOME/kodi-tizen-repo
```
A TV's Kodi gets the add-ons once `repository.kodi-tizen/repository.kodi-tizen-<version>.zip` of the directory is installed from it (*Add-ons > Install from zip file*). Kodi comes with the repository when it is configured with its URL:
```
cmake -DTIZEN_ADDON_REPOSITORY_URL=https://example.org/kodi-tizen <build directory>
```

## 5. Build Kodi
```
mkdir $HOME/kodi-build && cd $HOME/kodi-build
$HOME/kodi-deps/x86_64-linux-gnu-native/bin/cmake $HOME/kodi \
  -DCMAKE_TOOLCHAIN_FILE=$HOME/kodi-deps/arm-tizen-linux-gnueabi-release/share/Toolchain.cmake \
  -DTIZEN_SECURITY_PROFILE=<security profile>
make -j$(getconf _NPROCESSORS_ONLN)
```

## 6. Package and install
```
make tpk
tizen install -n kodi-<version>.tpk -t <TV>
```
`make tpk` assembles the package in `tools/tizen/packaging` of the build directory (`make bundle`), builds the launcher with `dotnet` and signs the `.tpk` with `TIZEN_SECURITY_PROFILE`.

Kodi's profile is in the app's data directory, uninstalling the app removes it.

Without `TIZEN_SECURITY_PROFILE`, the package that `dotnet` builds in `bin/Release/tizen40` is signed with the test certificates of the Tizen SDK only, and carries nothing of the builder's. Such a package can be shared; a TV installs it once it is signed again with a certificate for the TV:
```
tizen package -t tpk -s <security profile> -- kodi-<version>.tpk
```

A TV keeps the old version of an app until the new one is installed, so updating needs about as much free room as the app takes.

## 7. Development builds
Nothing but the app can read its data directory on a TV, `sdb` cannot. These variables of `make bundle` (or `make tpk`) add options to the package:
* `KODI_LOG_URL=<url>`: the launcher posts Kodi's output and `kodi.log` to the address, as they grow.
* `KODI_ARGS=<arguments>`: Kodi is started with the arguments, one per line, such as a file to play.
* `KODI_DEV_WEBSERVER=<password>`: a new profile starts with Kodi's web server enabled on port 8095, user `kodi`, for JSON-RPC. Port 8080 is taken by a service of the TV.

## 8. Limitations
* DRM: Samsung's CDM needs a privilege of partner level, which a public certificate does not give, and the Widevine CDM that Kodi uses on Linux is not built for the TV's ABI (softfp).
* The keys of the remote control that the TV keeps for itself (power, home, volume) do not reach Kodi.
