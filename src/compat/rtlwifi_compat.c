/*
 * rtlwifi_compat.c
 *
 * rtlwifi-side compat shim, structurally mirroring the confirmed shape of
 * Feixiao's rtw88_compat.c (see findings.md Sections 48-53 for the
 * source-verified rtw88 reference behavior this file replicates).
 *
 * Scope: this file owns the parts of the Linux/mac80211 API surface that
 * rtlwifi's *shared* code (pci.c, core.c, ps.c, base.c) calls into, plus
 * the small set of IOKit-facing bridge points (hw lookup, scan-wait,
 * callback registration) that the rtw88 port's compat layer provides.
 * Chip-specific rtl8188ee/* code is untouched real driver source and is
 * NOT part of this file.
 *
 * Every non-mechanical decision below cites the findings.md section it
 * came from. Anywhere the rtw88 reference and the rtlwifi target diverge
 * (or the answer isn't source-confirmed yet), it's marked FIXME/TODO
 * rather than guessed silently.
 */

#include "rtlwifi_compat.h"

/*
 * struct rtl_priv, struct rtl_mac, and the rtl_mac()/rtl_priv() accessor
 * macros come from rtlwifi's own real, unmodified wifi.h — confirmed
 * present at:
 *   linux-kernel/drivers/net/wireless/realtek/rtlwifi/wifi.h
 * per the handover doc's project layout ("linux-kernel/ — sparse Linux
 * kernel checkout containing rtlwifi") and live grep against the actual
 * checkout. This compat file compiles real rtlwifi source *against* it,
 * per the handover's stated approach — it does not redefine these types
 * itself.
 */
#include "wifi.h"

/* ------------------------------------------------------------------ */
/* Globals — mirrors rtw88_compat.c's g_rtw88_hw pattern               */
/* (findings.md Section 50.3-50.4, Section 51.2)                       */
/* ------------------------------------------------------------------ */

/*
 * Forward (tentative) declaration, matching rtw88_compat.c's own
 * intentional split-declaration idiom (findings.md 51.2): a file-scope
 * static tentative definition here, a defining declaration with
 * initializer later in the file. This lets earlier code in this
 * translation unit (e.g. callback-registration logic, see
 * rtlwifi_set_hw_callbacks() below) reference the pointer without a
 * header-level extern.
 */
static struct ieee80211_hw *g_rtlwifi_hw = NULL;

/*
 * Per-hw callback vtable, mirroring rtw88_compat.c's g_hw_cbs /
 * hw->kext_hw indirection (findings.md Section 52.3: ieee80211_rx_irqsafe
 * reads ctx = hw->kext_hw, falling back to a global if unset). Kept as a
 * forward declaration; struct layout defined in rtlwifi_compat.h.
 */
static struct rtlwifi_hw_callbacks *g_hw_cbs;
static void *g_kext_hw;

/* ------------------------------------------------------------------ */
/* ieee80211_alloc_hw() — two-layer private-data allocation            */
/* (findings.md Section 48.4, Section 49.2)                            */
/* ------------------------------------------------------------------ */

/*
 * rtl_pci_probe() (pci.c, confirmed full body read, findings.md Section
 * 49.2) allocates a single combined block:
 *
 *   hw = ieee80211_alloc_hw(sizeof(struct rtl_pci_priv) +
 *                            sizeof(struct rtl_priv), &rtl_ops);
 *   rtlpriv = hw->priv;                    // struct rtl_priv *
 *   pcipriv = (void *)rtlpriv->priv;       // tail-allocated rtl_pci_priv
 *
 * This is the mechanical mirror of rtw88's:
 *   drv_data_size = sizeof(struct rtw_dev) + sizeof(struct rtw_pci);
 *   hw = ieee80211_alloc_hw(drv_data_size, &rtw_ops);
 *   rtwdev = hw->priv;
 *   rtwpci = (struct rtw_pci *)rtwdev->priv;
 *
 * confirmed as structurally identical across both driver families
 * (findings.md Section 48.4 — "strong cross-family validation").
 *
 * NOTE the argument order is inverted from what a naive reading of
 * "rtl_priv then tail-allocated rtl_pci_priv" (Sections 31-39) would
 * suggest: rtl_pci_probe's actual sizeof() expression is
 * (rtl_pci_priv + rtl_priv), not (rtl_priv + rtl_pci_priv). What
 * matters for this shim is only the *total size* and that hw->priv
 * always resolves to struct rtl_priv* per rtl_priv(hw) (handover line
 * 144) — the tail struct's own internal offset is rtlwifi's problem,
 * not this allocator's.
 */
struct ieee80211_hw *ieee80211_alloc_hw(size_t priv_data_len,
                                         const struct ieee80211_ops *ops)
{
    struct ieee80211_hw *hw =
        (struct ieee80211_hw *)kzalloc(sizeof(*hw) + priv_data_len, GFP_KERNEL);
    if (!hw)
        return NULL;

    hw->priv = (u8 *)hw + sizeof(*hw);

    hw->wiphy = (struct wiphy *)kzalloc(sizeof(struct wiphy), GFP_KERNEL);
    if (!hw->wiphy) {
        kfree(hw);
        return NULL;
    }

    /*
     * offset-0 struct-layout trick (findings.md Section 51.3-51.4):
     * wiphy->_dev at offset 0 of struct wiphy, ieee80211_hw::priv at
     * offset 0 of struct ieee80211_hw. Reinterpreting a wiphy* as an
     * ieee80211_hw* and reading ->priv off it lands on the same memory
     * as the real rtl_priv pointer. Replicated here verbatim per
     * Section 51.4's guidance ("should replicate the same offset-0
     * trick... no new rtlwifi-specific design question raised").
     *
     * FIXME: this is fragile-by-construction (Section 51.4) and was
     * only ever verified against struct wiphy / struct ieee80211_hw's
     * *rtw88-compat-shim* layout, not necessarily upstream's real
     * struct definitions byte-for-byte. Low risk for this single-chip,
     * single-adapter port (same argument as 51.4), but worth a static
     * assert against offsetof(struct wiphy, _dev) == 0 and
     * offsetof(struct ieee80211_hw, priv) == 0 once both headers are
     * finalized, rather than trusting the comment alone.
     */
    hw->wiphy->_dev = hw->priv;

    /*
     * wiphy->name default (findings.md Section 73.3): kzalloc above
     * already zeroes struct wiphy, so wiphy->name starts as "" rather
     * than garbage — safe, but a poor diagnostic string for
     * wiphy_name()'s callers (currently pci.c's WARN_ONCE probe-failure
     * logging). "wlan0" mirrors the placeholder rtw88_compat.c's
     * equivalent init path uses; the kext driving layer may overwrite
     * this with the real assigned interface name later.
     */
    strlcpy(hw->wiphy->name, "wlan0", sizeof(hw->wiphy->name));

    hw->ops = ops;

    /*
     * hw->conf.chandef defaults: rtw88_compat.c sets a default chan/
     * width pair here (Section 50.3). rtlwifi's own default channel
     * plumbing (rtl_op_config's IEEE80211_CONF_CHANGE_CHANNEL path,
     * Section 40.3) may or may not need an equivalent seed value before
     * first use — carried over defensively since it's cheap and matches
     * the reference shim; safe to delete later if rtlwifi's probe path
     * turns out to always set this before anything reads it.
     */
        /* Static fallback channel: 2.4 GHz band, CH1 (2412 MHz). Some rx
     * paths dereference hw->conf.chandef.chan unconditionally, so it
     * must never be NULL before the driver's own config path runs. */
    static struct ieee80211_channel s_default_chan = {
        .band        = NL80211_BAND_2GHZ,
        .center_freq = 2412,
        .hw_value    = 1,
        .flags       = 0,
        .max_power   = 20,
    };
    hw->conf.chandef.chan = &s_default_chan;
    hw->conf.chandef.width = NL80211_CHAN_WIDTH_20_NOHT;

    /*
     * "belt: global fallback" (findings.md Section 50.3-50.5): populate
     * the global as early as physically possible — before
     * rtl_init_core(), before any chip setup, before
     * ieee80211_register_hw() proper (Section 49.2's confirmed
     * ordering: this call is the very first Linux-API call inside
     * rtl_pci_probe()). This makes rtlwifi_get_hw() safe to call even
     * from early-probe error-unwind paths (e.g. the fail3: block,
     * Section 49.5), not just after full registration succeeds.
     */
    g_rtlwifi_hw = hw;

    return hw;
}


