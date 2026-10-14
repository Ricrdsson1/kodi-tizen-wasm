/*
 *  Copyright (C) 2026 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "NetworkWasm.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <emscripten.h>

bool CNetworkInterfaceWasm::IsConnected() const
{
  return EM_ASM_INT({
    return (typeof navigator !== "undefined" && typeof navigator.onLine === "boolean")
             ? (navigator.onLine ? 1 : 0)
             : 1;
  }) != 0;
}

namespace
{
/*!
 * \brief A value of Samsung's network API, which a Tizen TV offers applications with the
 * network.public privilege, e.g. "getIp".
 * \return empty if unknown
 */
std::string SamsungNetworkValue(const char* getter)
{
  char* value = static_cast<char*>(MAIN_THREAD_EM_ASM_PTR(
      {
        try
        {
          var network = typeof webapis !== 'undefined' ? webapis.network : undefined;
          var getter = UTF8ToString($0);
          if (!network || typeof network[getter] !== 'function')
            return 0;
          var value = network[getter]();
          return value ? stringToNewUTF8(String(value)) : 0;
        }
        catch (e)
        {
          return 0;
        }
      },
      getter));
  if (!value)
    return {};
  std::string result(value);
  free(value);
  // The TV reports an unconfigured address as all zeroes.
  return result == "0.0.0.0" ? std::string() : result;
}

std::string ValueOr(const char* getter, const char* fallback)
{
  std::string value = SamsungNetworkValue(getter);
  return value.empty() ? fallback : value;
}
} // unnamed namespace

std::string CNetworkInterfaceWasm::GetMacAddress() const
{
  return ValueOr("getMac", "00:00:00:00:00:00");
}

void CNetworkInterfaceWasm::GetMacAddressRaw(char rawMac[6]) const
{
  std::memset(rawMac, 0, 6);
  unsigned int bytes[6];
  if (std::sscanf(GetMacAddress().c_str(), "%x:%x:%x:%x:%x:%x", &bytes[0], &bytes[1], &bytes[2],
                  &bytes[3], &bytes[4], &bytes[5]) == 6)
  {
    for (int i = 0; i < 6; ++i)
      rawMac[i] = static_cast<char>(bytes[i]);
  }
}

std::string CNetworkInterfaceWasm::GetCurrentIPAddress() const
{
  return ValueOr("getIp", "127.0.0.1");
}

std::string CNetworkInterfaceWasm::GetCurrentNetmask() const
{
  // Only valid together with the address it belongs to.
  if (SamsungNetworkValue("getIp").empty())
    return "255.0.0.0";
  return ValueOr("getSubnetMask", "255.255.255.0");
}

std::string CNetworkInterfaceWasm::GetCurrentDefaultGateway() const
{
  return ValueOr("getGateway", "127.0.0.1");
}

CNetworkWasm::CNetworkWasm()
{
  m_interfaces.push_back(&m_iface);
}

CNetworkWasm::~CNetworkWasm() = default;

std::unique_ptr<CNetworkBase> CNetworkBase::GetNetwork()
{
  return std::make_unique<CNetworkWasm>();
}

std::vector<CNetworkInterface*>& CNetworkWasm::GetInterfaceList()
{
  return m_interfaces;
}

bool CNetworkWasm::PingHost(unsigned long /*host*/, unsigned int /*timeout_ms*/)
{
  return false;
}

std::vector<std::string> CNetworkWasm::GetNameServers()
{
  const std::string dns = SamsungNetworkValue("getDns");
  if (dns.empty())
    return {};
  return {dns};
}
