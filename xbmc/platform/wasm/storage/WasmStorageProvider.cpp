/*
 *  Copyright (C) 2026 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "WasmStorageProvider.h"

#include "ServiceBroker.h"
#include "platform/wasm/SystemInfoWasm.h"
#include "resources/LocalizeStrings.h"
#include "resources/ResourcesComponent.h"
#include "utils/StringUtils.h"

std::unique_ptr<IStorageProvider> IStorageProvider::CreateInstance()
{
  return std::make_unique<CWasmStorageProvider>();
}

void CWasmStorageProvider::GetLocalDrives(std::vector<CMediaSource>& localDrives)
{
  CMediaSource share;
  share.strPath = "/";
  share.strName = "Root";
  share.m_iDriveType = SourceType::LOCAL;
  localDrives.push_back(share);
}

std::vector<std::string> CWasmStorageProvider::GetDiskUsage()
{
  // The only storage is the one the browser keeps the profile in.
  uint64_t quota;
  uint64_t usage;
  if (!KODI::PLATFORM::WASM::GetStorage(quota, usage))
    return {};

  const auto& strings = CServiceBroker::GetResourcesComponent().GetLocalizeStrings();
  constexpr uint64_t MB = 1024 * 1024;
  const uint64_t used = (usage + MB / 2) / MB;
  const uint64_t total = quota / MB;
  return {StringUtils::Format("{}: {} MB", strings.Get(20161), total),
          StringUtils::Format("{}: {} MB", strings.Get(20162), used),
          StringUtils::Format("{}: {} MB", strings.Get(160), total > used ? total - used : 0)};
}