/* ------------------------------------------------------------------ */
/* hw lookup — belt-and-suspenders pair                                */
/* (findings.md Section 51.3)                                          */
/* ------------------------------------------------------------------ */

/*
 * "Suspenders": primary lookup when a wiphy* is on hand. Not a real
 * search — a pointer-reinterpretation trick relying on the offset-0
 * layout coincidence set up in ieee80211_alloc_hw() above.
 */
struct ieee80211_hw *wiphy_to_ieee80211_hw(struct wiphy *wiphy)
{
    if (!wiphy)
        return g_rtlwifi_hw;
    return (struct ieee80211_hw *)wiphy;
}

/* "Belt": external-linkage accessor for the static global, for callers
 * with no wiphy* in scope at all (mirrors rtw88_get_hw()). */
struct ieee80211_hw *rtlwifi_get_hw(void)
{
    return g_rtlwifi_hw;
}

/* ------------------------------------------------------------------ */
/* Callback registration — mirrors rtw88_set_hw_callbacks()            */
/* (findings.md Section 52.1, 52.3)                                    */
/* ------------------------------------------------------------------ */

/*
 * CONFIRMED via live grep against the real repo — both the call site
 * and the full struct body, not just the signature:
 *
 *   src/kext/RTW88IEEE80211.cpp:565   rtw88_set_hw_callbacks(&cbs, this);
 *
 *   src/compat/rtw88_compat.c:363-365
 *     struct rtw88_hw_callbacks {
 *         void (*rx_frame)(void *kext_hw, struct sk_buff *skb);
 *         void (*tx_status)(void *kext_hw, struct sk_buff *skb);
 *         void (*scan_done)(void *kext_hw, bool aborted);
 *     };
 *
 *   src/compat/rtw88_compat.c:491-499
 *     void rtw88_set_hw_callbacks(struct rtw88_hw_callbacks *cbs, void *kext_hw)
 *     {
 *         g_hw_cbs  = cbs;
 *         g_kext_hw = kext_hw;
 *         // Populate the kext_hw back-pointer in the hw struct so all
 *         // callbacks that dereference hw->kext_hw actually reach the
 *         // RTW88IEEE80211 object.
 *         if (g_rtw88_hw)
 *             g_rtw88_hw->kext_hw = kext_hw;
 *     }
 *
 * This corrects two earlier drafts in this file:
 *  (a) the original guess that took a leading struct ieee80211_hw *hw
 *      parameter — wrong, confirmed no hw argument exists;
 *  (b) the immediately-prior fix's claim that there is therefore "no
 *      hw->kext_hw branch inside this function" — also wrong. The real
 *      function DOES write hw->kext_hw, it just reaches the hw pointer
 *      via the g_rtw88_hw global (populated earliest-possible in
 *      ieee80211_alloc_hw, per Section 50.3-50.4) rather than via a
 *      parameter. Both drafts were plausible-looking inferences from
 *      partial evidence; only the grep against the real body settled it.
 */
void rtlwifi_set_hw_callbacks(struct rtlwifi_hw_callbacks *cbs, void *kext_hw)
{
    g_hw_cbs = cbs;
    g_kext_hw = kext_hw;

    if (g_rtlwifi_hw)
        g_rtlwifi_hw->kext_hw = kext_hw;
}

/* ------------------------------------------------------------------ */
/* RX interception — mirrors ieee80211_rx_irqsafe()                    */
/* (findings.md Section 52.3)                                          */
/* ------------------------------------------------------------------ */

/*
 * RX interception, CONFIRMED against the real repo
 * (src/compat/rtw88_compat.c:501-509, ieee80211_rx_irqsafe):
 *   void *ctx = hw ? hw->kext_hw : NULL;
 *   if (!ctx) ctx = g_kext_hw;
 *   if (g_hw_cbs && g_hw_cbs->rx_frame)
 *       g_hw_cbs->rx_frame(ctx, skb);
 *   else
 *       kfree_skb(skb);
 *
 * Correction from the previous draft of this function: the else branch
 * MUST kfree_skb() when no callback is registered — omitted in the
 * earlier version of this file, which would have silently leaked the
 * skb whenever rtlwifi_set_hw_callbacks() hadn't run yet (e.g. any RX
 * that arrives before probe finishes wiring callbacks). Fixed below.
 *
 * rtlwifi's own RX code (deep in rtl8188ee/trx.c + shared base.c/core.c
 * RX paths — not traced call-by-call in this investigation the way
 * rtw88's mac.c/rx.c/pci.c RX code wasn't either, per Section 52.1's
 * scope note) calls into whatever Linux/mac80211 RX delivery function
 * rtlwifi uses. TODO: confirm rtlwifi calls ieee80211_rx_irqsafe() (or
 * ieee80211_rx_napi(), also present in the real shim, see below) at the
 * same call shape as rtw88 before assuming this intercept point is
 * correct — not yet grepped against the real rtlwifi tree specifically.
 */
void ieee80211_rx_irqsafe(struct ieee80211_hw *hw, struct sk_buff *skb)
{
    void *ctx = hw ? hw->kext_hw : NULL;
    if (!ctx)
        ctx = g_kext_hw;

    if (g_hw_cbs && g_hw_cbs->rx_frame)
        g_hw_cbs->rx_frame(ctx, skb);
    else
        kfree_skb(skb);
}

