/*
 *  Copyright (C) 2026 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "WinSystemWaylandTizen.h"

#include "Connection.h"
#include "OSScreenSaverTizen.h"
#include "Registry.h"
#include "SeatTizen.h"
#include "ServiceBroker.h"
#include "VideoRenderers/HwDecRender/RendererTizen.h"
#include "application/ApplicationComponents.h"
#include "application/ApplicationPlayer.h"
#include "guilib/WindowIDs.h"
#include "input/actions/Action.h"
#include "input/actions/ActionIDs.h"
#include "messaging/ApplicationMessenger.h"
#include "utils/log.h"

#include "platform/linux/tizen/SystemInfoTizen.h"
#include "platform/wasm/tizen/TizenEMSS.h"

#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <regex>
#include <string_view>
#include <vector>

#include <csignal>

#include <dlfcn.h>
#include <fcntl.h>
#include <unistd.h>
#include <xkbcommon/xkbcommon.h>

namespace
{
int g_lifecycleFd = -1;

// Events of the app's lifecycle
constexpr char EVENT_BACKGROUND = 'b'; //!< Kodi's window cannot be seen any more
constexpr char EVENT_VISIBLE = 'v'; //!< Kodi's window can be seen again
constexpr char EVENT_ACTIVATE = 'a'; //!< the app was started again and is to come to the front

void SendLifecycleEvent(char event)
{
  if (g_lifecycleFd >= 0)
    [[maybe_unused]] auto ignored = write(g_lifecycleFd, &event, 1);
}

// SIGUSR1: the app went to the background, SIGUSR2: it was started or resumed again.
extern "C" void OnLifecycleSignal(int sig)
{
  SendLifecycleEvent(sig == SIGUSR1 ? EVENT_BACKGROUND : EVENT_ACTIVATE);
}

// Keys of the remote control that Kodi has functions for, which only reach a window that
// grabs them. The others, such as those of other apps or of the TV's sources, stay with
// the TV.
constexpr std::array<std::string_view, 20> REMOTE_KEYS = {
    "XF86Back",          "XF86Red",           "XF86Green",
    "XF86Yellow",        "XF86Blue",          "XF86AudioPlay",
    "XF86AudioPause",    "XF86AudioStop",     "XF86AudioRewind",
    "XF86AudioForward",  "XF86AudioNext",     "XF86AudioPrev",
    "XF86AudioRecord",   "XF86ChannelUp",     "XF86ChannelDown",
    "XF86Info",          "XF86MediaSelectProgramGuide",
    "XF86Subtitle",      "XF86MediaLanguageMenu",
    "XF86MenuKB"};

// tizen_keyrouter mode for a key the window has registered, as a web app's registerKey()
constexpr uint32_t KEYGRAB_REGISTERED = 5;

// Feature keys of the TV, as Samsung's web APIs read them
template<typename T>
std::optional<T> Feature(const char* key, const char* getter)
{
  void* lib = dlopen("libcapi-system-info.so.0", RTLD_NOW);
  if (!lib)
    return {};
  using Get = int (*)(const char*, T*);
  auto get = reinterpret_cast<Get>(dlsym(lib, getter));
  T value{};
  const int ret = get ? get(key, &value) : -1;
  dlclose(lib);
  if (ret != 0)
    return {};
  return value;
}

bool FeatureBool(const char* key)
{
  return Feature<bool>(key, "system_info_get_custom_bool").value_or(false);
}

int FeatureInt(const char* key)
{
  return Feature<int>(key, "system_info_get_custom_int").value_or(0);
}

struct EdidHdr
{
  bool hdr10{false};
  bool hlg{false};
  bool hdr10Plus{false};
  bool dolbyVision{false};
};

// The HDR formats an EDID announces: the EOTFs of its HDR static metadata block, and the
// vendor video blocks of HDR10+ and Dolby Vision. The TV keeps its EDIDs as hex text.
std::optional<EdidHdr> ReadEdidHdr(const std::filesystem::path& path)
{
  std::ifstream file(path, std::ios::binary);
  const std::string text((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
  std::vector<uint8_t> edid;
  if (text.size() >= 256 &&
      std::all_of(text.begin(), text.begin() + 16, [](char c) { return std::isxdigit(c); }))
  {
    for (size_t i = 0; i + 1 < text.size() && std::isxdigit(text[i]) && std::isxdigit(text[i + 1]);
         i += 2)
      edid.push_back(static_cast<uint8_t>(std::stoi(text.substr(i, 2), nullptr, 16)));
  }
  else
    edid.assign(text.begin(), text.end());

  static constexpr uint8_t HEADER[] = {0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x00};
  if (edid.size() < 128 || !std::equal(std::begin(HEADER), std::end(HEADER), edid.begin()))
    return {};

  EdidHdr hdr;
  for (size_t ext = 128; ext + 128 <= edid.size(); ext += 128)
  {
    if (edid[ext] != 0x02) // CTA-861 extension
      continue;
    const size_t end = std::min<size_t>(ext + edid[ext + 2], ext + 127);
    for (size_t i = ext + 4; i < end;)
    {
      const int tag = edid[i] >> 5;
      const size_t length = edid[i] & 0x1f;
      if (i + length >= end)
        break;
      const uint8_t* data = &edid[i + 1];
      if (tag == 7 && length >= 3 && data[0] == 6) // HDR static metadata
      {
        hdr.hdr10 |= (data[1] & 0x04) != 0; // SMPTE ST 2084
        hdr.hlg |= (data[1] & 0x08) != 0;
      }
      else if (tag == 7 && length >= 4 && data[0] == 1) // vendor specific video
      {
        const uint32_t oui = data[1] | (data[2] << 8) | (data[3] << 16);
        hdr.hdr10Plus |= oui == 0x90848b;
        hdr.dolbyVision |= oui == 0x00d046;
      }
      i += length + 1;
    }
  }
  return hdr;
}

// The HDR formats the TV shows. The TV only says whether it shows HDR at all; which
// formats it takes is in the EDIDs it gives sources on its HDMI inputs. The firmware holds
// them for a range of models, as /lib/firmware/hdmiedid/<set>/<HDMI mode>/hdmi<n>, the sets
// named after the panel (2K_, 4K_, 8K_) and monitors (_MNT_). A format counts when every set
// for this kind of display announces it in one of its modes.
CHDRCapabilities TvHdrCapabilities()
{
  CHDRCapabilities caps;
  if (!FeatureBool("com.samsung/featureconf/hdr_support"))
  {
    CLog::Log(LOGINFO, "Tizen: the TV does not show HDR video");
    return caps;
  }

  namespace fs = std::filesystem;
  const bool monitor = FeatureBool("com.samsung/featureconf/product.smart_monitor_feature_support");
  const int width = FeatureInt("com.samsung/featureconf/panel_resolution_width");
  const std::string panel = width >= 7680 ? "8K_" : width >= 3840 ? "4K_" : width > 0 ? "2K_" : "";
  std::optional<EdidHdr> tv;
  std::error_code ec;
  for (const auto& set : fs::directory_iterator("/lib/firmware/hdmiedid", ec))
  {
    const std::string name = set.path().filename().string();
    if ((name.find("_MNT_") != std::string::npos) != monitor || !name.starts_with(panel))
      continue;
    std::optional<EdidHdr> best;
    for (const auto& file : fs::recursive_directory_iterator(set.path(), ec))
    {
      if (!file.is_regular_file())
        continue;
      if (const auto hdr = ReadEdidHdr(file.path()))
      {
        EdidHdr& b = best ? *best : best.emplace();
        b.hdr10 |= hdr->hdr10;
        b.hlg |= hdr->hlg;
        b.hdr10Plus |= hdr->hdr10Plus;
        b.dolbyVision |= hdr->dolbyVision;
      }
    }
    if (!best)
      continue;
    CLog::Log(LOGDEBUG, "Tizen: EDIDs {}: HDR10 {} HLG {} HDR10+ {} Dolby Vision {}", name,
              best->hdr10, best->hlg, best->hdr10Plus, best->dolbyVision);
    if (!tv)
      tv = best;
    tv->hdr10 &= best->hdr10;
    tv->hlg &= best->hlg;
    tv->hdr10Plus &= best->hdr10Plus;
    tv->dolbyVision &= best->dolbyVision;
  }

  // HDR at all means HDR10
  caps.SetHDR10();
  if (tv && tv->hlg)
    caps.SetHLG();
  if (tv && tv->hdr10Plus)
    caps.SetHDR10Plus();
  if (tv && tv->dolbyVision)
    caps.SetDolbyVision();
  CLog::Log(LOGINFO, "Tizen: the TV shows HDR video:{}{}{}{}{}", caps.SupportsHDR10() ? " HDR10" : "",
            caps.SupportsHLG() ? " HLG" : "", caps.SupportsHDR10Plus() ? " HDR10+" : "",
            caps.SupportsDolbyVision() ? " Dolby Vision" : "",
            tv ? "" : " (no EDID of the TV found)");
  return caps;
}
} // namespace

namespace KODI::WINDOWING::WAYLAND
{

bool CWinSystemWaylandTizen::InitWindowSystem()
{
  // Which software the TV runs, as its version number is not given to apps
  CLog::Log(LOGINFO, "Tizen: software build {}",
            KODI::PLATFORM::TIZEN::GetPlatformString("http://tizen.org/system/build.string"));
  if (!CWinSystemWayland::InitWindowSystem())
    return false;

  m_tizenRegistry = std::make_unique<CRegistry>(*GetConnection());
  m_tizenRegistry->RequestSingleton(m_tizenSurface, 1, 1, false);
  m_tizenRegistry->RequestSingleton(m_keyrouter, 1, 2, false);
  m_tizenRegistry->RequestSingleton(m_policy, 1, 1, false);
  m_tizenRegistry->Bind();

  if (tizen_emss_available())
    CRendererTizen::Register();

  m_hdrCaps = TvHdrCapabilities();

  if (pipe2(m_lifecyclePipe, O_CLOEXEC) == 0)
  {
    g_lifecycleFd = m_lifecyclePipe[1];
    struct sigaction action = {};
    action.sa_handler = OnLifecycleSignal;
    action.sa_flags = SA_RESTART;
    sigaction(SIGUSR1, &action, nullptr);
    sigaction(SIGUSR2, &action, nullptr);
    m_lifecycleThread = std::thread(&CWinSystemWaylandTizen::WatchLifecycle, this);
  }
  return true;
}

bool CWinSystemWaylandTizen::DestroyWindowSystem()
{
  if (m_lifecycleThread.joinable())
  {
    signal(SIGUSR1, SIG_IGN);
    signal(SIGUSR2, SIG_IGN);
    g_lifecycleFd = -1;
    close(m_lifecyclePipe[1]);
    m_lifecycleThread.join();
    close(m_lifecyclePipe[0]);
  }

  m_tizenResource = wayland::tizen_resource_t{};
  m_visibility = wayland::tizen_visibility_t{};
  m_policy = wayland::tizen_policy_t{};
  m_keyrouter = wayland::tizen_keyrouter_t{};
  m_tizenSurface = wayland::tizen_surface_t{};
  if (m_tizenRegistry)
    m_tizenRegistry->UnbindSingletons();
  m_tizenRegistry.reset();
  return CWinSystemWayland::DestroyWindowSystem();
}

bool CWinSystemWaylandTizen::CreateNewWindow(const std::string& name,
                                            bool fullScreen,
                                            RESOLUTION_INFO& res)
{
  if (!CWinSystemWayland::CreateNewWindow(name, fullScreen, res))
    return false;

  if (m_tizenSurface)
  {
    m_tizenResource = m_tizenSurface.get_tizen_resource(GetMainSurface());
    m_tizenResource.on_resource_id() = [this](uint32_t id)
    {
      CLog::LogF(LOGDEBUG, "Window resource id {}", id);
      m_resourceId = id;
    };
  }
  else
  {
    CLog::LogF(LOGWARNING, "No tizen_surface, the TV's player cannot show video on the window");
  }

  // The TV does not tell the app when it goes to the background, as the window is
  // Kodi's and not the app's, but the compositor tells Kodi.
  if (m_policy)
  {
    m_visibility = m_policy.get_visibility(GetMainSurface());
    m_visibility.on_notify() = [](uint32_t visibility)
    {
      CLog::Log(LOGDEBUG, "Tizen: window visibility {}", visibility);
      SendLifecycleEvent(visibility == static_cast<uint32_t>(wayland::tizen_visibility_visibility::fully_obscured)
                             ? EVENT_BACKGROUND
                             : EVENT_VISIBLE);
    };
    m_policy.on_iconify_state_changed() =
        [](wayland::surface_t const&, uint32_t iconified, uint32_t)
    {
      CLog::Log(LOGDEBUG, "Tizen: window {}", iconified ? "iconified" : "uniconified");
      if (iconified)
        SendLifecycleEvent(EVENT_BACKGROUND);
    };
  }
  else
  {
    CLog::LogF(LOGWARNING, "No tizen_policy, Kodi cannot come to the front when started again");
  }

  GrabRemoteKeys();
  return true;
}

void CWinSystemWaylandTizen::WatchLifecycle()
{
  char event;
  bool background = false;
  while (read(m_lifecyclePipe[0], &event, 1) == 1)
  {
    // The compositor and the launcher may both tell of the same change.
    if (event == EVENT_BACKGROUND && background)
      continue;
    if (event == EVENT_VISIBLE && !background)
      continue;
    if (event != EVENT_ACTIVATE)
      background = event == EVENT_BACKGROUND;
    OnLifecycleEvent(event);
  }
}

void CWinSystemWaylandTizen::OnLifecycleEvent(char event)
{
  switch (event)
  {
    case EVENT_BACKGROUND:
    {
      CLog::Log(LOGINFO, "Tizen: app in the background");
      // A TV gives the video decoder and the sound to the app in front, so playback stops.
      const auto player = CServiceBroker::GetAppComponents().GetComponent<CApplicationPlayer>();
      if (player && player->IsPlaying())
        CServiceBroker::GetAppMessenger()->SendMsg(TMSG_GUI_ACTION, WINDOW_INVALID, -1,
                                                   static_cast<void*>(new CAction(ACTION_STOP)));
      break;
    }
    case EVENT_VISIBLE:
      CLog::Log(LOGINFO, "Tizen: app in front");
      break;
    case EVENT_ACTIVATE:
      // Starting the app again brings the app's process to the front, which has no window,
      // so Kodi raises its own.
      CLog::Log(LOGINFO, "Tizen: app started again, bringing Kodi to the front");
      if (m_policy && GetMainSurface())
      {
        m_policy.uniconify(GetMainSurface());
        m_policy.activate(GetMainSurface());
        GetConnection()->GetDisplay().flush();
      }
      break;
  }
}

std::unique_ptr<CSeat> CWinSystemWaylandTizen::CreateSeat(std::uint32_t name,
                                                          wayland::seat_t& seat)
{
  auto newSeat = std::make_unique<CSeatTizen>(name, seat, *GetConnection());
  newSeat->AddRawInputHandlerKeyboard(this);
  return newSeat;
}

void CWinSystemWaylandTizen::OnKeyboardKeymap(CSeat* seat,
                                              wayland::keyboard_keymap_format format,
                                              std::string const& keymap)
{
  if (format != wayland::keyboard_keymap_format::xkb_v1)
    return;

  // The TV's keymap names keys that xkbcommon does not know, which it then drops. They
  // are listed with their keycodes, to tell which key of a remote control sends what.
  {
    std::map<std::string, std::string> codes;
    const std::regex keycode(R"(<([A-Za-z0-9+-]+)>\s*=\s*([0-9]+)\s*;)");
    for (std::sregex_iterator it(keymap.begin(), keymap.end(), keycode), end; it != end; ++it)
      codes[(*it)[1]] = (*it)[2];
    const std::regex symbol(R"(key\s+<([A-Za-z0-9+-]+)>\s*\{[^}]*?\[\s*(XF86[A-Za-z0-9_]+))");
    std::string keys;
    for (std::sregex_iterator it(keymap.begin(), keymap.end(), symbol), end; it != end; ++it)
      keys += " " + codes[(*it)[1]] + "=" + std::string((*it)[2]);
    CLog::LogF(LOGINFO, "Remote control keys of the TV:{}", keys);
  }

  xkb_context* context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
  xkb_keymap* map = context ? xkb_keymap_new_from_string(context, keymap.c_str(),
                                                         XKB_KEYMAP_FORMAT_TEXT_V1,
                                                         XKB_KEYMAP_COMPILE_NO_FLAGS)
                            : nullptr;
  if (map)
  {
    std::unique_lock lock(m_keysLock);
    m_remoteKeys.clear();
    xkb_keymap_key_for_each(
        map,
        [](xkb_keymap* keymap, xkb_keycode_t key, void* data)
        {
          const xkb_keysym_t* syms = nullptr;
          if (xkb_keymap_key_get_syms_by_level(keymap, key, 0, 0, &syms) < 1)
            return;
          char name[64];
          if (xkb_keysym_get_name(syms[0], name, sizeof(name)) <= 0)
            return;
          const std::string_view keyName(name);
          if (std::ranges::find(REMOTE_KEYS, keyName) == REMOTE_KEYS.end())
            return;
          static_cast<CWinSystemWaylandTizen*>(data)->m_remoteKeys.push_back(key);
        },
        this);
    CLog::LogF(LOGDEBUG, "{} remote control keys in the keymap", m_remoteKeys.size());
    m_keysGrabbed = false;
  }
  if (map)
    xkb_keymap_unref(map);
  if (context)
    xkb_context_unref(context);

  GrabRemoteKeys();
}

void CWinSystemWaylandTizen::GrabRemoteKeys()
{
  std::unique_lock lock(m_keysLock);
  if (m_keysGrabbed || !m_keyrouter || m_remoteKeys.empty() || !GetMainSurface())
    return;

  m_keyrouter.on_keygrab_notify() =
      [](const wayland::surface_t&, uint32_t key, uint32_t mode, uint32_t error)
  {
    // A public certificate does not give the privilege, the keys then reach the window
    // only while it has the focus.
    if (error)
      CLog::Log(LOGDEBUG, "tizen_keyrouter: key {} not grabbed: {}", key, error);
  };
  for (uint32_t key : m_remoteKeys)
    m_keyrouter.set_keygrab(GetMainSurface(), key, KEYGRAB_REGISTERED);
  m_keysGrabbed = true;
}

void CWinSystemWaylandTizen::ApplyOpaqueRegion()
{
  // The TV's player shows video on a plane under the window, which has to be transparent
  // where Kodi draws no GUI.
}

void CWinSystemWaylandTizen::OnConfigure(std::uint32_t serial,
                                         CSizeInt size,
                                         IShellSurface::StateBitset state)
{
  // Older TVs (zxdg_shell_v6) answer the request for full screen with a maximized window
  // of the screen's size. Kodi would draw window decorations into it then, and its GUI
  // ended up outside the screen. A TV has no other kind of window.
  if (state.test(IShellSurface::STATE_MAXIMIZED))
  {
    state.reset(IShellSurface::STATE_MAXIMIZED);
    state.set(IShellSurface::STATE_FULLSCREEN);
  }
  CWinSystemWayland::OnConfigure(serial, size, state);
}

bool CWinSystemWaylandTizen::HasCursor()
{
  return false;
}

bool CWinSystemWaylandTizen::IsHDRDisplay()
{
  return m_hdrCaps.SupportsHDR10() || m_hdrCaps.SupportsHLG() || m_hdrCaps.SupportsHDR10Plus() ||
         m_hdrCaps.SupportsDolbyVision();
}

CHDRCapabilities CWinSystemWaylandTizen::GetDisplayHDRCapabilities() const
{
  return m_hdrCaps;
}

std::unique_ptr<KODI::WINDOWING::IOSScreenSaver> CWinSystemWaylandTizen::GetOSScreenSaverImpl()
{
  if (auto screenSaver = COSScreenSaverTizen::TryCreate())
    return screenSaver;
  return CWinSystemWayland::GetOSScreenSaverImpl();
}

} // namespace KODI::WINDOWING::WAYLAND
