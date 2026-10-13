/*
 *  Copyright (C) 2026 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "utils/Base64.h"
#include "utils/StringUtils.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdarg>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <sys/select.h>

#define CURL CURL_HANDLE
typedef void CURL_HANDLE;
#include <curl/curl.h>
#undef CURL

#include "CurlWasm.h"

#include <emscripten/emscripten.h>

using namespace std::chrono_literals;

extern "C"
{
// wasm_http.js
void kodi_wasm_http_start(int id,
                          const char* method,
                          const char* url,
                          const char* headers,
                          const char* body,
                          int bodySize,
                          int timeoutMs);
void kodi_wasm_http_resume(int id);
void kodi_wasm_http_abort(int id);
}

namespace
{
// Values of the error argument of kodi_wasm_http_on_done().
constexpr int HTTP_ERROR_NETWORK = 1;
constexpr int HTTP_ERROR_TIMEOUT = 2;

// The browser stops reading a response above the high mark and resumes below the low one.
constexpr size_t QUEUE_HIGH_MARK = 4 * 1024 * 1024;
constexpr size_t QUEUE_LOW_MARK = 1 * 1024 * 1024;

// Largest piece of a response handed to a write callback at once.
constexpr size_t MAX_WRITE_SIZE = 64 * 1024;

constexpr auto IDLE_WAIT = 50ms;

// Bounds the data one curl_multi_perform() call hands over to MAX_WRITE_SIZE times this.
constexpr int MAX_STEPS_PER_PERFORM = 16;

/*!
 * \brief Response state shared between a Kodi thread and the browser main thread.
 */
struct Transfer
{
  std::mutex mutex;
  std::condition_variable cond;
  bool headersReady{false};
  long status{0};
  std::string effectiveUrl;
  std::string headers;
  std::deque<std::pair<uint8_t*, size_t>> chunks;
  size_t queued{0};
  bool paused{false};
  bool done{false};
  bool closed{false};
  int error{0};
};

std::mutex g_transfersMutex;
std::map<int, std::shared_ptr<Transfer>> g_transfers;
int g_nextTransferId = 1;

std::shared_ptr<Transfer> FindTransfer(int id)
{
  std::unique_lock lock(g_transfersMutex);
  const auto it = g_transfers.find(id);
  return it != g_transfers.end() ? it->second : nullptr;
}

struct Multi;

struct Options
{
  std::string url;
  std::string customRequest;
  std::string range;
  std::string username;
  std::string password;
  const char* postFields{nullptr};
  long postFieldSize{-1};
  curl_off_t resumeFrom{0};
  long noBody{0};
  long post{0};
  long upload{0};
  long failOnError{0};
  long noProgress{1};
  long connectTimeout{0};
  long timeout{0};
  curl_slist* headers{nullptr};
  curl_write_callback writeFunction{nullptr};
  void* writeData{nullptr};
  curl_write_callback headerFunction{nullptr};
  void* headerData{nullptr};
  curl_xferinfo_callback xferFunction{nullptr};
  void* xferData{nullptr};
};

struct Easy
{
  Options options;
  Multi* multi{nullptr};

  // The running transfer.
  int id{0};
  std::shared_ptr<Transfer> transfer;
  bool headersDelivered{false};
  bool finished{false};
  uint8_t* chunk{nullptr};
  size_t chunkSize{0};
  size_t chunkOffset{0};
  std::chrono::steady_clock::time_point start;

  // Results of the last transfer.
  CURLcode result{CURLE_OK};
  long responseCode{0};
  std::string effectiveUrl;
  std::string contentType;
  curl_off_t contentLength{-1};
  long fileTime{-1};
  curl_off_t downloaded{0};
  double totalTime{0.0};
};

struct Multi
{
  std::vector<Easy*> easies;
  std::deque<CURLMsg> messages;
  CURLMsg current;
};

void ReleaseTransfer(Easy& easy, bool abort)
{
  free(easy.chunk);
  easy.chunk = nullptr;
  easy.chunkSize = 0;
  easy.chunkOffset = 0;

  if (!easy.transfer)
    return;

  {
    std::unique_lock lock(g_transfersMutex);
    g_transfers.erase(easy.id);
  }
  {
    std::unique_lock lock(easy.transfer->mutex);
    easy.transfer->closed = true;
    for (const auto& chunk : easy.transfer->chunks)
      free(chunk.first);
    easy.transfer->chunks.clear();
    abort = abort && !easy.transfer->done;
  }
  if (abort)
    kodi_wasm_http_abort(easy.id);
  easy.transfer.reset();
}

