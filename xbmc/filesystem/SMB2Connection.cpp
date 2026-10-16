/*
 *  Copyright (C) 2026 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "SMB2Connection.h"

#include "URL.h"
#include "utils/log.h"

#include <chrono>
#include <cerrno>
#include <map>
#include <mutex>
#include <sstream>
#include <thread>
#include <vector>

#include <poll.h>

// libsmb2's headers rely on being included in this order
// clang-format off
#include <smb2/smb2.h>
#include <smb2/libsmb2.h>
// clang-format on

using namespace XFILE::SMB2;

namespace
{
// Servers keep an idle session for much longer, but a TV may have slept in between.
constexpr auto IDLE_TIMEOUT = std::chrono::seconds(60);
// Seconds after which a request is given up, so a server that went away cannot hang Kodi.
constexpr int REQUEST_TIMEOUT = 20;
constexpr int DEFAULT_PORT = 445;
} // namespace

CLocation::CLocation(const CURL& url)
{
  server = url.GetHostName();
  if (url.HasPort() && url.GetPort() != DEFAULT_PORT)
    server += ":" + std::to_string(url.GetPort());

  std::string file = url.GetFileName();
  while (!file.empty() && (file.front() == '/' || file.front() == '\\'))
    file.erase(0, 1);
  while (!file.empty() && (file.back() == '/' || file.back() == '\\'))
    file.pop_back();
  const size_t slash = file.find('/');
  share = file.substr(0, slash);
  if (slash != std::string::npos)
    path = file.substr(slash + 1);

  domain = url.GetDomain();
  user = url.GetUserName();
  // smb://DOMAIN;user@server is how libsmbclient took a domain
  if (const size_t semicolon = user.find(';'); semicolon != std::string::npos)
  {
    domain = user.substr(0, semicolon);
    user.erase(0, semicolon + 1);
  }
  password = url.GetPassWord();
}

std::string CLocation::Key() const
{
  return domain + ";" + user + "@" + server + "/" + share;
}

namespace XFILE::SMB2
{

class CConnection
{
public:
  CConnection() : m_context(smb2_init_context()) {}
  ~CConnection()
  {
    if (m_context)
    {
      if (m_connected)
        smb2_disconnect_share(m_context);
      smb2_destroy_context(m_context);
    }
  }
  CConnection(const CConnection&) = delete;
  CConnection& operator=(const CConnection&) = delete;

  //! An idle connection has nothing to read, unless the server has closed it.
  bool IsAlive() const
  {
    pollfd pfd{};
    pfd.fd = smb2_get_fd(m_context);
    pfd.events = POLLIN;
    return pfd.fd >= 0 && poll(&pfd, 1, 0) == 0;
  }

  smb2_context* m_context;
  bool m_connected{false};
  std::chrono::steady_clock::time_point m_released;
};

class CPool
{
public:
  static CPool& Get()
  {
    static CPool pool;
    return pool;
  }

  CConnectionPtr Take(const std::string& key)
  {
    std::unique_lock lock(m_lock);
    Prune();
    auto it = m_idle.find(key);
    if (it == m_idle.end() || it->second.empty())
      return {};
    while (!it->second.empty())
    {
      std::unique_ptr<CConnection> connection = std::move(it->second.back());
      it->second.pop_back();
      if (connection->IsAlive())
        return CConnectionPtr(std::move(connection), key);
    }
    return {};
  }

  CConnectionPtr Wrap(std::unique_ptr<CConnection> connection, const std::string& key)
  {
    return CConnectionPtr(std::move(connection), key);
  }

  void Give(std::unique_ptr<CConnection> connection, const std::string& key)
  {
    connection->m_released = std::chrono::steady_clock::now();
    std::unique_lock lock(m_lock);
    m_idle[key].push_back(std::move(connection));
    Prune();
  }

private:
  void Prune()
  {
    const auto now = std::chrono::steady_clock::now();
    for (auto it = m_idle.begin(); it != m_idle.end();)
    {
      auto& list = it->second;
      std::erase_if(list, [now](const std::unique_ptr<CConnection>& connection)
                    { return now - connection->m_released > IDLE_TIMEOUT; });
      it = list.empty() ? m_idle.erase(it) : std::next(it);
    }
  }

  std::mutex m_lock;
  std::map<std::string, std::vector<std::unique_ptr<CConnection>>> m_idle;
};

CConnectionPtr::CConnectionPtr() = default;

CConnectionPtr::CConnectionPtr(std::unique_ptr<CConnection> connection, std::string key)
  : m_connection(std::move(connection)), m_key(std::move(key))
{
}

CConnectionPtr::CConnectionPtr(CConnectionPtr&& other) noexcept
  : m_connection(std::move(other.m_connection)),
    m_key(std::move(other.m_key)),
    m_discard(other.m_discard)
{
}

CConnectionPtr& CConnectionPtr::operator=(CConnectionPtr&& other) noexcept
{
  if (this != &other)
  {
    Release();
    m_connection = std::move(other.m_connection);
    m_key = std::move(other.m_key);
    m_discard = other.m_discard;
  }
  return *this;
}

CConnectionPtr::~CConnectionPtr()
{
  Release();
}

smb2_context* CConnectionPtr::Get() const
{
  return m_connection ? m_connection->m_context : nullptr;
}

void CConnectionPtr::Release()
{
  if (!m_connection)
    return;
  if (m_discard)
    m_connection.reset();
  else
    CPool::Get().Give(std::move(m_connection), m_key);
  m_discard = false;
}

CConnectionPtr Connect(const CLocation& location, ConnectResult& result, std::string& error)
{
  const std::string share = location.share.empty() ? "IPC$" : location.share;
  // A thread only gets the connections it made: on Tizen, waiting on a socket another
  // thread connected can block for good once the server has closed it.
  std::ostringstream thread;
  thread << std::this_thread::get_id();
  const std::string key =
      location.Key() + (location.share.empty() ? "IPC$" : "") + "#" + thread.str();

  if (CConnectionPtr connection = CPool::Get().Take(key))
  {
    result = ConnectResult::OK;
    return connection;
  }

  auto connection = std::make_unique<CConnection>();
  smb2_context* smb2 = connection->m_context;
  if (!smb2)
  {
    result = ConnectResult::FAILED;
    error = "out of memory";
    return {};
  }

  smb2_set_security_mode(smb2, SMB2_NEGOTIATE_SIGNING_ENABLED);
  smb2_set_timeout(smb2, REQUEST_TIMEOUT);
  if (!location.domain.empty())
    smb2_set_domain(smb2, location.domain.c_str());
  // Without an account, the server is asked for guest access.
  smb2_set_user(smb2, location.user.empty() ? "guest" : location.user.c_str());
  smb2_set_password(smb2, location.password.c_str());

  if (smb2_connect_share(smb2, location.server.c_str(), share.c_str(), nullptr) < 0)
  {
    const uint32_t status = static_cast<uint32_t>(smb2_get_nterror(smb2));
    error = smb2_get_error(smb2);
    const bool refused = status == SMB2_STATUS_LOGON_FAILURE ||
                         status == SMB2_STATUS_ACCESS_DENIED ||
                         status == SMB2_STATUS_ACCOUNT_RESTRICTION ||
                         status == SMB2_STATUS_PASSWORD_EXPIRED ||
                         status == SMB2_STATUS_ACCOUNT_DISABLED;
    // A server that takes no guests does not always say why, but asking for an account is
    // what helps then, as long as the server could be reached at all.
    const bool unreachable = error.find("onnect") != std::string::npos ||
                             error.find("ddress") != std::string::npos;
    result = refused || (location.user.empty() && !unreachable) ? ConnectResult::AUTH_FAILED
                                                                : ConnectResult::FAILED;
    CLog::Log(LOGERROR, "SMB2: cannot connect to {}/{}: {}", location.server, share, error);
    return {};
  }

  connection->m_connected = true;
  CLog::Log(LOGDEBUG, "SMB2: connected to {}/{}, reads up to {} bytes", location.server, share,
            smb2_get_max_read_size(smb2));
  result = ConnectResult::OK;
  return CPool::Get().Wrap(std::move(connection), key);
}

bool IsConnectionError(int err)
{
  err = err < 0 ? -err : err;
  return err == ECONNRESET || err == ENOTCONN || err == EPIPE || err == ETIMEDOUT ||
         err == ECONNABORTED || err == EIO;
}

} // namespace XFILE::SMB2
