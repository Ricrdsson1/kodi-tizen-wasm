/*
 *  Copyright (C) 2026 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>

// C++ side of xbmc/platform/wasm/tizen/tizen_emss.js: Samsung's Tizen WASM Player
// (ElementaryMediaStreamSource), which decodes and renders elementary stream
// packets with the TV's hardware pipeline.

/*!
 * \brief Player state published by the browser main thread.
 *
 * The layout is mirrored by the OFF_* constants in tizen_emss.js.
 */
struct TizenEmssState
{
  std::atomic<int32_t> sourceState{0};
  std::atomic<int32_t> videoTrack{0};
  std::atomic<int32_t> audioTrack{0};
  std::atomic<int32_t> videoOpen{0};
  std::atomic<int32_t> audioOpen{0};
  std::atomic<int32_t> sessionId{0};
  std::atomic<int32_t> error{0};
  std::atomic<int32_t> ended{0};
  std::atomic<double> currentTime{0.0};
};

static_assert(sizeof(std::atomic<int32_t>) == 4, "TizenEmssState layout is shared with JS");
static_assert(sizeof(std::atomic<double>) == 8, "TizenEmssState layout is shared with JS");
static_assert(offsetof(TizenEmssState, currentTime) == 32,
              "TizenEmssState layout is shared with JS");

enum TizenEmssSourceState : int32_t
{
  TIZEN_EMSS_DETACHED = 0,
  TIZEN_EMSS_CLOSED = 1,
  TIZEN_EMSS_OPEN_PENDING = 2,
  TIZEN_EMSS_OPEN = 3,
  TIZEN_EMSS_ENDED = 4,
};

/*!
 * \brief Track layout passed to tizen_emss_open().
 *
 * The layout is read field by field in tizen_emss.js.
 */
struct TizenEmssConfig
{
  double duration; // seconds
  const char* videoMime;
  const uint8_t* videoExtradata;
  int32_t videoExtradataSize;
  int32_t width;
  int32_t height;
  int32_t framerateNum;
  int32_t framerateDen;
  int32_t hasAudio;
  const char* audioMime;
  const uint8_t* audioExtradata;
  int32_t audioExtradataSize;
  const char* sampleFormat;
  const char* channelLayout;
  int32_t sampleRate;
};

static_assert(sizeof(void*) == 4, "TizenEmssConfig layout is shared with JS");
static_assert(offsetof(TizenEmssConfig, hasAudio) == 36, "TizenEmssConfig layout is shared with JS");
static_assert(offsetof(TizenEmssConfig, sampleRate) == 60,
              "TizenEmssConfig layout is shared with JS");

enum TizenEmssAppendResult : int
{
  TIZEN_EMSS_APPEND_OK = 0,
  TIZEN_EMSS_APPEND_BUFFER_FULL = 1,
  TIZEN_EMSS_APPEND_TRACK_CLOSED = 2,
  TIZEN_EMSS_APPEND_DROPPED = 3,
  TIZEN_EMSS_APPEND_FATAL = 4,
};

extern "C"
{
int tizen_emss_available(void);
int tizen_emss_validate_video(const char* mime, int width, int height, int fpsNum, int fpsDen);
int tizen_emss_validate_audio(const char* mime,
                              const char* sampleFormat,
                              const char* channelLayout,
                              int sampleRate);
int tizen_emss_open(TizenEmssState* state, const TizenEmssConfig* config);
void tizen_emss_close(void);
void tizen_emss_play(void);
void tizen_emss_pause(void);
void tizen_emss_set_rate(double rate);
int tizen_emss_seek(double seconds);
void tizen_emss_set_rect(int x, int y, int width, int height);
int tizen_emss_append(int trackId,
                      const uint8_t* data,
                      int size,
                      double pts,
                      double dts,
                      double duration,
                      int keyFrame,
                      int sessionId);
void tizen_emss_end_of_track(int trackId, int sessionId);
}
