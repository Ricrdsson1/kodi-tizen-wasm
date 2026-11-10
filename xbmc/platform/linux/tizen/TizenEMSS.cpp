/*
 *  Copyright (C) 2026 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

// tizen_emss_* (see TizenEMSS.h) for a native Kodi, on Samsung's ESPlayer: the elementary
// stream player of a TV, which decodes the packets Kodi demuxes with the hardware and
// shows the video on a plane under Kodi's window. The WASM build has the same API on the
// TV's WASM player, so CMediaPipelineTizen does not tell them apart.
//
// libesplusplayer has no public headers. Its structures and constants are those that
// Samsung's Tizen.TV.Multimedia.ESPlayer passes to it.

#include "ServiceBroker.h"
#include "utils/log.h"
#include "windowing/wayland/WinSystemWaylandTizen.h"

#include "platform/wasm/tizen/TizenDRM.h"
#include "platform/wasm/tizen/TizenEMSS.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <dlfcn.h>

namespace
{
enum StreamType : int
{
  STREAM_AUDIO = 0,
  STREAM_VIDEO = 1,
};

// Track ids handed to Kodi: the stream type plus one, so that 0 means none.
constexpr int AUDIO_TRACK = STREAM_AUDIO + 1;
constexpr int VIDEO_TRACK = STREAM_VIDEO + 1;

enum SubmitStatus : int
{
  SUBMIT_NOT_PREPARED = 0,
  SUBMIT_INVALID_PACKET = 1,
  SUBMIT_OUT_OF_MEMORY = 2,
  SUBMIT_FULL = 3,
  SUBMIT_SUCCESS = 4,
};

enum PlayerState : int
{
  STATE_NONE = 0,
  STATE_IDLE = 1,
  STATE_READY = 2,
  STATE_PLAYING = 3,
  STATE_PAUSED = 4,
};

constexpr int DISPLAY_TYPE_OVERLAY = 1;
constexpr int DISPLAY_MODE_DST_ROI = 5;

enum VideoMime : int
{
  VIDEO_H264 = 2,
  VIDEO_HEVC = 3,
  VIDEO_MPEG2 = 5,
  VIDEO_MPEG4 = 6,
  VIDEO_VP8 = 7,
  VIDEO_VP9 = 8,
  VIDEO_AV1 = 10,
};

enum AudioMime : int
{
  AUDIO_UNKNOWN = 0,
  AUDIO_AAC = 1,
  AUDIO_MP3 = 3,
  AUDIO_AC3 = 4,
  AUDIO_EAC3 = 5,
  AUDIO_OPUS = 7,
  AUDIO_PCM_S16LE = 8,
};

struct EsPacket
{
  int type;
  const uint8_t* buffer;
  uint32_t size;
  uint64_t pts; // ms
  uint64_t duration; // ms
  void* matroskaColor;
  uint32_t hdr10pSize;
  void* hdr10p;
};

static_assert(offsetof(TizenEmssVideoColor, primaryRX) == 56 && sizeof(TizenEmssVideoColor) == 136,
              "TizenEmssVideoColor has the layout of esplusplayer_matroska_color");

struct VideoStreamInfo
{
  const uint8_t* codecData;
  uint32_t codecDataLength;
  int mime;
  uint32_t width;
  uint32_t height;
  uint32_t maxWidth;
  uint32_t maxHeight;
  uint32_t framerateNum;
  uint32_t framerateDen;
};

struct AudioStreamInfo
{
  const uint8_t* codecData;
  uint32_t codecDataLength;
  int mime;
  uint32_t bitrate;
  uint32_t channels;
  uint32_t sampleRate;
};

struct EsPlusPlayer
{
  void* (*create)();
  int (*open)(void*);
  int (*close)(void*);
  int (*destroy)(void*);
  int (*set_surface_display)(void*, int, unsigned int, int, int, int, int);
  int (*set_display_mode)(void*, int);
  int (*set_display_roi)(void*, int, int, int, int);
  int (*set_video_stream_info)(void*, const VideoStreamInfo*);
  int (*set_audio_stream_info)(void*, const AudioStreamInfo*);
  int (*set_ready_to_prepare_cb)(void*, void (*)(int, void*), void*);
  int (*set_prepare_async_done_cb)(void*, void (*)(bool, void*), void*);
  int (*set_ready_to_seek_cb)(void*, void (*)(int, uint64_t, void*), void*);
  int (*set_seek_done_cb)(void*, void (*)(void*), void*);
  int (*set_error_cb)(void*, void (*)(int, void*), void*);
  int (*set_eos_cb)(void*, void (*)(void*), void*);
  int (*prepare_async)(void*);
  int (*start)(void*);
  int (*stop)(void*);
  int (*pause)(void*);
  int (*resume)(void*);
  int (*seek)(void*, uint64_t);
  int (*set_playback_rate)(void*, double);
  int (*submit_packet)(void*, const EsPacket*);
  int (*submit_eos_packet)(void*, int);
  int (*get_playing_time)(void*, uint64_t*);
  int (*get_decoding_time)(void*, int, int*); //!< optional
  int (*get_state)(void*);

  bool Load()
  {
    void* lib = dlopen("libesplusplayer.so", RTLD_NOW | RTLD_GLOBAL);
    if (!lib)
    {
      CLog::Log(LOGERROR, "ESPlayer: {}", dlerror());
      return false;
    }
    bool ok = true;
    auto get = [&](auto& fn, const char* name)
    {
      fn = reinterpret_cast<std::remove_reference_t<decltype(fn)>>(dlsym(lib, name));
      if (!fn)
      {
        CLog::Log(LOGERROR, "ESPlayer: no {}", name);
        ok = false;
      }
    };
    get(create, "esplusplayer_create");
    get(open, "esplusplayer_open");
    get(close, "esplusplayer_close");
    get(destroy, "esplusplayer_destroy");
    get(set_surface_display, "esplusplayer_set_surface_display");
    get(set_display_mode, "esplusplayer_set_display_mode");
    get(set_display_roi, "esplusplayer_set_display_roi");
    get(set_video_stream_info, "esplusplayer_set_video_stream_info");
    get(set_audio_stream_info, "esplusplayer_set_audio_stream_info");
    get(set_ready_to_prepare_cb, "esplusplayer_set_ready_to_prepare_cb");
    get(set_prepare_async_done_cb, "esplusplayer_set_prepare_async_done_cb");
    get(set_ready_to_seek_cb, "esplusplayer_set_ready_to_seek_cb");
    get(set_seek_done_cb, "esplusplayer_set_seek_done_cb");
    get(set_error_cb, "esplusplayer_set_error_cb");
    get(set_eos_cb, "esplusplayer_set_eos_cb");
    get(prepare_async, "esplusplayer_prepare_async");
    get(start, "esplusplayer_start");
    get(stop, "esplusplayer_stop");
    get(pause, "esplusplayer_pause");
    get(resume, "esplusplayer_resume");
    get(seek, "esplusplayer_seek");
    get(set_playback_rate, "esplusplayer_set_playback_rate");
    get(submit_packet, "esplusplayer_submit_packet");
    get(submit_eos_packet, "esplusplayer_submit_eos_packet");
    get(get_playing_time, "esplusplayer_get_playing_time");
    get(get_state, "esplusplayer_get_state");
    get_decoding_time =
        reinterpret_cast<decltype(get_decoding_time)>(dlsym(lib, "esplusplayer_get_decoding_time"));
    return ok;
  }
};

EsPlusPlayer* Api()
{
  static EsPlusPlayer api;
  static const bool loaded = api.Load();
  return loaded ? &api : nullptr;
}

int VideoMimeOf(std::string_view mime)
{
  if (mime.find("avc1") != std::string_view::npos || mime.find("avc3") != std::string_view::npos)
    return VIDEO_H264;
  if (mime.find("hev1") != std::string_view::npos || mime.find("hvc1") != std::string_view::npos)
    return VIDEO_HEVC;
  if (mime.find("vp09") != std::string_view::npos)
    return VIDEO_VP9;
  if (mime.find("vp8") != std::string_view::npos)
    return VIDEO_VP8;
  if (mime.find("av01") != std::string_view::npos)
    return VIDEO_AV1;
  if (mime.find("mp4v") != std::string_view::npos)
    return VIDEO_MPEG4;
  if (mime.find("mpeg2") != std::string_view::npos || mime.find("mp2v") != std::string_view::npos)
    return VIDEO_MPEG2;
  return 0;
}

int AudioMimeOf(std::string_view mime)
{
  if (mime.find("mp4a") != std::string_view::npos)
    return AUDIO_AAC;
  if (mime.find("ec-3") != std::string_view::npos)
    return AUDIO_EAC3;
  if (mime.find("ac-3") != std::string_view::npos)
    return AUDIO_AC3;
  if (mime.find("opus") != std::string_view::npos)
    return AUDIO_OPUS;
  if (mime.find("pcm") != std::string_view::npos)
    return AUDIO_PCM_S16LE;
  if (mime == "audio/mpeg")
    return AUDIO_MP3;
  return AUDIO_UNKNOWN;
}

uint32_t ChannelsOf(std::string_view layout)
{
  if (layout == "ChannelLayoutMono")
    return 1;
  if (layout == "ChannelLayoutSurround")
    return 3;
  if (layout == "ChannelLayoutQuad")
    return 4;
  if (layout == "ChannelLayout5_0")
    return 5;
  if (layout == "ChannelLayout5_1")
    return 6;
  if (layout == "ChannelLayout6_1")
    return 7;
  if (layout == "ChannelLayout7_1")
    return 8;
  return 2;
}

uint64_t Milliseconds(double seconds)
{
  return seconds > 0 ? static_cast<uint64_t>(std::llround(seconds * 1000)) : 0;
}

/*!
 * \brief The one player of the TV that Kodi uses at a time, and the state Kodi polls.
 */
