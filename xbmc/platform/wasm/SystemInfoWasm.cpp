/*
 *  Copyright (C) 2026 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "SystemInfoWasm.h"

#include <cstdlib>

#include <emscripten.h>

namespace KODI::PLATFORM::WASM
{

// The tizen object only exists on the browser main thread.

std::string GetSystemCapability(const std::string& key)
{
  char* value = static_cast<char*>(MAIN_THREAD_EM_ASM_PTR(
      {
        try
        {
          if (typeof tizen === 'undefined' || !tizen.systeminfo)
            return 0;
          var value = tizen.systeminfo.getCapability(UTF8ToString($0));
          return value === undefined || value === null ? 0 : stringToNewUTF8(String(value));
        }
        catch (e)
        {
          return 0;
        }
      },
      key.c_str()));
  if (!value)
    return {};
  std::string result(value);
  free(value);
  return result;
}

bool GetSystemMemory(uint64_t& total, uint64_t& available)
{
  const double totalBytes = MAIN_THREAD_EM_ASM_DOUBLE({
    try
    {
      return typeof tizen !== 'undefined' && tizen.systeminfo ? tizen.systeminfo.getTotalMemory()
                                                              : -1;
    }
    catch (e)
    {
      return -1;
    }
  });
  const double availableBytes = MAIN_THREAD_EM_ASM_DOUBLE({
    try
    {
      return typeof tizen !== 'undefined' && tizen.systeminfo
                 ? tizen.systeminfo.getAvailableMemory()
                 : -1;
    }
    catch (e)
    {
      return -1;
    }
  });
  if (totalBytes <= 0 || availableBytes < 0)
    return false;

  total = static_cast<uint64_t>(totalBytes);
  available = static_cast<uint64_t>(availableBytes);
  return true;
}

int GetSystemCpuLoad()
{
  // The load is only reported asynchronously, so it is polled in the background from
  // the first request on and the latest value is returned.
  const double load = MAIN_THREAD_EM_ASM_DOUBLE({
    var state = globalThis.kodiCpuLoad;
    if (!state)
    {
      state = globalThis.kodiCpuLoad = {load : -1};
      try
      {
        if (typeof tizen === 'undefined' || !tizen.systeminfo)
          return -1;
        var poll = function()
        {
          tizen.systeminfo.getPropertyValue(
              'CPU', function(cpu) { state.load = cpu.load; }, function() {});
        };
        poll();
        setInterval(poll, 2000);
      }
      catch (e)
      {
      }
    }
    return state.load;
  });
  return load < 0 ? -1 : static_cast<int>(load * 100.0 + 0.5);
}

} // namespace KODI::PLATFORM::WASM
