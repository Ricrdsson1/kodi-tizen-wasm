/*
 *  Copyright (C) 2026 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

#include "cores/IPlayer.h"
#include "threads/CriticalSection.h"
#include "threads/Thread.h"

#include <atomic>
#include <string>
#include <vector>

class CFileItem;

/*!
 * \brief Player for DRM protected adaptive streams in a browser.
 *
 * Plays the manifest of an item that InputStream Adaptive would play with DRM through
 * Shaka Player in the browser (web_player.js), as the browser's DRM decrypts reliably
 * only for its own media pipeline. The video is shown under the GUI, so Kodi's OSD and
 * controls work as with VideoPlayer.
 */
class CWebPlayer : public IPlayer, public CThread
{
public:
  explicit CWebPlayer(IPlayerCallback& callback);
  ~CWebPlayer() override;

  /*!
   * \brief Whether an item is a stream for InputStream Adaptive with DRM settings, which
   * this player is meant for.
   */
  static bool IsDrmItem(const CFileItem& item);

  bool OpenFile(const CFileItem& file, const CPlayerOptions& options) override;
  bool CloseFile(bool reopen = false) override;
  bool IsPlaying() const override;
  bool CanPause() const override { return true; }
  void Pause() override;
  bool HasVideo() const override { return true; }
  bool HasAudio() const override { return true; }
  bool CanSeek() const override { return true; }
  void Seek(bool bPlus = true, bool bLargeStep = false, bool bChapterOverride = false) override;
  void SeekPercentage(float fPercent = 0) override;
  void SeekTime(int64_t iTime = 0) override;
  bool SeekTimeRelative(int64_t iTime) override;
  void SetSpeed(float speed) override;
  bool IsCaching() const override { return m_buffering; }
  bool IsStreaming() const override { return true; }

  int GetAudioStreamCount() const override;
  int GetAudioStream() override;
  void SetAudioStream(int iStream) override;
  void GetAudioStreamInfo(int index, AudioStreamInfo& info) const override;

  int GetSubtitleCount() const override;
  int GetSubtitle() override;
  void GetSubtitleStreamInfo(int index, SubtitleStreamInfo& info) const override;
  void SetSubtitle(int iStream) override;
  bool GetSubtitleVisible() const override;
  void SetSubtitleVisible(bool bVisible) override;

  bool IsRenderingVideo() const override { return m_started; }

private:
  struct AudioTrack
  {
    std::string language;
    std::string name;
    int channels{0};
    std::string codec;
  };
  struct TextTrack
  {
    std::string language;
    std::string name;
    bool forced{false};
  };

  void Process() override;
  void UpdateTracks();

  CFileItem* m_item{nullptr};
  bool m_fullscreen{false};
  std::atomic<bool> m_open{false};
  std::atomic<bool> m_started{false};
  std::atomic<bool> m_paused{false};
  std::atomic<bool> m_buffering{false};
  std::atomic<int64_t> m_time{0}; // ms
  std::atomic<int64_t> m_duration{0}; // ms

  mutable CCriticalSection m_tracksLock;
  std::vector<AudioTrack> m_audio;
  int m_audioIndex{-1};
  std::vector<TextTrack> m_text;
  int m_textIndex{-1};
  bool m_textVisible{false};
};