class CEsPlayer
{
public:
  bool Open(TizenEmssState* state, const TizenEmssConfig& config);
  void Close();
  bool CreatePlayer();
  void DestroyPlayer();
  void Play();
  void Pause();
  void SetRate(double rate);
  bool Seek(double seconds);
  void SetRect(int x, int y, int width, int height);
  int Append(int trackId, const uint8_t* data, int size, double pts, double duration, int session);
  void EndOfTrack(int trackId, int session);
  void SetVideoColor(const TizenEmssVideoColor* color);

private:
  static void OnReadyToPrepare(int type, void* self);
  static void OnPrepared(bool result, void* self);
  static void OnReadyToSeek(int type, uint64_t offset, void* self);
  static void OnSeekDone(void* self);
  static void OnError(int error, void* self);
  static void OnEos(void* self);

  std::unique_lock<std::recursive_mutex> LockForCallback();
  void SetTrackOpen(int type, bool open);
  void StartIfWanted();
  void WatchTime();

  EsPlusPlayer* m_api{nullptr};
  void* m_player{nullptr};
  TizenEmssState* m_state{nullptr};
  std::recursive_mutex m_lock;
  //! Set while the player is destroyed, which waits for its callbacks under m_lock
  std::atomic<bool> m_destroying{false};
  bool m_hasAudio{false};
  //! Given with every video packet, which is how the TV learns of HDR in a stream
  TizenEmssVideoColor m_videoColor{};
  bool m_hasVideoColor{false};
  bool m_wantPlay{false};
  bool m_prepared{false};
  bool m_started{false};
  //! The time of the latest seek, ms: the player still answers earlier seeks after it
  uint64_t m_seekTarget{0};
  std::vector<uint64_t> m_earlierSeekTargets;
  std::array<int, 2> m_loggedPackets{}; //!< packets logged of each track since it opened

