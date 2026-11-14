/*
 *  Copyright (C) 2026 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "SystemInfoTizen.h"

#include <cstdlib>

#include <dlfcn.h>

namespace KODI::PLATFORM::TIZEN
{
namespace
{
// Takes the string a TV library allocated for its caller.
std::string Take(char* value)
{
  std::string result = value ? value : "";
  free(value);
  return result;
}
} // namespace

std::string GetPlatformString(const char* key)
{
  void* library = dlopen("libcapi-system-info.so.0", RTLD_NOW);
  if (!library)
    return {};
  using GetString = int (*)(const char*, char**);
  const auto get = reinterpret_cast<GetString>(dlsym(library, "system_info_get_platform_string"));
  char* value = nullptr;
  std::string result;
  if (get && get(key, &value) == 0)
    result = Take(value);
  dlclose(library);
  return result;
}

std::string GetTVName()
{
  void* library = dlopen("libvconf.so.0", RTLD_NOW);
  if (!library)
    return {};
  using GetString = char* (*)(const char*);
  const auto get = reinterpret_cast<GetString>(dlsym(library, "vconf_get_str"));
  std::string result = get ? Take(get("db/menu/network/devicename/tv_name")) : "";
  dlclose(library);
  return result;
}
} // namespace KODI::PLATFORM::TIZEN
