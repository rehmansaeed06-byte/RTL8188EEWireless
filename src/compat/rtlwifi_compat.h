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
 * Diagnostics accessors for RTW88IEEE80211.cpp's cmdGetState() — Bucket E,
 * findings.md Section 81.2/83.2. Same pattern as rtlwifi_is_scanning():
 * cast g_rtlwifi_hw->priv to struct rtl_priv* (real type, from the vendored
 * wifi.h, confirmed via rtl_hal()/rtl_efuse() macros at wifi.h:2755-2757)
 * and read the real fields, no invented layout.
 *
 * Signatures match RTW88StateResult's actual field widths (RTW88UserClient.hpp)
 * rather than wifi.h's native widths, to avoid a pointer-width mismatch at
 * the call site: fw_version is u16 both sides, but fw_sub_version is only
 * u8 in RTW88StateResult (wifi.h's fw_subversion is u16) and tx/rx_byte_count
 * are u32 (wifi.h's txbytesunicast/rxbytesunicast are u64) — both narrowed
 * explicitly inside the .c function body, not via a raw pointer of the
 * wrong width handed across the call site.
 *
 * rtlwifi_get_chip_name() intentionally does NOT exist: wifi.h's struct
 * rtl_priv/rtl_hal/rtl_efuse carry no chip-name string anywhere (confirmed
 * by direct grep of the real header — only a read_chip_version() callback
 * and an hw_type enum, neither of which is a name string). The call site
 * fix drops this field rather than fabricate a value for it.
 */
void rtlwifi_get_fw_version(u16 *fw_version, u8 *fw_subversion);
void rtlwifi_get_stats(u32 *tx_bytes, u32 *rx_bytes);

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

/*
 * Bucket A (findings.md Section 81.2/85.4, handover THIRTEENTH UPDATE) —
 * the five rtw88_* names RTW88IEEE80211.cpp calls that were never ported
 * to this rtlwifi-based tree. Each renamed rtlwifi_* below, confirmed
 * against a live read of the real rtlwifi core.c (see block comments in
 * rtlwifi_compat.c for the exact line numbers/evidence per function).
 */

/*
 * rtlwifi_register_vif() / rtlwifi_unregister_vif() — CONFIRMED no-ops.
 * Real rtl_op_add_interface()/rtl_op_remove_interface() (core.c:199-341)
 * already do 100% of rtlwifi's own vif-registration bookkeeping
 * (mac->vif assignment, set_network_type, HW_VAR_ETHER_ADDR, retry
 * limits) as a direct, unconditional part of their bodies — there is no
 * separate rtlwifi-side registration step left over for these to bridge
 * to. Kept as named stubs (not deleted) so the call sites in
 * RTW88IEEE80211.cpp don't need restructuring, and so this reasoning is
 * visible at both ends rather than silently dropped.
 */
void rtlwifi_register_vif(struct ieee80211_vif *vif);
void rtlwifi_unregister_vif(void);

/*
 * rtlwifi_hw_scan_supported() — CONFIRMED always false. rtlwifi's
 * rtl_ops table (core.c, the .add_interface/.remove_interface table
 * dump at core.c:1888-1889 and surrounding members) has no hw_scan/
 * cancel_hw_scan entries anywhere — rtlwifi is sw_scan-only
 * (rtl_op_sw_scan_start/_complete, core.c:1410/1444), a genuine
 * architectural difference from rtw88's firmware-offload scan
 * (handover item 28). Not a per-hw runtime query; a static fact about
 * this driver family.
 */
bool rtlwifi_hw_scan_supported(struct ieee80211_hw *hw);

/*
 * rtlwifi_connect_hw_setup() / rtlwifi_restore_connected_hw() —
 * rtlwifi-side equivalents of rtw88_connect_hw_setup()/
 * _restore_connected_hw(). Per RTW88IEEE80211.cpp's own existing
 * comments (doAuthenticate(), restoreConnectedChannel()), these
 * exist specifically to set channel + BSSID WITHOUT going through
 * hw->ops->config()/bss_info_changed() — CONFIRMED via live core.c
 * read that the normal mac80211-ops path risks exactly the stall
 * these were written to avoid: rtl_op_bss_info_changed()'s
 * BSS_CHANGED_BSSID branch (core.c:~1246) sits in the same function
 * that calls rtl_lps_leave(hw, true) (core.c:1143) when the link goes
 * down — a polling-read wake sequence, same hazard class already
 * documented for other MMIO-read paths in this project.
 *
 * Both functions instead call directly into rtlpriv->cfg->ops
 * (rtl_hal_ops) — switch_channel() and set_hw_reg(HW_VAR_BSSID) —
 * the same per-chip internal calls rtl_op_config()/
 * rtl_op_bss_info_changed() make internally, just without the
 * surrounding mac80211-ops-layer logic that carries the stall risk.
 * This is a deliberate, narrow exception to this file's general rule
 * of not reaching into rtlpriv->cfg->ops directly (see
 * rtlwifi_sw_scan_switch_channel()'s comment for the general rule) —
 * here it's the entire point, not a shortcut.
 *
 * Known simplification, flagged not hidden: real core.c's channel-
 * switch branch (core.c:625-753) also derives 20/40/80MHz bandwidth
 * state (mac->bw_40/bw_80, cur_40_prime_sc, etc.) from
 * hw->conf.chandef before calling switch_channel(). Neither function
 * below replicates that — they call switch_channel() using whatever
 * rtlphy->current_channel/bandwidth state is already set (this port
 * has no 40/80MHz negotiation path elsewhere either, so this matches
 * the existing scope, not a new gap).
 */
void rtlwifi_connect_hw_setup(struct ieee80211_hw *hw,
                               struct ieee80211_vif *vif,
                               const u8 *bssid);
void rtlwifi_restore_connected_hw(struct ieee80211_hw *hw,
                                   struct ieee80211_vif *vif,
                                   const u8 *bssid);

#endif /* RTLWIFI_COMPAT_H */
