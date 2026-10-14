/*
 *  Copyright (C) 2026 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "CPUInfoWasm.h"

#include "SystemInfoWasm.h"
#include "utils/StringUtils.h"

#include <cstdlib>

#include <emscripten.h>

std::shared_ptr<CCPUInfo> CCPUInfo::GetCPUInfo()
{
  return std::make_shared<CCPUInfoWasm>();
}

CCPUInfoWasm::CCPUInfoWasm()
{
  m_cpuCount = EM_ASM_INT({
    if (typeof navigator !== "undefined" && Number.isFinite(navigator.hardwareConcurrency) &&
        navigator.hardwareConcurrency > 0)
      return Math.floor(navigator.hardwareConcurrency);

    return 1;
  });

  // A Tizen TV names the architecture, e.g. "armv7".
  std::string arch =
      KODI::PLATFORM::WASM::GetSystemCapability("http://tizen.org/feature/platform.core.cpu.arch");
  if (StringUtils::StartsWith(arch, "arm"))
    arch = "ARM" + arch.substr(3);
  m_cpuModel = arch.empty() ? "WebAssembly" : arch;

  for (int core = 0; core < m_cpuCount; core++)
  {
    CoreInfo coreInfo;
    coreInfo.m_id = core;
    m_cores.emplace_back(coreInfo);
  }
}

bool CCPUInfoWasm::SupportsCPUUsage() const
{
  return KODI::PLATFORM::WASM::GetSystemCpuLoad() >= 0;
}

int CCPUInfoWasm::GetUsedPercentage()
{
  const int load = KODI::PLATFORM::WASM::GetSystemCpuLoad();
  if (load < 0)
    return 0;

  // Only the load of all cores together is known.
  for (auto& core : m_cores)
    core.m_usagePercent = load;
  return load;
}

float CCPUInfoWasm::GetCPUFrequency()
{
  // In MHz, the nominal frequency
  const std::string frequency = KODI::PLATFORM::WASM::GetSystemCapability(
      "http://tizen.org/feature/platform.core.cpu.frequency");
  return frequency.empty() ? 0.0f : static_cast<float>(std::atof(frequency.c_str()));
}