  //! The tracks as Open() was given them, which the player is opened with again on a seek
  struct SavedConfig
  {
    TizenEmssConfig config{};
    std::string videoMime;
    std::string audioMime;
    std::string sampleFormat;
    std::string channelLayout;
    std::vector<uint8_t> videoExtradata;
    std::vector<uint8_t> audioExtradata;
  } m_saved;
  uint32_t m_surface{0};
  int m_videoMime{0};
  std::array<int, 4> m_rect{}; //!< where Kodi shows the video: x, y, width, height
  //! Held by Append() while it gives the player a packet, and to destroy the player
  std::shared_mutex m_playerUse;
  std::atomic<int> m_packetsSinceCreate{0};
  std::array<std::atomic<int>, 2> m_takenPackets{}; //!< packets the player took of each track
  std::array<std::atomic<double>, 2> m_lastPts{}; //!< of the last packet taken of each track
  bool m_seeking{false};
  double m_rate{1.0};
  std::atomic<bool> m_watching{false};
  std::thread m_timeThread;
};

CEsPlayer g_player;

bool CEsPlayer::Open(TizenEmssState* state, const TizenEmssConfig& config)
{
  Close();
  std::unique_lock lock(m_lock);
  m_api = Api();
  if (!m_api)
    return false;

  auto* winSystem =
      dynamic_cast<KODI::WINDOWING::WAYLAND::CWinSystemWaylandTizen*>(CServiceBroker::GetWinSystem());
  const uint32_t surface = winSystem ? winSystem->GetSurfaceResourceId() : 0;
  if (!surface)
  {
    CLog::LogF(LOGERROR, "ESPlayer: Kodi's window has no resource id to show video on");
    return false;
  }

  const int videoMime = VideoMimeOf(config.videoMime ? config.videoMime : "");
  if (!videoMime)
  {
    CLog::LogF(LOGERROR, "ESPlayer: unsupported video {}", config.videoMime);
    return false;
  }

  m_state = state;
  m_state->error = 0;
  m_state->ended = 0;
  m_state->videoOpen = 0;
  m_state->audioOpen = 0;
  m_state->sessionId = 1;
  m_state->currentTime = 0.0;
  m_state->videoTrack = VIDEO_TRACK;
  m_state->audioTrack = config.hasAudio ? AUDIO_TRACK : 0;
  m_state->sourceState = TIZEN_EMSS_CLOSED;
  m_hasAudio = config.hasAudio != 0;
  m_wantPlay = false;
  m_rate = 1.0;
  m_surface = surface;
  m_videoMime = videoMime;

  // The tracks as given, to open the player again on a seek
  m_saved.videoMime = config.videoMime ? config.videoMime : "";
  m_saved.audioMime = config.audioMime ? config.audioMime : "";
  m_saved.sampleFormat = config.sampleFormat ? config.sampleFormat : "";
  m_saved.channelLayout = config.channelLayout ? config.channelLayout : "";
  m_saved.videoExtradata.assign(config.videoExtradata,
                                config.videoExtradata + std::max(config.videoExtradataSize, 0));
  m_saved.audioExtradata.assign(config.audioExtradata,
                                config.audioExtradata + std::max(config.audioExtradataSize, 0));
  m_saved.config = config;
  m_saved.config.videoMime = m_saved.videoMime.c_str();
  m_saved.config.audioMime = m_saved.audioMime.c_str();
  m_saved.config.sampleFormat = m_saved.sampleFormat.c_str();
  m_saved.config.channelLayout = m_saved.channelLayout.c_str();
  m_saved.config.videoExtradata = m_saved.videoExtradata.data();
  m_saved.config.audioExtradata = m_saved.audioExtradata.data();

  if (!CreatePlayer())
  {
    Close();
    return false;
  }

  m_watching = true;
  m_timeThread = std::thread(&CEsPlayer::WatchTime, this);
  return true;
}

