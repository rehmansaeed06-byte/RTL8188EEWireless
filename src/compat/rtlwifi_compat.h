/*
 * rtlwifi_compat.h
 *
 * Companion header for rtlwifi_compat.c. Declares the bridge-layer types
 * this compat shim needs beyond what the existing (rtw88-built)
 * src/compat/linux/* and src/compat/net/mac80211.h already provide.
 *
 * Per findings.md Section 47, the rtw88-built compat layer's Linux-API
 * coverage is already broad (net/mac80211.h alone has 252 confirmed
 * cfg80211/wiphy/ieee80211 symbol references) — this header intentionally
 * does NOT redeclare struct ieee80211_hw, struct wiphy, sk_buff, etc. It
 * assumes those come from the existing compat headers and only adds what's
 * new/rtlwifi-specific.
 *
 * IMPORTANT — BUILD-TARGET WARNING, confirmed by live grep (not a
 * theoretical concern): mac80211.h already declares, and rtw88_compat.c
 * already DEFINES, these exact real mac80211 API function names:
 *   ieee80211_rx_irqsafe, ieee80211_rx_napi, ieee80211_tx_status_irqsafe,
 *   ieee80211_tx_status, ieee80211_free_txskb, ieee80211_scan_completed,
 *   ieee80211_stop_queues, ieee80211_wake_queues, ieee80211_stop_queue
 * CONFIRMED, not hypothetical: Feixiao is a single-target build
 * (Feixiao/rtw88.xcodeproj, Feixiao/Makefile — no separate RTL8188EE
 * target exists) and rtw88_compat.c has zero chip-family #ifdef guards
 * anywhere in it (grepped directly, only unrelated RTW88_DBG_* register
 * #defines matched). Dropping rtlwifi_compat.c into this project
 * as-is, compiled alongside the existing rtw88_compat.c, WILL produce
 * duplicate-symbol link errors for all nine functions listed above —
 * this is the default outcome, not an edge case to guard against.
 *
 * This needs a real build-system decision before rtlwifi_compat.c is
 * added to any target, e.g.:
 *   - a genuinely separate Xcode target/kext product for RTL8188EE
 *     that excludes rtw88_compat.c from its source list, or
 *   - renaming this file's functions to a distinct rtlwifi_-prefixed
 *     set and wrapping mac80211.h's declarations differently per
 *     build, or
 *   - some other explicit mutual-exclusion mechanism.
 * Not resolved here — this is a project-structure decision, not
 * something to default silently in source.
 */

#ifndef RTLWIFI_COMPAT_H
#define RTLWIFI_COMPAT_H

#include "compat/net/mac80211.h"   /* existing rtw88-built shim; provides
                                       struct ieee80211_hw, struct wiphy,
                                       struct ieee80211_ops, etc. */

/*
 * Callback vtable, CONFIRMED against the real repo
 * (src/compat/rtw88_compat.c:363-365), full struct body:
 *
 *   struct rtw88_hw_callbacks {
 *       void (*rx_frame)(void *kext_hw, struct sk_buff *skb);
 *       void (*tx_status)(void *kext_hw, struct sk_buff *skb);
 *       void (*scan_done)(void *kext_hw, bool aborted);
 *   };
 *
 * This replaces the earlier draft, which only had rx_frame (inferred
 * from the ieee80211_rx_irqsafe call site alone) and left tx_status/
 * scan_done as unconfirmed TODOs. All three are now confirmed
 * one-for-one.
 */
struct rtlwifi_hw_callbacks {
    void (*rx_frame)(void *kext_hw, struct sk_buff *skb);
    void (*tx_status)(void *kext_hw, struct sk_buff *skb);
    void (*scan_done)(void *kext_hw, bool aborted);
};

/* hw lookup pair — findings.md Section 51.3 */
struct ieee80211_hw *wiphy_to_ieee80211_hw(struct wiphy *wiphy);
struct ieee80211_hw *rtlwifi_get_hw(void);

/* callback registration — CONFIRMED signature, src/compat/rtw88_compat.c:491 */
void rtlwifi_set_hw_callbacks(struct rtlwifi_hw_callbacks *cbs, void *kext_hw);

/* scan-wait accessor — findings.md Section 53.2/53.5 (body currently
 * gated behind #error in the .c file until the rtl_priv field is
 * identified; declared here so the IOKit-side call site can be written
 * against a stable signature now). */
bool rtlwifi_is_scanning(void);

#endif /* RTLWIFI_COMPAT_H */
