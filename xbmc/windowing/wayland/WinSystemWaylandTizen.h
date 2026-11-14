/*
 *  Copyright (C) 2026 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

#include "Registry.h"
#include "Seat.h"
#include "WinSystemWayland.h"

#include <atomic>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include <wayland-tizen-protocols.hpp>

namespace KODI::WINDOWING::WAYLAND
{

/*!
 * \brief Kodi's Wayland window on a Samsung Tizen TV.
 *
 * The compositor of a TV speaks xdg-shell v6, which Kodi's Wayland windowing handles, and
 * Tizen's own extensions: the TV's video player is shown on a surface by its global
 * resource id, the media and colour keys of the remote control only reach a window
 * that grabs them, and a window of a process other than the app's own has to tell when it
 * goes to the background and bring itself to the front.
 */
class CWinSystemWaylandTizen : public CWinSystemWayland, public IRawInputHandlerKeyboard
{
public:
  bool InitWindowSystem() override;
  bool DestroyWindowSystem() override;
  bool CreateNewWindow(const std::string& name, bool fullScreen, RESOLUTION_INFO& res) override;
  bool HasCursor() override;
  bool IsHDRDisplay() override;
  CHDRCapabilities GetDisplayHDRCapabilities() const override;
  std::unique_ptr<KODI::WINDOWING::IOSScreenSaver> GetOSScreenSaverImpl() override;

  /*!
   * \brief The id by which the TV's players show video on Kodi's window, 0 until the
   * compositor has told it.
   */
  uint32_t GetSurfaceResourceId() const { return m_resourceId; }

  void OnKeyboardKeymap(CSeat* seat,
                        wayland::keyboard_keymap_format format,
                        std::string const& keymap) override;

protected:
  std::unique_ptr<CSeat> CreateSeat(std::uint32_t name, wayland::seat_t& seat) override;
  void ApplyOpaqueRegion() override;
  void OnConfigure(std::uint32_t serial, CSizeInt size, IShellSurface::StateBitset state) override;

private:
  void GrabRemoteKeys();
  void WatchLifecycle();
  void OnLifecycleEvent(char event);

  std::unique_ptr<CRegistry> m_tizenRegistry;
  wayland::tizen_surface_t m_tizenSurface;
  wayland::tizen_resource_t m_tizenResource;
  wayland::tizen_keyrouter_t m_keyrouter;
  wayland::tizen_policy_t m_policy;
  wayland::tizen_visibility_t m_visibility;
  std::atomic<uint32_t> m_resourceId{0};
  CHDRCapabilities m_hdrCaps; //!< the HDR formats the TV shows

  std::mutex m_keysLock;
  std::vector<uint32_t> m_remoteKeys; //!< keycodes to grab, from the compositor's keymap
  bool m_keysGrabbed{false};

  //! Events of the app's lifecycle, from the launcher and from the compositor (see
  //! OnLifecycleEvent()).
  int m_lifecyclePipe[2]{-1, -1};
  std::thread m_lifecycleThread;
};

} // namespace KODI::WINDOWING::WAYLAND