void Finish(Easy& easy, CURLcode result)
{
  ReleaseTransfer(easy, true);
  easy.finished = true;
  easy.result = result;
  easy.totalTime =
      std::chrono::duration<double>(std::chrono::steady_clock::now() - easy.start).count();

  if (easy.multi)
  {
    CURLMsg message{};
    message.msg = CURLMSG_DONE;
    message.easy_handle = &easy;
    message.data.result = result;
    easy.multi->messages.push_back(message);
  }
}

void ResetTransfer(Easy& easy)
{
  ReleaseTransfer(easy, true);
  easy.headersDelivered = false;
  easy.finished = false;
}

void Start(Easy& easy)
{
  const Options& options = easy.options;

  easy.start = std::chrono::steady_clock::now();
  easy.result = CURLE_OK;
  easy.responseCode = 0;
  easy.effectiveUrl = options.url;
  easy.contentType.clear();
  easy.contentLength = -1;
  easy.fileTime = -1;
  easy.downloaded = 0;
  easy.totalTime = 0.0;

  if (options.upload || (!StringUtils::StartsWithNoCase(options.url, "http://") &&
                         !StringUtils::StartsWithNoCase(options.url, "https://")))
  {
    Finish(easy, CURLE_UNSUPPORTED_PROTOCOL);
    return;
  }

  std::string method = "GET";
  if (!options.customRequest.empty())
    method = options.customRequest;
  else if (options.noBody)
    method = "HEAD";
  else if (options.post || options.postFields)
    method = "POST";

  std::string headers;
  for (const curl_slist* header = options.headers; header; header = header->next)
  {
    headers += header->data;
    headers += '\n';
  }
  if (options.resumeFrom > 0)
    headers += "Range: bytes=" + std::to_string(options.resumeFrom) + "-\n";
  else if (!options.range.empty())
    headers += "Range: bytes=" + options.range + "\n";
  if (!options.username.empty())
    headers +=
        "Authorization: Basic " + Base64::Encode(options.username + ":" + options.password) + "\n";

  int bodySize = 0;
  if (options.postFields && method != "GET" && method != "HEAD")
    bodySize = options.postFieldSize >= 0 ? static_cast<int>(options.postFieldSize)
                                          : static_cast<int>(strlen(options.postFields));

  // Bounds the wait for the response headers; a body may take as long as it needs.
  long timeout = options.connectTimeout;
  if (options.timeout > 0 && (timeout == 0 || options.timeout < timeout))
    timeout = options.timeout;

  easy.transfer = std::make_shared<Transfer>();
  {
    std::unique_lock lock(g_transfersMutex);
    easy.id = g_nextTransferId++;
    g_transfers[easy.id] = easy.transfer;
  }

  kodi_wasm_http_start(easy.id, method.c_str(), options.url.c_str(), headers.c_str(),
                       options.postFields, bodySize, static_cast<int>(timeout * 1000));
}

long ParseHttpDate(const std::string& date)
{
  struct tm time = {};
  if (!strptime(date.c_str(), "%a, %d %b %Y %H:%M:%S", &time))
    return -1;
  return static_cast<long>(timegm(&time));
}

CURLcode TransferResult(const Easy& easy, int error)
{
  switch (error)
  {
    case 0:
      return CURLE_OK;
    case HTTP_ERROR_TIMEOUT:
      return CURLE_OPERATION_TIMEDOUT;
    default:
      return easy.headersDelivered ? CURLE_RECV_ERROR : CURLE_COULDNT_CONNECT;
  }
}

void DeliverHeaders(Easy& easy, long status, const std::string& headers)
{
  easy.headersDelivered = true;
  easy.responseCode = status;

  std::vector<std::string> lines = StringUtils::Split(headers, '\n');
  std::erase_if(lines, [](const std::string& line) { return line.empty(); });

  bool encoded = false;
  for (const std::string& line : lines)
  {
    const size_t colon = line.find(':');
    if (colon == std::string::npos)
      continue;

    const std::string name = StringUtils::ToLower(line.substr(0, colon));
    std::string value = line.substr(colon + 1);
    StringUtils::Trim(value);

    if (name == "content-length")
      easy.contentLength = strtoll(value.c_str(), nullptr, 10);
    else if (name == "content-type")
      easy.contentType = value;
    else if (name == "content-encoding")
      encoded = true;
    else if (name == "last-modified")
      easy.fileTime = ParseHttpDate(value);
  }

  // The browser decodes the body, so the length of the encoded one says nothing.
  if (encoded)
    easy.contentLength = -1;

  if (!easy.options.headerFunction)
    return;

  auto deliver = [&easy](std::string line)
  {
    line += "\r\n";
    easy.options.headerFunction(line.data(), 1, line.size(), easy.options.headerData);
  };

  deliver("HTTP/1.1 " + std::to_string(status));
  for (const std::string& line : lines)
  {
    const std::string name = StringUtils::ToLower(line.substr(0, line.find(':')));
    if (encoded && (name == "content-length" || name == "content-encoding"))
      continue;
    deliver(line);
  }
  deliver("");
}

