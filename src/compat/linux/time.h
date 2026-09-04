/* SPDX-License-Identifier: GPL-2.0 OR BSD-3-Clause */
#ifndef _RTL8188EE_COMPAT_TIME_H
#define _RTL8188EE_COMPAT_TIME_H

#include "types.h"

/*
 * time64_t itself is just a type — no kernel epoch/wall-clock semantics
 * to replicate here, since nothing in rtlwifi's actual TX/RX/hw-control
 * paths does epoch-relative logic; it's used for coarse elapsed-time
 * bookkeeping (e.g. connection/scan timestamps), the same class of use
 * jiffies.h already covers with mach_absolute_time(). Kept as a distinct
 * header (not folded into jiffies.h) because real rtlwifi source
 * #includes <linux/time.h> / <linux/time64.h> by name, not jiffies.h.
 */
typedef int64_t time64_t;

#include <kern/clock.h>

static inline time64_t ktime_get_real_seconds(void)
{
    uint64_t nsecs;
    absolutetime_to_nanoseconds(mach_absolute_time(), &nsecs);
    return (time64_t)(nsecs / 1000000000ULL);
}

static inline time64_t ktime_get_boottime_seconds(void)
{
    return ktime_get_real_seconds();
}

#endif /* _RTL8188EE_COMPAT_TIME_H */
