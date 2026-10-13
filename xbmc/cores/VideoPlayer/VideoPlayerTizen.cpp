/*
 *  Copyright (C) 2026 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "VideoPlayerTizen.h"

#include "MediaPipelineTizen.h"
#include "VideoPlayerAudioTizen.h"
#include "VideoPlayerVideoTizen.h"

#include <algorithm>

CVideoPlayerTizen::CVideoPlayerTizen(IPlayerCallback& callback) : CVideoPlayer(callback)
{
}

CVideoPlayerTizen::~CVideoPlayerTizen() = default;

void CVideoPlayerTizen::CreatePlayers()
{
  const auto canTizen =
      CMediaPipelineTizen::Available() &&
      std::ranges::any_of(m_SelectionStreams.Get(StreamType::VIDEO),
                          [](const auto& stream)
                          {
                            if (stream.codecId != AV_CODEC_ID_NONE)
                              return CMediaPipelineTizen::SupportsVideo(stream.codecId);
                            const AVCodec* codec =
                                avcodec_find_decoder_by_name(stream.codec.data());
                            return codec && CMediaPipelineTizen::SupportsVideo(codec->id);
                          });

  if (canTizen)
  {
    const bool hasAudio = m_SelectionStreams.CountType(StreamType::AUDIO);
    const bool subtitlesEnabled = m_VideoPlayerVideo->IsSubtitleEnabled();
    const double subtitleDelay = m_VideoPlayerVideo->GetSubtitleDelay();

    if (!m_mediaPipelineTizen)
    {
      m_mediaPipelineTizen = std::make_unique<CMediaPipelineTizen>(
          *m_processInfo, m_renderManager, m_clock, m_messenger, m_overlayContainer, hasAudio);
      m_VideoPlayerVideo =
          std::make_unique<CVideoPlayerVideoTizen>(*m_mediaPipelineTizen, *m_processInfo);
      m_VideoPlayerAudio =
          std::make_unique<CVideoPlayerAudioTizen>(*m_mediaPipelineTizen, *m_processInfo);
      m_VideoPlayerVideo->EnableSubtitle(subtitlesEnabled);
      m_VideoPlayerVideo->SetSubtitleDelay(subtitleDelay);
    }
  }
  else if (m_mediaPipelineTizen || (!m_VideoPlayerVideo && !m_VideoPlayerAudio))
  {
    // The stream players refer to the pipeline, so they go first.
    m_VideoPlayerVideo = std::make_unique<CVideoPlayerVideo>(
        &m_clock, &m_overlayContainer, m_messenger, m_renderManager, *m_processInfo,
        m_messageQueueTimeSize);
    m_VideoPlayerAudio = std::make_unique<CVideoPlayerAudio>(&m_clock, m_messenger, *m_processInfo,
                                                             m_messageQueueTimeSize);
    m_mediaPipelineTizen = nullptr;
  }

  if (m_players_created)
    return;

  m_VideoPlayerSubtitle =
      std::make_unique<CVideoPlayerSubtitle>(&m_overlayContainer, *m_processInfo);
  m_VideoPlayerTeletext = std::make_unique<CDVDTeletextData>(*m_processInfo);
  m_VideoPlayerRadioRDS = std::make_unique<CDVDRadioRDSData>(*m_processInfo);
  m_VideoPlayerAudioID3 = std::make_unique<CVideoPlayerAudioID3>(*m_processInfo);
  m_players_created = true;
}

void CVideoPlayerTizen::GetVideoResolution(unsigned int& width, unsigned int& height)
{
  if (m_mediaPipelineTizen)
    m_mediaPipelineTizen->GetVideoResolution(width, height);
  else
    CVideoPlayer::GetVideoResolution(width, height);
}

void CVideoPlayerTizen::UpdateContent()
{
  CVideoPlayer::UpdateContent();
  CreatePlayers();
}

bool CVideoPlayerTizen::CanTempo()
{
  return m_mediaPipelineTizen || CVideoPlayer::CanTempo();
}
