/*
 *  Copyright (C) 2026 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "OSScreenSaverWasm.h"

#include <emscripten.h>
#include <emscripten/em_asm.h>

using namespace KODI::WINDOWING::WASM;

namespace
{
// The browser releases a wake lock whenever the page is hidden, so the lock is
// re-requested on the next visibility change for as long as it is wanted.
void SetWakeLockWanted(bool wanted)
{
  // clang-format off
  MAIN_THREAD_EM_ASM(({
    if (!('wakeLock' in navigator))
      return;
    const kodi = (Module.kodi = Module.kodi || {});
    if (!kodi.wakeLock) {
      const wl = (kodi.wakeLock = { wanted: false, pending: false, sentinel: null, warned: false });
      wl.acquire = () => {
        if (!wl.wanted || wl.pending || wl.sentinel || document.visibilityState !== 'visible')
          return;
        wl.pending = true;
        navigator.wakeLock.request('screen').then((sentinel) => {
          wl.pending = false;
          if (!wl.wanted) {
            sentinel.release();
            return;
          }
          wl.sentinel = sentinel;
          sentinel.addEventListener('release', () => {
            if (wl.sentinel === sentinel)
              wl.sentinel = null;
          });
        }).catch((e) => {
          wl.pending = false;
          if (!wl.warned) {
            wl.warned = true;
            console.warn('[kodi] screen wake lock:', e);
          }
        });
      };
      document.addEventListener('visibilitychange', wl.acquire);
    }
    const wl = kodi.wakeLock;
    wl.wanted = !!$0;
    if (wl.wanted) {
      wl.acquire();
    } else if (wl.sentinel) {
      wl.sentinel.release();
      wl.sentinel = null;
    }
  }), wanted ? 1 : 0);
  // clang-format on
}

// A Samsung TV starts its screensaver over an app regardless of a wake lock, unless the
// app turns it off through the AppCommon API. The TV may drop the request while its
// screensaver covers the app, so it is made again when the app is shown.
// Returns false without that API.
bool SetTvScreenSaver(bool on)
{
  // clang-format off
  return MAIN_THREAD_EM_ASM_INT(({
    if (typeof webapis === 'undefined' || !webapis.appcommon)
      return 0;
    const kodi = (Module.kodi = Module.kodi || {});
    const apply = () => {
      try {
        const states = webapis.appcommon.AppCommonScreenSaverState;
        webapis.appcommon.setScreenSaver(
            kodi.tvScreenSaverOn ? states.SCREEN_SAVER_ON : states.SCREEN_SAVER_OFF, () => {},
            (e) => console.warn('[kodi] setScreenSaver:', e && (e.name + ' ' + e.message)));
      } catch (e) {
        console.warn('[kodi] setScreenSaver:', e);
      }
    };
    if (kodi.tvScreenSaverOn === undefined) {
      document.addEventListener('visibilitychange', () => {
        if (document.visibilityState === 'visible' && !kodi.tvScreenSaverOn)
          apply();
      });
    }
    kodi.tvScreenSaverOn = !!$0;
    apply();
    return 1;
  }), on ? 1 : 0) != 0;
  // clang-format on
}
} // namespace

void COSScreenSaverWasm::Inhibit()
{
  SetTvScreenSaver(false);
  SetWakeLockWanted(true);
}

void COSScreenSaverWasm::Uninhibit()
{
  SetTvScreenSaver(true);
  SetWakeLockWanted(false);
}
