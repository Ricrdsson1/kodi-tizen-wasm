/*
 *  Copyright (C) 2026 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "VideoPlayerAudioTizen.h"

CVideoPlayerAudioTizen::CVideoPlayerAudioTizen(CMediaPipelineTizen& mediaPipeline,
                                               CProcessInfo& processInfo)
  : IDVDStreamPlayerAudio(processInfo), m_mediaPipeline(mediaPipeline)
{
}

void CVideoPlayerAudioTizen::FlushMessages()
{
  m_mediaPipeline.FlushAudioMessages();
}

bool CVideoPlayerAudioTizen::OpenStream(CDVDStreamInfo hints)
{
  return m_mediaPipeline.OpenAudioStream(hints);
}

void CVideoPlayerAudioTizen::CloseStream(const bool waitForBuffers)
{
  m_mediaPipeline.CloseAudioStream(waitForBuffers);
}

void CVideoPlayerAudioTizen::SetSpeed(const int speed)
{
  m_mediaPipeline.SetSpeed(speed);
}

void CVideoPlayerAudioTizen::Flush(const bool sync)
{
  // VideoPlayer calls this right before calling flush on the video player
  // We can skip this call because it would essential flush the pipeline twice
}

bool CVideoPlayerAudioTizen::AcceptsData() const
{
  return m_mediaPipeline.AcceptsAudioData();
}

bool CVideoPlayerAudioTizen::HasData() const
{
  return m_mediaPipeline.HasAudioData();
}

int CVideoPlayerAudioTizen::GetLevel() const
{
  return m_mediaPipeline.GetAudioLevel();
}

bool CVideoPlayerAudioTizen::IsInited() const
{
  return m_mediaPipeline.IsAudioInited();
}

void CVideoPlayerAudioTizen::SendMessage(const std::shared_ptr<CDVDMsg> msg, const int priority)
{
  m_mediaPipeline.SendAudioMessage(msg, priority);
}

void CVideoPlayerAudioTizen::SetDynamicRangeCompression(const long drc)
{
  m_mediaPipeline.SetDynamicRangeCompression(drc);
}

std::string CVideoPlayerAudioTizen::GetPlayerInfo()
{
  return m_mediaPipeline.GetAudioInfo();
}

int CVideoPlayerAudioTizen::GetAudioChannels()
{
  return m_mediaPipeline.GetAudioChannels();
}

double CVideoPlayerAudioTizen::GetCurrentPts()
{
  return m_mediaPipeline.GetCurrentPts();
}

bool CVideoPlayerAudioTizen::IsStalled() const
{
  return m_mediaPipeline.IsStalled();
}

bool CVideoPlayerAudioTizen::IsPassthrough() const
{
  return true;
}

float CVideoPlayerAudioTizen::GetDynamicRangeAmplification() const
{
  return 0.0f;
}
