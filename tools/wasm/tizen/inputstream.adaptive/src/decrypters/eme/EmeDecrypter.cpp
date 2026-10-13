/*
 *  Copyright (C) 2026 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "EmeDecrypter.h"

#include "EmeCencSingleSampleDecrypter.h"
#include "decrypters/Helpers.h"
#include "utils/Base64Utils.h"
#include "utils/log.h"

using namespace UTILS;

bool CEmeDecrypter::IsKeySystemSupported(std::string_view keySystem)
{
  return keySystem == DRM::KS_WIDEVINE || keySystem == DRM::KS_PLAYREADY;
}

std::shared_ptr<Adaptive_CencSingleSampleDecrypter> CEmeDecrypter::CreateSingleSampleDecrypter(
    const DRM::Config& config, const std::vector<uint8_t>& defaultKeyId, CryptoMode cryptoMode)
{
  return std::make_shared<CEmeCencSingleSampleDecrypter>(config, defaultKeyId);
}

void CEmeDecrypter::GetCapabilities(std::shared_ptr<Adaptive_CencSingleSampleDecrypter> decrypter,
                                    const std::vector<uint8_t>& keyId,
                                    DRM::Capabilities& caps,
                                    DRM::DRMMediaType mediaType)
{
  auto emeDecrypter = std::dynamic_pointer_cast<CEmeCencSingleSampleDecrypter>(decrypter);
  if (emeDecrypter)
    emeDecrypter->GetCapabilities(caps);
  else
    caps = {0, 0, 0};
}

std::optional<bool> CEmeDecrypter::HasLicenseKey(
    std::shared_ptr<Adaptive_CencSingleSampleDecrypter> decrypter,
    const std::vector<uint8_t>& keyId)
{
  // The browser only says how many keys are usable, not which.
  auto emeDecrypter = std::dynamic_pointer_cast<CEmeCencSingleSampleDecrypter>(decrypter);
  if (emeDecrypter)
    return emeDecrypter->HasKeys();
  return std::nullopt;
}

std::string CEmeDecrypter::GetChallengeB64Data(
    std::shared_ptr<Adaptive_CencSingleSampleDecrypter> decrypter)
{
  auto emeDecrypter = std::dynamic_pointer_cast<CEmeCencSingleSampleDecrypter>(decrypter);
  if (emeDecrypter)
    return BASE64::Encode(emeDecrypter->GetChallengeData());
  return "";
}
