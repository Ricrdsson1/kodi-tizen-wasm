/*
 *  Copyright (C) 2026 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "PlatformWasm.h"

#include "ServiceBroker.h"
#include "cores/AudioEngine/Sinks/AESinkWasmAudioWorklet.h"
#include "messaging/ApplicationMessenger.h"
#include "windowing/wasm/WinSystemWasmGLESContext.h"

#include <cstdlib>
#include <string>

#include <emscripten.h>

// Lets the page run a built-in command, e.g. from the browser console:
// Module.ccall('kodi_wasm_execute_builtin', null, ['string'], ['PlayMedia(http://...)'])
extern "C" EMSCRIPTEN_KEEPALIVE void kodi_wasm_execute_builtin(const char* command)
{
  auto messenger = CServiceBroker::GetAppMessenger();
  if (messenger && command)
    messenger->PostMsg(TMSG_EXECUTE_BUILT_IN, -1, -1, nullptr, std::string(command));
}

CPlatform* CPlatform::CreateInstance()
{
  return new CPlatformWasm();
}

bool CPlatformWasm::InitStageOne()
{
  if (!std::getenv("HOME"))
    setenv("HOME", "/home/web_user", 1);

  if (!CPlatformPosix::InitStageOne())
    return false;

  CWinSystemWasmGLESContext::Register();
  CAESinkWasmAudioWorklet::Register();

  return true;
}
