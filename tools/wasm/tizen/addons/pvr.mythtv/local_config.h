/* SPDX-License-Identifier: GPL-2.0-or-later */
/* What lib/cppmyth's own build generates from src/local_config.h.in, for Emscripten. */
#ifndef CPPMYTH_CONFIG_H
#define CPPMYTH_CONFIG_H

#define HAVE_TIMEGM 1
#define HAVE_LOCALTIME_R 1
#define HAVE_GMTIME_R 1
#define HAVE_ZLIB 1
#define HAVE_OPENSSL 0
#define CC_INLINE inline
#define NSROOT Myth
#define LIBTAG "CPPMyth"
#define LIBVERSION "2.17"

#endif /* CPPMYTH_CONFIG_H */