/*
 * ieee80211_rx_napi() — CONFIRMED, src/compat/rtw88_compat.c:511-515:
 *   void ieee80211_rx_napi(struct ieee80211_hw *hw, struct ieee80211_sta *sta,
 *                          struct sk_buff *skb, struct napi_struct *napi)
 *   {
 *       ieee80211_rx_irqsafe(hw, skb);
 *   }
 * A thin forwarding wrapper — extra sta/napi args accepted and dropped,
 * same underlying delivery path as ieee80211_rx_irqsafe. rtlwifi may
 * call either the _irqsafe or _napi form; providing both means the
 * intercept works regardless of which one rtlwifi's own RX code uses
 * (still TODO to confirm which, per the note above).
 */
void ieee80211_rx_napi(struct ieee80211_hw *hw, struct ieee80211_sta *sta,
                        struct sk_buff *skb, struct napi_struct *napi)
{
    ieee80211_rx_irqsafe(hw, skb);
}

/*
 * TX-status interception — CONFIRMED, src/compat/rtw88_compat.c:518-525:
 *   void ieee80211_tx_status_irqsafe(struct ieee80211_hw *hw, struct sk_buff *skb)
 *   {
 *       void *ctx = hw ? hw->kext_hw : NULL;
 *       if (!ctx) ctx = g_kext_hw;
 *       if (g_hw_cbs && g_hw_cbs->tx_status)
 *           g_hw_cbs->tx_status(ctx, skb);
 *       else
 *           kfree_skb(skb);
 *   }
 * Same ctx-then-global-fallback shape as RX, dispatching to the
 * tx_status member confirmed in the callback struct (rtlwifi_compat.h).
 * This resolves the earlier open TODO ("tx_status/scan_done... exact
 * function names weren't captured").
 */
void ieee80211_tx_status_irqsafe(struct ieee80211_hw *hw, struct sk_buff *skb)
{
    void *ctx = hw ? hw->kext_hw : NULL;
    if (!ctx)
        ctx = g_kext_hw;

    if (g_hw_cbs && g_hw_cbs->tx_status)
        g_hw_cbs->tx_status(ctx, skb);
    else
        kfree_skb(skb);
}

/*
 * ieee80211_tx_status() — CONFIRMED, src/compat/rtw88_compat.c:527-530:
 *   void ieee80211_tx_status(struct ieee80211_hw *hw, struct sk_buff *skb)
 *   {
 *       ieee80211_tx_status_irqsafe(hw, skb);
 *   }
 * Thin forwarding wrapper, same relationship as rx_irqsafe/rx_napi
 * above.
 */
void ieee80211_tx_status(struct ieee80211_hw *hw, struct sk_buff *skb)
{
    ieee80211_tx_status_irqsafe(hw, skb);
}

/*
 * ieee80211_free_txskb() — CONFIRMED, src/compat/rtw88_compat.c:532-535:
 *   void ieee80211_free_txskb(struct ieee80211_hw *hw, struct sk_buff *skb)
 *   {
 *       kfree_skb(skb);
 *   }
 * Not a callback intercept at all — just a plain skb-free wrapper
 * matching upstream mac80211's own ieee80211_free_txskb() semantics
 * (driver calls this when a TX frame is being dropped without ever
 * reaching a real tx-status report). Included here for API completeness
 * since rtlwifi's shared tx-error paths are likely to call it.
 */
void ieee80211_free_txskb(struct ieee80211_hw *hw, struct sk_buff *skb)
{
    kfree_skb(skb);
}

/*
 * Scan-completion interception — CONFIRMED,
 * src/compat/rtw88_compat.c:537-544:
 *   void ieee80211_scan_completed(struct ieee80211_hw *hw,
 *                                  struct cfg80211_scan_info *info)
 *   {
 *       void *ctx = hw ? hw->kext_hw : NULL;
 *       if (!ctx) ctx = g_kext_hw;
 *       if (g_hw_cbs && g_hw_cbs->scan_done)
 *           g_hw_cbs->scan_done(ctx, info ? info->aborted : false);
 *   }
 * Same ctx-then-global-fallback shape, dispatching to scan_done.
 * NOTE this one has no kfree_skb-equivalent else-branch fallback (there
 * is no skb here to free) — confirmed intentional, not an oversight
 * carried over from the RX/TX-status functions.
 *
 * This is the real function rtlwifi's rtl_op_sw_scan_complete()
 * (core.c:1442+, already confirmed in the scan-flag investigation
 * above — the same function that clears mac->act_scanning) would need
 * to call, or whatever rtlwifi's actual scan-completion call chain
 * turns out to invoke. TODO: confirm rtl_op_sw_scan_complete() (or
 * whatever ultimately signals scan completion up through rtlwifi's
 * mac80211-facing API) actually calls ieee80211_scan_completed() with
 * this signature — not yet grepped against the real rtlwifi tree.
 */
void ieee80211_scan_completed(struct ieee80211_hw *hw,
                               struct cfg80211_scan_info *info)
{
    void *ctx = hw ? hw->kext_hw : NULL;
    if (!ctx)
        ctx = g_kext_hw;

    if (g_hw_cbs && g_hw_cbs->scan_done)
        g_hw_cbs->scan_done(ctx, info ? info->aborted : false);
}

