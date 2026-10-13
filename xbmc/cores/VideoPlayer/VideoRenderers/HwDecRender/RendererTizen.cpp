/*
 *  Copyright (C) 2026 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "RendererTizen.h"

#include "../RenderFactory.h"
#include "cores/VideoPlayer/Buffers/VideoBufferTizen.h"
#include "cores/VideoPlayer/DVDCodecs/Video/DVDVideoCodec.h"

#include "platform/wasm/tizen/TizenEMSS.h"

#include <cmath>

#include <emscripten/html5.h>

CBaseRenderer* CRendererTizen::Create(CVideoBuffer* buffer)
{
  if (buffer && dynamic_cast<CTizenVideoBuffer*>(buffer))
    return new CRendererTizen();
  return nullptr;
}

bool CRendererTizen::Register()
{
  VIDEOPLAYER::CRendererFactory::RegisterRenderer("tizen", CRendererTizen::Create);
  return true;
}

bool CRendererTizen::Configure(const VideoPicture& picture, float fps, unsigned int orientation)
{
  m_format = picture.videoBuffer->GetFormat();
  m_sourceWidth = picture.iWidth;
  m_sourceHeight = picture.iHeight;
  m_renderOrientation = orientation;

  CalculateFrameAspectRatio(picture.iDisplayWidth, picture.iDisplayHeight);
  SetViewMode(m_videoSettings.m_ViewMode);
  m_exportedDestRect = CRect();
  ManageRenderArea();

  m_configured = true;
  return true;
}

bool CRendererTizen::IsConfigured()
{
  return m_configured;
}

bool CRendererTizen::ConfigChanged(const VideoPicture& picture)
{
  return picture.videoBuffer->GetFormat() != m_format;
}

void CRendererTizen::ManageRenderArea()
{
  CBaseRenderer::ManageRenderArea();

  if (m_exportedDestRect == m_destRect || m_destRect.IsEmpty())
    return;

  // The video element is positioned in CSS pixels, the destination is in canvas pixels.
  int canvasWidth = 0;
  int canvasHeight = 0;
  double cssWidth = 0.0;
  double cssHeight = 0.0;
  if (emscripten_get_canvas_element_size("#canvas", &canvasWidth, &canvasHeight) !=
          EMSCRIPTEN_RESULT_SUCCESS ||
      emscripten_get_element_css_size("#canvas", &cssWidth, &cssHeight) !=
          EMSCRIPTEN_RESULT_SUCCESS ||
      canvasWidth <= 0 || canvasHeight <= 0)
    return;

  const float scaleX = static_cast<float>(cssWidth / canvasWidth);
  const float scaleY = static_cast<float>(cssHeight / canvasHeight);
  tizen_emss_set_rect(static_cast<int>(std::lround(m_destRect.x1 * scaleX)),
                      static_cast<int>(std::lround(m_destRect.y1 * scaleY)),
                      static_cast<int>(std::lround(m_destRect.Width() * scaleX)),
                      static_cast<int>(std::lround(m_destRect.Height() * scaleY)));
  m_exportedDestRect = m_destRect;
}

bool CRendererTizen::Supports(ERENDERFEATURE feature) const
{
  return feature == RENDERFEATURE_ZOOM || feature == RENDERFEATURE_STRETCH ||
         feature == RENDERFEATURE_PIXEL_RATIO || feature == RENDERFEATURE_VERTICAL_SHIFT;
}

bool CRendererTizen::Supports(ESCALINGMETHOD method) const
{
  return false;
}

bool CRendererTizen::SupportsMultiPassRendering()
{
  return false;
}

void CRendererTizen::AddVideoPicture(const VideoPicture& picture, int index)
{
}

void CRendererTizen::ReleaseBuffer(int idx)
{
}

CRenderInfo CRendererTizen::GetRenderInfo()
{
  CRenderInfo info;
  info.max_buffer_size = 4;
  return info;
}

bool CRendererTizen::IsGuiLayer()
{
  return false;
}

void CRendererTizen::UnInit()
{
  m_configured = false;
}

void CRendererTizen::Update()
{
}

void CRendererTizen::RenderUpdate(
    int index, int index2, bool clear, unsigned int flags, unsigned int alpha)
{
  if (m_configured)
    ManageRenderArea();
}
