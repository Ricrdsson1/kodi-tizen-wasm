/*
 *  Copyright (C) 2026 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

#include "DVDCodecs/Video/DVDVideoCodec.h"
#include "DVDMessageQueue.h"
#include "DVDStreamInfo.h"
#include "IVideoPlayer.h"
#include "threads/Thread.h"

#include "platform/wasm/tizen/TizenEMSS.h"

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

class CBitstreamConverter;
class CDVDClock;
class CDVDOverlayContainer;
class CProcessInfo;
class CRenderManager;

/*!
 * \brief Plays elementary streams with the Tizen WASM Player.
 *
 * Demuxed packets are handed to the TV undecoded: it decodes them with its
 * hardware pipeline, keeps audio and video in sync and presents the video on a
 * plane below the GUI. Kodi's clock follows the TV's playback position.
 */
class CMediaPipelineTizen final : public CThread
{
public:
  CMediaPipelineTizen(CProcessInfo& processInfo,
                      CRenderManager& renderManager,
                      CDVDClock& clock,
                      CDVDMessageQueue& parent,
                      CDVDOverlayContainer& overlay,
                      bool hasAudio);
  ~CMediaPipelineTizen() override;

  /*!
   * \brief Whether the Tizen WASM Player exists on this device.
   */
  static bool Available();

  static bool SupportsVideo(AVCodecID codec);
  static bool SupportsAudio(AVCodecID codec);

  void FlushVideoMessages();
  void FlushAudioMessages();
  bool OpenAudioStream(CDVDStreamInfo hint);
  bool OpenVideoStream(CDVDStreamInfo hint);
  void CloseAudioStream(bool waitForBuffers);
  void CloseVideoStream(bool waitForBuffers);
  void Flush(bool sync);
  bool AcceptsAudioData() const;
  bool AcceptsVideoData() const;
  bool HasAudioData() const;
  bool HasVideoData() const;
  bool IsAudioInited() const;
  bool IsVideoInited() const;
  int GetAudioLevel() const;
  void SendAudioMessage(const std::shared_ptr<CDVDMsg>& msg, int priority);
  void SendVideoMessage(const std::shared_ptr<CDVDMsg>& msg, int priority);
  void SetSpeed(int speed);

  /*!
   * \brief Playback position of the TV in DVD_TIME_BASE units.
   */
  double GetCurrentPts() const;

  int GetAudioChannels() const { return m_audioHint.channels; }
  bool IsStalled() const { return false; }
  int GetVideoBitrate() const { return 0; }
  void SetDynamicRangeCompression(long drc) {}
  void EnableSubtitle(bool enable) { m_subtitle = enable; }
  bool IsSubtitleEnabled() const { return m_subtitle; }
  double GetSubtitleDelay() const { return m_subtitleDelay; }
  void SetSubtitleDelay(double delay) { m_subtitleDelay = delay; }
  std::string GetAudioInfo() const;
  std::string GetVideoInfo() const;
  void GetVideoResolution(unsigned int& width, unsigned int& height) const;

protected:
  void Process() override;

private:
  void ProcessAudio();
  bool Load();
  void Unload();

  /*!
   * \brief Hand a packet to the TV.
   * \return false if the packet has to be offered again later
   */
  bool FeedVideoData(const std::shared_ptr<CDVDMsg>& msg);
  bool FeedAudioData(const std::shared_ptr<CDVDMsg>& msg);
  bool Append(int trackId,
              const DemuxPacket& packet,
              const uint8_t* data,
              int size,
              double pts,
              bool keyFrame);

  /*!
   * \brief Prepare the conversion of length-prefixed H.264/HEVC to Annex B.
   *
   * Annex B carries the parameter sets in the stream, which lets the resolution
   * change without the TV's tracks being set up again.
   */
  void SetupBitstreamConverter();

  /*!
   * \brief Start the TV at the position of the first packet that follows a flush.
   * \return false while the TV is not ready to take packets for that position
   */
  bool Restart(double pts);

  void UpdatePosition();
  void ProcessOverlays(double pts) const;
  void ReportStarted() const;

  TizenEmssState m_state;

  CDVDStreamInfo m_audioHint;
  CDVDStreamInfo m_videoHint;
  VideoPicture m_picture{};
  double m_frameDuration{0.04}; // seconds
  std::unique_ptr<CBitstreamConverter> m_bitstream;
  const DemuxPacket* m_convertedPacket{nullptr};
  // Set once the demuxer has flagged a key frame; until then they are looked for here.
  bool m_demuxerFlagsKeyFrames{false};
  // Largest picture the TV was told to expect.
  int m_configWidth{0};
  int m_configHeight{0};

  // Serialise feeding against Load(), Unload() and Flush().
  std::mutex m_videoFeedMutex;
  std::mutex m_audioFeedMutex;

  std::atomic<bool> m_loaded{false};
  std::atomic<bool> m_flushed{false};
  std::atomic<bool> m_restarting{false};
  std::atomic<bool> m_played{false};
  std::atomic<bool> m_drainAudio{false};
  std::atomic<bool> m_subtitle{false};
  std::atomic<double> m_subtitleDelay{0.0};
  std::atomic<bool> m_hasAudio;
  std::atomic<bool> m_hasAudioTrack{false};
  std::atomic<bool> m_videoClosed{true};
  std::atomic<bool> m_audioClosed{true};
  std::atomic<double> m_fedVideoPts{-1.0}; // seconds
  std::atomic<double> m_fedAudioPts{-1.0}; // seconds
  double m_shownPosition{-1.0}; // seconds
  int m_restartWait{0};

  CDVDMessageQueue m_messageQueueAudio;
  CDVDMessageQueue m_messageQueueVideo;
  CDVDMessageQueue& m_messageQueueParent;
  CProcessInfo& m_processInfo;
  CRenderManager& m_renderManager;
  CDVDClock& m_clock;
  CDVDOverlayContainer& m_overlayContainer;

  std::thread m_audioThread;
};
