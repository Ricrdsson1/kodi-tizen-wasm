/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Emscripten has no Linux kernel headers; musl declares sockaddr_ll here. */
#pragma once
#include <netpacket/packet.h>
