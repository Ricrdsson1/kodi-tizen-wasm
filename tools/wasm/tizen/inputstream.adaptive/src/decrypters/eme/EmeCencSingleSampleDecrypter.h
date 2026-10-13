/*
 *  Copyright (C) 2026 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

#include "common/AdaptiveCencSampleDecrypter.h"
#include "decrypters/IDecrypter.h"

#include <atomic>
#include <string>
#include <vector>

// Mirrors TizenDrmState of Kodi's xbmc/platform/wasm/tizen/TizenDRM.h.
struct TizenDrmState
{
  std::atomic<int32_t> messages{0};
  std::atomic<int32_t> updates{0};
  std::atomic<int32_t> usableKeys{0};
  std::atomic<int32_t> error{0};
};

/*!
 * \brief A DRM session of the browser's Encrypted Media Extensions.
 *
 * The browser keeps the keys and the TV's player decrypts with them, so samples
 * leave here still encrypted, along with what the player needs to know about them.
 * This is the job the Android decrypter does with MediaDrm, which it is derived from.
 */
class ATTR_DLL_LOCAL CEmeCencSingleSampleDecrypter : public Adaptive_CencSingleSampleDecrypter
{
public:
  CEmeCencSingleSampleDecrypter(const DRM::Config& config,
                                const std::vector<uint8_t>& defaultKeyId);
  ~CEmeCencSingleSampleDecrypter() override;

  SResult CreateSession(const std::vector<uint8_t>& pssh,
                        std::string_view licenseUrl,
                        bool skipSessionMessage) override;
  std::string GetSessionId() override;
  std::vector<uint8_t> GetChallengeData();
  bool HasKeys() const;

  AP4_Result SetFragmentInfo(AP4_UI32 poolId,
                             const std::vector<uint8_t>& keyId,
                             const AP4_UI08 nalLengthSize,
                             const std::vector<uint8_t>& annexbSpsPps,
                             DRM::Capabilities caps,
                             CryptoInfo cryptoInfo) override;
  AP4_UI32 AddPool() override;
  void RemovePool(AP4_UI32 poolId) override;
  AP4_Result DecryptSampleData(AP4_UI32 poolId,
                               AP4_DataBuffer& dataIn,
                               AP4_DataBuffer& dataOut,
                               const AP4_UI08* iv,
                               unsigned int subsampleCount,
                               const AP4_UI16* bytesOfCleartextData,
                               const AP4_UI32* bytesOfEncryptedData,
                               DRM::DRMMediaType streamType) override;

  void GetCapabilities(DRM::Capabilities& caps);

private:
  struct FINFO
  {
    std::vector<uint8_t> m_key;
    AP4_UI08 m_nalLengthSize;
    DRM::Capabilities capabilities;
    std::vector<uint8_t> m_annexbSpsPps;
  };

  struct SubsampleEntry
  {
    uint32_t clear_bytes;
    uint32_t cipher_bytes;
  };

  bool ReadSessionMessage(std::vector<uint8_t>& message);
  bool SendSessionMessage(const std::vector<uint8_t>& challenge);
  AP4_Result ConvertToAnnexBandInject(AP4_DataBuffer& dataIn,
                                      AP4_DataBuffer& dataOut,
                                      unsigned int subsampleCount,
                                      FINFO& fragInfo,
                                      const AP4_UI08* iv,
                                      const AP4_UI16* bytesOfCleartextData,
                                      const AP4_UI32* bytesOfEncryptedData,
                                      bool convertAnnexB,
                                      std::vector<SubsampleEntry>& rebuiltSubs);

  DRM::Config m_config;
  TizenDrmState m_state;
  int m_handle{0};
  std::string m_sessionId;
  std::vector<uint8_t> m_pssh;
  std::vector<uint8_t> m_keyRequestData;
  std::vector<uint8_t> m_defaultKeyId;
  std::vector<FINFO> m_fragmentPool;
  uint16_t m_hdcpVersion{DRM::HDCP_V_MAX};
  int m_hdcpLimit{0};
  int m_resolutionLimit{0};
};
