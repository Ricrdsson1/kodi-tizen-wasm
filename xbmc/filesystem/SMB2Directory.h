/*
 *  Copyright (C) 2026 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

#include "IDirectory.h"

namespace XFILE
{
/*!
 * \brief smb:// directories through libsmb2, see CSMB2File.
 *
 * Lists the shares of a server and the files of a share. Finding servers on the network
 * is not supported, a server has to be given by name or address.
 */
class CSMB2Directory : public IDirectory
{
public:
  bool GetDirectory(const CURL& url, CFileItemList& items) override;
  CacheType GetCacheType(const CURL& url) const override { return CacheType::ONCE; }
  bool Create(const CURL& url) override;
  bool Exists(const CURL& url) override;
  bool Remove(const CURL& url) override;

private:
  bool GetShares(const CURL& url, CFileItemList& items);
  bool HandleError(const CURL& url, int err);
};
} // namespace XFILE
