/*
 *  Copyright (C) 2026 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

#include "cores/VideoPlayer/VideoRenderers/BaseRenderer.h"

/*!
 * \brief Renderer for video presented by the Tizen WASM Player.
 *
 * The TV draws the video on a plane below the GUI canvas; this renderer only
 * keeps that plane's rectangle in sync with Kodi's view mode.
 */
class CRendererTizen : public CBaseRenderer
{
public:
  CRendererTizen() = default;
  ~CRendererTizen() override = default;

  // Registration
  static CBaseRenderer* Create(CVideoBuffer* buffer);
  static bool Register();

  // Player functions
  void AddVideoPicture(const VideoPicture& picture, int index) override;
  void ReleaseBuffer(int idx) override;

  // Feature support
  CRenderInfo GetRenderInfo() override;

  bool IsGuiLayer() override;
  bool IsConfigured() override;
  bool Configure(const VideoPicture& picture, float fps, unsigned int orientation) override;
  bool Supports(ERENDERFEATURE feature) const override;
  bool Supports(ESCALINGMETHOD method) const override;
  bool SupportsMultiPassRendering() override;
  void UnInit() override;
  void Update() override;
  void RenderUpdate(
      int index, int index2, bool clear, unsigned int flags, unsigned int alpha) override;
  bool ConfigChanged(const VideoPicture& picture) override;

protected:
  void ManageRenderArea() override;

private:
  CRect m_exportedDestRect;
  bool m_configured{false};
};
