/*
 *  Copyright (C) 2026 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "EmeCencSingleSampleDecrypter.h"

#include "decrypters/HelperWv.h"
#include "decrypters/Helpers.h"
#include "utils/Base64Utils.h"
#include "utils/CurlUtils.h"
#include "utils/GUIUtils.h"
#include "utils/StringUtils.h"
#include "utils/log.h"

#include <chrono>
#include <thread>

using namespace UTILS;
using namespace std::chrono_literals;

extern "C"
{
// Kodi's xbmc/platform/wasm/tizen/tizen_drm.js, see TizenDRM.h there.
int tizen_drm_open(TizenDrmState* state,
                   const char* keySystem,
                   const char* initDataType,
                   const uint8_t* initData,
                   int initDataSize,
                   const uint8_t* keyId);
int tizen_drm_read_message(int handle, uint8_t* buffer, int capacity);
void tizen_drm_update(int handle, const uint8_t* response, int size);
void tizen_drm_close(int handle);
}

namespace
{
constexpr auto SESSION_TIMEOUT = 15s;

// A session that needs a service certificate asks twice, and may ask again later.
constexpr int MAX_LICENSE_ROUNDS = 4;

template<typename Predicate>
bool WaitFor(Predicate predicate, std::chrono::milliseconds timeout = SESSION_TIMEOUT)
{
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (!predicate())
  {
    if (std::chrono::steady_clock::now() >= deadline)
      return false;
    std::this_thread::sleep_for(10ms);
  }
  return true;
}
} // unnamed namespace

CEmeCencSingleSampleDecrypter::CEmeCencSingleSampleDecrypter(
    const DRM::Config& config, const std::vector<uint8_t>& defaultKeyId)
  : m_config(config), m_defaultKeyId{defaultKeyId}
{
  SetParentIsOwner(false);
}

CEmeCencSingleSampleDecrypter::~CEmeCencSingleSampleDecrypter()
{
  if (m_handle)
    tizen_drm_close(m_handle);
}

SResult CEmeCencSingleSampleDecrypter::CreateSession(const std::vector<uint8_t>& pssh,
                                                     std::string_view licenseUrl,
                                                     bool skipSessionMessage)
{
  if (m_config.license.serverUri.empty())
  {
    LOG::LogF(LOGERROR, "The DRM license server url has not been configured");
    return SResult::Error(GUI::GetLocalizedString(30306));
  }

  if (pssh.size() < 4 || pssh.size() > 4096)
  {
    LOG::LogF(LOGERROR, "Cannot request license, PSSH init data has unexpected size (%zu)",
              pssh.size());
    return SResult::Error(GUI::GetLocalizedString(30303));
  }

  m_pssh = pssh;
  m_handle = tizen_drm_open(&m_state, m_config.keySystem.c_str(), "cenc", m_pssh.data(),
                            static_cast<int>(m_pssh.size()),
                            m_defaultKeyId.size() == 16 ? m_defaultKeyId.data() : nullptr);
  if (!m_handle)
  {
    LOG::LogF(LOGERROR, "The browser has no %s", m_config.keySystem.c_str());
    return SResult::Error(GUI::GetLocalizedString(30303));
  }
  m_sessionId = std::to_string(m_handle);
  LOG::Log(LOGDEBUG, "Session ID: %s", m_sessionId.c_str());

  // The key came with the license of an earlier session, which this one shares.
  if (m_state.usableKeys > 0)
  {
    LOG::Log(LOGDEBUG, "Sharing the browser session that holds the key, usable keys: %d",
             m_state.usableKeys.load());
    return SResult::Ok();
  }

  for (int round = 0; round < MAX_LICENSE_ROUNDS; ++round)
  {
    // After the first response, a session that is content only stays quiet.
    const auto timeout = round == 0 ? std::chrono::milliseconds(SESSION_TIMEOUT) : 500ms;
    if (!WaitFor([&] { return m_state.messages > round || m_state.error; }, timeout))
    {
      if (round > 0)
        break;
      LOG::LogF(LOGERROR, "Timeout waiting for the license request of the session");
      return SResult::Error(GUI::GetLocalizedString(30303));
    }

    if (m_state.error || !ReadSessionMessage(m_keyRequestData))
    {
      LOG::LogF(LOGERROR, "The session failed to issue a license request");
      return SResult::Error(GUI::GetLocalizedString(30303));
    }
    LOG::Log(LOGDEBUG, "Key request successful size: %zu", m_keyRequestData.size());

    if (skipSessionMessage)
      return SResult::Ok();

    if (!SendSessionMessage(m_keyRequestData))
      return SResult::Error(GUI::GetLocalizedString(30303));
  }

  WaitFor([&] { return m_state.usableKeys > 0 || m_state.error; }, 2s);
  LOG::Log(LOGDEBUG, "Usable keys: %d", m_state.usableKeys.load());
  return SResult::Ok();
}

bool CEmeCencSingleSampleDecrypter::ReadSessionMessage(std::vector<uint8_t>& message)
{
  message.resize(16 * 1024);
  int size = tizen_drm_read_message(m_handle, message.data(), static_cast<int>(message.size()));
  if (size < 0)
  {
    message.resize(static_cast<size_t>(-size));
    size = tizen_drm_read_message(m_handle, message.data(), static_cast<int>(message.size()));
  }
  message.resize(size > 0 ? static_cast<size_t>(size) : 0);
  return size > 0;
}

std::string CEmeCencSingleSampleDecrypter::GetSessionId()
{
  return m_sessionId;
}

std::vector<uint8_t> CEmeCencSingleSampleDecrypter::GetChallengeData()
{
  return m_keyRequestData;
}

bool CEmeCencSingleSampleDecrypter::HasKeys() const
{
  return m_state.usableKeys > 0;
}

void CEmeCencSingleSampleDecrypter::GetCapabilities(DRM::Capabilities& caps)
{
  caps = {DRM::Capabilities::SECURE_PATH | DRM::Capabilities::ANNEXB_REQUIRED, m_hdcpVersion,
          m_hdcpLimit};

  if (caps.hdcpLimit == 0)
    caps.hdcpLimit = m_resolutionLimit;
}

bool CEmeCencSingleSampleDecrypter::SendSessionMessage(const std::vector<uint8_t>& challenge)
{
  const DRM::Config& drmCfg = m_config;
  const DRM::Config::License& licConfig = drmCfg.license;
  std::string reqData;

  if (!licConfig.isHttpGetRequest) // Make HTTP POST request
  {
    if (licConfig.reqData.empty()) // By default add raw challenge
    {
      reqData.assign(challenge.cbegin(), challenge.cend());
    }
    else
    {
      if (BASE64::IsValidBase64(licConfig.reqData))
        reqData = BASE64::DecodeToStr(licConfig.reqData);
      else //! @todo: this fallback as plain text must be removed when the deprecated DRM properties are removed, and so replace it to return error
        reqData = licConfig.reqData;

      // Some services have a customized license server that require data to be wrapped with their formats (e.g. JSON).
      // Here we provide a built-in way to customize the license data to be sent, this avoid force add-ons to integrate
      // an HTTP server proxy to manage the license data request/response, and so use Kodi properties to set wrappers.
      if (m_config.keySystem == DRM::KS_WIDEVINE &&
          !DRM::WvWrapLicense(reqData, challenge, m_sessionId, m_defaultKeyId, m_pssh,
                              licConfig.wrapper, drmCfg.isNewConfig))
      {
        return false;
      }
    }
  }

  std::string url = licConfig.serverUri;
  DRM::TranslateLicenseUrlPh(url, challenge, drmCfg.isNewConfig);

  CURL::CUrl cUrl{url, reqData};
  cUrl.AddHeaders(licConfig.reqHeaders);

  const int statusCode = cUrl.Open();

  if (statusCode == -1 || statusCode >= 400)
  {
    LOG::Log(LOGERROR, "License server returned failure (HTTP error %i)", statusCode);
    return false;
  }

  std::string respData;
  if (cUrl.Read(respData) == CURL::ReadStatus::ERROR)
  {
    LOG::LogF(LOGERROR, "Cannot read license server response");
    return false;
  }

  const std::string resLimit = cUrl.GetResponseHeader("X-Limit-Video"); // Custom header
  const std::string respContentType = cUrl.GetResponseHeader("Content-Type");

  if (m_config.keySystem == DRM::KS_WIDEVINE)
  {
    // Force limit playable streams resolutions
    m_resolutionLimit = DRM::ParseXLimitVideoHeader(resLimit);
  }

  // The first request could be the license certificate request
  // this request is done by sending a challenge of 2 bytes, 0x08 0x04 (CAQ=)
  const bool isCertRequest = challenge.size() == 2 && challenge[0] == 0x08 && challenge[1] == 0x04;

  // Unwrap license response
  if (!licConfig.unwrapper.empty() && m_config.keySystem == DRM::KS_WIDEVINE)
  {
    std::string unwrappedData;
    // Some services have a customized license server that require data to be wrapped with their formats (e.g. JSON).
    // Here we provide a built-in way to unwrap the license data received, this avoid force add-ons to integrate
    // a HTTP server proxy to manage the license data request/response, and so use Kodi properties to set wrappers.
    if (!DRM::WvUnwrapLicense(licConfig.unwrapper, licConfig.unwrapperParams, respContentType,
                              respData, unwrappedData, m_hdcpLimit, m_hdcpVersion))
    {
      return false;
    }
    respData = unwrappedData;
  }

  if (!isCertRequest)
  {
    if (m_config.keySystem == DRM::KS_PLAYREADY &&
        respData.find("<LicenseNonce>") == std::string::npos)
    {
      size_t dstPos = respData.find("</Licenses>");
      std::string challengeStr(challenge.cbegin(), challenge.cend());
      size_t srcPosS = challengeStr.find("<LicenseNonce>");
      if (dstPos != std::string::npos && srcPosS != std::string::npos)
      {
        LOG::Log(LOGDEBUG, "Injecting missing PlayReady <LicenseNonce> tag to license response");
        size_t srcPosE = challengeStr.find("</LicenseNonce>", srcPosS);
        if (srcPosE != std::string::npos)
          respData.insert(dstPos + 11, challengeStr.c_str() + srcPosS, srcPosE - srcPosS + 15);
      }
    }
  }

  // if offline request, will return keysetid for offline keys, otherwise empty
  const int updates = m_state.updates;
  tizen_drm_update(m_handle, reinterpret_cast<const uint8_t*>(respData.data()),
                   static_cast<int>(respData.size()));
  if (!WaitFor([&] { return m_state.updates > updates || m_state.error; }) || m_state.error)
  {
    LOG::LogF(LOGERROR, "The browser did not accept the license server response");
    return false;
  }

  LOG::Log(LOGDEBUG, "License update successful");
  return true;
}

AP4_Result CEmeCencSingleSampleDecrypter::SetFragmentInfo(AP4_UI32 poolId,
                                                          const std::vector<uint8_t>& keyId,
                                                          const AP4_UI08 nalLengthSize,
                                                          const std::vector<uint8_t>& annexbSpsPps,
                                                          DRM::Capabilities caps,
                                                          CryptoInfo cryptoInfo)
{
  if (poolId >= m_fragmentPool.size())
    return AP4_ERROR_OUT_OF_RANGE;

  m_fragmentPool[poolId].m_key = keyId;
  m_fragmentPool[poolId].m_nalLengthSize = nalLengthSize;
  m_fragmentPool[poolId].m_annexbSpsPps = annexbSpsPps;
  m_fragmentPool[poolId].capabilities = caps;

  return AP4_SUCCESS;
}

AP4_UI32 CEmeCencSingleSampleDecrypter::AddPool()
{
  for (size_t i(0); i < m_fragmentPool.size(); ++i)
    if (m_fragmentPool[i].m_nalLengthSize == 99)
    {
      m_fragmentPool[i].m_nalLengthSize = 0;
      return i;
    }
  m_fragmentPool.push_back(FINFO());
  m_fragmentPool.back().m_nalLengthSize = 0;
  return static_cast<AP4_UI32>(m_fragmentPool.size() - 1);
}

void CEmeCencSingleSampleDecrypter::RemovePool(AP4_UI32 poolId)
{
  m_fragmentPool[poolId].m_nalLengthSize = 99;
  m_fragmentPool[poolId].m_key.clear();
}

AP4_Result CEmeCencSingleSampleDecrypter::DecryptSampleData(AP4_UI32 poolId,
                                                           AP4_DataBuffer& dataIn,
                                                           AP4_DataBuffer& dataOut,
                                                           const AP4_UI08* iv,
                                                           unsigned int subsampleCount,
                                                           const AP4_UI16* bytesOfCleartextData,
                                                           const AP4_UI32* bytesOfEncryptedData,
                                                           DRM::DRMMediaType streamType)
{
  if (!m_handle)
    return AP4_ERROR_INVALID_STATE;

  FINFO& fragInfo(m_fragmentPool[poolId]);

  if (fragInfo.m_nalLengthSize > 4)
  {
    LOG::LogF(LOGERROR, "Nalu length size > 4 not supported");
    return AP4_ERROR_NOT_SUPPORTED;
  }

  AP4_DataBuffer payload;
  std::vector<SubsampleEntry> rebuiltSubs;
  const bool convertAnnexB = fragInfo.capabilities.HasFlag(DRM::Capabilities::ANNEXB_REQUIRED);

  // Convert only when safe to look at clear_bytes[0]
  if (fragInfo.m_nalLengthSize && (!iv || (subsampleCount > 0 && bytesOfCleartextData[0] > 0)))
  {
    AP4_Result res = ConvertToAnnexBandInject(dataIn, payload, subsampleCount, fragInfo, iv,
                                              bytesOfCleartextData, bytesOfEncryptedData,
                                              convertAnnexB, rebuiltSubs);

    if (res != AP4_SUCCESS)
    {
      LOG::LogF(LOGERROR, "ConvertToAnnexBandInject failed: %d", res);
      return res;
    }
  }
  else
  {
    payload.AppendData(dataIn.GetData(), dataIn.GetDataSize());

    if (subsampleCount)
    {
      rebuiltSubs.reserve(subsampleCount);
      for (unsigned i = 0; i < subsampleCount; ++i)
      {
        rebuiltSubs.push_back({bytesOfCleartextData[i], bytesOfEncryptedData[i]});
      }
    }
  }

  // If IV present but no subs were provided, treat as fully encrypted payload
  if (iv && rebuiltSubs.empty())
  {
    rebuiltSubs.push_back({0u, static_cast<uint32_t>(payload.GetDataSize())});
  }

  dataOut.SetDataSize(0);

  if (iv)
  {
    const AP4_UI32 subsOut = static_cast<AP4_UI32>(rebuiltSubs.size());
    std::vector<AP4_UI16> rebuiltBytesOfCleartextData(subsOut);
    std::vector<AP4_UI32> rebuiltBytesOfEncryptedData(subsOut);

    for (AP4_UI32 i = 0; i < subsOut; ++i)
    {
      rebuiltBytesOfCleartextData[i] = rebuiltSubs[i].clear_bytes;
      rebuiltBytesOfEncryptedData[i] = rebuiltSubs[i].cipher_bytes;
    }

    // CENC-like prefix: [count:32][clear[]:16*N][cipher[]:32*N][IV:16][key]
    dataOut.SetData(reinterpret_cast<const AP4_Byte*>(&subsOut), sizeof(AP4_UI32));
    dataOut.AppendData(reinterpret_cast<const AP4_Byte*>(rebuiltBytesOfCleartextData.data()),
                       subsOut * sizeof(AP4_UI16));
    dataOut.AppendData(reinterpret_cast<const AP4_Byte*>(rebuiltBytesOfEncryptedData.data()),
                       subsOut * sizeof(AP4_UI32));
    dataOut.AppendData(reinterpret_cast<const AP4_Byte*>(iv), 16);
    dataOut.AppendData(fragInfo.m_key.data(), static_cast<AP4_Size>(fragInfo.m_key.size()));
  }
  else
  {
    fragInfo.m_annexbSpsPps.clear();
  }

  // Prefix (if IV) + raw payload
  dataOut.AppendData(payload.GetData(), payload.GetDataSize());

  return AP4_SUCCESS;
}

AP4_Result CEmeCencSingleSampleDecrypter::ConvertToAnnexBandInject(
    AP4_DataBuffer& dataIn,
    AP4_DataBuffer& dataOut,
    unsigned int subsampleCount,
    FINFO& fragInfo,
    const AP4_UI08* iv,
    const AP4_UI16* bytesOfCleartextData,
    const AP4_UI32* bytesOfEncryptedData,
    bool convertAnnexB,
    std::vector<SubsampleEntry>& rebuiltSubs)
{
  //check NAL / subsample
  const AP4_Byte *packetIn(dataIn.GetData()), *packetInEnd(dataIn.GetData() + dataIn.GetDataSize());
  // Byte position of "bytesOfCleartextData" where to set the size of the data,
  // by default starts after the subsample count data (so the size of subsampleCount data type)
  size_t clrDataBytePos = sizeof(subsampleCount);
  // size_t nalUnitCount = 0; // is there a use for this?
  size_t nalUnitSum = 0;

  // Prepare subs from originals
  rebuiltSubs.clear();
  if (iv && subsampleCount)
  {
    rebuiltSubs.reserve(subsampleCount);
    for (unsigned i = 0; i < subsampleCount; ++i)
    {
      SubsampleEntry e;
      e.clear_bytes = bytesOfCleartextData[i];
      e.cipher_bytes = bytesOfEncryptedData[i];
      rebuiltSubs.push_back(e);
    }
  }

  size_t curSub = 0;
  bool injectedStartupNALs = false;

  while (packetIn < packetInEnd)
  {
    // Read length-prefixed NAL size
    uint32_t nalSize = 0;
    for (size_t i = 0; i < fragInfo.m_nalLengthSize; ++i)
    {
      nalSize = (nalSize << 8) + *packetIn++;
    }

    // Inject SPS/PPS once before first non-AUD
    if (!injectedStartupNALs && !fragInfo.m_annexbSpsPps.empty() &&
        ((*packetIn & 0x1F) != 9 /*AVC_NAL_AUD*/))
    {
      dataOut.AppendData(fragInfo.m_annexbSpsPps.data(),
                         static_cast<AP4_Size>(fragInfo.m_annexbSpsPps.size()));

      if (iv && curSub < rebuiltSubs.size())
      {
        // attribute injected bytes to clear region of current subsample
        rebuiltSubs[curSub].clear_bytes += static_cast<uint32_t>(fragInfo.m_annexbSpsPps.size());
      }

      fragInfo.m_annexbSpsPps.clear();
      injectedStartupNALs = true;
    }

    // Write Annex-B start code and payload
    static const AP4_Byte annexbStartCode[4] = {0x00, 0x00, 0x00, 0x01};
    if (convertAnnexB && !(nalSize >= 4 && packetIn[0] == 0x00 && packetIn[1] == 0x00 &&
                           packetIn[2] == 0x00 && packetIn[3] == 0x01))
    {
      dataOut.AppendData(annexbStartCode, 4);
    }

    dataOut.AppendData(packetIn, nalSize);
    packetIn += nalSize;

    if (iv)
    {
      // Start-code delta: 4 - nalLengthSize (0,2,3) goes to clear_bytes of current subsample
      if (curSub < rebuiltSubs.size())
      {
        rebuiltSubs[curSub].clear_bytes += static_cast<uint32_t>(4 - fragInfo.m_nalLengthSize);
      }
    }

    if (!iv)
    {
      nalUnitSum = 0;
    }
    else if (nalSize + fragInfo.m_nalLengthSize + nalUnitSum >=
             *bytesOfCleartextData + *bytesOfEncryptedData)
    {
      // We may cross one or more subsample boundaries; advance pointers and curSub in lockstep
      AP4_UI32 summedBytes = 0;
      size_t advanced = 0;

      do
      {
        summedBytes += *bytesOfCleartextData + *bytesOfEncryptedData;
        ++bytesOfCleartextData;
        ++bytesOfEncryptedData;
        ++advanced;
        --subsampleCount;
      } while (subsampleCount && nalSize + fragInfo.m_nalLengthSize + nalUnitSum > summedBytes);

      if (nalSize + fragInfo.m_nalLengthSize + nalUnitSum > summedBytes)
      {
        LOG::LogF(LOGERROR, "NAL Unit exceeds subsample definition (nls: %u) %u -> %u ",
                  static_cast<unsigned>(fragInfo.m_nalLengthSize),
                  static_cast<unsigned>(nalSize + fragInfo.m_nalLengthSize + nalUnitSum),
                  summedBytes);
        return AP4_ERROR_NOT_SUPPORTED;
      }

      curSub += advanced;
      nalUnitSum = 0;
    }
    else
    {
      nalUnitSum += nalSize + fragInfo.m_nalLengthSize;
    }
  }

  if (packetIn != packetInEnd || subsampleCount)
  {
    LOG::Log(LOGERROR, "NAL Unit definition incomplete (nls: %u) %u -> %u ",
             static_cast<unsigned>(fragInfo.m_nalLengthSize),
             static_cast<unsigned>(packetInEnd - packetIn), subsampleCount);
    return AP4_ERROR_NOT_SUPPORTED;
  }

  return AP4_SUCCESS;
}
