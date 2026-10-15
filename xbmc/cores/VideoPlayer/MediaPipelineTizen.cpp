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
#include "DVDMessage.h"
#include "DVDCodecs/Overlay/DVDOverlay.h"
#include "DVDOverlayContainer.h"
#include "Interface/DemuxCrypto.h"
#include "Interface/DemuxPacket.h"
#include "Interface/TimingConstants.h"
#include "Process/ProcessInfo.h"
#include "ServiceBroker.h"
#include "VideoRenderers/RenderManager.h"
#include "platform/wasm/tizen/TizenDRM.h"
#include "cores/AudioEngine/Utils/AEUtil.h"
#include "settings/Settings.h"
#include "settings/SettingsComponent.h"
#include "utils/BitstreamConverter.h"
#include "utils/StringUtils.h"
#include "utils/log.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <vector>

#include <fmt/format.h>

extern "C"
{
#include <libavcodec/avcodec.h>
#include <libavcodec/defs.h>
#include <libavutil/channel_layout.h>
#include <libswresample/swresample.h>
}

using namespace std::chrono_literals;

namespace
{
// Timeline length given to the TV when the duration of the stream is not known.
constexpr double UNKNOWN_DURATION = 24.0 * 60.0 * 60.0;

// Polls of 10ms the TV is given to reopen its tracks after a seek.
constexpr int RESTART_TIMEOUT_POLLS = 500;

// Adaptive streams start small; the TV is told to expect at least this much.
constexpr int MIN_CONFIG_WIDTH = 1920;
constexpr int MIN_CONFIG_HEIGHT = 1080;

// Seconds of audio ahead of the playback position that count as a full buffer.
constexpr double FULL_AUDIO_BUFFER = 2.0;

// Audio the TV cannot decode is handed to it in this form.
constexpr const char* PCM_MIME = "audio/webm; codecs=\"pcm\"";
constexpr const char* PCM_SAMPLE_FORMAT = "SampleFormatS16";
constexpr int PCM_MAX_CHANNELS = 8;
constexpr int PCM_FALLBACK_RATE = 48000;

// Video packets held at most at the start while their decode times are not known.
constexpr size_t MAX_HELD_VIDEO = 16;

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

/*!
 * \brief Whether a packet can be decoded on its own.
 *
 * Only the FFmpeg demuxer says so itself; the TV refuses to start on anything else.
 * H.264 and HEVC are expected in Annex B.
 */
bool IsKeyFrame(AVCodecID codec, const uint8_t* data, int size)
{
  switch (codec)
  {
    case AV_CODEC_ID_H264:
    case AV_CODEC_ID_HEVC:
    {
      for (int i = 0; i + 3 < size; ++i)
      {
        if (data[i] != 0 || data[i + 1] != 0 || data[i + 2] != 1)
          continue;

        const uint8_t header = data[i + 3];
        if (codec == AV_CODEC_ID_H264)
        {
          const int type = header & 0x1f;
          if (type == 5) // IDR slice
            return true;
          if (type == 1) // any other slice
            return false;
        }
        else
        {
          const int type = (header >> 1) & 0x3f;
          if (type >= 16 && type <= 23) // IRAP picture
            return true;
          if (type < 16) // any other slice
            return false;
        }
        i += 3;
      }
      return false;
    }
    case AV_CODEC_ID_VP8:
      return size > 0 && (data[0] & 0x01) == 0;
    case AV_CODEC_ID_VP9:
    {
      if (size < 1)
        return false;
      // frame_marker(2) profile(2) [reserved(1) for profile 3] show_existing_frame(1) frame_type(1)
      const bool profile3 = (data[0] & 0x30) == 0x30;
      const int showExisting = profile3 ? (data[0] >> 2) & 1 : (data[0] >> 3) & 1;
      const int frameType = profile3 ? (data[0] >> 1) & 1 : (data[0] >> 2) & 1;
      return !showExisting && frameType == 0;
    }
    case AV_CODEC_ID_AV1:
    {
      // A key frame comes with a sequence header OBU (type 1).
      int pos = 0;
      while (pos < size)
      {
        const uint8_t header = data[pos];
        const int type = (header >> 3) & 0x0f;
        const bool extension = header & 0x04;
        const bool hasSize = header & 0x02;
        if (type == 1)
          return true;
        if (!hasSize)
          return false;

        pos += extension ? 2 : 1;
        int obuSize = 0;
        for (int shift = 0; pos < size && shift < 35; shift += 7)
        {
          const uint8_t byte = data[pos++];
          obuSize |= (byte & 0x7f) << shift;
          if (!(byte & 0x80))
            break;
        }
        pos += obuSize;
      }
      return false;
    }
    default:
      return true;
  }
}

/*!
 * \brief Handle of the tizen_drm.js session a stream is encrypted for, 0 if it is not.
 *
 * InputStream Adaptive's decrypter passes the handle as its session id.
 */
int DrmSession(const CDVDStreamInfo& hint)
{
  return hint.cryptoSession ? atoi(hint.cryptoSession->sessionId.c_str()) : 0;
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
  CloseAudioDecoder();
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
  const bool sameCodec = m_videoHint.codec == hint.codec;
  m_videoClosed = false;

  if (m_loaded)
  {
    if (!changed)
      return true;

    // A change of quality that stays within what the TV was told to expect.
    if (sameCodec && hint.width <= m_configWidth && hint.height <= m_configHeight)
    {
      std::unique_lock lock(m_videoFeedMutex);
      m_videoHint = hint;
      m_frameInfo.width = hint.width;
      m_frameInfo.height = hint.height;
      SetupBitstreamConverter();
      m_processInfo.SetVideoDimensions(hint.width, hint.height);
      return true;
    }

    Unload();
  }

  m_videoHint = hint;
  if (m_audioHint.codec != AV_CODEC_ID_NONE || !m_hasAudio)
    return Load();
  return true;
}

void CMediaPipelineTizen::SetupBitstreamConverter()
{
  m_bitstream.reset();
  m_convertedPacket = nullptr;

  if (m_videoHint.codec != AV_CODEC_ID_H264 && m_videoHint.codec != AV_CODEC_ID_HEVC)
    return;

  // The offsets of the encrypted parts of a packet do not survive a conversion;
  // InputStream Adaptive delivers encrypted streams in Annex B itself.
  if (m_videoHint.cryptoSession)
    return;

  // Only a stream with an avcC/hvcC record is length-prefixed.
  if (m_videoHint.extradata.GetSize() < 7 || m_videoHint.extradata.GetData()[0] != 1)
    return;

  auto converter = std::make_unique<CBitstreamConverter>();
  if (converter->Open(m_videoHint.codec, m_videoHint.extradata.GetData(),
                      static_cast<int>(m_videoHint.extradata.GetSize()), true))
    m_bitstream = std::move(converter);
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
  if (m_reloadAudio.exchange(false) && m_loaded)
  {
    // Requested by FeedAudioData() to decode the audio elsewhere; the TV cannot
    // reconfigure a track of a source that is open. The packets from before the
    // seek are dropped below as with any flush.
    Unload();
    Load();
  }

  std::scoped_lock lock(m_videoFeedMutex, m_audioFeedMutex);

  FlushAudioMessages();
  FlushVideoMessages();
  m_fedAudioPts = -1.0;
  m_fedVideoPts = -1.0;
  m_lastVideoDts = DVD_NOPTS_VALUE;
  m_heldVideo.clear();
  m_videoDtsKnown = false;
  m_restarting = false;
  m_drainAudio = false;
  m_flushed = true;

  m_pcmPacket = nullptr;
  if (m_audioDecoder)
    avcodec_flush_buffers(m_audioDecoder);
  if (m_resampler)
    swr_close(m_resampler);
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

  // A live stream may be opened before its size is known. The TV reads it from the
  // stream, so it only has to be prepared for a large enough picture.
  const bool sizeKnown = m_videoHint.width > 0 && m_videoHint.height > 0;
  if (!sizeKnown)
  {
    CLog::LogF(LOGINFO, "Video size not known yet, preparing the TV for {}x{}",
               MIN_CONFIG_WIDTH, MIN_CONFIG_HEIGHT);
    m_videoHint.width = MIN_CONFIG_WIDTH;
    m_videoHint.height = MIN_CONFIG_HEIGHT;
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
  // Packets only tell the TV a size that is known.
  m_frameInfo = {sizeKnown ? m_videoHint.width : 0, sizeKnown ? m_videoHint.height : 0,
                 fpsRate, fpsScale};

  const std::string videoMime = VideoMime(m_videoHint);
  m_configWidth = std::max(m_videoHint.width, MIN_CONFIG_WIDTH);
  m_configHeight = std::max(m_videoHint.height, MIN_CONFIG_HEIGHT);
  if (tizen_emss_validate_video(videoMime.c_str(), m_configWidth, m_configHeight, fpsRate,
                                fpsScale) == 0)
  {
    m_configWidth = m_videoHint.width;
    m_configHeight = m_videoHint.height;
  }
  if (tizen_emss_validate_video(videoMime.c_str(), m_configWidth, m_configHeight, fpsRate,
                                fpsScale) == 0)
  {
    CLog::LogF(LOGERROR, "The TV rejects {} at {}x{}@{:.3f}", videoMime, m_videoHint.width,
               m_videoHint.height, fps);
    return false;
  }

  const char* audioMime = m_hasAudio ? AudioMime(m_audioHint.codec) : nullptr;
  const char* sampleFormat =
      m_audioHint.codec == AV_CODEC_ID_MP3 ? "SampleFormatPlanarS16" : "SampleFormatPlanarF32";
  // Like the video size, the audio format of a live stream may not be known yet. The
  // TV's decoders read it from the stream; audio decoded here is converted to it.
  if (m_hasAudio && m_audioHint.codec != AV_CODEC_ID_NONE &&
      (m_audioHint.channels <= 0 || m_audioHint.samplerate <= 0))
  {
    CLog::LogF(LOGINFO, "Audio format not known yet, assuming stereo at {} Hz",
               PCM_FALLBACK_RATE);
    if (m_audioHint.channels <= 0)
      m_audioHint.channels = 2;
    if (m_audioHint.samplerate <= 0)
      m_audioHint.samplerate = PCM_FALLBACK_RATE;
  }
  const char* channelLayout = ChannelLayout(m_audioHint.channels);
  int sampleRate = m_audioHint.samplerate;
  if (audioMime &&
      tizen_emss_validate_audio(audioMime, sampleFormat, channelLayout, sampleRate) == 0)
    audioMime = nullptr;

  // Audio the TV can decode is decoded here as well while its settings adjust it.
  m_audioNative = audioMime != nullptr && !DrmSession(m_audioHint);
  {
    const CVideoSettings settings = m_processInfo.GetVideoSettings();
    CLog::LogF(LOGDEBUG, "Audio amplification {:.1f} dB, centre level {} dB",
               settings.m_VolumeAmplification, settings.m_CenterMixLevel);
  }
  CloseAudioDecoder();
  if (m_hasAudio && m_audioHint.codec != AV_CODEC_ID_NONE && (!audioMime || AdjustsAudio()) &&
      !DrmSession(m_audioHint))
  {
    if (OpenAudioDecoder())
    {
      channelLayout = ChannelLayout(m_pcmChannels);
      if (tizen_emss_validate_audio(PCM_MIME, PCM_SAMPLE_FORMAT, channelLayout, m_pcmRate) == 0)
        m_pcmRate = PCM_FALLBACK_RATE;
      if (tizen_emss_validate_audio(PCM_MIME, PCM_SAMPLE_FORMAT, channelLayout, m_pcmRate) != 0)
      {
        audioMime = PCM_MIME;
        sampleFormat = PCM_SAMPLE_FORMAT;
        sampleRate = m_pcmRate;
      }
      else
        CloseAudioDecoder();
    }
  }
  m_audioAdjustable = !m_audioNative || m_audioDecoder || !AdjustsAudio();
  if (m_hasAudio && m_audioHint.codec != AV_CODEC_ID_NONE && !audioMime)
    CLog::LogF(LOGWARNING, "The TV cannot decode audio codec {}, playing without sound",
               avcodec_get_name(m_audioHint.codec));

  const int64_t maxTime = m_processInfo.GetMaxTime();

  TizenEmssConfig config{};
  config.duration = maxTime > 0 ? maxTime / 1000.0 : UNKNOWN_DURATION;
  config.videoMime = videoMime.c_str();
  SetupBitstreamConverter();
  if (m_bitstream)
  {
    config.videoExtradata = m_bitstream->GetExtraData();
    config.videoExtradataSize = m_bitstream->GetExtraSize();
  }
  else
  {
    config.videoExtradata = m_videoHint.extradata.GetData();
    config.videoExtradataSize = static_cast<int32_t>(m_videoHint.extradata.GetSize());
  }
  config.width = m_configWidth;
  config.height = m_configHeight;
  config.framerateNum = fpsRate;
  config.framerateDen = fpsScale;
  config.hasAudio = audioMime ? 1 : 0;
  config.audioMime = audioMime;
  if (!m_audioDecoder)
  {
    config.audioExtradata = m_audioHint.extradata.GetData();
    config.audioExtradataSize = static_cast<int32_t>(m_audioHint.extradata.GetSize());
  }
  config.sampleFormat = sampleFormat;
  config.channelLayout = channelLayout;
  config.sampleRate = sampleRate;

  CLog::LogF(LOGDEBUG, "video {} {}x{}@{:.3f} extradata {}, audio {}, duration {:.1f}s", videoMime,
             m_videoHint.width, m_videoHint.height, fps, config.videoExtradataSize,
             audioMime ? audioMime : "none", config.duration);

  tizen_emss_set_media_keys(DrmSession(m_videoHint), audioMime ? DrmSession(m_audioHint) : 0);

  m_audioClearLeadKey.clear();
  if (audioMime && DrmSession(m_audioHint))
  {
    m_audioClearLeadKey.resize(16);
    if (!tizen_drm_usable_key_id(DrmSession(m_audioHint), m_audioClearLeadKey.data()))
      m_audioClearLeadKey.clear();
  }
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
  if (m_audioDecoder)
  {
    m_processInfo.SetAudioChannels(CAEUtil::GuessChLayout(m_pcmChannels));
    m_processInfo.SetAudioSampleRate(m_pcmRate);
    m_processInfo.SetAudioBitsPerSample(16);
    m_processInfo.SetAudioDecoderName(
        fmt::format("ff-{} (tizen-pcm)", avcodec_get_name(m_audioHint.codec)));
  }
  else if (audioMime)
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
  m_lastVideoDts = DVD_NOPTS_VALUE;
  m_heldVideo.clear();
  m_videoDtsKnown = false;
  m_demuxerFlagsKeyFrames = false;
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
                                 const uint8_t* data,
                                 const int size,
                                 const double pts,
                                 const double dts,
                                 const bool keyFrame,
                                 double duration)
{
  if (duration <= 0.0)
    duration = packet.duration > 0.0 ? ToSeconds(packet.duration)
                                     : (trackId == m_state.videoTrack ? m_frameDuration : 0.02);

  const TizenEmssFrameInfo* frame = trackId == m_state.videoTrack ? &m_frameInfo : nullptr;

  int result;
  const DEMUX_CRYPTO_INFO* crypto = packet.cryptoInfo;
  if (crypto && crypto->numSubSamples > 0)
  {
    std::vector<uint32_t> subsamples;
    subsamples.reserve(2 * crypto->numSubSamples);
    for (unsigned int i = 0; i < crypto->numSubSamples; ++i)
    {
      subsamples.push_back(crypto->clearBytes[i]);
      subsamples.push_back(crypto->cipherBytes[i]);
    }
    // 2 is AES-CBC, the cbcs scheme; anything else here is AES-CTR, cenc.
    result = tizen_emss_append_encrypted(
        trackId, data, size, pts, dts, duration, keyFrame ? 1 : 0, m_state.sessionId, crypto->kid,
        crypto->iv, subsamples.data(), static_cast<int>(crypto->numSubSamples),
        crypto->mode == 2 ? 1 : 0, nullptr /* the TV stalls on encrypted video with it */);
  }
  else if (trackId == m_state.audioTrack && !m_audioClearLeadKey.empty())
  {
    // The audio decoder of the TV fails when encrypted packets follow clear ones, so the
    // clear lead of encrypted audio is passed as encrypted packets without encrypted
    // bytes. The key does not matter, but has to be one the session holds.
    static const uint8_t iv[16] = {};
    const uint32_t subsample[2] = {static_cast<uint32_t>(size), 0};
    result = tizen_emss_append_encrypted(trackId, data, size, pts, dts, duration,
                                         keyFrame ? 1 : 0, m_state.sessionId,
                                         m_audioClearLeadKey.data(), iv, subsample, 1, 0,
                                         nullptr);
  }
  else
    result = tizen_emss_append(trackId, data, size, pts, dts, duration, keyFrame ? 1 : 0,
                               m_state.sessionId, frame);

  switch (result)
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

  if (!AppendHeldVideo())
    return false;

  const double dts = packet->dts == DVD_NOPTS_VALUE ? DVD_NOPTS_VALUE : ToSeconds(packet->dts);
  if (!m_videoDtsKnown)
  {
    HoldVideo(msg, pts, dts);
    AppendHeldVideo();
    return true;
  }
  return AppendVideo(*packet, pts, dts);
}

void CMediaPipelineTizen::HoldVideo(const std::shared_ptr<CDVDMsg>& msg, double pts, double dts)
{
  // Ahead of B-frames, the first packets are decoded before the stream starts. Demuxers
  // other than the one for MPEG-TS raise these decode times to 0 or give none, and the
  // TV stops decoding after packets whose decode times do not rise. So the first
  // packets are held until the decode time rises, and theirs are counted back from it.
  double knownDts = DVD_NOPTS_VALUE;
  for (auto it = m_heldVideo.rbegin(); it != m_heldVideo.rend(); ++it)
  {
    if (it->dts != DVD_NOPTS_VALUE)
    {
      knownDts = it->dts;
      break;
    }
  }
  const bool rises = dts != DVD_NOPTS_VALUE && knownDts != DVD_NOPTS_VALUE && dts > knownDts;
  if (!rises && m_heldVideo.size() < MAX_HELD_VIDEO)
  {
    m_heldVideo.push_back({msg, pts, dts});
    return;
  }

  // Without a rise, the packets are given decode times a frame apart.
  const size_t count = m_heldVideo.size();
  const double first = m_heldVideo.empty() ? pts : m_heldVideo.front().pts;
  for (size_t i = 0; i < count; ++i)
    m_heldVideo[i].dts = rises ? dts - static_cast<double>(count - i) * m_frameDuration
                               : first + static_cast<double>(i) * m_frameDuration;
  m_heldVideo.push_back(
      {msg, pts, rises ? dts : first + static_cast<double>(count) * m_frameDuration});
  m_videoDtsKnown = true;
}

bool CMediaPipelineTizen::AppendHeldVideo()
{
  while (m_videoDtsKnown && !m_heldVideo.empty())
  {
    const HeldVideoPacket& held = m_heldVideo.front();
    const DemuxPacket* packet =
        std::static_pointer_cast<CDVDMsgDemuxerPacket>(held.msg)->GetPacket();
    if (!AppendVideo(*packet, held.pts, held.dts))
      return false;
    m_heldVideo.pop_front();
  }
  return true;
}

bool CMediaPipelineTizen::AppendVideo(const DemuxPacket& packet, double pts, double dts)
{
  const uint8_t* data = packet.pData;
  int size = packet.iSize;
  if (m_bitstream)
  {
    // A packet that the TV did not take yet is offered again as it was converted.
    if (m_convertedPacket != &packet)
    {
      if (!m_bitstream->Convert(packet.pData, packet.iSize))
        return true;
      m_convertedPacket = &packet;
    }
    data = m_bitstream->GetConvertBuffer();
    size = m_bitstream->GetConvertSize();
  }

  // The decode time has to rise and must not pass the presentation time.
  if (dts == DVD_NOPTS_VALUE)
    dts = pts;
  if (m_lastVideoDts != DVD_NOPTS_VALUE && dts <= m_lastVideoDts)
    dts = std::min(m_lastVideoDts + 0.001, pts);

  if (packet.m_keyFrame)
    m_demuxerFlagsKeyFrames = true;
  const bool keyFrame = m_demuxerFlagsKeyFrames ? packet.m_keyFrame
                                                : IsKeyFrame(m_videoHint.codec, data, size);
  if (!Append(m_state.videoTrack, packet, data, size, pts, dts, keyFrame))
    return false;

  m_convertedPacket = nullptr;
  m_lastVideoDts = dts;
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

  // Audio the TV can decode is only decoded here while the settings adjust it. Switching
  // sets the source up again, which a seek to the current position does.
  // A seek that VideoPlayer skipped is asked for again.
  const auto now = std::chrono::steady_clock::now();
  if (m_reloadAudio && now - m_reloadRequested > 3s)
    m_reloadAudio = false;
  if (m_audioNative && m_audioAdjustable && (m_audioDecoder != nullptr) != AdjustsAudio() &&
      !m_reloadAudio.exchange(true))
  {
    m_reloadRequested = now;
    CLog::LogF(LOGINFO, "Audio is now decoded by {}", m_audioDecoder ? "the TV" : "Kodi");
    CDVDMsgPlayerSeek::CMode mode;
    mode.time = m_state.currentTime * 1000.0;
    mode.accurate = true;
    mode.sync = true;
    mode.restore = false;
    m_messageQueueParent.Put(std::make_shared<CDVDMsgPlayerSeek>(mode));
  }

  const double pts = ToSeconds(packet->pts);
  if (m_audioDecoder)
  {
    // A packet that the TV did not take yet is offered again as it was decoded.
    if (m_pcmPacket != packet)
    {
      if (!DecodeAudio(*packet))
        return true;
      m_pcmPacket = packet;
    }
    if (m_pcmSamples > 0)
    {
      const double duration = static_cast<double>(m_pcmSamples) / m_pcmRate;
      if (!Append(m_state.audioTrack, *packet, m_pcm.data(), static_cast<int>(m_pcm.size()), pts,
                  pts, true, duration))
        return false;
    }
    m_pcmPacket = nullptr;
  }
  else if (!Append(m_state.audioTrack, *packet, packet->pData, packet->iSize, pts,
                   packet->dts == DVD_NOPTS_VALUE ? pts : ToSeconds(packet->dts), true))
    return false;

  m_fedAudioPts = pts;
  return true;
}

bool CMediaPipelineTizen::OpenAudioDecoder()
{
  const AVCodec* codec = avcodec_find_decoder(m_audioHint.codec);
  if (!codec)
    return false;

  m_audioDecoder = avcodec_alloc_context3(codec);
  m_audioFrame = av_frame_alloc();
  m_audioPacket = av_packet_alloc();
  if (!m_audioDecoder || !m_audioFrame || !m_audioPacket)
  {
    CloseAudioDecoder();
    return false;
  }

  m_audioDecoder->sample_rate = m_audioHint.samplerate;
  if (m_audioHint.channellayout)
    av_channel_layout_from_mask(&m_audioDecoder->ch_layout, m_audioHint.channellayout);
  else if (m_audioHint.channels > 0)
    av_channel_layout_default(&m_audioDecoder->ch_layout, m_audioHint.channels);
  m_audioDecoder->bit_rate = m_audioHint.bitrate;
  m_audioDecoder->block_align = m_audioHint.blockalign;
  m_audioDecoder->bits_per_coded_sample = m_audioHint.bitspersample;
  if (m_audioHint.extradata.GetSize() > 0)
  {
    const size_t size = m_audioHint.extradata.GetSize();
    m_audioDecoder->extradata =
        static_cast<uint8_t*>(av_mallocz(size + AV_INPUT_BUFFER_PADDING_SIZE));
    if (m_audioDecoder->extradata)
    {
      memcpy(m_audioDecoder->extradata, m_audioHint.extradata.GetData(), size);
      m_audioDecoder->extradata_size = static_cast<int>(size);
    }
  }

  if (avcodec_open2(m_audioDecoder, codec, nullptr) < 0)
  {
    CLog::LogF(LOGERROR, "Cannot open the {} decoder", codec->name);
    CloseAudioDecoder();
    return false;
  }

  const int channels = m_audioDecoder->ch_layout.nb_channels > 0
                           ? m_audioDecoder->ch_layout.nb_channels
                           : m_audioHint.channels;
  m_pcmChannels = std::clamp(channels, 1, PCM_MAX_CHANNELS);
  m_pcmRate = m_audioDecoder->sample_rate > 0 ? m_audioDecoder->sample_rate : PCM_FALLBACK_RATE;
  m_pcmCenter = -1;
  if (m_pcmChannels >= 3)
  {
    AVChannelLayout layout;
    av_channel_layout_default(&layout, m_pcmChannels);
    m_pcmCenter = av_channel_layout_index_from_channel(&layout, AV_CHAN_FRONT_CENTER);
    av_channel_layout_uninit(&layout);
  }
  m_pcmPacket = nullptr;
  CLog::LogF(LOGINFO, "Decoding {} for the TV, {} channels at {} Hz", codec->name, m_pcmChannels,
             m_pcmRate);
  return true;
}

void CMediaPipelineTizen::CloseAudioDecoder()
{
  swr_free(&m_resampler);
  av_packet_free(&m_audioPacket);
  av_frame_free(&m_audioFrame);
  avcodec_free_context(&m_audioDecoder);
  m_pcmPacket = nullptr;
  m_pcm.clear();
  m_pcmSamples = 0;
}

bool CMediaPipelineTizen::DecodeAudio(const DemuxPacket& packet)
{
  m_pcm.clear();
  m_pcmSamples = 0;

  // Demux packets come with the padding FFmpeg needs.
  m_audioPacket->data = packet.pData;
  m_audioPacket->size = packet.iSize;
  const int sent = avcodec_send_packet(m_audioDecoder, m_audioPacket);
  av_packet_unref(m_audioPacket);
  if (sent < 0)
    return false;

  while (avcodec_receive_frame(m_audioDecoder, m_audioFrame) == 0)
  {
    // The format of the decoded samples is only known now; the TV gets the
    // channel count and rate it was told about at the start.
    if (!m_resampler || !swr_is_initialized(m_resampler))
    {
      AVChannelLayout out;
      av_channel_layout_default(&out, m_pcmChannels);
      swr_free(&m_resampler);
      const int ret = swr_alloc_set_opts2(&m_resampler, &out, AV_SAMPLE_FMT_S16, m_pcmRate,
                                          &m_audioFrame->ch_layout,
                                          static_cast<AVSampleFormat>(m_audioFrame->format),
                                          m_audioFrame->sample_rate, 0, nullptr);
      av_channel_layout_uninit(&out);
      if (ret < 0 || swr_init(m_resampler) < 0)
      {
        CLog::LogF(LOGERROR, "Cannot convert {} audio to PCM",
                   avcodec_get_name(m_audioHint.codec));
        swr_free(&m_resampler);
        av_frame_unref(m_audioFrame);
        return false;
      }
    }

    const int capacity = swr_get_out_samples(m_resampler, m_audioFrame->nb_samples);
    const size_t frameBytes = static_cast<size_t>(m_pcmChannels) * 2;
    const size_t offset = m_pcm.size();
    m_pcm.resize(offset + static_cast<size_t>(std::max(capacity, 0)) * frameBytes);
    uint8_t* out = m_pcm.data() + offset;
    const int converted =
        swr_convert(m_resampler, &out, capacity,
                    const_cast<const uint8_t**>(m_audioFrame->extended_data),
                    m_audioFrame->nb_samples);
    av_frame_unref(m_audioFrame);
    if (converted < 0)
    {
      m_pcm.resize(offset);
      return false;
    }
    m_pcm.resize(offset + static_cast<size_t>(converted) * frameBytes);
    m_pcmSamples += converted;
  }
  AdjustAudio();
  return true;
}

bool CMediaPipelineTizen::AdjustsAudio() const
{
  const CVideoSettings settings = m_processInfo.GetVideoSettings();
  return settings.m_VolumeAmplification != 0.0f || settings.m_CenterMixLevel != 0;
}

void CMediaPipelineTizen::AdjustAudio()
{
  const CVideoSettings settings = m_processInfo.GetVideoSettings();
  const float gain = std::pow(10.0f, settings.m_VolumeAmplification / 20.0f);
  // Without a downmix here the centre level is applied to the centre channel itself,
  // which comes to the same when the TV downmixes.
  const float centerGain =
      m_pcmCenter >= 0 ? gain * std::pow(10.0f, settings.m_CenterMixLevel / 20.0f) : gain;
  if (gain == 1.0f && centerGain == 1.0f)
    return;

  auto* samples = reinterpret_cast<int16_t*>(m_pcm.data());
  const size_t count = m_pcm.size() / sizeof(int16_t);
  for (size_t i = 0; i < count; ++i)
  {
    const bool center = m_pcmCenter >= 0 && static_cast<int>(i % m_pcmChannels) == m_pcmCenter;
    const float value = samples[i] * (center ? centerGain : gain);
    samples[i] = static_cast<int16_t>(std::clamp(value, -32768.0f, 32767.0f));
  }
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

void CMediaPipelineTizen::ReleaseHeldVideo()
{
  // A stream that ends while its first packets are held.
  std::unique_lock lock(m_videoFeedMutex);
  if (!m_videoDtsKnown && !m_heldVideo.empty())
  {
    const double first = m_heldVideo.front().pts;
    for (size_t i = 0; i < m_heldVideo.size(); ++i)
      m_heldVideo[i].dts = first + static_cast<double>(i) * m_frameDuration;
    m_videoDtsKnown = true;
  }
  while (!m_bStop && !m_flushed && !AppendHeldVideo())
  {
    lock.unlock();
    std::this_thread::sleep_for(10ms);
    lock.lock();
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
      ReleaseHeldVideo();
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