/* ------------------------------------------------------------------ */
/* rtlwifi_sw_scan_start / _switch_channel / _complete()               */
/* rtlwifi-side equivalents of rtw88_sw_scan_start/_switch_channel/    */
/* _complete(), which RTW88IEEE80211.cpp's runManualScan() calls       */
/* (findings.md Section 59.4). CONFIRMED against live core.c read      */
/* this session, not guessed:                                          */
/*                                                                      */
/*   - rtl_ops has real sw_scan_start/sw_scan_complete members         */
/*     (core.c:1895 table, core.c:1409/1444 bodies) — called via       */
/*     hw->ops->, same as every other member this port already uses.  */
/*   - rtl_op_sw_scan_complete() does NOT call                         */
/*     ieee80211_scan_completed() anywhere (grep against the full      */
/*     file: zero matches) — it only clears mac->act_scanning and does */
/*     internal bookkeeping (BT coexist, LED, link-state). This        */
/*     resolves the TODO on ieee80211_scan_completed() above: that     */
/*     function is real and correctly wired to the scan_done compat    */
/*     callback, but rtlwifi's own sw_scan_complete never reaches it.  */
/*     The actual scan-done signal for this port's manual-scan loop    */
/*     comes from RTW88IEEE80211.cpp's runManualScan() calling         */
/*     scanDone() directly at the end of its channel loop — this is    */
/*     unaffected by rtlwifi_sw_scan_complete() below and needs no     */
/*     change on the IOKit side.                                       */
/*   - There is no scan-specific channel-switch member in rtl_ops.     */
/*     Real per-channel switching happens via rtlpriv->cfg->ops->      */
/*     switch_channel(hw) (core.c:754), invoked FROM INSIDE             */
/*     rtl_op_config() when called with                                */
/*     changed & IEEE80211_CONF_CHANGE_CHANNEL set (core.c:625-756,    */
/*     confirmed full body read). rtlpriv->cfg->ops is rtl_hal_ops —   */
/*     the SAME per-chip internal vtable sw.c's set_key belongs to     */
/*     (Section 59, the earlier rtl_hal_ops/ieee80211_ops distinction) */
/*     — this compat layer must not reach into it directly, matching   */
/*     the same abstraction boundary already respected elsewhere in    */
/*     this file. rtlwifi_sw_scan_switch_channel() below therefore     */
/*     goes through hw->ops->config(), exactly as real rtlwifi/        */
/*     mac80211 callers do, never touching rtlpriv->cfg->ops directly. */
/*                                                                      */
/*   rtl_op_config()'s real signature (confirmed, core.c line 569) is: */
/*       static int rtl_op_config(struct ieee80211_hw *hw,             */
/*                                 int radio_idx, u32 changed)          */
/*   THREE parameters — CONFIRMED to exactly match the compat          */
/*   mac80211.h struct's config member (grepped directly:              */
/*   compat/net/mac80211.h:831 — "int (*config)(struct ieee80211_hw    */
/*   *hw, int radio_idx, u32 changed);"). No signature mismatch.       */
/*   radio_idx passed as 0 below, the same single-radio assumption     */
/*   already used elsewhere in this port (no multi-radio/MLO handling  */
/*   anywhere) — radio_idx is part of this compat layer's existing     */
/*   ieee80211_ops shape generally, not rtlwifi-specific, so 0 is the  */
/*   correct default here.                                             */
/* ------------------------------------------------------------------ */

void rtlwifi_sw_scan_start(struct ieee80211_hw *hw, struct ieee80211_vif *vif,
                            const u8 *mac_addr)
{
    if (!hw || !hw->ops || !hw->ops->sw_scan_start)
        return;
    hw->ops->sw_scan_start(hw, vif, mac_addr);
}

void rtlwifi_sw_scan_switch_channel(struct ieee80211_hw *hw)
{
    /*
     * Caller (runManualScan()) is expected to set hw->conf.chandef.chan
     * (and .width/.center_freq1 if relevant) BEFORE calling this, same
     * pattern as the rtw88 reference and as rtl_op_config's own body
     * assumes (it reads hw->conf.chandef.chan directly, does not take
     * a channel parameter itself). radio_idx=0: confirmed correct per
     * file-header comment above (single-radio default, compat header's
     * config member signature verified to match rtl_op_config exactly).
     */
    if (!hw || !hw->ops || !hw->ops->config)
        return;
    hw->ops->config(hw, /* radio_idx */ 0, IEEE80211_CONF_CHANGE_CHANNEL);
}

void rtlwifi_sw_scan_complete(struct ieee80211_hw *hw, struct ieee80211_vif *vif)
{
    /*
     * Passthrough only. Per the comment block above, this does NOT
     * itself signal scan completion to the IOKit layer — it only runs
     * rtlwifi's real internal scan-complete bookkeeping (clearing
     * mac->act_scanning, BT coexist/LED/link-state updates). The
     * caller (runManualScan()) still calls scanDone() itself
     * separately, unchanged.
     */
    if (!hw || !hw->ops || !hw->ops->sw_scan_complete)
        return;
    hw->ops->sw_scan_complete(hw, vif);
}

/* ------------------------------------------------------------------ */
/* Bucket A — rtlwifi_register_vif/_unregister_vif/_hw_scan_supported/ */
/* _connect_hw_setup/_restore_connected_hw()                           */
/* (findings.md Section 81.2, handover THIRTEENTH UPDATE)              */
/* ------------------------------------------------------------------ */

/*
 * CONFIRMED no-ops — see rtlwifi_compat.h's block comment for the full
 * rationale. rtl_op_add_interface()/rtl_op_remove_interface() (real
 * core.c, live-read this session) already do everything rtlwifi itself
 * needs for vif registration as a direct part of their own bodies;
 * there is no separate step left over to bridge to. Not deleted outright
 * so the call sites in RTW88IEEE80211.cpp (right after
 * ops->add_interface / right before ops->remove_interface) don't need
 * restructuring, and so a future reader sees this was checked, not
 * missed.
 */
void rtlwifi_register_vif(struct ieee80211_vif *vif)
{
    (void)vif;
}

void rtlwifi_unregister_vif(void)
{
}

/*
 * CONFIRMED always false — rtlwifi's rtl_ops vtable has no hw_scan/
 * cancel_hw_scan members (live core.c read, this session and handover
 * item 28 previously); rtlwifi is sw_scan-only for every chip in this
 * driver family, not just RTL8188EE. A static architectural fact, not
 * a per-hw runtime property, but takes hw for call-site-signature
 * symmetry with the rest of this bridge layer.
 */
bool rtlwifi_hw_scan_supported(struct ieee80211_hw *hw)
{
    (void)hw;
    return false;
}

/*
 * Shared implementation for rtlwifi_connect_hw_setup() and
 * rtlwifi_restore_connected_hw() — both call sites in
 * RTW88IEEE80211.cpp do the identical job (set channel + BSSID,
 * bypassing the mac80211-ops path) at two different points in the
 * connection lifecycle (initial auth vs. post-scan restore), so one
 * real implementation backs both public names rather than duplicating
 * the mutex/mmio-call sequence twice.
 *
 * CONFIRMED sequence, live core.c read:
 *   - rtlpriv->locks.conf_mutex: the same mutex rtl_op_config() and
 *     rtl_op_add_interface()/_remove_interface() all take around their
 *     hardware-touching bodies (core.c:579, 216, 305) — held here for
 *     the same reason, not a new invented lock.
 *   - rtlpriv->cfg->ops->switch_channel(hw): the real per-chip channel
 *     switch (core.c:754, inside rtl_op_config's
 *     IEEE80211_CONF_CHANGE_CHANNEL branch) — called directly, skipping
 *     the rest of rtl_op_config's body (LPS/IDLE handling, bw40/80
 *     derivation — see header comment for the bandwidth-derivation
 *     simplification this drops).
 *   - rtlpriv->cfg->ops->set_hw_reg(hw, HW_VAR_BSSID, bssid): the real
 *     BSSID register write (core.c:1246, inside rtl_op_bss_info_changed's
 *     BSS_CHANGED_BSSID branch) — called directly, skipping the rest of
 *     that branch (rtl_lps_leave() on disconnect, sta lookup/RCU
 *     section) which is exactly the stall-risk path these functions
 *     exist to avoid.
 *   - mac->bssid updated via rtl_mac(rtlpriv), mirroring the real
 *     branch's own `memcpy(mac->bssid, bss_conf->bssid, ETH_ALEN);`
 *     (core.c, same BSS_CHANGED_BSSID branch) — kept so any other real
 *     rtlwifi code that reads mac->bssid directly (not just the
 *     hardware register) stays consistent.
 */