bool CEsPlayer::CreatePlayer()
{
  const TizenEmssConfig& config = m_saved.config;
  const uint32_t surface = m_surface;
  const int videoMime = m_videoMime;
  m_prepared = false;
  m_started = false;
  m_packetsSinceCreate = 0;

  m_player = m_api->create();
  if (!m_player || m_api->open(m_player) != 0)
  {
    CLog::LogF(LOGERROR, "ESPlayer: cannot open the player");
    return false;
  }

  m_api->set_ready_to_prepare_cb(m_player, OnReadyToPrepare, this);
  m_api->set_prepare_async_done_cb(m_player, OnPrepared, this);
  m_api->set_ready_to_seek_cb(m_player, OnReadyToSeek, this);
  m_api->set_seek_done_cb(m_player, OnSeekDone, this);
  m_seeking = false;
  m_api->set_error_cb(m_player, OnError, this);
  m_api->set_eos_cb(m_player, OnEos, this);

  int ret = m_api->set_surface_display(m_player, DISPLAY_TYPE_OVERLAY, surface, 0, 0,
                                       config.width > 0 ? config.width : 1920,
                                       config.height > 0 ? config.height : 1080);
  if (ret != 0)
    CLog::LogF(LOGWARNING, "ESPlayer: set_surface_display {} failed: {}", surface, ret);
  m_api->set_display_mode(m_player, DISPLAY_MODE_DST_ROI);
  if (m_rect[2] > 0 && m_rect[3] > 0)
    m_api->set_display_roi(m_player, m_rect[0], m_rect[1], m_rect[2], m_rect[3]);

  const VideoStreamInfo video{config.videoExtradata,
                              static_cast<uint32_t>(std::max(config.videoExtradataSize, 0)),
                              videoMime,
                              static_cast<uint32_t>(config.width),
                              static_cast<uint32_t>(config.height),
                              static_cast<uint32_t>(std::max(config.width, 1920)),
                              static_cast<uint32_t>(std::max(config.height, 1080)),
                              static_cast<uint32_t>(config.framerateNum),
                              static_cast<uint32_t>(std::max(config.framerateDen, 1))};
  ret = m_api->set_video_stream_info(m_player, &video);
  if (ret != 0)
  {
    CLog::LogF(LOGERROR, "ESPlayer: video stream {} {}x{} rejected: {}", config.videoMime,
               config.width, config.height, ret);
    return false;
  }

  if (m_hasAudio)
  {
    const AudioStreamInfo audio{config.audioExtradata,
                                static_cast<uint32_t>(std::max(config.audioExtradataSize, 0)),
                                AudioMimeOf(config.audioMime ? config.audioMime : ""),
                                0,
                                ChannelsOf(config.channelLayout ? config.channelLayout : ""),
                                static_cast<uint32_t>(config.sampleRate)};
    ret = m_api->set_audio_stream_info(m_player, &audio);
    if (ret != 0)
    {
      CLog::LogF(LOGERROR, "ESPlayer: audio stream {} rejected: {}", config.audioMime, ret);
      return false;
    }
  }

  m_state->sourceState = TIZEN_EMSS_OPEN_PENDING;
  if (const int ret = m_api->prepare_async(m_player); ret != 0)
  {
    CLog::LogF(LOGERROR, "ESPlayer: prepare failed: {:#x}", static_cast<unsigned>(ret));
    return false;
  }
  CLog::LogF(LOGINFO, "ESPlayer: preparing {} {}x{}{}", config.videoMime, config.width,
             config.height, m_hasAudio ? std::string(" with ") + config.audioMime : "");
  return true;
}

