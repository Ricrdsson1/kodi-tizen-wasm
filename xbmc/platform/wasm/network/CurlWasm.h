/*
 *  Copyright (C) 2026 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

// A browser has no sockets for libcurl to use. This is the part of the libcurl
// API that Kodi calls, implemented on fetch(); include it after <curl/curl.h>,
// whose types it shares. Only http and https transfers are supported.

#undef curl_easy_setopt
#undef curl_easy_getinfo

#define curl_global_init kodi_wasm_curl_global_init
#define curl_global_cleanup kodi_wasm_curl_global_cleanup
#define curl_easy_init kodi_wasm_curl_easy_init
#define curl_easy_setopt kodi_wasm_curl_easy_setopt
#define curl_easy_perform kodi_wasm_curl_easy_perform
#define curl_easy_pause kodi_wasm_curl_easy_pause
#define curl_easy_reset kodi_wasm_curl_easy_reset
#define curl_easy_getinfo kodi_wasm_curl_easy_getinfo
#define curl_easy_cleanup kodi_wasm_curl_easy_cleanup
#define curl_easy_duphandle kodi_wasm_curl_easy_duphandle
#define curl_easy_strerror kodi_wasm_curl_easy_strerror
#define curl_multi_init kodi_wasm_curl_multi_init
#define curl_multi_add_handle kodi_wasm_curl_multi_add_handle
#define curl_multi_perform kodi_wasm_curl_multi_perform
#define curl_multi_remove_handle kodi_wasm_curl_multi_remove_handle
#define curl_multi_fdset kodi_wasm_curl_multi_fdset
#define curl_multi_timeout kodi_wasm_curl_multi_timeout
#define curl_multi_info_read kodi_wasm_curl_multi_info_read
#define curl_multi_cleanup kodi_wasm_curl_multi_cleanup
#define curl_slist_append kodi_wasm_curl_slist_append
#define curl_slist_free_all kodi_wasm_curl_slist_free_all

extern "C"
{
CURLcode kodi_wasm_curl_global_init(long flags);
void kodi_wasm_curl_global_cleanup(void);
void* kodi_wasm_curl_easy_init(void);
CURLcode kodi_wasm_curl_easy_setopt(void* handle, CURLoption option, ...);
CURLcode kodi_wasm_curl_easy_perform(void* handle);
CURLcode kodi_wasm_curl_easy_pause(void* handle, int bitmask);
void kodi_wasm_curl_easy_reset(void* handle);
CURLcode kodi_wasm_curl_easy_getinfo(void* handle, CURLINFO info, ...);
void kodi_wasm_curl_easy_cleanup(void* handle);
void* kodi_wasm_curl_easy_duphandle(void* handle);
const char* kodi_wasm_curl_easy_strerror(CURLcode code);
CURLM* kodi_wasm_curl_multi_init(void);
CURLMcode kodi_wasm_curl_multi_add_handle(CURLM* multi, void* handle);
CURLMcode kodi_wasm_curl_multi_perform(CURLM* multi, int* runningHandles);
CURLMcode kodi_wasm_curl_multi_remove_handle(CURLM* multi, void* handle);
CURLMcode kodi_wasm_curl_multi_fdset(
    CURLM* multi, fd_set* readSet, fd_set* writeSet, fd_set* excSet, int* maxFd);
CURLMcode kodi_wasm_curl_multi_timeout(CURLM* multi, long* timeout);
CURLMsg* kodi_wasm_curl_multi_info_read(CURLM* multi, int* messagesInQueue);
CURLMcode kodi_wasm_curl_multi_cleanup(CURLM* multi);
curl_slist* kodi_wasm_curl_slist_append(curl_slist* list, const char* data);
void kodi_wasm_curl_slist_free_all(curl_slist* list);
}
