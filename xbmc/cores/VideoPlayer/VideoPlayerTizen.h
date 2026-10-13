/*
 *  Copyright (C) 2026 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

#include "VideoPlayer.h"

#include <memory>

class CMediaPipelineTizen;

/*!
 * \brief VideoPlayer that plays through the Tizen WASM Player where the TV can decode the video.
 */
class CVideoPlayerTizen final : public CVideoPlayer
{
public:
  explicit CVideoPlayerTizen(IPlayerCallback& callback);
  ~CVideoPlayerTizen() override;
  void GetVideoResolution(unsigned int& width, unsigned int& height) override;

protected:
  void UpdateContent() override;
  bool CanTempo() override;

  void CreatePlayers() override;

private:
  std::unique_ptr<CMediaPipelineTizen> m_mediaPipelineTizen;
};
