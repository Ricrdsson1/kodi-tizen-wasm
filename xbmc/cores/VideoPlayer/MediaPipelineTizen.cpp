/*
 *  Copyright (C) 2026 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "MediaPipelineTizen.h"

#include "Buffers/VideoBufferTizen.h"
#include "DVDClock.h"
#include "DVDCodecs/Overlay/DVDOverlay.h"
#include "DVDOverlayContainer.h"
#include "Interface/DemuxPacket.h"
#include "Interface/TimingConstants.h"
#include "Process/ProcessInfo.h"
#include "ServiceBroker.h"
#include "VideoRenderers/RenderManager.h"
#include "cores/AudioEngine/Utils/AEUtil.h"
#include "settings/Settings.h"
#include "settings/SettingsComponent.h"
#include "utils/StringUtils.h"
#include "utils/log.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <vector>

#include <fmt/format.h>

extern "C"
{
#include <libavcodec/avcodec.h>
#include <libavcodec/defs.h>
}

using namespace std::chrono_literals;

namespace
{
// Timeline length given to the TV when the duration of the stream is not known.
constexpr double UNKNOWN_DURATION = 24.0 * 60.0 * 60.0;

// Polls of 10ms the TV is given to reopen its tracks after a seek.
constexpr int RESTART_TIMEOUT_POLLS = 500;

// Seconds of audio ahead of the playback position that count as a full buffer.
constexpr double FULL_AUDIO_BUFFER = 2.0;

const char* AudioMime(AVCodecID codec)
{
  switch (codec)
  {
    case AV_CODEC_ID_AAC:
      return "audio/mp4; codecs=\"mp4a.40.2\"";
    case AV_CODEC_ID_MP3:
      return "audio/mpeg";
    case AV_CODEC_ID_AC3:
      return "audio/mp4; codecs=\"ac-3\"";
    case AV_CODEC_ID_EAC3:
      return "audio/mp4; codecs=\"ec-3\"";
    case AV_CODEC_ID_OPUS:
      return "audio/webm; codecs=\"opus\"";
    default:
      return nullptr;
  }
}

const char* ChannelLayout(int channels)
{
  switch (channels)
  {
    case 1:
      return "ChannelLayoutMono";
    case 3:
      return "ChannelLayoutSurround";
    case 4:
      return "ChannelLayoutQuad";
    case 5:
      return "ChannelLayout5_0";
    case 6:
      return "ChannelLayout5_1";
    case 7:
      return "ChannelLayout6_1";
    case 8:
      return "ChannelLayout7_1";
    default:
      return "ChannelLayoutStereo";
  }
}

std::string VideoMime(const CDVDStreamInfo& hint)
{
  const int bitDepth = hint.bitdepth > 0 ? hint.bitdepth : 8;

  switch (hint.codec)
  {
    case AV_CODEC_ID_H264:
    {
      // An avcC record carries the exact profile, constraints and level.
      const uint8_t* extradata = hint.extradata.GetData();
      if (hint.extradata.GetSize() >= 4 && extradata[0] == 1)
        return fmt::format("video/mp4; codecs=\"avc1.{:02x}{:02x}{:02x}\"", extradata[1],
                           extradata[2], extradata[3]);

      const int profile = hint.profile > 0 ? (hint.profile & 0xff) : AV_PROFILE_H264_HIGH;
      const int level = hint.level > 0 ? hint.level : 40;
      return fmt::format("video/mp4; codecs=\"avc1.{:02x}00{:02x}\"", profile, level);
    }
    case AV_CODEC_ID_HEVC:
    {
      const bool main10 = hint.profile == AV_PROFILE_HEVC_MAIN_10 || bitDepth > 8;
      const int level = hint.level > 0 ? hint.level : 153;
      return fmt::format("video/mp4; codecs=\"hev1.{}.{}.L{}.B0\"", main10 ? 2 : 1, main10 ? 4 : 6,
                         level);
    }
    case AV_CODEC_ID_VP9:
    {
      const int profile = hint.profile > 0 ? hint.profile : (bitDepth > 8 ? 2 : 0);
      const int level = hint.level > 0 ? hint.level : 41;
      return fmt::format("video/webm; codecs=\"vp09.{:02}.{:02}.{:02}\"", profile, level, bitDepth);
    }
    case AV_CODEC_ID_VP8:
      return "video/webm; codecs=\"vp8\"";
    case AV_CODEC_ID_AV1:
    {
      const int profile = hint.profile > 0 ? hint.profile : 0;
      const int level = hint.level > 0 ? hint.level : 12;
      return fmt::format("video/mp4; codecs=\"av01.{}.{:02}M.{:02}\"", profile, level, bitDepth);
    }
    default:
      return {};
  }
}

double ToSeconds(double dvdTime)
{
  return dvdTime / DVD_TIME_BASE;
}
} // namespace

CMediaPipelineTizen::CMediaPipelineTizen(CProcessInfo& processInfo,
                                         CRenderManager& renderManager,
                                         CDVDClock& clock,
                                         CDVDMessageQueue& parent,
                                         CDVDOverlayContainer& overlay,
                                         const bool hasAudio)
  : CThread("MediaPipelineTizen"),
    m_hasAudio(hasAudio),
    m_messageQueueAudio("audio"),
    m_messageQueueVideo("video"),
    m_messageQueueParent(parent),
    m_processInfo(processInfo),
    m_renderManager(renderManager),
    m_clock(clock),
    m_overlayContainer(overlay)
{
  m_messageQueueAudio.Init();
  m_messageQueueVideo.Init();

  const auto settings = CServiceBroker::GetSettingsComponent()->GetSettings();
  const int tenthsSeconds = settings->GetInt(CSettings::SETTING_VIDEOPLAYER_QUEUETIMESIZE);
  const int sizeMB = settings->GetInt(CSettings::SETTING_VIDEOPLAYER_QUEUEDATASIZE);
  m_messageQueueAudio.SetMaxTimeSize(tenthsSeconds / 10.0);
  m_messageQueueVideo.SetMaxTimeSize(tenthsSeconds / 10.0);
  // matches the value in CVideoPlayerAudio
  m_messageQueueAudio.SetMaxDataSize(18 * (tenthsSeconds / 10.0) / 8 * 1024 * 1024);
  m_messageQueueVideo.SetMaxDataSize(sizeMB * 1024 * 1024);

  m_picture.Reset();
  m_picture.videoBuffer = new CTizenVideoBuffer();
}

CMediaPipelineTizen::~CMediaPipelineTizen()
{
  Unload();
}

bool CMediaPipelineTizen::Available()
{
  static const bool available = tizen_emss_available() != 0;
  return available;
}

bool CMediaPipelineTizen::SupportsVideo(const AVCodecID codec)
{
  switch (codec)
  {
    case AV_CODEC_ID_H264:
    case AV_CODEC_ID_HEVC:
    case AV_CODEC_ID_VP8:
    case AV_CODEC_ID_VP9:
    case AV_CODEC_ID_AV1:
      return true;
    default:
      return false;
  }
}

bool CMediaPipelineTizen::SupportsAudio(const AVCodecID codec)
{
  return AudioMime(codec) != nullptr;
}

void CMediaPipelineTizen::FlushVideoMessages()
{
  m_processInfo.SetLevelVQ(0);
  m_messageQueueVideo.Flush();
}

void CMediaPipelineTizen::FlushAudioMessages()
{
  m_messageQueueAudio.Flush();
}

bool CMediaPipelineTizen::OpenAudioStream(CDVDStreamInfo hint)
{
  const bool changed = m_audioHint != hint;
  m_audioHint = hint;
  m_hasAudio = true;
  m_audioClosed = false;

  if (m_loaded)
  {
    if (!changed)
      return true;

    // The TV cannot reconfigure a track of a source that is open.
    Unload();
  }

  if (m_videoHint.codec != AV_CODEC_ID_NONE)
    return Load();
  return true;
}

bool CMediaPipelineTizen::OpenVideoStream(CDVDStreamInfo hint)
{
  if (!SupportsVideo(hint.codec))
  {
    CLog::LogF(LOGERROR, "Unsupported codec: {}", hint.codec);
    return false;
  }

  const bool changed = m_videoHint != hint;
  m_videoHint = hint;
  m_videoClosed = false;

  if (m_loaded)
  {
    if (!changed)
      return true;

    Unload();
  }

  if (m_audioHint.codec != AV_CODEC_ID_NONE || !m_hasAudio)
    return Load();
  return true;
}

void CMediaPipelineTizen::CloseAudioStream(const bool waitForBuffers)
{
  m_audioClosed = true;
  if (m_videoClosed)
  {
    Unload();
    m_audioHint = CDVDStreamInfo();
    m_videoHint = CDVDStreamInfo();
  }
}

void CMediaPipelineTizen::CloseVideoStream(const bool waitForBuffers)
{
  m_videoClosed = true;
  if (m_audioClosed)
  {
    Unload();
    m_audioHint = CDVDStreamInfo();
    m_videoHint = CDVDStreamInfo();
  }
}

void CMediaPipelineTizen::Flush(const bool sync)
{
  std::scoped_lock lock(m_videoFeedMutex, m_audioFeedMutex);

  FlushAudioMessages();
  FlushVideoMessages();
  m_fedAudioPts = -1.0;
  m_fedVideoPts = -1.0;
  m_restarting = false;
  m_drainAudio = false;
  m_flushed = true;
}

bool CMediaPipelineTizen::AcceptsAudioData() const
{
  return !m_messageQueueAudio.IsFull();
}

bool CMediaPipelineTizen::AcceptsVideoData() const
{
  return !m_messageQueueVideo.IsFull();
}

bool CMediaPipelineTizen::HasAudioData() const
{
  return m_loaded && !m_state.ended && m_fedAudioPts > m_state.currentTime + 0.1;
}

bool CMediaPipelineTizen::HasVideoData() const
{
  return m_loaded && !m_state.ended && m_fedVideoPts > m_state.currentTime + 0.1;
}

bool CMediaPipelineTizen::IsAudioInited() const
{
  return m_messageQueueAudio.IsInited();
}

bool CMediaPipelineTizen::IsVideoInited() const
{
  return m_messageQueueVideo.IsInited();
}

int CMediaPipelineTizen::GetAudioLevel() const
{
  if (!m_loaded)
    return 0;

  const double ahead = m_fedAudioPts - m_state.currentTime;
  return std::clamp(static_cast<int>(ahead / FULL_AUDIO_BUFFER * 100.0), 0, 99);
}

void CMediaPipelineTizen::SendAudioMessage(const std::shared_ptr<CDVDMsg>& msg, const int priority)
{
  m_messageQueueAudio.Put(msg, priority);
}

void CMediaPipelineTizen::SendVideoMessage(const std::shared_ptr<CDVDMsg>& msg, const int priority)
{
  m_messageQueueVideo.Put(msg, priority);
  m_processInfo.SetLevelVQ(m_messageQueueVideo.GetLevel());
}

void CMediaPipelineTizen::SetSpeed(const int speed)
{
  if (!m_loaded)
    return;

  if (speed == DVD_PLAYSPEED_PAUSE)
  {
    tizen_emss_pause();
    return;
  }

  tizen_emss_set_rate(static_cast<double>(speed) / DVD_PLAYSPEED_NORMAL);
  tizen_emss_play();
}

double CMediaPipelineTizen::GetCurrentPts() const
{
  return m_state.currentTime * DVD_TIME_BASE;
}

std::string CMediaPipelineTizen::GetAudioInfo() const
{
  return fmt::format("aq:{:2}%, fed:{:.3f}s", m_messageQueueAudio.GetLevel(),
                     m_fedAudioPts.load());
}

std::string CMediaPipelineTizen::GetVideoInfo() const
{
  return fmt::format("vq:{:2}%, fed:{:.3f}s, pos:{:.3f}s", m_messageQueueVideo.GetLevel(),
                     m_fedVideoPts.load(), m_state.currentTime.load());
}

void CMediaPipelineTizen::GetVideoResolution(unsigned int& width, unsigned int& height) const
{
  width = m_videoHint.width;
  height = m_videoHint.height;
}

bool CMediaPipelineTizen::Load()
{
  std::scoped_lock lock(m_videoFeedMutex, m_audioFeedMutex);

  if (!m_videoHint.width || !m_videoHint.height)
  {
    CLog::LogF(LOGERROR, "null size, cannot handle");
    return false;
  }

  int fpsRate = m_videoHint.fpsrate;
  int fpsScale = m_videoHint.fpsscale;
  if (fpsRate <= 0 || fpsScale <= 0)
  {
    fpsRate = 25;
    fpsScale = 1;
  }
  const double fps = static_cast<double>(fpsRate) / fpsScale;
  m_frameDuration = 1.0 / fps;

  const std::string videoMime = VideoMime(m_videoHint);
  if (tizen_emss_validate_video(videoMime.c_str(), m_videoHint.width, m_videoHint.height, fpsRate,
                                fpsScale) == 0)
  {
    CLog::LogF(LOGERROR, "The TV rejects {} at {}x{}@{:.3f}", videoMime, m_videoHint.width,
               m_videoHint.height, fps);
    return false;
  }

  const char* audioMime = m_hasAudio ? AudioMime(m_audioHint.codec) : nullptr;
  const char* sampleFormat =
      m_audioHint.codec == AV_CODEC_ID_MP3 ? "SampleFormatPlanarS16" : "SampleFormatPlanarF32";
  const char* channelLayout = ChannelLayout(m_audioHint.channels);
  if (audioMime && tizen_emss_validate_audio(audioMime, sampleFormat, channelLayout,
                                             m_audioHint.samplerate) == 0)
    audioMime = nullptr;
  if (m_hasAudio && m_audioHint.codec != AV_CODEC_ID_NONE && !audioMime)
    CLog::LogF(LOGWARNING, "The TV cannot decode audio codec {}, playing without sound",
               avcodec_get_name(m_audioHint.codec));

  const int64_t maxTime = m_processInfo.GetMaxTime();

  TizenEmssConfig config{};
  config.duration = maxTime > 0 ? maxTime / 1000.0 : UNKNOWN_DURATION;
  config.videoMime = videoMime.c_str();
  config.videoExtradata = m_videoHint.extradata.GetData();
  config.videoExtradataSize = static_cast<int32_t>(m_videoHint.extradata.GetSize());
  config.width = m_videoHint.width;
  config.height = m_videoHint.height;
  config.framerateNum = fpsRate;
  config.framerateDen = fpsScale;
  config.hasAudio = audioMime ? 1 : 0;
  config.audioMime = audioMime;
  config.audioExtradata = m_audioHint.extradata.GetData();
  config.audioExtradataSize = static_cast<int32_t>(m_audioHint.extradata.GetSize());
  config.sampleFormat = sampleFormat;
  config.channelLayout = channelLayout;
  config.sampleRate = m_audioHint.samplerate;

  CLog::LogF(LOGDEBUG, "video {} {}x{}@{:.3f} extradata {}, audio {}, duration {:.1f}s", videoMime,
             m_videoHint.width, m_videoHint.height, fps, config.videoExtradataSize,
             audioMime ? audioMime : "none", config.duration);

  if (!tizen_emss_open(&m_state, &config))
  {
    CLog::LogF(LOGERROR, "The Tizen WASM Player did not open");
    m_messageQueueParent.Put(std::make_shared<CDVDMsg>(CDVDMsg::PLAYER_ABORT));
    return false;
  }

  m_clock.UpdateFramerate(fps);
  m_picture.iDuration = 1000.0 / fps;
  m_picture.iWidth = m_videoHint.width;
  m_picture.iHeight = m_videoHint.height;
  m_picture.iDisplayWidth = m_videoHint.width;
  m_picture.iDisplayHeight = m_videoHint.height;
  m_picture.stereoMode = m_videoHint.stereo_mode;
  m_picture.hdrType = m_videoHint.hdrType;
  m_picture.color_transfer = m_videoHint.colorTransferCharacteristic;

  if (m_videoHint.forced_aspect && m_videoHint.aspect > 0.0)
  {
    m_picture.iDisplayWidth = std::lround(m_picture.iDisplayHeight * m_videoHint.aspect);
    if (m_picture.iDisplayWidth > m_picture.iWidth)
    {
      m_picture.iDisplayWidth = m_picture.iWidth;
      m_picture.iDisplayHeight = std::lround(m_picture.iDisplayWidth / m_videoHint.aspect);
    }
  }

  if (!m_renderManager.Configure(m_picture, static_cast<float>(fps), m_videoHint.orientation))
  {
    CLog::LogF(LOGERROR, "RenderManager configure failed");
    tizen_emss_close();
    m_messageQueueParent.Put(std::make_shared<CDVDMsg>(CDVDMsg::PLAYER_ABORT));
    return false;
  }

  m_processInfo.SetVideoDecoderName(
      fmt::format("tizen-{}", avcodec_get_name(m_videoHint.codec)), true);
  m_processInfo.SetVideoPixelFormat("Surface");
  m_processInfo.SetVideoDimensions(m_videoHint.width, m_videoHint.height);
  m_processInfo.SetVideoInterlaced(m_videoHint.interlaced);
  m_processInfo.SetVideoDeintMethod("hardware");
  m_processInfo.SetVideoDAR(static_cast<float>(m_videoHint.aspect));
  m_processInfo.SetVideoFps(static_cast<float>(fps));
  if (audioMime)
  {
    if (m_audioHint.channellayout)
      m_processInfo.SetAudioChannels(CAEUtil::GetAEChannelLayout(m_audioHint.channellayout));
    else
      m_processInfo.SetAudioChannels(CAEUtil::GuessChLayout(m_audioHint.channels));
    m_processInfo.SetAudioSampleRate(m_audioHint.samplerate);
    m_processInfo.SetAudioBitsPerSample(m_audioHint.bitspersample);
    m_processInfo.SetAudioDecoderName(
        fmt::format("tizen-{}", avcodec_get_name(m_audioHint.codec)));
  }

  m_hasAudioTrack = audioMime != nullptr;
  m_fedAudioPts = -1.0;
  m_fedVideoPts = -1.0;
  m_shownPosition = -1.0;
  m_restarting = false;
  m_played = false;
  m_flushed = true;
  m_loaded = true;

  tizen_emss_play();
  Create();
  m_audioThread = std::thread([this] { ProcessAudio(); });
  m_renderManager.ShowVideo(true);
  return true;
}

void CMediaPipelineTizen::Unload()
{
  if (!m_loaded)
    return;

  StopThread(true);
  if (m_audioThread.joinable())
    m_audioThread.join();

  tizen_emss_close();
  m_loaded = false;
}

void CMediaPipelineTizen::ReportStarted() const
{
  SStartMsg msg{.timestamp = GetCurrentPts(),
                .player = VideoPlayer_VIDEO,
                .cachetime = DVD_MSEC_TO_TIME(50),
                .cachetotal = DVD_MSEC_TO_TIME(100)};
  m_messageQueueParent.Put(std::make_shared<CDVDMsgType<SStartMsg>>(CDVDMsg::PLAYER_STARTED, msg));
  msg.player = VideoPlayer_AUDIO;
  m_messageQueueParent.Put(std::make_shared<CDVDMsgType<SStartMsg>>(CDVDMsg::PLAYER_STARTED, msg));
}

bool CMediaPipelineTizen::Restart(const double pts)
{
  if (m_flushed)
  {
    // Playback that begins at the start of the timeline needs no repositioning.
    if (m_played || pts > 1.0)
    {
      if (!tizen_emss_seek(pts))
        CLog::LogF(LOGERROR, "Seek to {:.3f}s failed", pts);
      m_restarting = true;
      m_restartWait = 0;
    }
    else
      m_state.currentTime = pts;

    m_fedAudioPts = -1.0;
    m_fedVideoPts = -1.0;
    m_shownPosition = -1.0;
    m_played = true;
    m_flushed = false;
    ReportStarted();
  }

  if (m_restarting)
  {
    if (!m_state.videoOpen)
    {
      if (++m_restartWait == RESTART_TIMEOUT_POLLS)
      {
        CLog::LogF(LOGERROR, "The TV did not reopen its tracks after a seek");
        m_messageQueueParent.Put(std::make_shared<CDVDMsg>(CDVDMsg::PLAYER_ABORT));
      }
      return false;
    }
    m_restarting = false;
  }

  return true;
}

bool CMediaPipelineTizen::Append(const int trackId,
                                 const DemuxPacket& packet,
                                 const double pts,
                                 const double duration,
                                 const bool keyFrame)
{
  const double dts = packet.dts == DVD_NOPTS_VALUE ? pts : ToSeconds(packet.dts);

  switch (tizen_emss_append(trackId, packet.pData, packet.iSize, pts, dts, duration,
                            keyFrame ? 1 : 0, m_state.sessionId))
  {
    case TIZEN_EMSS_APPEND_BUFFER_FULL:
    case TIZEN_EMSS_APPEND_TRACK_CLOSED:
      return false;
    case TIZEN_EMSS_APPEND_FATAL:
      CLog::LogF(LOGWARNING, "The TV refused a packet of track {} at {:.3f}s", trackId, pts);
      return true;
    default:
      return true;
  }
}

bool CMediaPipelineTizen::FeedVideoData(const std::shared_ptr<CDVDMsg>& msg)
{
  std::unique_lock lock(m_videoFeedMutex);

  const DemuxPacket* packet = std::static_pointer_cast<CDVDMsgDemuxerPacket>(msg)->GetPacket();
  if (!packet->pData || packet->iSize <= 0)
    return true;

  double dvdPts = packet->pts;
  if (dvdPts == DVD_NOPTS_VALUE || m_videoHint.ptsinvalid)
    dvdPts = packet->dts;
  if (dvdPts == DVD_NOPTS_VALUE || dvdPts < 0.0)
    return true;

  const double pts = ToSeconds(dvdPts);
  if (!Restart(pts) || !m_state.videoOpen)
    return false;

  const double duration = packet->duration > 0.0 ? ToSeconds(packet->duration) : m_frameDuration;
  if (!Append(m_state.videoTrack, *packet, pts, duration, packet->m_keyFrame))
    return false;

  m_fedVideoPts = pts;
  return true;
}

bool CMediaPipelineTizen::FeedAudioData(const std::shared_ptr<CDVDMsg>& msg)
{
  std::unique_lock lock(m_audioFeedMutex);

  const DemuxPacket* packet = std::static_pointer_cast<CDVDMsgDemuxerPacket>(msg)->GetPacket();
  if (!m_hasAudioTrack || !packet->pData || packet->iSize <= 0 ||
      packet->pts == DVD_NOPTS_VALUE || packet->pts < 0.0)
    return true;

  // The video thread positions the TV first.
  if (m_flushed || m_restarting || !m_state.audioOpen)
    return false;

  const double pts = ToSeconds(packet->pts);
  const double duration = packet->duration > 0.0 ? ToSeconds(packet->duration) : 0.02;
  if (!Append(m_state.audioTrack, *packet, pts, duration, true))
    return false;

  m_fedAudioPts = pts;
  return true;
}

void CMediaPipelineTizen::UpdatePosition()
{
  if (m_flushed || m_restarting)
    return;

  if (m_state.error.exchange(0))
  {
    CLog::LogF(LOGERROR, "The Tizen WASM Player reported an error");
    m_messageQueueParent.Put(std::make_shared<CDVDMsg>(CDVDMsg::PLAYER_ABORT));
    return;
  }

  const double position = m_state.currentTime;
  if (position == m_shownPosition)
    return;
  m_shownPosition = position;

  const double pts = position * DVD_TIME_BASE;
  ProcessOverlays(pts);
  m_picture.dts = pts;
  m_picture.pts = pts;
  std::atomic<bool> stop(false);
  m_renderManager.AddVideoPicture(m_picture, stop, VS_INTERLACEMETHOD_AUTO, false);
  m_clock.Discontinuity(pts);
}

void CMediaPipelineTizen::ProcessOverlays(const double pts) const
{
  // remove any overlays that are out of time
  m_overlayContainer.CleanUp(pts - m_subtitleDelay);

  std::vector<std::shared_ptr<CDVDOverlay>> overlays;
  std::scoped_lock<CCriticalSection> lock(m_overlayContainer);

  for (const std::shared_ptr<CDVDOverlay>& overlay : *m_overlayContainer.GetOverlays())
  {
    if (!overlay->bForced && !m_subtitle)
      continue;

    const double pts2 = overlay->bForced ? pts : pts - m_subtitleDelay;

    if (overlay->iPTSStartTime <= pts2 &&
        (overlay->iPTSStopTime > pts2 || overlay->iPTSStopTime == 0LL))
    {
      if (overlay->IsOverlayType(DVDOVERLAY_TYPE_GROUP))
        overlays.insert(overlays.end(), static_cast<CDVDOverlayGroup&>(*overlay).m_overlays.begin(),
                        static_cast<CDVDOverlayGroup&>(*overlay).m_overlays.end());
      else
        overlays.push_back(overlay);
    }
  }

  for (const std::shared_ptr<CDVDOverlay>& overlay : overlays)
  {
    const double pts2 = overlay->bForced ? pts : pts - m_subtitleDelay;
    m_renderManager.AddOverlay(overlay, pts2);
  }
}

void CMediaPipelineTizen::Process()
{
  while (!m_bStop)
  {
    UpdatePosition();

    std::shared_ptr<CDVDMsg> msg;
    int priority = 0;
    m_messageQueueVideo.Get(msg, 10ms, priority);
    if (!msg)
      continue;

    if (msg->IsType(CDVDMsg::DEMUXER_PACKET))
    {
      if (!FeedVideoData(msg))
      {
        m_messageQueueVideo.PutBack(msg);
        std::this_thread::sleep_for(10ms);
      }
    }
    else if (msg->IsType(CDVDMsg::PLAYER_REQUEST_STATE))
    {
      constexpr SStateMsg stateMsg{.syncState = IDVDStreamPlayer::SYNC_INSYNC,
                                   .player = VideoPlayer_VIDEO};
      m_messageQueueParent.Put(
          std::make_shared<CDVDMsgType<SStateMsg>>(CDVDMsg::PLAYER_REPORT_STATE, stateMsg));
    }
    else if (msg->IsType(CDVDMsg::VIDEO_DRAIN))
    {
      tizen_emss_end_of_track(m_state.videoTrack, m_state.sessionId);
      m_drainAudio = true;
    }
    else if (msg->IsType(CDVDMsg::GENERAL_SYNCHRONIZE))
    {
      if (!std::static_pointer_cast<CDVDMsgGeneralSynchronize>(msg)->Wait(100ms, SYNCSOURCE_VIDEO))
        // push back as prio message, to process other prio messages
        m_messageQueueVideo.Put(msg, 1);
    }
  }
}

void CMediaPipelineTizen::ProcessAudio()
{
  while (!m_bStop)
  {
    std::shared_ptr<CDVDMsg> msg;
    int priority = 0;
    m_messageQueueAudio.Get(msg, 10ms, priority);
    if (!msg)
    {
      // The audio track ends once the packets that were still queued have reached the TV.
      if (m_drainAudio.exchange(false) && m_hasAudioTrack)
        tizen_emss_end_of_track(m_state.audioTrack, m_state.sessionId);
      continue;
    }

    if (msg->IsType(CDVDMsg::DEMUXER_PACKET))
    {
      if (!FeedAudioData(msg))
      {
        m_messageQueueAudio.PutBack(msg);
        std::this_thread::sleep_for(10ms);
      }
    }
    else if (msg->IsType(CDVDMsg::PLAYER_REQUEST_STATE))
    {
      constexpr SStateMsg stateMsg{.syncState = IDVDStreamPlayer::SYNC_INSYNC,
                                   .player = VideoPlayer_AUDIO};
      m_messageQueueParent.Put(
          std::make_shared<CDVDMsgType<SStateMsg>>(CDVDMsg::PLAYER_REPORT_STATE, stateMsg));
    }
    else if (msg->IsType(CDVDMsg::GENERAL_SYNCHRONIZE))
    {
      if (!std::static_pointer_cast<CDVDMsgGeneralSynchronize>(msg)->Wait(100ms, SYNCSOURCE_AUDIO))
        // push back as prio message, to process other prio messages
        m_messageQueueAudio.Put(msg, 1);
    }
  }
}
