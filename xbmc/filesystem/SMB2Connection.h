/*
 *  Copyright (C) 2026 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

#include <cerrno>
#include <memory>
#include <string>

class CURL;
struct smb2_context;

namespace XFILE::SMB2
{

/*!
 * \brief An smb:// URL split up the way libsmb2 takes it.
 */
struct CLocation
{
  explicit CLocation(const CURL& url);

  std::string server; //!< host, or host:port for a port other than 445
  std::string share;
  std::string path; //!< below the share, without a leading slash
  std::string domain;
  std::string user;
  std::string password;

  //! Connections are shared between users of the same server, share and account.
  std::string Key() const;
};

class CConnection;

/*!
 * \brief A connection to a share, taken from a pool and given back to it when released.
 *
 * libsmb2 contexts are not thread safe, so a connection is only ever used by its holder.
 * Connecting costs several round trips, so released connections are kept for a while.
 */
class CConnectionPtr
{
public:
  CConnectionPtr();
  CConnectionPtr(CConnectionPtr&& other) noexcept;
  CConnectionPtr& operator=(CConnectionPtr&& other) noexcept;
  ~CConnectionPtr();

  explicit operator bool() const { return m_connection != nullptr; }
  smb2_context* Get() const;

  //! A connection that failed is closed instead of being kept.
  void Discard() { m_discard = true; }
  bool IsDiscarded() const { return m_discard; }

  //! Hands the connection back to the pool.
  void Release();

private:
  friend class CPool;
  CConnectionPtr(std::unique_ptr<CConnection> connection, std::string key);

  std::unique_ptr<CConnection> m_connection;
  std::string m_key;
  bool m_discard{false};
};

enum class ConnectResult
{
  OK,
  AUTH_FAILED,
  FAILED,
};

/*!
 * \brief Gets a connection to the share of a location, connected to IPC$ when it has none.
 * \param error set to libsmb2's message when connecting fails
 */
CConnectionPtr Connect(const CLocation& location, ConnectResult& result, std::string& error);

//! Whether an error leaves the connection unusable.
bool IsConnectionError(int err);

/*!
 * \brief Runs an operation on a connection to a location, and once more on a new connection
 * when the one it got had gone stale.
 * \param operation called with the libsmb2 context, returns 0 or more, or -errno
 * \return what the operation returned, or -EACCES or -EHOSTUNREACH when connecting failed
 */
template<typename Operation>
int Run(const CLocation& location, Operation&& operation)
{
  for (int attempt = 0; attempt < 2; ++attempt)
  {
    ConnectResult result;
    std::string error;
    CConnectionPtr connection = Connect(location, result, error);
    if (!connection)
      return result == ConnectResult::AUTH_FAILED ? -EACCES : -EHOSTUNREACH;
    const int ret = operation(connection.Get());
    if (ret < 0 && IsConnectionError(ret))
    {
      connection.Discard();
      continue;
    }
    return ret;
  }
  return -EIO;
}

} // namespace XFILE::SMB2
