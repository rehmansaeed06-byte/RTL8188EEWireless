/* SPDX-License-Identifier: GPL-2.0 OR BSD-3-Clause */
#ifndef _RTL8188EE_COMPAT_RANDOM_H
#define _RTL8188EE_COMPAT_RANDOM_H

/*
 * get_random_bytes() - real rtlwifi source calls this. Wraps
 * read_random(), already proven to link (RTL8188EEIEEE80211.cpp already
 * calls it directly for WPA2 SNonce generation).
 *
 * u_int typedef: sys/random.h itself uses u_int in its own
 * declaration but doesn't pull in sys/types.h - this kernel-compile
 * context doesn't have it in scope otherwise (confirmed via real
 * build error: "unknown type name 'u_int'"). Defined here, guarded
 * with the SDK's own _U_INT macro so it's a harmless no-op if the
 * real header (sys/_types/_u_int.h) ends up in scope too.
 */
#ifndef _U_INT
#define _U_INT
typedef unsigned int u_int;
#endif

#include <sys/random.h>

static inline void get_random_bytes(void *buf, int len)
{
    read_random(buf, (u_int)len);
}

#endif /* _RTL8188EE_COMPAT_RANDOM_H */
