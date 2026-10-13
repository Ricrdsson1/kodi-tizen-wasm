/*
 *  Copyright (C) 2026 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "StaticAddons.h"

#include "utils/URIUtils.h"
#include "utils/log.h"

#include <string_view>

CStaticAddonLoader::CStaticAddonLoader(const std::string& libraryFile) : LibraryLoader(libraryFile)
{
}

bool CStaticAddonLoader::Load()
{
  const std::string fileName = URIUtils::GetFileName(GetFileName());
  for (const StaticAddonLibrary* library = g_staticAddonLibraries; library->fileName; ++library)
  {
    if (fileName == library->fileName)
    {
      m_library = library;
      return true;
    }
  }

  CLog::LogF(LOGERROR, "{} is not linked into this build", fileName);
  return false;
}

void CStaticAddonLoader::Unload()
{
  m_library = nullptr;
}

int CStaticAddonLoader::ResolveExport(const char* symbol, void** ptr, bool logging)
{
  const std::string_view name = symbol;

  void* address = nullptr;
  if (m_library)
  {
    if (name == "ADDON_Create")
      address = m_library->create;
    else if (name == "ADDON_GetTypeVersion")
      address = m_library->getTypeVersion;
    else if (name == "ADDON_GetTypeMinVersion")
      address = m_library->getTypeMinVersion;
  }

  if (!address)
  {
    if (logging)
      CLog::LogF(LOGWARNING, "Unable to resolve {} in {}", symbol, GetName());
    return 0;
  }

  *ptr = address;
  return 1;
}
