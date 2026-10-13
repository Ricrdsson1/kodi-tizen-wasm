/*
 *  Copyright (C) 2026 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

#include "decrypters/IDecrypter.h"

#include <string>

/*!
 * \brief Decrypter for browsers: sessions of the Encrypted Media Extensions.
 */
class ATTR_DLL_LOCAL CEmeDecrypter : public DRM::IDecrypter
{
public:
  const std::string GetName() const override { return "EME"; }

  bool IsKeySystemSupported(std::string_view keySystem) override;
  std::shared_ptr<Adaptive_CencSingleSampleDecrypter> CreateSingleSampleDecrypter(
      const DRM::Config& config,
      const std::vector<uint8_t>& defaultKeyId,
      CryptoMode cryptoMode) override;
  void GetCapabilities(std::shared_ptr<Adaptive_CencSingleSampleDecrypter> decrypter,
                       const std::vector<uint8_t>& keyId,
                       DRM::Capabilities& caps,
                       DRM::DRMMediaType mediaType) override;
  std::optional<bool> HasLicenseKey(std::shared_ptr<Adaptive_CencSingleSampleDecrypter> decrypter,
                                    const std::vector<uint8_t>& keyId) override;
  std::string GetChallengeB64Data(
      std::shared_ptr<Adaptive_CencSingleSampleDecrypter> decrypter) override;

  // The player of the TV decodes; there is no decoder to offer.
  bool OpenVideoDecoder(std::shared_ptr<Adaptive_CencSingleSampleDecrypter> decrypter,
                        const VIDEOCODEC_INITDATA* initData) override
  {
    return false;
  }
  VIDEOCODEC_RETVAL DecryptAndDecodeVideo(kodi::addon::CInstanceVideoCodec* codecInstance,
                                          const DEMUX_PACKET* sample) override
  {
    return VC_ERROR;
  }
  VIDEOCODEC_RETVAL VideoFrameDataToPicture(kodi::addon::CInstanceVideoCodec* codecInstance,
                                            VIDEOCODEC_PICTURE* picture) override
  {
    return VC_ERROR;
  }
  void ResetVideo() override {}

  void SetLibraryPath(std::string_view libraryPath) override { m_libraryPath = libraryPath; }
  std::string_view GetLibraryPath() const override { return m_libraryPath; }

private:
  std::string m_libraryPath;
};
