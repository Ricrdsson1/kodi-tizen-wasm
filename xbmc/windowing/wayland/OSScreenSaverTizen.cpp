/*
 *  Copyright (C) 2026 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "OSScreenSaverTizen.h"

#include "utils/log.h"

#include <chrono>

#include <dlfcn.h>

using namespace KODI::WINDOWING::WAYLAND;

namespace
{
// The TV starts its screensaver after two minutes at the least
constexpr auto RESET_INTERVAL = std::chrono::seconds(30);
} // unnamed namespace

std::unique_ptr<COSScreenSaverTizen> COSScreenSaverTizen::TryCreate()
{
  void* library = dlopen("libcapi-screensaver.so", RTLD_NOW);
  if (!library)
  {
    CLog::LogF(LOGINFO, "No screensaver library of the TV: {}", dlerror());
    return {};
  }
  using Init = int (*)();
  auto init = reinterpret_cast<Init>(dlsym(library, "screensaver_init"));
  auto resetTimeout = reinterpret_cast<ResetTimeout>(dlsym(library, "screensaver_reset_timeout"));
  if (!init || !resetTimeout)
  {
    CLog::LogF(LOGWARNING, "The screensaver library of the TV lacks a function");
    dlclose(library);
    return {};
  }
  if (const int ret = init(); ret != 0)
    CLog::LogF(LOGWARNING, "screensaver_init failed: {}", ret);
  return std::unique_ptr<COSScreenSaverTizen>(new COSScreenSaverTizen(library, resetTimeout));
}

COSScreenSaverTizen::COSScreenSaverTizen(void* library, ResetTimeout resetTimeout)
  : m_library(library), m_resetTimeout(resetTimeout), m_thread(&COSScreenSaverTizen::Run, this)
{
}

COSScreenSaverTizen::~COSScreenSaverTizen()
{
  {
    std::unique_lock lock(m_mutex);
    m_stop = true;
  }
  m_wake.notify_all();
  m_thread.join();

  using Fini = void (*)();
  if (auto fini = reinterpret_cast<Fini>(dlsym(m_library, "screensaver_fini")))
    fini();
  dlclose(m_library);
}

void COSScreenSaverTizen::Inhibit()
{
  {
    std::unique_lock lock(m_mutex);
    m_inhibited = true;
  }
  m_wake.notify_all();
}

void COSScreenSaverTizen::Uninhibit()
{
  std::unique_lock lock(m_mutex);
  m_inhibited = false;
}

void COSScreenSaverTizen::Run()
{
  std::unique_lock lock(m_mutex);
  while (!m_stop)
  {
    if (!m_inhibited)
    {
      m_wake.wait(lock, [this] { return m_stop || m_inhibited; });
      continue;
    }
    if (const int ret = m_resetTimeout(); ret != 0)
      CLog::LogF(LOGDEBUG, "screensaver_reset_timeout failed: {}", ret);
    m_wake.wait_for(lock, RESET_INTERVAL, [this] { return m_stop || !m_inhibited; });
  }
}
