/*
 *  Copyright (C) 2026 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

#include "network/Network.h"

#include <string>
#include <vector>

class CNetworkInterfaceWasm : public CNetworkInterface
{
public:
  CNetworkInterfaceWasm() = default;
  ~CNetworkInterfaceWasm() override = default;

  bool IsEnabled() const override { return true; }
  bool IsConnected() const override;

  std::string GetMacAddress() const override;
  void GetMacAddressRaw(char rawMac[6]) const override;

  bool GetHostMacAddress(unsigned long host, std::string& mac) const override { return false; }

  // A browser does not expose the interface addresses, a Samsung TV does. Without them
  // loopback at least gives CNetworkBase::HasInterfaceForIP() a subnet to match against.
  std::string GetCurrentIPAddress() const override;
  std::string GetCurrentNetmask() const override;
  std::string GetCurrentDefaultGateway() const override;
};

class CNetworkWasm : public CNetworkBase
{
public:
  CNetworkWasm();
  ~CNetworkWasm() override;

  std::vector<CNetworkInterface*>& GetInterfaceList() override;
  bool PingHost(unsigned long host, unsigned int timeout_ms = 2000) override;
  std::vector<std::string> GetNameServers() override;

private:
  CNetworkInterfaceWasm m_iface;
  std::vector<CNetworkInterface*> m_interfaces;
};
