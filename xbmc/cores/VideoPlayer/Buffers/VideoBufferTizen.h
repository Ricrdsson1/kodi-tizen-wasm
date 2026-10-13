/*
 *  Copyright (C) 2026 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

#include "VideoBuffer.h"

/*!
 * \brief Placeholder buffer for video the Tizen WASM Player decodes and presents itself.
 */
class CTizenVideoBuffer final : public CVideoBuffer
{
public:
  CTizenVideoBuffer() : CVideoBuffer(0) {}
  AVPixelFormat GetFormat() override { return AV_PIX_FMT_NONE; }
};
