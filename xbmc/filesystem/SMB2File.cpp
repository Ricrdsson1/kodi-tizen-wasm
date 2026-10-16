/*
 *  Copyright (C) 2026 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "SMB2File.h"

#include "PasswordManager.h"
#include "utils/log.h"

#include <algorithm>
#include <cstring>

#include <fcntl.h>
#include <sys/stat.h>

// libsmb2's headers rely on being included in this order
// clang-format off
#include <smb2/smb2.h>
#include <smb2/libsmb2.h>
// clang-format on

using namespace XFILE;

namespace
{
// Asked for when the server does not say how much it reads at once.
constexpr uint32_t DEFAULT_CHUNK_SIZE = 64 * 1024;
// More than this is not read at once even when the server allows it, so a read on a slow
// network does not run into the request timeout.
constexpr uint32_t MAX_CHUNK_SIZE = 1024 * 1024;

SMB2::CLocation Locate(const CURL& url)
{
  CURL authenticated(url);
  CPasswordManager::GetInstance().AuthenticateURL(authenticated);
  return SMB2::CLocation(authenticated);
}

void ToStat(const smb2_stat_64& st, struct __stat64* buffer)
{
  *buffer = {};
  buffer->st_ino = st.smb2_ino;
  buffer->st_nlink = st.smb2_nlink;
  buffer->st_size = st.smb2_size;
  buffer->st_atime = st.smb2_atime;
  buffer->st_mtime = st.smb2_mtime;
  buffer->st_ctime = st.smb2_ctime;
  buffer->st_mode = st.smb2_type == SMB2_TYPE_DIRECTORY ? S_IFDIR | 0755 : S_IFREG | 0644;
}
} // namespace

CSMB2File::~CSMB2File()
{
  Close();
}

bool CSMB2File::OpenHandle(const CURL& url, int flags)
{
  Close();

  const SMB2::CLocation location = Locate(url);
  if (location.share.empty() || location.path.empty())
    return false;

  // A kept connection may have gone stale, so a failure is tried again on a new one.
  for (int attempt = 0; attempt < 2 && !m_handle; ++attempt)
  {
    SMB2::ConnectResult result;
    std::string error;
    m_connection = SMB2::Connect(location, result, error);
    if (!m_connection)
      return false;

    m_handle = smb2_open(m_connection.Get(), location.path.c_str(), flags);
    if (!m_handle)
    {
      CLog::Log(LOGDEBUG, "SMB2: cannot open {}: {}", url.GetRedacted(),
                smb2_get_error(m_connection.Get()));
      if (smb2_get_nterror(m_connection.Get()) == 0)
        m_connection.Discard(); // no answer from the server
      else
        attempt = 1;
      m_connection.Release();
    }
  }
  if (!m_handle)
    return false;

  m_path = location.path;
  smb2_stat_64 st{};
  if (StatHandle(st))
    m_size = static_cast<int64_t>(st.smb2_size);

  const uint32_t maxRead = smb2_get_max_read_size(m_connection.Get());
  m_chunkSize = maxRead ? std::min(maxRead, MAX_CHUNK_SIZE) : DEFAULT_CHUNK_SIZE;
  m_position = 0;
  m_url = url;
  return true;
}

bool CSMB2File::Reopen()
{
  const int64_t position = m_position;
  if (!OpenHandle(CURL(m_url), O_RDONLY))
    return false;
  m_position = position;
  CLog::Log(LOGINFO, "SMB2: reconnected to {}", m_url.GetRedacted());
  return true;
}

bool CSMB2File::Open(const CURL& url)
{
  if (!OpenHandle(url, O_RDONLY))
    return false;
  CLog::Log(LOGDEBUG, "SMB2: opened {} ({} bytes)", url.GetRedacted(), m_size);
  return true;
}

bool CSMB2File::OpenForWrite(const CURL& url, bool bOverWrite)
{
  const int flags = O_WRONLY | O_CREAT | (bOverWrite ? O_TRUNC : 0);
  if (!OpenHandle(url, flags))
    return false;
  if (!bOverWrite)
    m_position = m_size;
  return true;
}

void CSMB2File::Close()
{
  if (m_handle)
  {
    // Closing on a connection that is gone would only wait for the request timeout.
    if (!m_connection.IsDiscarded())
      smb2_close(m_connection.Get(), m_handle);
    m_handle = nullptr;
  }
  m_connection.Release();
  m_position = 0;
  m_size = 0;
}

ssize_t CSMB2File::Read(void* lpBuf, size_t uiBufSize)
{
  if (!m_handle)
    return -1;
  if (m_position >= m_size)
    return 0;

  const uint32_t count = static_cast<uint32_t>(std::min<size_t>(uiBufSize, m_chunkSize));
  int ret = smb2_pread(m_connection.Get(), m_handle, static_cast<uint8_t*>(lpBuf), count,
                       static_cast<uint64_t>(m_position));
  // A file that is played over Wi-Fi survives the connection dropping for a moment.
  if (ret < 0 && SMB2::IsConnectionError(ret))
  {
    CLog::Log(LOGWARNING, "SMB2: read failed at {}: {}", m_position,
              smb2_get_error(m_connection.Get()));
    m_connection.Discard();
    if (Reopen())
      ret = smb2_pread(m_connection.Get(), m_handle, static_cast<uint8_t*>(lpBuf), count,
                       static_cast<uint64_t>(m_position));
  }
  if (ret < 0)
  {
    CLog::Log(LOGERROR, "SMB2: read failed at {}: {}", m_position,
              m_connection ? smb2_get_error(m_connection.Get()) : "not connected");
    return -1;
  }
  m_position += ret;
  return ret;
}

ssize_t CSMB2File::Write(const void* lpBuf, size_t uiBufSize)
{
  if (!m_handle)
    return -1;

  const uint32_t maxWrite = smb2_get_max_write_size(m_connection.Get());
  const uint32_t count = static_cast<uint32_t>(
      std::min<size_t>(uiBufSize, maxWrite ? std::min(maxWrite, MAX_CHUNK_SIZE) : DEFAULT_CHUNK_SIZE));
  const int ret =
      smb2_pwrite(m_connection.Get(), m_handle, static_cast<const uint8_t*>(lpBuf), count,
                  static_cast<uint64_t>(m_position));
  if (ret < 0)
  {
    CLog::Log(LOGERROR, "SMB2: write failed: {}", smb2_get_error(m_connection.Get()));
    if (SMB2::IsConnectionError(ret))
      m_connection.Discard();
    return -1;
  }
  m_position += ret;
  m_size = std::max(m_size, m_position);
  return ret;
}

int64_t CSMB2File::Seek(int64_t iFilePosition, int iWhence)
{
  if (!m_handle)
    return -1;

  int64_t position;
  switch (iWhence)
  {
    case SEEK_SET:
      position = iFilePosition;
      break;
    case SEEK_CUR:
      position = m_position + iFilePosition;
      break;
    case SEEK_END:
      position = m_size + iFilePosition;
      break;
    default:
      return -1;
  }
  if (position < 0)
    return -1;
  m_position = position;
  return m_position;
}

int CSMB2File::Truncate(int64_t size)
{
  if (!m_handle)
    return -1;
  if (smb2_ftruncate(m_connection.Get(), m_handle, static_cast<uint64_t>(size)) < 0)
    return -1;
  m_size = size;
  return 0;
}

int CSMB2File::Stat(const CURL& url, struct __stat64* buffer)
{
  const SMB2::CLocation location = Locate(url);
  if (location.share.empty())
    return -1;

  smb2_stat_64 st{};
  const int ret = SMB2::Run(location,
                            [&](smb2_context* smb2)
                            { return smb2_stat(smb2, location.path.c_str(), &st); });
  if (ret < 0)
  {
    errno = -ret;
    return -1;
  }
  if (buffer)
    ToStat(st, buffer);
  return 0;
}

bool CSMB2File::StatHandle(smb2_stat_64& st)
{
  // Not every server answers what libsmb2 asks of an open file, so asking for the path
  // is the fallback.
  return smb2_fstat(m_connection.Get(), m_handle, &st) == 0 ||
         smb2_stat(m_connection.Get(), m_path.c_str(), &st) == 0;
}

int CSMB2File::Stat(struct __stat64* buffer)
{
  if (!m_handle)
    return -1;
  smb2_stat_64 st{};
  if (!StatHandle(st))
    return -1;
  if (buffer)
    ToStat(st, buffer);
  return 0;
}

bool CSMB2File::Exists(const CURL& url)
{
  struct __stat64 buffer;
  return Stat(url, &buffer) == 0 && S_ISREG(buffer.st_mode);
}

bool CSMB2File::Delete(const CURL& url)
{
  const SMB2::CLocation location = Locate(url);
  if (location.share.empty() || location.path.empty())
    return false;
  return SMB2::Run(location, [&](smb2_context* smb2)
                   { return smb2_unlink(smb2, location.path.c_str()); }) == 0;
}

bool CSMB2File::Rename(const CURL& url, const CURL& urlnew)
{
  const SMB2::CLocation location = Locate(url);
  const SMB2::CLocation target = Locate(urlnew);
  if (location.share.empty() || location.path.empty() || target.path.empty() ||
      location.Key() != target.Key())
    return false;
  return SMB2::Run(location,
                   [&](smb2_context* smb2) {
                     return smb2_rename(smb2, location.path.c_str(), target.path.c_str());
                   }) == 0;
}