void CEsPlayer::DestroyPlayer()
{
  if (!m_player)
    return;
  // No packet is being given to it now (see Append()).
  std::unique_lock use(m_playerUse);
  m_destroying = true;
  const int stopped = m_api->stop(m_player);
  const int closed = m_api->close(m_player);
  const int destroyed = m_api->destroy(m_player);
  m_destroying = false;
  CLog::LogF(LOGINFO, "ESPlayer: closed ({:#x} {:#x} {:#x})", static_cast<unsigned>(stopped),
             static_cast<unsigned>(closed), static_cast<unsigned>(destroyed));
  m_player = nullptr;
  m_prepared = false;
  m_started = false;
}

void CEsPlayer::Close()
{
  m_watching = false;
  if (m_timeThread.joinable())
    m_timeThread.join();

  std::unique_lock lock(m_lock);
  DestroyPlayer();
  if (m_state)
  {
    m_state->videoOpen = 0;
    m_state->audioOpen = 0;
    m_state->sourceState = TIZEN_EMSS_DETACHED;
    m_state = nullptr;
  }
}

void CEsPlayer::SetTrackOpen(int type, bool open)
{
  if (!m_state)
    return;
  if (type == STREAM_VIDEO)
    m_state->videoOpen = open ? 1 : 0;
  else
    m_state->audioOpen = open ? 1 : 0;
  if (open)
    m_loggedPackets[type == STREAM_VIDEO ? 0 : 1] = 0;
  if (open)
    m_state->sourceState = TIZEN_EMSS_OPEN;
}

