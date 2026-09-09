/*
 *  Copyright (C) 2026 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "HttpClientFactory.h"

#include "CurlHttpClient.h"

#if defined(TARGET_WASM)
#include "wasm/XhrHttpClient.h"
#endif

using namespace XFILE;

std::unique_ptr<IHttpClient> XFILE::CreateHttpClient()
{
#if defined(TARGET_WASM)
  return std::make_unique<CXhrHttpClient>();
#else
  return std::make_unique<CCurlHttpClient>();
#endif
}
