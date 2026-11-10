/*
 *  Copyright (C) 2026 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

#include "windowing/OSScreenSaver.h"

#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>

namespace KODI::WINDOWING::WAYLAND
{

/*!
 * \brief The screensaver of a Samsung TV, which starts over an app after a few minutes
 * without a key press and does not know of video playing in another process's window.
 *
 * While inhibited, the TV's idle timeout is reset periodically, as a key press does,
 * through the TV's screensaver library.
 */
class COSScreenSaverTizen : public IOSScreenSaver
{
public:
  //! \return nullptr if the TV has no screensaver library
  static std::unique_ptr<COSScreenSaverTizen> TryCreate();
  ~COSScreenSaverTizen() override;

  void Inhibit() override;
  void Uninhibit() override;

private:
  using ResetTimeout = int (*)();

  COSScreenSaverTizen(void* library, ResetTimeout resetTimeout);
  void Run();

  void* m_library;
  ResetTimeout m_resetTimeout;
  std::mutex m_mutex;
  std::condition_variable m_wake;
  bool m_inhibited{false};
  bool m_stop{false};
  std::thread m_thread;
};

} // namespace KODI::WINDOWING::WAYLAND