/*!
 * \brief Advance a transfer by one step.
 * \return true if there was something to do
 */
bool Step(Easy& easy)
{
  if (easy.finished)
    return false;

  if (!easy.transfer)
  {
    Start(easy);
    return true;
  }

  if (!easy.chunk)
  {
    Transfer& transfer = *easy.transfer;
    std::unique_lock lock(transfer.mutex);

    if (!easy.headersDelivered)
    {
      if (!transfer.headersReady)
      {
        if (!transfer.done)
          return false;

        const int error = transfer.error;
        lock.unlock();
        Finish(easy, TransferResult(easy, error ? error : HTTP_ERROR_NETWORK));
        return true;
      }

      const long status = transfer.status;
      const std::string headers = transfer.headers;
      easy.effectiveUrl = transfer.effectiveUrl;
      lock.unlock();

      DeliverHeaders(easy, status, headers);
      if (easy.options.failOnError && status >= 400)
        Finish(easy, CURLE_HTTP_RETURNED_ERROR);
      return true;
    }

    if (transfer.chunks.empty())
    {
      if (!transfer.done)
        return false;

      const int error = transfer.error;
      lock.unlock();
      Finish(easy, TransferResult(easy, error));
      return true;
    }

    easy.chunk = transfer.chunks.front().first;
    easy.chunkSize = transfer.chunks.front().second;
    easy.chunkOffset = 0;
    transfer.chunks.pop_front();
    transfer.queued -= easy.chunkSize;

    const bool resume = transfer.paused && transfer.queued < QUEUE_LOW_MARK;
    if (resume)
      transfer.paused = false;
    lock.unlock();

    if (resume)
      kodi_wasm_http_resume(easy.id);
  }

  const size_t size = std::min(easy.chunkSize - easy.chunkOffset, MAX_WRITE_SIZE);
  char* data = reinterpret_cast<char*>(easy.chunk) + easy.chunkOffset;
  easy.chunkOffset += size;
  easy.downloaded += size;

  const Options& options = easy.options;
  CURLcode result = CURLE_OK;
  if (!options.noProgress && options.xferFunction &&
      options.xferFunction(options.xferData, std::max<curl_off_t>(easy.contentLength, 0),
                           easy.downloaded, 0, 0) != 0)
    result = CURLE_ABORTED_BY_CALLBACK;
  else if (options.writeFunction && options.writeFunction(data, 1, size, options.writeData) != size)
    result = CURLE_WRITE_ERROR;

  if (easy.chunkOffset == easy.chunkSize)
  {
    free(easy.chunk);
    easy.chunk = nullptr;
  }

  if (result != CURLE_OK)
    Finish(easy, result);
  return true;
}

/*!
 * \brief Sleep until the browser has something for the transfer, or a short while passes.
 */
void WaitForTransfer(Easy& easy)
{
  const std::shared_ptr<Transfer> transfer = easy.transfer;
  if (!transfer)
    return;

  std::unique_lock lock(transfer->mutex);
  transfer->cond.wait_for(lock, IDLE_WAIT,
                          [&]
                          {
                            return transfer->done || !transfer->chunks.empty() ||
                                   (!easy.headersDelivered && transfer->headersReady);
                          });
}

Easy* ToEasy(void* handle)
{
  return static_cast<Easy*>(handle);
}

Multi* ToMulti(CURLM* handle)
{
  return static_cast<Multi*>(handle);
}

void SetString(std::string& target, const char* value)
{
  target = value ? value : "";
}
} // namespace