static void _rtlwifi_set_channel_and_bssid(struct ieee80211_hw *hw,
                                            struct ieee80211_vif *vif,
                                            const u8 *bssid)
{
    (void)vif;

    if (!hw || !hw->priv || !bssid)
        return;

    struct rtl_priv *rtlpriv = (struct rtl_priv *)hw->priv;

    if (!rtlpriv->cfg || !rtlpriv->cfg->ops)
        return;

    mutex_lock(&rtlpriv->locks.conf_mutex);

    if (hw->conf.chandef.chan && rtlpriv->cfg->ops->switch_channel)
        rtlpriv->cfg->ops->switch_channel(hw);

    if (rtlpriv->cfg->ops->set_hw_reg)
        rtlpriv->cfg->ops->set_hw_reg(hw, HW_VAR_BSSID, (u8 *)bssid);

    memcpy(rtl_mac(rtlpriv)->bssid, bssid, ETH_ALEN);

    mutex_unlock(&rtlpriv->locks.conf_mutex);
}

void rtlwifi_connect_hw_setup(struct ieee80211_hw *hw,
                               struct ieee80211_vif *vif,
                               const u8 *bssid)
{
    _rtlwifi_set_channel_and_bssid(hw, vif, bssid);
}

void rtlwifi_restore_connected_hw(struct ieee80211_hw *hw,
                                   struct ieee80211_vif *vif,
                                   const u8 *bssid)
{
    _rtlwifi_set_channel_and_bssid(hw, vif, bssid);
}

/*
 * Queue-control no-ops — CONFIRMED, src/compat/rtw88_compat.c:546-548:
 *   void ieee80211_stop_queues(struct ieee80211_hw *hw)  {}
 *   void ieee80211_wake_queues(struct ieee80211_hw *hw)  {}
 *   void ieee80211_stop_queue(struct ieee80211_hw *hw, int q) {}
 * Deliberate no-ops in the rtw88 reference — consistent with Section
 * 52.2's confirmed finding that mac80211's own TX queueing/txq
 * machinery is bypassed entirely in this port (frames are hand-built
 * and dispatched directly via hw->ops->tx). Since rtlwifi's TX path is
 * meant to be bypassed the same way (per the TX/RX dispatch note
 * below), these should stay no-ops here too rather than growing real
 * queue-management logic.
 */
void ieee80211_stop_queues(struct ieee80211_hw *hw) {}
void ieee80211_wake_queues(struct ieee80211_hw *hw) {}
void ieee80211_stop_queue(struct ieee80211_hw *hw, int q) {}

/* ------------------------------------------------------------------ */
/* rtlwifi_is_scanning() — equivalent of rtw88_is_scanning()           */
/* (findings.md Section 53.2, 53.5)                                    */
/* ------------------------------------------------------------------ */

/*
 * rtlwifi's own scan-in-progress flag, CONFIRMED via live grep + read
 * against the real linux-kernel/.../rtlwifi source tree (not inferred):
 *
 *   wifi.h:2664    struct rtl_mac mac80211;      // field inside rtl_priv
 *   wifi.h:1467    bool act_scanning;             // field inside rtl_mac
 *   wifi.h:2755    #define rtl_mac(rtlpriv) (&((rtlpriv)->mac80211))
 *
 *   core.c:1418 (rtl_op_sw_scan_start, the real .sw_scan_start mac80211 op)
 *       mac->act_scanning = true;
 *   core.c:1451 (rtl_op_sw_scan_complete, the real .sw_scan_complete op)
 *       mac->act_scanning = false;
 *
 * act_scanning is read at 7 call sites across base.c, core.c, pci.c,
 * ps.c, rc.c — the driver's general "is a scan in progress" gate,
 * structurally the same role RTW_FLAG_SCANNING plays for rtw88
 * (Section 53.2), just a plain bool instead of a bitflag-in-a-word.
 *
 * NOT MAC80211_LINKED_SCANNING (wifi.h ~line 931) — that's one value of
 * a 4-state rtl_link_state connection-state enum (NOLINK/LINKING/
 * LINKED/LINKED_SCANNING), not a standalone scan flag. It only becomes
 * that value when a scan starts *while already connected*
 * (core.c:1428-1429: mac->link_state only flips to
 * MAC80211_LINKED_SCANNING from MAC80211_LINKED); it stays at whatever
 * it already was if a scan starts pre-association. act_scanning, by
 * contrast, is unconditionally true/false around every scan regardless
 * of link state — the correct general-purpose flag for this accessor.
 *
 * This replaces the earlier #error stub (findings.md Section 53.5/54.5
 * item 2, "not yet located" — now located and read directly).
 */
bool rtlwifi_is_scanning(void)
{
    if (!g_rtlwifi_hw || !g_rtlwifi_hw->priv)
        return false;

    struct rtl_priv *rtlpriv = (struct rtl_priv *)g_rtlwifi_hw->priv;

    return rtl_mac(rtlpriv)->act_scanning;
}

/* ------------------------------------------------------------------ */
/* rtlwifi_get_fw_version() / rtlwifi_get_stats() — Bucket E            */
/* (findings.md Section 81.2/83.2)                                      */
/* ------------------------------------------------------------------ */

/*
 * fw_version/fw_subversion: struct rtl_hal fields, wifi.h:1613-1614,
 * reached via rtl_hal(rtlpriv) (wifi.h:2756) — same cast/macro pattern
 * as rtlwifi_is_scanning() above, just a different sub-struct.
 *
 * Output fw_subversion is u8 (matching RTW88StateResult's actual field
 * width, RTW88UserClient.hpp), narrowed here from wifi.h's real u16
 * fw_subversion — an explicit, visible truncation, not a raw pointer of
 * the wrong width handed across the call site.
 */
void rtlwifi_get_fw_version(u16 *fw_version, u8 *fw_subversion)
{
    if (fw_version)
        *fw_version = 0;
    if (fw_subversion)
        *fw_subversion = 0;

    if (!g_rtlwifi_hw || !g_rtlwifi_hw->priv)
        return;

    struct rtl_priv *rtlpriv = (struct rtl_priv *)g_rtlwifi_hw->priv;

    if (fw_version)
        *fw_version = rtl_hal(rtlpriv)->fw_version;
    if (fw_subversion)
        *fw_subversion = (u8)rtl_hal(rtlpriv)->fw_subversion;
}

/*
 * tx_bytes/rx_bytes: struct wireless_stats's txbytesunicast/
 * rxbytesunicast fields (wifi.h:~1096-1099), reached via the plain
 * rtlpriv->stats field (wifi.h:2680 — struct wireless_stats stats;
 * no accessor macro exists for this one, unlike mac80211/rtlhal/efuse).
 * Unicast-only, matching what the struct actually separates out;
 * multicast/broadcast counters exist too but no existing call site
 * asks for them.
 *
 * Output width is u32 (matching RTW88StateResult's actual field width,
 * RTW88UserClient.hpp), narrowed here from wifi.h's real u64 counters —
 * an explicit, visible truncation, not a raw pointer of the wrong width
 * handed across the call site. Wraps past ~4GB of unicast traffic; this
 * is a diagnostics counter, not a byte-accurate accounting field.
 */
