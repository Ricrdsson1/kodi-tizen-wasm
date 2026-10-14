/*
 *  Copyright (C) 2026 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

#include <cstdint>
#include <string>

/*!
 * \brief What the device a browser runs on reports about itself.
 *
 * A browser hides the system from a page, but a Tizen TV hands its web applications
 * the Tizen SystemInfo API. Elsewhere these report nothing.
 */
namespace KODI::PLATFORM::WASM
{
/*!
 * \brief A capability of the device, e.g. "http://tizen.org/system/model_name".
 * \return the value as text, empty if it is unknown
 */
std::string GetSystemCapability(const std::string& key);

/*!
 * \brief Memory of the whole device in bytes.
 * \return false if unknown
 */
bool GetSystemMemory(uint64_t& total, uint64_t& available);

/*!
 * \brief Load of the CPUs of the device in percent, averaged over the last seconds.
 * \return -1 if unknown
 */
int GetSystemCpuLoad();

/*!
 * \brief The browser Kodi runs in, which takes the place of the kernel.
 * \param name e.g. "Chromium"
 * \param version e.g. "120.0.6099.5"
 * \return false if unknown
 */
bool GetBrowser(std::string& name, std::string& version);

/*!
 * \brief The storage the profile is kept in, as last reported by the browser.
 * \return false if unknown
 */
bool GetStorage(uint64_t& quota, uint64_t& usage);

/*!
 * \brief Whether an add-on package of the given size can be downloaded and unpacked.
 *
 * Kodi's files are held in the memory of the browser and copied into its storage, which
 * can be small, so a package that does not fit would make the browser stop Kodi.
 * \return true if it fits or nothing is known about the space
 */
bool HasRoomForAddonPackage(uint64_t packageSize);
} // namespace KODI::PLATFORM::WASM
