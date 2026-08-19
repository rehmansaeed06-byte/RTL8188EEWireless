/* SPDX-License-Identifier: GPL-2.0 OR BSD-3-Clause */
#ifndef _RTLWIFI_COMPAT_SCHED_H
#define _RTLWIFI_COMPAT_SCHED_H

/*
 * Minimal stub. findings.md Section 63 (this build session): confirmed
 * by direct grep against wifi.h and the vendored completion.h that
 * nothing in the compiled driver set as of this build attempt uses any
 * real symbol from linux/sched.h (no struct task_struct, no TASK_*,
 * no schedule()/current-> usage anywhere in wifi.h). completion.h is
 * fully self-contained against types.h/jiffies.h/iokit_shim.h and does
 * NOT depend on anything from sched.h either, despite wifi.h including
 * both.
 *
 * wifi.h's #include <linux/sched.h> (line 9) is therefore treated as
 * present for real-Linux completeness (linux/completion.h upstream
 * genuinely does depend on sched.h for struct task_struct etc.) but
 * dead weight for this port's actual compiled surface — this file
 * exists only to satisfy the #include and let compilation proceed,
 * not because any symbol from it is used.
 *
 * IMPORTANT: this is scoped to what's been compiled SO FAR (base.c,
 * cam.c, core.c, debug.c). If a later file in DRIVER_SRCS/CHIP_SRCS
 * (rc.c is the most likely candidate — real Linux rc.c/rate-control
 * code often touches scheduling/jiffies-adjacent things) fails to
 * compile referencing a real sched.h symbol, that is a sign this stub
 * needs a real definition added, not a sign this file is wrong for
 * what's been checked so far. Extend this file then, grounded in
 * whatever the actual compiler error names, rather than pre-guessing
 * additional content now.
 */

#endif /* _RTLWIFI_COMPAT_SCHED_H */
