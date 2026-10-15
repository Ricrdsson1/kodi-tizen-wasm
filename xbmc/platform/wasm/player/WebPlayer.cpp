/*
 *  Copyright (C) 2026 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "WebPlayer.h"

#include "FileItem.h"
#include "ServiceBroker.h"
#include "URL.h"
#include "cores/DataCacheCore.h"
#include "cores/IPlayerCallback.h"
#include "cores/VideoPlayer/Interface/StreamInfo.h"
#include "language/LanguageTag.h"
#include "messaging/ApplicationMessenger.h"
#include "utils/JSONVariantParser.h"
#include "utils/JSONVariantWriter.h"
#include "utils/StringUtils.h"
#include "utils/Variant.h"
#include "utils/log.h"
#include "video/Bookmark.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>

using namespace std::chrono_literals;

extern "C"
{
int web_player_open(const char* url, const char* properties, double startSeconds);
void web_player_close();
void web_player_poll(double* state);
void web_player_pause(int pause);
void web_player_seek(double ms);
char* web_player_tracks();
void web_player_set_audio(const char* language, int channels);
void web_player_set_text(int index, int visible);
}

namespace
{
// The doubles web_player_poll() fills, in web_player.js order.
enum State
{
  TIME,
  DURATION,
  PAUSED,
  ENDED,
  ERROR,
  BUFFERING,
  WIDTH,
  HEIGHT,
  LOADED,
  COUNT
};

constexpr const char* INPUTSTREAM_ADAPTIVE = "inputstream.adaptive";

// Properties of InputStream Adaptive that configure DRM.
constexpr const char* DRM_PROPERTIES[] = {
    "inputstream.adaptive.drm_legacy",
    "inputstream.adaptive.drm",
    "inputstream.adaptive.license_key",
};

constexpr auto POLL_INTERVAL = 200ms;
constexpr int TRACK_UPDATE_POLLS = 10;
constexpr int64_t SMALL_STEP_MS = 30 * 1000;
constexpr int64_t LARGE_STEP_MS = 10 * 60 * 1000;
} // namespace

CWebPlayer::CWebPlayer(IPlayerCallback& callback)
  : IPlayer(callback), CThread("WebPlayer")
{
}

CWebPlayer::~CWebPlayer()
{
  CloseFile();
  delete m_item;
}

bool CWebPlayer::IsDrmItem(const CFileItem& item)
{
  if (item.GetProperty("inputstream").asString() != INPUTSTREAM_ADAPTIVE)
    return false;
  return std::ranges::any_of(DRM_PROPERTIES, [&item](const char* name)
                             { return !item.GetProperty(name).asString().empty(); });
}

bool CWebPlayer::OpenFile(const CFileItem& file, const CPlayerOptions& options)
{
  CloseFile();

  // The properties of the inputstream add-on carry the DRM and request settings.
  CVariant properties(CVariant::VariantTypeObject);
  for (const auto& [name, value] : file.GetProperties())
  {
    if (StringUtils::StartsWith(name, "inputstream"))
      properties[name] = value.asString();
  }
  std::string json;
  CJSONVariantWriter::Write(properties, json, true);

  const std::string url = file.GetDynPath();
  CLog::LogF(LOGINFO, "Playing {} in the browser", CURL::GetRedacted(url));
  if (!web_player_open(url.c_str(), json.c_str(), options.starttime))
  {
    CLog::LogF(LOGERROR, "The browser cannot play adaptive streams");
    return false;
  }

  delete m_item;
  m_item = new CFileItem(file);
  m_fullscreen = options.fullscreen;
  m_time = static_cast<int64_t>(options.starttime * 1000);
  m_duration = 0;
  m_started = false;
  m_paused = false;
  m_open = true;
  {
    std::unique_lock lock(m_tracksLock);
    m_audio.clear();
    m_text.clear();
    m_audioIndex = -1;
    m_textIndex = -1;
    m_textVisible = false;
  }

  m_callback.OnPlayBackStarted(file);
  Create();
  return true;
}

bool CWebPlayer::CloseFile(bool reopen)
{
  if (!m_open.exchange(false))
    return true;

  StopThread(true);

  if (m_started && m_item)
  {
    CBookmark bookmark;
    bookmark.timeInSeconds = static_cast<double>(m_time) / 1000.0;
    bookmark.totalTimeInSeconds = static_cast<double>(m_duration) / 1000.0;
    bookmark.player = m_name;
    m_callback.OnPlayerCloseFile(*m_item, bookmark);
  }

  web_player_close();
  m_started = false;
  CServiceBroker::GetDataCacheCore().Reset();
  m_callback.OnPlayBackStopped();
  return true;
}

bool CWebPlayer::IsPlaying() const
{
  return m_open;
}

void CWebPlayer::Pause()
{
  SetSpeed(m_paused ? 1.0f : 0.0f);
}

void CWebPlayer::SetSpeed(float speed)
{
  const bool pause = speed == 0.0f;
  if (pause == m_paused)
    return;

  web_player_pause(pause ? 1 : 0);
  m_paused = pause;
  CServiceBroker::GetDataCacheCore().SetSpeed(1.0f, pause ? 0.0f : 1.0f);
  if (pause)
    m_callback.OnPlayBackPaused();
  else
    m_callback.OnPlayBackResumed();
}

void CWebPlayer::Seek(bool bPlus, bool bLargeStep, bool bChapterOverride)
{
  const int64_t step = bLargeStep ? LARGE_STEP_MS : SMALL_STEP_MS;
  SeekTimeRelative(bPlus ? step : -step);
}

void CWebPlayer::SeekPercentage(float fPercent)
{
  if (m_duration > 0)
    SeekTime(static_cast<int64_t>(m_duration * fPercent / 100.0f));
}

void CWebPlayer::SeekTime(int64_t iTime)
{
  const int64_t duration = m_duration;
  int64_t time = std::max<int64_t>(iTime, 0);
  if (duration > 0)
    time = std::min(time, duration - 1000);
  const int64_t offset = time - m_time;
  CLog::LogF(LOGDEBUG, "Seeking to {} ms", time);

  web_player_seek(static_cast<double>(time));
  m_time = time;
  CDataCacheCore& dataCache = CServiceBroker::GetDataCacheCore();
  dataCache.SetPlayTimes(0, time, 0, duration);
  dataCache.SeekFinished(offset);
  m_callback.OnPlayBackSeek(time, offset);
}

bool CWebPlayer::SeekTimeRelative(int64_t iTime)
{
  SeekTime(m_time + iTime);
  return true;
}

void CWebPlayer::Process()
{
  double state[COUNT] = {};
  int polls = 0;
  bool wasBuffering = false;

  while (!m_bStop)
  {
    web_player_poll(state);
    CDataCacheCore& dataCache = CServiceBroker::GetDataCacheCore();

    if (state[ERROR] != 0.0)
    {
      CLog::LogF(LOGERROR, "Playback failed with error {}", state[ERROR]);
      m_open = false;
      web_player_close();
      if (m_started)
        m_callback.OnPlayBackEnded();
      else
        m_callback.OnPlayBackError();
      m_started = false;
      return;
    }

    if (state[LOADED] != 0.0 && !m_started)
    {
      CLog::LogF(LOGINFO, "Playback started, {}x{}, fullscreen {}", state[WIDTH], state[HEIGHT],
                 m_fullscreen);
      m_started = true;
      // The GUI clears to transparent over the video, as for a video plane of its own.
      dataCache.SetVideoRender(true);
      dataCache.SetGuiRender(true);
      dataCache.SetSpeed(1.0f, 1.0f);
      dataCache.SetVideoDecoderName("browser", true);
      dataCache.SetVideoDimensions(static_cast<int>(state[WIDTH]),
                                   static_cast<int>(state[HEIGHT]));
      UpdateTracks();
      m_callback.OnAVStarted(*m_item);
      if (m_fullscreen)
        CServiceBroker::GetAppMessenger()->PostMsg(TMSG_SWITCHTOFULLSCREEN);
    }

    if (m_started)
    {
      m_time = static_cast<int64_t>(state[TIME]);
      m_duration = static_cast<int64_t>(state[DURATION]);
      dataCache.SetPlayTimes(0, m_time, 0, m_duration);

      if (state[WIDTH] > 0.0)
        dataCache.SetVideoDimensions(static_cast<int>(state[WIDTH]),
                                     static_cast<int>(state[HEIGHT]));

      // Kodi resets the speed it shows when playback starts.
      if (dataCache.GetSpeed() != (m_paused ? 0.0f : 1.0f))
        dataCache.SetSpeed(1.0f, m_paused ? 0.0f : 1.0f);

      // The browser pauses by itself, e.g. when the TV puts the app in the background.
      const bool paused = state[PAUSED] != 0.0;
      if (paused != m_paused && !(paused && state[ENDED] != 0.0))
      {
        m_paused = paused;
        dataCache.SetSpeed(1.0f, paused ? 0.0f : 1.0f);
        if (paused)
          m_callback.OnPlayBackPaused();
        else
          m_callback.OnPlayBackResumed();
      }

      const bool buffering = state[BUFFERING] != 0.0;
      if (buffering != wasBuffering)
      {
        wasBuffering = buffering;
        m_buffering = buffering;
      }

      if (++polls % TRACK_UPDATE_POLLS == 0)
        UpdateTracks();

      if (state[ENDED] != 0.0)
      {
        CLog::LogF(LOGINFO, "Playback ended");
        CBookmark bookmark;
        bookmark.timeInSeconds = static_cast<double>(m_duration) / 1000.0;
        bookmark.totalTimeInSeconds = static_cast<double>(m_duration) / 1000.0;
        bookmark.player = m_name;
        m_callback.OnPlayerCloseFile(*m_item, bookmark);
        m_open = false;
        web_player_close();
        m_started = false;
        m_callback.OnPlayBackEnded();
        return;
      }
    }

    CThread::Sleep(POLL_INTERVAL);
  }
}

void CWebPlayer::UpdateTracks()
{
  char* json = web_player_tracks();
  if (!json)
    return;
  CVariant tracks;
  const bool parsed = CJSONVariantParser::Parse(json, tracks);
  free(json);
  if (!parsed)
    return;

  std::unique_lock lock(m_tracksLock);
  m_audio.clear();
  const CVariant& audio = tracks["audio"];
  for (auto track = audio.begin_array(); track != audio.end_array(); ++track)
  {
    m_audio.push_back({(*track)["language"].asString(), (*track)["name"].asString(),
                       static_cast<int>((*track)["channels"].asInteger()),
                       (*track)["codec"].asString()});
  }
  m_audioIndex = static_cast<int>(tracks["audioIndex"].asInteger(-1));

  m_text.clear();
  const CVariant& text = tracks["text"];
  for (auto track = text.begin_array(); track != text.end_array(); ++track)
  {
    m_text.push_back({(*track)["language"].asString(), (*track)["name"].asString(),
                      (*track)["forced"].asBoolean()});
  }
  m_textIndex = static_cast<int>(tracks["textIndex"].asInteger(-1));
  m_textVisible = tracks["textVisible"].asBoolean();
}

int CWebPlayer::GetAudioStreamCount() const
{
  std::unique_lock lock(m_tracksLock);
  return static_cast<int>(m_audio.size());
}

int CWebPlayer::GetAudioStream()
{
  std::unique_lock lock(m_tracksLock);
  return m_audioIndex;
}

void CWebPlayer::SetAudioStream(int iStream)
{
  std::string language;
  int channels = 0;
  {
    std::unique_lock lock(m_tracksLock);
    if (iStream < 0 || iStream >= static_cast<int>(m_audio.size()))
      return;
    language = m_audio[iStream].language;
    channels = m_audio[iStream].channels;
    m_audioIndex = iStream;
  }
  web_player_set_audio(language.c_str(), channels);
}

void CWebPlayer::GetAudioStreamInfo(int index, AudioStreamInfo& info) const
{
  std::unique_lock lock(m_tracksLock);
  if (index < 0 || index >= static_cast<int>(m_audio.size()))
    return;
  const AudioTrack& track = m_audio[index];
  info.valid = true;
  info.language = KODI::LANGUAGE::CLanguageTag::Parse(track.language);
  info.name = track.name;
  info.channels = track.channels;
  info.codecName = track.codec;
}

int CWebPlayer::GetSubtitleCount() const
{
  std::unique_lock lock(m_tracksLock);
  return static_cast<int>(m_text.size());
}

int CWebPlayer::GetSubtitle()
{
  std::unique_lock lock(m_tracksLock);
  return m_textIndex;
}

void CWebPlayer::GetSubtitleStreamInfo(int index, SubtitleStreamInfo& info) const
{
  std::unique_lock lock(m_tracksLock);
  if (index < 0 || index >= static_cast<int>(m_text.size()))
    return;
  const TextTrack& track = m_text[index];
  info.valid = true;
  info.language = KODI::LANGUAGE::CLanguageTag::Parse(track.language);
  info.name = track.name;
  if (track.forced)
    info.flags = StreamFlags::FLAG_FORCED;
}

void CWebPlayer::SetSubtitle(int iStream)
{
  bool visible;
  {
    std::unique_lock lock(m_tracksLock);
    if (iStream < 0 || iStream >= static_cast<int>(m_text.size()))
      return;
    m_textIndex = iStream;
    visible = m_textVisible;
  }
  web_player_set_text(iStream, visible ? 1 : 0);
}

bool CWebPlayer::GetSubtitleVisible() const
{
  std::unique_lock lock(m_tracksLock);
  return m_textVisible;
}

void CWebPlayer::SetSubtitleVisible(bool bVisible)
{
  int index;
  {
    std::unique_lock lock(m_tracksLock);
    m_textVisible = bVisible;
    index = m_textIndex;
  }
  web_player_set_text(index, bVisible ? 1 : 0);
}