void rtlwifi_get_stats(u32 *tx_bytes, u32 *rx_bytes)
{
    if (tx_bytes)
        *tx_bytes = 0;
    if (rx_bytes)
        *rx_bytes = 0;

    if (!g_rtlwifi_hw || !g_rtlwifi_hw->priv)
        return;

    struct rtl_priv *rtlpriv = (struct rtl_priv *)g_rtlwifi_hw->priv;

    if (tx_bytes)
        *tx_bytes = (u32)rtlpriv->stats.txbytesunicast;
    if (rx_bytes)
        *rx_bytes = (u32)rtlpriv->stats.rxbytesunicast;
}

/*
 * Expected call site, mirroring rtw88's doAuthenticate() bounded wait
 * (findings.md Section 53.3): a 100 x 50ms IOSleep loop, 5s ceiling,
 * *sleeping* not spinning, waiting for the scan-complete path to clear
 * the flag. On the rtlwifi side, the flag is now confirmed cleared
 * inside rtl_op_sw_scan_complete() (core.c:1451,
 * mac->act_scanning = false; — the real .sw_scan_complete mac80211 op),
 * the direct structural analog of rtw88's rtw_core_scan_complete().
 * Classified not-a-hazard for the same reason rtw88's version isn't
 * (Section 53.4: software-flag wait, not MMIO polling) — act_scanning
 * is a plain in-memory bool, set/cleared by real mac80211 ops, not a
 * hardware register. This comment documents the expected shape for
 * whoever wires up the rtlwifi-side doAuthenticate() equivalent — the
 * loop itself lives in the IOKit layer, not in this compat file.
 *
 *   for (int i = 0; i < 100; i++) {
 *       if (!rtlwifi_is_scanning()) break;
 *       IOSleep(50);
 *   }
 */

/*
 * CCMP-IV question — RESOLVED (findings.md Sections 52.4/54, live-read
 * against the real rtlwifi source tree):
 *
 *   base.c: rtl_skb_ether_type_ptr(hw, skb, is_enc)
 *     u8 encrypt_header_len = 0;
 *     switch (rtlpriv->sec.pairwise_enc_algorithm) {
 *     ...
 *     case AESCCMP_ENCRYPTION:
 *         encrypt_header_len = 8;  // CCMP_HDR_LEN
 *         break;
 *     }
 *     offset = mac_hdr_len + SNAP_SIZE;
 *     if (is_enc) offset += encrypt_header_len;
 *     return skb->data + offset;
 *
 * Confirms rtlwifi's own driver code treats the 8-byte CCMP header
 * (PN/ExtIV) as a distinct offset that must be explicitly skipped when
 * locating payload past it — the same 8-byte magic number and the same
 * mental model as rtw88's documented compensation (Section 52.3:
 * "rtw88 leaves the CCMP IV in the frame... deliverDataFrame() accounts
 * for it directly"). The caller comment ("should call before software
 * enc") ties this specifically to the not-yet-hardware-decrypted case,
 * so this alone doesn't 100% pin down every code path's post-hw-decrypt
 * behavior, but it does confirm rtlwifi uses the identical 8-byte
 * CCMP-header-skip shape internally, not a different offset or a
 * hardware-strips-it-cleanly model.
 *
 * Port decision: treat this the same as rtw88 — skip header + 8 bytes
 * CCMP at the IOKit-side deliverDataFrame()-equivalent, matching both
 * rtw88's confirmed behavior and rtlwifi's own internal offset
 * arithmetic. Still worth a boot-test confirmation (a wrong guess here
 * fails obviously — garbled payload / dropped frames — so low risk to
 * ship provisionally and verify at runtime, per the original plan of
 * not blocking on this).
 */

/* ------------------------------------------------------------------ */
/* TX/RX dispatch — direct-call bypass pattern                         */
/* (findings.md Section 52.2, 52.4 item 1)                             */
/* ------------------------------------------------------------------ */

/*
 * No wrapper function is defined here for TX. Per Section 52.2/52.4,
 * rtw88's IOKit layer calls _hw->ops->tx(_hw, &ctrl, skb) directly —
 * a real function-pointer call into rtw_ops_tx/rtw_tx, entirely
 * bypassing mac80211's own TX queueing/txq machinery. The rtlwifi
 * equivalent is the same shape: the IOKit-side txDataFrame() analog
 * should call hw->ops->tx(hw, &ctrl, skb) directly, landing on
 * rtl_op_tx -> intf_ops->adapter_tx -> cfg->ops->fill_tx_desc/
 * tx_polling (handover's confirmed "Full TX path", line 276).
 *
 * This compat file's job for TX is therefore just making sure
 * hw->ops is populated correctly (it is — passed straight through in
 * ieee80211_alloc_hw() above) and NOT interposing anything extra on
 * the tx op, mirroring rtw88_compat.c's own choice not to wrap ->tx.
 *
 * TODO: rtlwifi's RX_FLAG_DECRYPTED / CCMP-IV handling (findings.md
 * Section 54, resolved above this block) means the IOKit-side
 * deliverDataFrame()-equivalent should skip header + 8 bytes CCMP,
 * matching rtw88's confirmed behavior and rtlwifi's own
 * rtl_skb_ether_type_ptr() offset arithmetic in base.c:
 *
 *   // skip 802.11 header + 8-byte CCMP header/IV, matching rtw88's
 *   // compensation and rtlwifi's own base.c offset math
 *
 * Ship this provisionally; confirm at boot-test time (findings.md open
 * item 8 — "not answerable by source reading" for the final hw-behavior
 * confirmation, though the driver's own internal model is now settled).
 */

/* ------------------------------------------------------------------ */
/* Rate-control registration — real functions, not stubs               */
/* (new this session, findings.md Section 66's rc.c fix cluster)       */
/* ------------------------------------------------------------------ */

/*
 * ieee80211_rate_control_register()/_unregister() — real upstream
 * mac80211 maintains a list of registered rate_control_ops and lets
 * drivers select one by name (hw->rate_control_algorithm, confirmed
 * real in base.c — Section 66.3). This compat layer has no such
 * registry: there is exactly one rate-control algorithm ever compiled
 * in for a single-chip RTL8188EE port (rc.c's own rtl_rate_ops), so a
 * full named-lookup registry would be machinery with nothing to
 * dispatch between.
 *
 * Implementation: store the one registered ops pointer in a static
 * global and expose it via rtlwifi_get_rate_control_ops() for
 * whichever real call site ends up needing to invoke .get_rate/
 * .rate_init/etc (the IOKit TX path per findings.md Section 52, or
 * rtl_op_sta_add's real rate_control_rate_init()-equivalent call —
 * neither is traced yet; this only wires the registration half real
 * rc.c code needs to link and run rtl_rate_control_register() without
 * crashing, matching this compat layer's existing minimal-real-
 * function approach). FIXME: no real call site for get_rate/rate_init
 * has been traced yet — TX path (Section 52) currently bypasses
 * mac80211 rate selection entirely by calling hw->ops->tx() directly,
 * so it's not yet confirmed whether/where this registered ops table
 * is actually invoked from in this port's architecture. Flagged, not
 * silently assumed unreachable.
 */