std::unique_lock<std::recursive_mutex> CEsPlayer::LockForCallback()
{
  // A callback of a player that is being destroyed has nothing left to do.
  std::unique_lock lock(m_lock, std::defer_lock);
  while (!lock.try_lock())
  {
    if (m_destroying)
      return lock;
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  return lock;
}

void CEsPlayer::OnReadyToPrepare(int type, void* self)
{
  auto* player = static_cast<CEsPlayer*>(self);
  CLog::Log(LOGDEBUG, "ESPlayer: {} track ready to prepare", type == STREAM_VIDEO ? "video" : "audio");
  const auto lock = player->LockForCallback();
  if (!lock.owns_lock())
    return;
  player->SetTrackOpen(type, true);
}

void CEsPlayer::OnPrepared(bool result, void* self)
{
  auto* player = static_cast<CEsPlayer*>(self);
  const auto lock = player->LockForCallback();
  if (!lock.owns_lock())
    return;
  if (!result)
  {
    CLog::Log(LOGERROR, "ESPlayer: prepare failed");
    if (player->m_state)
      player->m_state->error = 1;
    return;
  }
  CLog::Log(LOGINFO, "ESPlayer: prepared");
  player->m_prepared = true;
  player->StartIfWanted();
}

void CEsPlayer::OnReadyToSeek(int type, uint64_t offset, void* self)
{
  auto* player = static_cast<CEsPlayer*>(self);
  const auto lock = player->LockForCallback();
  if (!lock.owns_lock())
    return;
  // A track answering an earlier seek would take the packets from after the latest.
  if (offset != player->m_seekTarget &&
      std::ranges::find(player->m_earlierSeekTargets, offset) !=
          player->m_earlierSeekTargets.end())
  {
    CLog::Log(LOGDEBUG, "ESPlayer: {} track ready for an earlier seek to {} ms, waiting for {} ms",
              type == STREAM_VIDEO ? "video" : "audio", offset, player->m_seekTarget);
    return;
  }
  CLog::Log(LOGDEBUG, "ESPlayer: {} track ready after the seek to {} ms",
            type == STREAM_VIDEO ? "video" : "audio", offset);
  player->SetTrackOpen(type, true);
}

void CEsPlayer::OnSeekDone(void* self)
{
  auto* player = static_cast<CEsPlayer*>(self);
  const auto lock = player->LockForCallback();
  if (!lock.owns_lock())
    return;
  CLog::Log(LOGDEBUG, "ESPlayer: seek done");
  player->m_seeking = false;
  player->m_earlierSeekTargets.clear();
}

void CEsPlayer::OnError(int error, void* self)
{
  auto* player = static_cast<CEsPlayer*>(self);
  CLog::Log(LOGERROR, "ESPlayer: error {:#x}", static_cast<unsigned>(error));
  const auto lock = player->LockForCallback();
  if (!lock.owns_lock())
    return;
  if (player->m_state)
    player->m_state->error = 1;
}

void CEsPlayer::OnEos(void* self)
{
  auto* player = static_cast<CEsPlayer*>(self);
  const auto lock = player->LockForCallback();
  if (!lock.owns_lock())
    return;
  if (player->m_state)
  {
    player->m_state->ended = 1;
    player->m_state->sourceState = TIZEN_EMSS_ENDED;
  }
}

void CEsPlayer::StartIfWanted()
{
  if (!m_player || !m_prepared || !m_wantPlay)
    return;
  if (!m_started)
  {
    m_started = m_api->start(m_player) == 0;
    if (m_started && m_rate != 1.0)
      m_api->set_playback_rate(m_player, m_rate);
  }
  else if (m_api->get_state(m_player) == STATE_PAUSED)
  {
    m_api->resume(m_player);
  }
}

void CEsPlayer::Play()
{
  std::unique_lock lock(m_lock);
  m_wantPlay = true;
  StartIfWanted();
}

void CEsPlayer::Pause()
{
  std::unique_lock lock(m_lock);
  m_wantPlay = false;
  if (m_player && m_api->get_state(m_player) == STATE_PLAYING)
    m_api->pause(m_player);
}

void CEsPlayer::SetRate(double rate)
{
  std::unique_lock lock(m_lock);
  m_rate = rate;
  if (m_player && m_started)
    m_api->set_playback_rate(m_player, rate);
}

bool CEsPlayer::Seek(double seconds)
{
  std::unique_lock lock(m_lock);
  if (!m_player || !m_state)
    return false;
  // A player that is not prepared yet starts with the first packets it gets, such as of
  // a stream whose time does not start at 0, and cannot seek.
  if (!m_prepared && m_packetsSinceCreate == 0)
  {
    m_state->currentTime = seconds;
    return true;
  }
  // The player's own seek leaves the sound off on a TV, while a player that is opened
  // again plays both tracks from the first packets it gets, as when playback starts.
  // The tracks take packets again once it is ready, under a new session.
  CLog::LogF(LOGDEBUG, "ESPlayer: seek to {:.3f} s, opening the player again", seconds);
  m_state->videoOpen = 0;
  m_state->audioOpen = 0;
  m_state->ended = 0;
  m_state->sessionId = m_state->sessionId + 1;
  m_state->currentTime = seconds;
  m_state->sourceState = TIZEN_EMSS_CLOSED;
  DestroyPlayer();
  if (!CreatePlayer())
  {
    CLog::LogF(LOGERROR, "ESPlayer: cannot open the player again for a seek");
    DestroyPlayer();
    m_state->error = 1;
    return false;
  }
  return true;
}

void CEsPlayer::SetRect(int x, int y, int width, int height)
{
  std::unique_lock lock(m_lock);
  if (width > 0 && height > 0)
  {
    m_rect = {x, y, width, height};
    if (m_player)
      m_api->set_display_roi(m_player, x, y, width, height);
  }
}

int CEsPlayer::Append(int trackId, const uint8_t* data, int size, double pts, double duration,
                      int session)
{
  // Not under the lock: the player takes packets of both tracks at once, while Kodi's
  // threads play, pause or seek it. Only a seek's opening it again waits for a packet.
  std::shared_lock use(m_playerUse);
  void* player = m_player;
  TizenEmssState* state = m_state;
  if (!state || (trackId != VIDEO_TRACK && trackId != AUDIO_TRACK))
    return TIZEN_EMSS_APPEND_FATAL;
  if (!player || session != state->sessionId ||
      !(trackId == VIDEO_TRACK ? state->videoOpen.load() : state->audioOpen.load()))
    return TIZEN_EMSS_APPEND_TRACK_CLOSED;

  void* color = trackId == VIDEO_TRACK && m_hasVideoColor ? &m_videoColor : nullptr;
  const EsPacket packet{trackId - 1,           data,    static_cast<uint32_t>(size),
                        Milliseconds(pts),     Milliseconds(duration),
                        color,                 0,       nullptr};
  const int submitted = m_api->submit_packet(player, &packet);
  if (submitted == SUBMIT_SUCCESS)
  {
    ++m_packetsSinceCreate;
    ++m_takenPackets[trackId == VIDEO_TRACK ? 0 : 1];
    m_lastPts[trackId == VIDEO_TRACK ? 0 : 1] = pts;
  }
  // The first packets of a track after it opened, and any the player does not take
  {
    int& logged = m_loggedPackets[trackId == VIDEO_TRACK ? 0 : 1];
    if (logged < 5 || (submitted != SUBMIT_SUCCESS && submitted != SUBMIT_FULL))
    {
      CLog::Log(LOGDEBUG, "ESPlayer: {} packet {:.3f} s, {} bytes, session {}: {}",
                trackId == VIDEO_TRACK ? "video" : "audio", pts, size, session, submitted);
      ++logged;
    }
  }
  switch (submitted)
  {
    case SUBMIT_SUCCESS:
      return TIZEN_EMSS_APPEND_OK;
    case SUBMIT_FULL:
    case SUBMIT_OUT_OF_MEMORY:
      return TIZEN_EMSS_APPEND_BUFFER_FULL;
    case SUBMIT_NOT_PREPARED:
      return TIZEN_EMSS_APPEND_TRACK_CLOSED;
    default:
      return TIZEN_EMSS_APPEND_DROPPED;
  }
}

void CEsPlayer::SetVideoColor(const TizenEmssVideoColor* color)
{
  std::unique_lock lock(m_lock);
  m_hasVideoColor = color != nullptr;
  if (color)
    m_videoColor = *color;
}

void CEsPlayer::EndOfTrack(int trackId, int session)
{
  if (m_player && m_state && session == m_state->sessionId)
    m_api->submit_eos_packet(m_player, trackId - 1);
}

void CEsPlayer::WatchTime()
{
  int polls = 0;
  while (m_watching)
  {
    {
      std::unique_lock lock(m_lock);
      uint64_t ms = 0;
      if (m_player && m_state && m_started && m_api->get_playing_time(m_player, &ms) == 0)
        m_state->currentTime = static_cast<double>(ms) / 1000.0;
      // How far the decoders are, to tell a track that stopped
      // How much of each track the player took, to tell a track that stopped
      if (m_player && m_started && ++polls % 250 == 0)
      {
        CLog::Log(LOGDEBUG,
                  "ESPlayer: playing {:.3f} s, took {} video packets up to {:.3f} s, {} audio "
                  "packets up to {:.3f} s, tracks open {}/{}",
                  ms / 1000.0, m_takenPackets[0].exchange(0), m_lastPts[0].load(),
                  m_takenPackets[1].exchange(0), m_lastPts[1].load(),
                  m_state ? m_state->videoOpen.load() : -1, m_state ? m_state->audioOpen.load() : -1);
      }
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
}
} // namespace

extern "C"
{
int tizen_emss_available(void)
{
  return Api() ? 1 : 0;
}

// The TV says what it takes only once it is given the stream.
int tizen_emss_validate_video(const char* mime, int, int, int, int)
{
  return VideoMimeOf(mime ? mime : "") ? -1 : 0;
}

int tizen_emss_validate_audio(const char* mime, const char*, const char*, int)
{
  return AudioMimeOf(mime ? mime : "") != AUDIO_UNKNOWN ? -1 : 0;
}

int tizen_emss_open(TizenEmssState* state, const TizenEmssConfig* config)
{
  return state && config && g_player.Open(state, *config) ? 1 : 0;
}

void tizen_emss_close(void)
{
  g_player.Close();
}

void tizen_emss_play(void)
{
  g_player.Play();
}

void tizen_emss_pause(void)
{
  g_player.Pause();
}

void tizen_emss_set_rate(double rate)
{
  g_player.SetRate(rate);
}

int tizen_emss_seek(double seconds)
{
  return g_player.Seek(seconds) ? 1 : 0;
}

void tizen_emss_set_rect(int x, int y, int width, int height)
{
  g_player.SetRect(x, y, width, height);
}

int tizen_emss_append(int trackId,
                      const uint8_t* data,
                      int size,
                      double pts,
                      double dts,
                      double duration,
                      int keyFrame,
                      int sessionId,
                      const TizenEmssFrameInfo* frame)
{
  return g_player.Append(trackId, data, size, pts, duration, sessionId);
}

// Encrypted streams need the TV's DRM, which a native Kodi does not reach yet.
int tizen_emss_append_encrypted(int, const uint8_t*, int, double, double, double, int, int,
                                const uint8_t*, const uint8_t*, const uint32_t*, int, int,
                                const TizenEmssFrameInfo*)
{
  return TIZEN_EMSS_APPEND_FATAL;
}

void tizen_emss_set_media_keys(int, int)
{
}

void tizen_emss_set_video_color(const TizenEmssVideoColor* color)
{
  g_player.SetVideoColor(color);
}

void tizen_emss_end_of_track(int trackId, int sessionId)
{
  g_player.EndOfTrack(trackId, sessionId);
}

int tizen_drm_open(TizenDrmState*, const char*, const char*, const uint8_t*, int, const uint8_t*)
{
  return 0;
}

int tizen_drm_read_message(int, uint8_t*, int)
{
  return 0;
}

void tizen_drm_update(int, const uint8_t*, int)
{
}

void tizen_drm_close(int)
{
}

int tizen_drm_usable_key_id(int, uint8_t*)
{
  return 0;
}
}
