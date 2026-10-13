/*
 *  Copyright (C) 2026 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "VideoPlayerVideoTizen.h"

CVideoPlayerVideoTizen::CVideoPlayerVideoTizen(CMediaPipelineTizen& mediaPipeline,
                                               CProcessInfo& processInfo)
  : IDVDStreamPlayerVideo(processInfo), m_mediaPipeline(mediaPipeline)
{
}

void CVideoPlayerVideoTizen::FlushMessages()
{
  m_mediaPipeline.FlushVideoMessages();
}

bool CVideoPlayerVideoTizen::OpenStream(const CDVDStreamInfo hint)
{
  return m_mediaPipeline.OpenVideoStream(hint);
}

void CVideoPlayerVideoTizen::CloseStream(const bool waitForBuffers)
{
  m_mediaPipeline.CloseVideoStream(waitForBuffers);
}

void CVideoPlayerVideoTizen::Flush(const bool sync)
{
  m_mediaPipeline.Flush(sync);
}

bool CVideoPlayerVideoTizen::AcceptsData() const
{
  return m_mediaPipeline.AcceptsVideoData();
}

bool CVideoPlayerVideoTizen::HasData() const
{
  return m_mediaPipeline.HasVideoData();
}

bool CVideoPlayerVideoTizen::IsInited() const
{
  return m_mediaPipeline.IsVideoInited();
}

void CVideoPlayerVideoTizen::SendMessage(const std::shared_ptr<CDVDMsg> msg, const int priority)
{
  m_mediaPipeline.SendVideoMessage(msg, priority);
}

void CVideoPlayerVideoTizen::EnableSubtitle(const bool enable)
{
  m_mediaPipeline.EnableSubtitle(enable);
}

bool CVideoPlayerVideoTizen::IsSubtitleEnabled()
{
  return m_mediaPipeline.IsSubtitleEnabled();
}

double CVideoPlayerVideoTizen::GetSubtitleDelay()
{
  return m_mediaPipeline.GetSubtitleDelay();
}

void CVideoPlayerVideoTizen::SetSubtitleDelay(const double delay)
{
  m_mediaPipeline.SetSubtitleDelay(delay);
}

bool CVideoPlayerVideoTizen::IsStalled() const
{
  return m_mediaPipeline.IsStalled();
}

double CVideoPlayerVideoTizen::GetCurrentPts()
{
  return m_mediaPipeline.GetCurrentPts();
}

double CVideoPlayerVideoTizen::GetOutputDelay()
{
  return 0.0;
}
std::string CVideoPlayerVideoTizen::GetPlayerInfo()
{
  return m_mediaPipeline.GetVideoInfo();
}

int CVideoPlayerVideoTizen::GetVideoBitrate()
{
  return m_mediaPipeline.GetVideoBitrate();
}

void CVideoPlayerVideoTizen::SetSpeed(const int speed)
{
  m_mediaPipeline.SetSpeed(speed);
}