static const struct rate_control_ops *g_rtlwifi_rate_ops;

int ieee80211_rate_control_register(const struct rate_control_ops *ops)
{
    if (!ops)
        return -1; /* -EINVAL, avoiding a new errno.h dependency here */
    g_rtlwifi_rate_ops = ops;
    return 0;
}

void ieee80211_rate_control_unregister(const struct rate_control_ops *ops)
{
    if (g_rtlwifi_rate_ops == ops)
        g_rtlwifi_rate_ops = NULL;
}

const struct rate_control_ops *rtlwifi_get_rate_control_ops(void)
{
    return g_rtlwifi_rate_ops;
}

/* ------------------------------------------------------------------ */
/* Firmware-load teardown blocking                                     */
/* (findings.md Section 49.5)                                          */
/* ------------------------------------------------------------------ */

/*
 * Not implemented in this compat file directly — this is an IOKit
 * detach/teardown-path concern, not a Linux-API shim concern. Noted
 * here only as a cross-reference so whoever writes the teardown path
 * doesn't miss it:
 *
 * rtl_pci_probe()'s fail3: unwind label (Section 49.5) does:
 *   wait_for_completion(&rtlpriv->firmware_loading_complete);
 *   rtlpriv->cfg->ops->deinit_sw_vars(hw);
 *
 * i.e. teardown BLOCKS on firmware-load completion before calling
 * deinit_sw_vars, if core/PCI-init failed after firmware load was
 * already kicked off. The IOKit teardown/detach path must replicate
 * this specifically in whatever its fail3:-equivalent branch is, not
 * just at final detach (Section 49.5 "Port implication").
 */

/* ------------------------------------------------------------------ */
/* NOT YET PORTED — explicitly out of scope for this skeleton pass     */
/* ------------------------------------------------------------------ */

/*
 * - IRQ registration (rtl_pci_intr_mode_decide and friends): confirmed
 *   to run AFTER ieee80211_register_hw() (Section 49.4), same relative
 *   ordering as rtw88. Belongs in the IOKit probe-driving layer, not
 *   this compat file — no shim-level API gap identified for it.
 * - linux/sched.h exact symbol usage: still open, minor (Section 47.2,
 *   carried forward through every subsequent section's open-items
 *   list). Low risk per Section 47.3's assessment; not blocking this
 *   skeleton.
 * - linux/ip.h / linux/udp.h (rtl_is_special_data, base.c): data-layout
 *   task only (two fixed RFC 791/768 header structs, read-only field
 *   access) per Section 47.2 — deferred to whichever compat header
 *   ends up hosting rtlwifi's linux/* shim headers, not this .c file.
 */

/* ------------------------------------------------------------------ */
/* timer_list — real thread_call-backed implementation                 */
/* (findings.md Section 72.9 — closes the §67.8/§72.7 workqueue gap,   */
/* which turned out to include an equally-unimplemented timer.h too)   */
/* ------------------------------------------------------------------ */

/*
 * XNU thread_call callbacks take two opaque params; Linux timer
 * callbacks take one (struct timer_list *). The timer is passed as
 * param0; param1 is unused.
 */
static void rtlwifi_timer_thread_call_trampoline(thread_call_param_t param0,
                                                   thread_call_param_t param1)
{
    struct timer_list *t = (struct timer_list *)param0;
    (void)param1;
    if (t && t->function)
        t->function(t);
}

void timer_setup(struct timer_list *timer,
                 void (*func)(struct timer_list *t),
                 unsigned int flags)
{
    (void)flags;
    timer->function = func;
    timer->data     = 0;
    timer->expires  = 0;
    timer->active   = 0;
    timer->call     = thread_call_allocate(rtlwifi_timer_thread_call_trampoline,
                                            (thread_call_param_t)timer);
}

int mod_timer(struct timer_list *timer, unsigned long expires)
{
    uint64_t deadline;
    unsigned long now = jiffies;
    long delay_ms = (long)expires - (long)now;

    if (!timer->call) {
        /* Defensive: some rtlwifi paths may call mod_timer() on a
         * timer that only went through the legacy setup_timer() path
         * (which doesn't allocate .call). Lazily allocate here so
         * mod_timer still works rather than silently no-op'ing. */
        timer->call = thread_call_allocate(rtlwifi_timer_thread_call_trampoline,
                                            (thread_call_param_t)timer);
        if (!timer->call)
            return 0;
    }

    if (delay_ms < 0)
        delay_ms = 0;

    clock_interval_to_deadline((uint32_t)delay_ms, kMillisecondScale, &deadline);

    timer->expires = expires;
    timer->active  = 1;

    /* thread_call_enter_delayed re-arms if already pending, matching
     * mod_timer()'s real "reschedule, don't duplicate" semantics. */
    return thread_call_enter_delayed(timer->call, deadline) ? 1 : 0;
}

int del_timer(struct timer_list *timer)
{
    int was_pending;

    if (!timer->call) {
        timer->active = 0;
        return 0;
    }

    was_pending   = thread_call_cancel(timer->call) ? 1 : 0;
    timer->active = 0;
    return was_pending;
}

int del_timer_sync(struct timer_list *timer)
{
    int was_pending;

    if (!timer->call) {
        timer->active = 0;
        return 0;
    }

    /* _wait variant blocks until any in-flight callback finishes —
     * required by *_sync's real contract, and relied on by callers
     * (driver teardown) to guarantee the callback can't fire after
     * this returns. */
    was_pending   = thread_call_cancel_wait(timer->call) ? 1 : 0;
    timer->active = 0;
    return was_pending;
}

/* ------------------------------------------------------------------ */
/* workqueue — real thread_call-backed implementation                  */
/* ------------------------------------------------------------------ */

