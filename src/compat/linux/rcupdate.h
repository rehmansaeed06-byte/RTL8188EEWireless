/* src/compat/linux/rcupdate.h
 *
 * rcu_read_lock()/rcu_read_unlock() compat shim -- deliberate no-ops.
 *
 * Every rcu_read_lock()/rcu_read_unlock() call site in the files this
 * project actually compiles (base.c, core.c, stats.c, rtl8188ee/dm.c --
 * confirmed by grep; other-chip files like rtl8723be/rtl8192ce are not
 * in DRIVER_SRCS/CHIP_SRCS and don't matter here) exists solely to
 * bracket a call into ieee80211_find_sta() (directly, or via wifi.h's
 * rtl_find_sta()/get_sta() inline wrappers).
 *
 * This port tracks at most ONE associated station at a time (confirmed:
 * RTL8188EEIEEE80211.hpp's _sta is a scalar struct ieee80211_sta*, not a
 * list -- this driver has no AP-mode/multi-station support), so the real
 * concurrency hazard Linux's RCU protects against here (safe traversal
 * of a station list against concurrent removal from another CPU) does
 * not apply in the same shape. The actual protection for the single
 * tracked station pointer lives in ieee80211_find_sta() itself
 * (src/compat/rtlwifi_compat.c), guarded there -- NOT emulated here as a
 * guessed-at RCU replacement. These macros are genuine no-ops, not a
 * correctness shortcut: the real lock is elsewhere, on purpose.
 */

#ifndef _RTLWIFI_COMPAT_RCUPDATE_H_
#define _RTLWIFI_COMPAT_RCUPDATE_H_

#define rcu_read_lock()   do { } while (0)
#define rcu_read_unlock() do { } while (0)

#endif /* _RTLWIFI_COMPAT_RCUPDATE_H_ */
