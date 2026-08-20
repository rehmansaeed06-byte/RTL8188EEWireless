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

/*
 * rtlwifi's core.c/pci.c reference tasklet_struct, request_irq(), and
 * friends. The existing compat/linux/interrupt.h shim already provides
 * these (built for the rtw88 port), but wifi.h — rtlwifi's own vendored
 * upstream master header — is left unmodified on purpose, to keep this
 * port diffable against upstream. Since this file is force-included
 * ahead of every rtlwifi driver translation unit (-include in
 * Makefile.rtl8188ee), pulling linux/interrupt.h in here has the same
 * effect as if wifi.h had included it, without touching vendored source.
 */
#include <linux/time.h>      /* time64_t — needed by wifi.h before interrupt.h/mac80211.h
                                 pull in anything transitively; Section 63.3 wired in
                                 interrupt.h but missed this one, causing wifi.h:1664
                                 "unknown type name 'time64_t'" (build-log confirmed). */
#include <linux/atomic.h>    /* atomic_t — same gap, wifi.h:1945 "unknown type name
                                 'atomic_t'" (build-log confirmed). */
#include <linux/interrupt.h>

/*
 * fallthrough; — a C23/recent-kernel pseudo-keyword core.c uses as a
 * bare statement inside switch cases (real usage: `fallthrough;` on its
 * own line, no arguments). Not a real identifier or function — just a
 * marker for "no break here, this is intentional." No compat/linux/*.h
 * previously defined it, so it errored as an unknown identifier. A
 * no-op statement macro is the correct compat shim (matches upstream
 * Linux's own <linux/compiler_attributes.h> fallback definition when
 * the compiler doesn't support the fallthrough attribute).
 */
#ifndef fallthrough
#define fallthrough do {} while (0)
#endif

#include "net/mac80211.h"   /* existing rtw88-built shim; provides
                                struct ieee80211_hw, struct wiphy,
                                struct ieee80211_ops, etc. Corrected
                                from an earlier "compat/net/mac80211.h"
                                path — COMPAT_DIR itself already IS
                                src/compat (both in the vendored-local
                                and, previously, the Feixiao-relative
                                Makefile setup), so a leading compat/
                                segment never resolved against either
                                -I path (findings.md Section 62). */

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

/* scan-wait accessor — findings.md Section 53.2/53.5, RESOLVED (Section
 * 55.2): reads rtl_mac(rtlpriv)->act_scanning directly. No longer
 * gated behind #error. */
bool rtlwifi_is_scanning(void);

/*
 * rtlwifi-side equivalents of rtw88_sw_scan_start/_switch_channel/
 * _complete() — findings.md Section 59.4/59 (this session). See the
 * corresponding block comment in rtlwifi_compat.c for the full
 * source-confirmed rationale, including the open radio_idx/config-
 * signature-arity question noted there.
 */
void rtlwifi_sw_scan_start(struct ieee80211_hw *hw, struct ieee80211_vif *vif,
                            const u8 *mac_addr);
void rtlwifi_sw_scan_switch_channel(struct ieee80211_hw *hw);
void rtlwifi_sw_scan_complete(struct ieee80211_hw *hw, struct ieee80211_vif *vif);

/*
 * rate-control registry accessor — findings.md Section 66's rc.c fix
 * cluster. See the block comment on ieee80211_rate_control_register()
 * in rtlwifi_compat.c for why this exists and what's still unresolved
 * (no confirmed real call site for .get_rate/.rate_init yet).
 */
const struct rate_control_ops *rtlwifi_get_rate_control_ops(void);

#endif /* RTLWIFI_COMPAT_H */
