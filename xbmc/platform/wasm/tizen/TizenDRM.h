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

// C++ side of xbmc/platform/wasm/tizen/tizen_drm.js: DRM sessions of the browser's
// Encrypted Media Extensions, whose keys the Tizen WASM Player decrypts with.
//
// A session lives on the browser main thread. The caller, InputStream Adaptive's
// decrypter, runs the license exchange: it waits for a message, sends it to the
// license server and hands the response back.

/*!
 * \brief Session state published by the browser main thread.
 *
 * The layout is mirrored by the OFF_* constants in tizen_drm.js.
 */
struct TizenDrmState
{
  std::atomic<int32_t> messages{0}; //!< license requests issued so far
  std::atomic<int32_t> updates{0}; //!< responses the session has accepted so far
  std::atomic<int32_t> usableKeys{0};
  std::atomic<int32_t> error{0};
};

static_assert(sizeof(TizenDrmState) == 16, "TizenDrmState layout is shared with JS");

extern "C"
{
/*!
 * \brief Create a session and ask it for a license request.
 * \param state has to stay valid until tizen_drm_close()
 * \param initDataType "cenc", "keyids" or "webm"
 * \return a handle, or 0 if the key system is not available
 */
int tizen_drm_open(TizenDrmState* state,
                   const char* keySystem,
                   const char* initDataType,
                   const uint8_t* initData,
                   int initDataSize);

/*!
 * \brief Copy the oldest license request that was not read yet.
 * \return its size, 0 if there is none, or minus the size if the buffer is too small
 */
int tizen_drm_read_message(int handle, uint8_t* buffer, int capacity);

/*!
 * \brief Hand the license server's response to the session.
 *
 * TizenDrmState::updates or TizenDrmState::error changes once it is processed.
 */
void tizen_drm_update(int handle, const uint8_t* response, int size);

void tizen_drm_close(int handle);
}