extern "C"
{
// Called by wasm_http.js on the browser main thread. Takes ownership of the strings.
EMSCRIPTEN_KEEPALIVE int kodi_wasm_http_on_headers(int id, int status, char* url, char* headers)
{
  const std::shared_ptr<Transfer> transfer = FindTransfer(id);
  if (transfer)
  {
    std::unique_lock lock(transfer->mutex);
    transfer->status = status;
    transfer->effectiveUrl = url;
    transfer->headers = headers;
    transfer->headersReady = true;
    transfer->cond.notify_all();
  }
  free(url);
  free(headers);
  return transfer ? 1 : 0;
}

// Called by wasm_http.js on the browser main thread. Takes ownership of data.
// Returns non-zero when the browser should stop reading until it is resumed.
EMSCRIPTEN_KEEPALIVE int kodi_wasm_http_on_data(int id, uint8_t* data, int size)
{
  const std::shared_ptr<Transfer> transfer = FindTransfer(id);
  if (!transfer)
  {
    free(data);
    return 0;
  }

  std::unique_lock lock(transfer->mutex);
  if (transfer->closed)
  {
    free(data);
    return 0;
  }

  transfer->chunks.emplace_back(data, static_cast<size_t>(size));
  transfer->queued += size;
  if (transfer->queued > QUEUE_HIGH_MARK)
    transfer->paused = true;
  transfer->cond.notify_all();
  return transfer->paused ? 1 : 0;
}

// Called by wasm_http.js on the browser main thread.
EMSCRIPTEN_KEEPALIVE void kodi_wasm_http_on_done(int id, int error)
{
  const std::shared_ptr<Transfer> transfer = FindTransfer(id);
  if (!transfer)
    return;

  std::unique_lock lock(transfer->mutex);
  transfer->error = error;
  transfer->done = true;
  transfer->cond.notify_all();
}

CURLcode kodi_wasm_curl_global_init(long flags)
{
  return CURLE_OK;
}

void kodi_wasm_curl_global_cleanup(void)
{
}

void* kodi_wasm_curl_easy_init(void)
{
  return new Easy();
}

CURLcode kodi_wasm_curl_easy_setopt(void* handle, CURLoption option, ...)
{
  if (!handle)
    return CURLE_BAD_FUNCTION_ARGUMENT;

  Options& options = ToEasy(handle)->options;

  va_list args;
  va_start(args, option);

  if (option >= CURLOPTTYPE_BLOB)
  {
    // Certificates are the browser's business.
  }
  else if (option >= CURLOPTTYPE_OFF_T)
  {
    const curl_off_t value = va_arg(args, curl_off_t);
    if (option == CURLOPT_RESUME_FROM_LARGE)
      options.resumeFrom = value;
  }
  else if (option >= CURLOPTTYPE_FUNCTIONPOINT)
  {
    switch (option)
    {
      case CURLOPT_WRITEFUNCTION:
        options.writeFunction = va_arg(args, curl_write_callback);
        break;
      case CURLOPT_HEADERFUNCTION:
        options.headerFunction = va_arg(args, curl_write_callback);
        break;
      case CURLOPT_XFERINFOFUNCTION:
        options.xferFunction = va_arg(args, curl_xferinfo_callback);
        break;
      default:
        break;
    }
  }
  else if (option >= CURLOPTTYPE_OBJECTPOINT)
  {
    void* value = va_arg(args, void*);
    switch (option)
    {
      case CURLOPT_URL:
        SetString(options.url, static_cast<const char*>(value));
        break;
      case CURLOPT_CUSTOMREQUEST:
        SetString(options.customRequest, static_cast<const char*>(value));
        break;
      case CURLOPT_RANGE:
        SetString(options.range, static_cast<const char*>(value));
        break;
      case CURLOPT_USERNAME:
        SetString(options.username, static_cast<const char*>(value));
        break;
      case CURLOPT_PASSWORD:
        SetString(options.password, static_cast<const char*>(value));
        break;
      case CURLOPT_POSTFIELDS:
        options.postFields = static_cast<const char*>(value);
        break;
      case CURLOPT_HTTPHEADER:
        options.headers = static_cast<curl_slist*>(value);
        break;
      case CURLOPT_WRITEDATA:
        options.writeData = value;
        break;
      case CURLOPT_HEADERDATA:
        options.headerData = value;
        break;
      case CURLOPT_XFERINFODATA:
        options.xferData = value;
        break;
      default:
        break;
    }
  }
  else
  {
    const long value = va_arg(args, long);
    switch (option)
    {
      case CURLOPT_NOBODY:
        options.noBody = value;
        break;
      case CURLOPT_POST:
        options.post = value;
        break;
      case CURLOPT_POSTFIELDSIZE:
        options.postFieldSize = value;
        break;
      case CURLOPT_UPLOAD:
        options.upload = value;
        break;
      case CURLOPT_FAILONERROR:
        options.failOnError = value;
        break;
      case CURLOPT_NOPROGRESS:
        options.noProgress = value;
        break;
      case CURLOPT_CONNECTTIMEOUT:
        options.connectTimeout = value;
        break;
      case CURLOPT_TIMEOUT:
        options.timeout = value;
        break;
      default:
        break;
    }
  }

  va_end(args);
  return CURLE_OK;
}

CURLcode kodi_wasm_curl_easy_perform(void* handle)
{
  if (!handle)
    return CURLE_BAD_FUNCTION_ARGUMENT;

  Easy& easy = *ToEasy(handle);
  ResetTransfer(easy);
  while (!easy.finished)
  {
    if (!Step(easy))
      WaitForTransfer(easy);
  }
  easy.finished = false;
  return easy.result;
}

CURLcode kodi_wasm_curl_easy_pause(void* handle, int bitmask)
{
  return CURLE_OK;
}

void kodi_wasm_curl_easy_reset(void* handle)
{
  if (!handle)
    return;

  Easy& easy = *ToEasy(handle);
  ResetTransfer(easy);
  easy.options = Options();
}

CURLcode kodi_wasm_curl_easy_getinfo(void* handle, CURLINFO info, ...)
{
  if (!handle)
    return CURLE_BAD_FUNCTION_ARGUMENT;

  const Easy& easy = *ToEasy(handle);

  va_list args;
  va_start(args, info);
  void* out = va_arg(args, void*);
  va_end(args);

  if (!out)
    return CURLE_BAD_FUNCTION_ARGUMENT;

  switch (info)
  {
    case CURLINFO_RESPONSE_CODE:
      *static_cast<long*>(out) = easy.responseCode;
      break;
    case CURLINFO_FILETIME:
      *static_cast<long*>(out) = easy.fileTime;
      break;
    case CURLINFO_EFFECTIVE_URL:
      *static_cast<const char**>(out) = easy.effectiveUrl.c_str();
      break;
    case CURLINFO_CONTENT_TYPE:
      *static_cast<const char**>(out) =
          easy.contentType.empty() ? nullptr : easy.contentType.c_str();
      break;
    case CURLINFO_REDIRECT_URL:
      *static_cast<const char**>(out) = nullptr;
      break;
    case CURLINFO_COOKIELIST:
      *static_cast<curl_slist**>(out) = nullptr;
      break;
    case CURLINFO_CONTENT_LENGTH_DOWNLOAD_T:
      *static_cast<curl_off_t*>(out) = easy.contentLength;
      break;
    case CURLINFO_CONTENT_LENGTH_DOWNLOAD:
      *static_cast<double*>(out) = static_cast<double>(easy.contentLength);
      break;
    case CURLINFO_SIZE_DOWNLOAD:
      *static_cast<double*>(out) = static_cast<double>(easy.downloaded);
      break;
    case CURLINFO_TOTAL_TIME:
      *static_cast<double*>(out) =
          easy.transfer ? std::chrono::duration<double>(std::chrono::steady_clock::now() -
                                                        easy.start)
                              .count()
                        : easy.totalTime;
      break;
    case CURLINFO_SPEED_DOWNLOAD_T:
    {
      const double seconds =
          std::chrono::duration<double>(std::chrono::steady_clock::now() - easy.start).count();
      *static_cast<curl_off_t*>(out) =
          seconds > 0.0 ? static_cast<curl_off_t>(static_cast<double>(easy.downloaded) / seconds)
                        : 0;
      break;
    }
    default:
      return CURLE_UNKNOWN_OPTION;
  }

  return CURLE_OK;
}

void kodi_wasm_curl_easy_cleanup(void* handle)
{
  if (!handle)
    return;

  Easy* easy = ToEasy(handle);
  if (easy->multi)
    kodi_wasm_curl_multi_remove_handle(easy->multi, easy);
  ReleaseTransfer(*easy, true);
  delete easy;
}

void* kodi_wasm_curl_easy_duphandle(void* handle)
{
  if (!handle)
    return nullptr;

  Easy* easy = new Easy();
  easy->options = ToEasy(handle)->options;
  return easy;
}

const char* kodi_wasm_curl_easy_strerror(CURLcode code)
{
  switch (code)
  {
    case CURLE_OK:
      return "No error";
    case CURLE_UNSUPPORTED_PROTOCOL:
      return "Unsupported protocol";
    case CURLE_COULDNT_CONNECT:
      return "Could not connect to server";
    case CURLE_HTTP_RETURNED_ERROR:
      return "HTTP response code said error";
    case CURLE_WRITE_ERROR:
      return "Failed writing received data to disk/application";
    case CURLE_OPERATION_TIMEDOUT:
      return "Timeout was reached";
    case CURLE_ABORTED_BY_CALLBACK:
      return "Operation was aborted by an application callback";
    case CURLE_RECV_ERROR:
      return "Failure when receiving data from the peer";
    default:
      return "Unknown error";
  }
}

CURLM* kodi_wasm_curl_multi_init(void)
{
  return new Multi();
}

CURLMcode kodi_wasm_curl_multi_add_handle(CURLM* multi, void* handle)
{
  if (!multi || !handle)
    return CURLM_BAD_HANDLE;

  Easy* easy = ToEasy(handle);
  if (easy->multi)
    return CURLM_ADDED_ALREADY;

  ResetTransfer(*easy);
  easy->multi = ToMulti(multi);
  easy->multi->easies.push_back(easy);
  return CURLM_OK;
}

CURLMcode kodi_wasm_curl_multi_perform(CURLM* multi, int* runningHandles)
{
  if (!multi)
    return CURLM_BAD_HANDLE;

  Multi& m = *ToMulti(multi);

  bool progressed = false;
  Easy* waiting = nullptr;
  for (Easy* easy : m.easies)
  {
    // Hand over what has arrived, within reason for one call.
    int steps = 0;
    while (steps < MAX_STEPS_PER_PERFORM && Step(*easy))
      ++steps;

    if (steps > 0)
      progressed = true;
    else if (!easy->finished)
      waiting = easy;
  }

  // The caller polls; without sockets to select() on, the wait happens here.
  if (!progressed && waiting)
    WaitForTransfer(*waiting);

  if (runningHandles)
    *runningHandles = static_cast<int>(
        std::ranges::count_if(m.easies, [](const Easy* easy) { return !easy->finished; }));
  return CURLM_OK;
}

CURLMcode kodi_wasm_curl_multi_remove_handle(CURLM* multi, void* handle)
{
  if (!multi || !handle)
    return CURLM_BAD_HANDLE;

  Multi& m = *ToMulti(multi);
  Easy* easy = ToEasy(handle);
  if (easy->multi != &m)
    return CURLM_OK;

  ResetTransfer(*easy);
  easy->multi = nullptr;
  std::erase(m.easies, easy);
  std::erase_if(m.messages, [easy](const CURLMsg& message) { return message.easy_handle == easy; });
  return CURLM_OK;
}

CURLMcode kodi_wasm_curl_multi_fdset(
    CURLM* multi, fd_set* readSet, fd_set* writeSet, fd_set* excSet, int* maxFd)
{
  if (maxFd)
    *maxFd = -1;
  return CURLM_OK;
}

CURLMcode kodi_wasm_curl_multi_timeout(CURLM* multi, long* timeout)
{
  if (timeout)
    *timeout = 0;
  return CURLM_OK;
}

CURLMsg* kodi_wasm_curl_multi_info_read(CURLM* multi, int* messagesInQueue)
{
  if (!multi)
    return nullptr;

  Multi& m = *ToMulti(multi);
  if (m.messages.empty())
  {
    if (messagesInQueue)
      *messagesInQueue = 0;
    return nullptr;
  }

  m.current = m.messages.front();
  m.messages.pop_front();
  if (messagesInQueue)
    *messagesInQueue = static_cast<int>(m.messages.size());
  return &m.current;
}

CURLMcode kodi_wasm_curl_multi_cleanup(CURLM* multi)
{
  if (!multi)
    return CURLM_BAD_HANDLE;

  Multi* m = ToMulti(multi);
  for (Easy* easy : m->easies)
  {
    ResetTransfer(*easy);
    easy->multi = nullptr;
  }
  delete m;
  return CURLM_OK;
}

curl_slist* kodi_wasm_curl_slist_append(curl_slist* list, const char* data)
{
  curl_slist* item = static_cast<curl_slist*>(malloc(sizeof(curl_slist)));
  if (!item)
    return nullptr;

  item->data = strdup(data);
  item->next = nullptr;
  if (!list)
    return item;

  curl_slist* last = list;
  while (last->next)
    last = last->next;
  last->next = item;
  return list;
}

void kodi_wasm_curl_slist_free_all(curl_slist* list)
{
  while (list)
  {
    curl_slist* next = list->next;
    free(list->data);
    free(list);
    list = next;
  }
}
}
