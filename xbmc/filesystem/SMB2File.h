/*
 *  Copyright (C) 2026 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

#include "IFile.h"
#include "SMB2Connection.h"
#include "URL.h"

struct smb2fh;
struct smb2_stat_64;

namespace XFILE
{
/*!
 * \brief smb:// through libsmb2, for platforms that cannot have libsmbclient.
 *
 * Speaks SMB 2 and 3 only, which every server since Windows Vista and Samba 3.6 offers.
 */
class CSMB2File : public IFile
{
public:
  CSMB2File() = default;
  ~CSMB2File() override;

  bool Open(const CURL& url) override;
  bool OpenForWrite(const CURL& url, bool bOverWrite = false) override;
  void Close() override;

  ssize_t Read(void* lpBuf, size_t uiBufSize) override;
  ssize_t Write(const void* lpBuf, size_t uiBufSize) override;
  int64_t Seek(int64_t iFilePosition, int iWhence = SEEK_SET) override;
  int Truncate(int64_t size) override;
  int64_t GetPosition() override { return m_position; }
  int64_t GetLength() override { return m_size; }
  int GetChunkSize() override { return static_cast<int>(m_chunkSize); }
  int IoControl(IOControl request, void* param) override
  {
    return request == IOControl::SEEK_POSSIBLE ? 1 : -1;
  }

  bool Exists(const CURL& url) override;
  int Stat(const CURL& url, struct __stat64* buffer) override;
  int Stat(struct __stat64* buffer) override;
  bool Delete(const CURL& url) override;
  bool Rename(const CURL& url, const CURL& urlnew) override;

private:
  bool OpenHandle(const CURL& url, int flags);
  bool Reopen();
  bool StatHandle(smb2_stat_64& st);

  CURL m_url;
  std::string m_path; //!< below the share

  SMB2::CConnectionPtr m_connection;
  smb2fh* m_handle{nullptr};
  int64_t m_position{0};
  int64_t m_size{0};
  uint32_t m_chunkSize{0};
};
} // namespace XFILE
