/*
 *  Copyright (C) 2026 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

#include <string>

namespace KODI::PLATFORM::TIZEN
{
/*!
 * \brief A value the TV's system information gives, e.g. "http://tizen.org/system/model_name".
 * \return empty if unknown
 */
std::string GetPlatformString(const char* key);

/*!
 * \brief The name of the TV on the network, as set in its settings.
 * \return empty if unknown
 */
std::string GetTVName();
} // namespace KODI::PLATFORM::TIZEN