/*
 * Design: rather than a real kernel thread per workqueue (struct
 * workqueue_struct's .thread field is intentionally left unused —
 * see below), each work item's execution is driven by its own
 * thread_call, same idiom as timer_list above. XNU's thread_call
 * mechanism already runs on its own dedicated kernel threads, so a
 * second layer of manually-managed worker threads underneath
 * workqueue_struct would only duplicate what thread_call already
 * does, for the actual usage this driver has: exactly one
 * alloc_workqueue() call, 6 delayed-work items, 2 immediate-work
 * items (confirmed by full grep of DRIVER_SRCS/PCI_SRCS this
 * session — base.c, core.c, pci.c, ps.c). If true single-worker-
 * thread FIFO ordering is ever needed, workqueue_struct's
 * .thread/.lock/.queue fields are already there to build that out;
 * not required for current usage.
 *
 * Immediate (non-delayed) work items use work_struct.call directly.
 * Delayed work items use delayed_work.timer.call via a dedicated
 * trampoline below (kept separate from the generic timer trampoline
 * above so it can call work.func() with the right argument type
 * directly, rather than needing timer_list to carry a second,
 * work-flavored function pointer).
 */

static void rtlwifi_work_thread_call_trampoline(thread_call_param_t param0,
                                                  thread_call_param_t param1)
{
    struct work_struct *w = (struct work_struct *)param0;
    (void)param1;
    if (w) {
        w->pending = 0;
        if (w->func)
            w->func(w);
    }
}

static void rtlwifi_delayed_work_thread_call_trampoline(thread_call_param_t param0,
                                                          thread_call_param_t param1)
{
    struct delayed_work *dw = (struct delayed_work *)param0;
    (void)param1;
    if (dw) {
        dw->work.pending = 0;
        dw->timer.active = 0;
        if (dw->work.func)
            dw->work.func(&dw->work);
    }
}

/* system_wq / system_long_wq: real Linux code (and schedule_work()/
 * schedule_delayed_work() below, per real Linux semantics) targets
 * these globals directly rather than a driver-owned workqueue. Given
 * true per-queue ordering isn't implemented (each work item's
 * thread_call is independently scheduled regardless of which
 * workqueue_struct it's nominally queued on — see design note above),
 * these just need to be real, non-NULL, distinguishable pointers. */
static struct workqueue_struct rtlwifi_system_wq_storage;
static struct workqueue_struct rtlwifi_system_long_wq_storage;
struct workqueue_struct *system_wq      = &rtlwifi_system_wq_storage;
struct workqueue_struct *system_long_wq = &rtlwifi_system_long_wq_storage;

struct workqueue_struct *alloc_workqueue(const char *fmt, unsigned int flags,
                                          int max_active, ...)
{
    struct workqueue_struct *wq;
    va_list args;

    (void)flags;
    (void)max_active;

    wq = (struct workqueue_struct *)IOMalloc(sizeof(*wq));
    if (!wq)
        return NULL;

    wq->thread  = NULL;   /* deliberately unused — see design note above */
    wq->lock    = IOLockAlloc();
    INIT_LIST_HEAD(&wq->queue);
    wq->running = 1;
    wq->done    = 0;

    va_start(args, max_active);
    vsnprintf(wq->name, sizeof(wq->name), fmt, args);
    va_end(args);

    return wq;
}

struct workqueue_struct *alloc_ordered_workqueue(const char *name,
                                                  unsigned int flags)
{
    return alloc_workqueue("%s", flags, 1, name);
}

void destroy_workqueue(struct workqueue_struct *wq)
{
    if (!wq || wq == system_wq || wq == system_long_wq)
        return; /* never free the static globals */

    if (wq->lock)
        IOLockFree(wq->lock);
    IOFree(wq, sizeof(*wq));
}

bool queue_work(struct workqueue_struct *wq, struct work_struct *work)
{
    (void)wq; /* see design note above: dispatch is per-work-item */

    if (!work || !work->func)
        return false;

    if (work->pending)
        return true; /* already queued — matches real queue_work() */

    if (!work->call) {
        work->call = thread_call_allocate(rtlwifi_work_thread_call_trampoline,
                                           (thread_call_param_t)work);
        if (!work->call)
            return false;
    }

    work->pending = 1;
    thread_call_enter(work->call);
    return true;
}

bool queue_delayed_work(struct workqueue_struct *wq,
                        struct delayed_work *dwork, unsigned long delay)
{
    uint64_t deadline;

    (void)wq;

    if (!dwork || !dwork->work.func)
        return false;

    if (dwork->work.pending)
        return true; /* already queued */

    if (!dwork->timer.call) {
        dwork->timer.call = thread_call_allocate(
            rtlwifi_delayed_work_thread_call_trampoline,
            (thread_call_param_t)dwork);
        if (!dwork->timer.call)
            return false;
    }

    clock_interval_to_deadline((uint32_t)delay, kMillisecondScale, &deadline);

    dwork->work.pending  = 1;
    dwork->timer.active  = 1;

    return thread_call_enter_delayed(dwork->timer.call, deadline) ? true : false;
}

void flush_workqueue(struct workqueue_struct *wq)
{
    (void)wq;
    /* No per-queue tracking of outstanding thread_calls exists (see
     * design note above) — a full implementation would need every
     * work item ever queued on this wq recorded and
     * thread_call_cancel_wait()'d here. Not needed by any call site
     * in DRIVER_SRCS/PCI_SRCS (confirmed by grep — nothing calls
     * flush_workqueue()); documented no-op rather than silently wrong
     * behavior under a caller that doesn't exist yet. */
}

bool cancel_work_sync(struct work_struct *work)
{
    bool was_pending;

    if (!work || !work->call) {
        if (work)
            work->pending = 0;
        return false;
    }

    was_pending   = thread_call_cancel_wait(work->call) ? true : false;
    work->pending = 0;
    return was_pending;
}

bool cancel_delayed_work(struct delayed_work *dwork)
{
    bool was_pending;

    if (!dwork || !dwork->timer.call)
        return false;

    was_pending          = thread_call_cancel(dwork->timer.call) ? true : false;
    dwork->work.pending  = 0;
    dwork->timer.active  = 0;
    return was_pending;
}

bool cancel_delayed_work_sync(struct delayed_work *dwork)
{
    bool was_pending;

    if (!dwork || !dwork->timer.call)
        return false;

    was_pending          = thread_call_cancel_wait(dwork->timer.call) ? true : false;
    dwork->work.pending  = 0;
    dwork->timer.active  = 0;
    return was_pending;
}

void flush_work(struct work_struct *work)
{
    if (work && work->call)
        thread_call_cancel_wait(work->call);
    /* Real flush_work() waits for in-flight execution without
     * cancelling a not-yet-run item; thread_call_cancel_wait() both
     * cancels *and* waits, which is stronger than real semantics if
     * the item hasn't started yet (it'll be prevented from running
     * rather than run-then-waited-on). No call site in
     * DRIVER_SRCS/PCI_SRCS uses flush_work() (confirmed by grep), so
     * this over-strong behavior is currently unreachable — flagged
     * here rather than left silently wrong for whenever it is used. */
}

bool schedule_work(struct work_struct *work)
{
    return queue_work(system_wq, work);
}

bool schedule_delayed_work(struct delayed_work *dwork, unsigned long delay)
{
    return queue_delayed_work(system_long_wq, dwork, delay);
}

void flush_scheduled_work(void)
{
    flush_workqueue(system_wq);
}
