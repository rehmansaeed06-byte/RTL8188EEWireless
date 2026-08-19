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
static struct ieee80211_hw *g_rtlwifi_hw;

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

/* Defining declaration with initializer — see comment at top of file. */
static struct ieee80211_hw *g_rtlwifi_hw = NULL;

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
/*   IMPORTANT — rtl_op_config()'s real signature (confirmed, core.c   */
/*   line 569) is:                                                     */
/*       static int rtl_op_config(struct ieee80211_hw *hw,             */
/*                                 int radio_idx, u32 changed)          */
/*   THREE parameters, not the two-parameter (hw, changed) shape a     */
/*   generic mac80211 .config op has in most kernel versions. The      */
/*   compat mac80211.h struct definition's `config` member signature   */
/*   has NOT yet been independently checked against this — if it only  */
/*   declares two parameters, this is a real signature mismatch that   */
/*   needs resolving (either the compat header's ieee80211_ops.config  */
/*   member needs a radio_idx parameter added, or rtl_op_config's      */
/*   extra parameter means it doesn't actually satisfy that member     */
/*   slot the way assumed here). Flagged, not silently reconciled by   */
/*   guessing a radio_idx value. radio_idx is passed as 0 below as the */
/*   single-radio assumption already used elsewhere in this project    */
/*   (no multi-radio/MLO handling anywhere in this port) — that        */
/*   default itself is reasonable, but does not resolve the open       */
/*   signature-arity question above.                                   */
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
     * a channel parameter itself).
     *
     * TODO (see file-header comment above): radio_idx hardcoded to 0
     * pending confirmation of the compat ieee80211_ops.config member's
     * real parameter count.
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
