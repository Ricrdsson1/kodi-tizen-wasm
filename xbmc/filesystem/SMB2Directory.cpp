/*
 *  Copyright (C) 2026 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "SMB2Directory.h"

#include "FileItem.h"
#include "FileItemList.h"
#include "PasswordManager.h"
#include "SMB2Connection.h"
#include "URL.h"
#include "utils/URIUtils.h"
#include "utils/XTimeUtils.h"
#include "utils/log.h"

#include <cstring>
#include <memory>
#include <vector>

// libsmb2's headers rely on being included in this order
// clang-format off
#include <smb2/smb2.h>
#include <smb2/libsmb2.h>
#include <smb2/libsmb2-raw.h>
#include <smb2/libsmb2-share-enum.h>
// clang-format on

using namespace XFILE;

namespace
{
SMB2::CLocation Locate(const CURL& url)
{
  CURL authenticated(url);
  CPasswordManager::GetInstance().AuthenticateURL(authenticated);
  return SMB2::CLocation(authenticated);
}

KODI::TIME::FileTime ToLocalFileTime(uint64_t time)
{
  KODI::TIME::FileTime fileTime{};
  KODI::TIME::FileTime localTime{};
  KODI::TIME::TimeTToFileTime(static_cast<time_t>(time), &fileTime);
  KODI::TIME::FileTimeToLocalFileTime(&fileTime, &localTime);
  return localTime;
}
} // namespace

bool CSMB2Directory::HandleError(const CURL& url, int err)
{
  err = err < 0 ? -err : err;
  CLog::Log(LOGERROR, "SMB2: cannot list {}: {}", url.GetRedacted(), strerror(err));
  if (m_flags & DIR_FLAG_ALLOW_PROMPT)
  {
    if (err == EACCES)
      RequireAuthentication(url);
    else
      SetErrorDialog(257, strerror(err));
  }
  return false;
}

bool CSMB2Directory::GetShares(const CURL& url, CFileItemList& items)
{
  const SMB2::CLocation location = Locate(url);
  std::vector<std::string> shares;
  const int ret = SMB2::Run(
      location,
      [&](smb2_context* smb2)
      {
        shares.clear();
        smb2_share_enum_reply* reply = smb2_share_enum_sync(smb2, SMB2_SHARE_INFO_1);
        if (!reply)
          return -ECONNRESET;
        for (uint32_t i = 0; i < reply->entries_read; ++i)
        {
          const smb2_share_info_1& info = reply->share_info.info_1[i];
          // Printers, IPC$ and the administrative shares (C$, ADMIN$) cannot be browsed.
          if ((info.type & 3) != SMB2_SHARE_TYPE_DISKTREE || (info.type & SMB2_SHARE_TYPE_HIDDEN) ||
              !info.netname)
            continue;
          const std::string name = info.netname;
          if (!name.empty() && name.back() != '$')
            shares.push_back(name);
        }
        smb2_free_data(smb2, reply);
        return 0;
      });
  if (ret < 0)
    return HandleError(url, ret);

  std::string root = url.Get();
  URIUtils::AddSlashAtEnd(root);
  for (const std::string& share : shares)
  {
    auto item = std::make_shared<CFileItem>(share);
    item->SetPath(root + share + "/");
    item->SetFolder(true);
    items.Add(std::move(item));
  }
  return true;
}

bool CSMB2Directory::GetDirectory(const CURL& url, CFileItemList& items)
{
  // Servers are not discovered, there is no list of them to show.
  if (url.GetHostName().empty())
    return false;

  const SMB2::CLocation location = Locate(url);
  if (location.share.empty())
    return GetShares(url, items);

  std::string root = url.Get();
  URIUtils::AddSlashAtEnd(root);

  std::vector<CFileItemPtr> fileItems;
  const int ret = SMB2::Run(
      location,
      [&](smb2_context* smb2)
      {
        fileItems.clear();
        smb2dir* dir = smb2_opendir(smb2, location.path.c_str());
        if (!dir)
        {
          const int err = nterror_to_errno(static_cast<uint32_t>(smb2_get_nterror(smb2)));
          return err ? -err : -ECONNRESET;
        }

        while (const smb2dirent* entry = smb2_readdir(smb2, dir))
        {
          const std::string name = entry->name ? entry->name : "";
          if (name.empty() || name == "." || name == "..")
            continue;

          const bool isDir = entry->st.smb2_type == SMB2_TYPE_DIRECTORY;
          std::string path = root + name;
          if (isDir)
            URIUtils::AddSlashAtEnd(path);

          auto item = std::make_shared<CFileItem>(name);
          item->SetPath(path);
          item->SetFolder(isDir);
          if (!isDir)
            item->SetSize(static_cast<int64_t>(entry->st.smb2_size));
          const uint64_t mtime = entry->st.smb2_mtime ? entry->st.smb2_mtime : entry->st.smb2_ctime;
          item->SetDateTime(ToLocalFileTime(mtime));
          item->SetProperty(DIR_PROPERTY_STAT_MTIME, static_cast<int64_t>(entry->st.smb2_mtime));
          item->SetProperty(DIR_PROPERTY_STAT_CTIME, static_cast<int64_t>(entry->st.smb2_ctime));
          if (name.starts_with('.'))
            item->SetProperty("file:hidden", true);
          fileItems.push_back(std::move(item));
        }
        smb2_closedir(smb2, dir);
        return 0;
      });
  if (ret < 0)
    return HandleError(url, ret);

  items.AddItems(std::move(fileItems));
  return true;
}

bool CSMB2Directory::Create(const CURL& url)
{
  const SMB2::CLocation location = Locate(url);
  if (location.share.empty() || location.path.empty())
    return false;
  const int ret = SMB2::Run(location, [&](smb2_context* smb2)
                            { return smb2_mkdir(smb2, location.path.c_str()); });
  return ret == 0 || Exists(url);
}

bool CSMB2Directory::Exists(const CURL& url)
{
  const SMB2::CLocation location = Locate(url);
  if (location.share.empty())
    return !location.server.empty();
  smb2_stat_64 st{};
  const int ret = SMB2::Run(location, [&](smb2_context* smb2)
                            { return smb2_stat(smb2, location.path.c_str(), &st); });
  return ret == 0 && st.smb2_type == SMB2_TYPE_DIRECTORY;
}

bool CSMB2Directory::Remove(const CURL& url)
{
  const SMB2::CLocation location = Locate(url);
  if (location.share.empty() || location.path.empty())
    return false;
  const int ret = SMB2::Run(location, [&](smb2_context* smb2)
                            { return smb2_rmdir(smb2, location.path.c_str()); });
  return ret == 0 || ret == -ENOENT;
}
