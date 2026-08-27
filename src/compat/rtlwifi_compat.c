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

/* mach_types.h -> kmod_info_t/kern_return_t, needed for
 * rtw88_module_start/_stop's real signatures (must match
 * src/kext/kmod_info.c's extern declarations exactly). KERN_SUCCESS
 * itself comes from iokit_shim.h (pulled in transitively) — a
 * project-local #define, not this header's Mach KERN_SUCCESS family —
 * confirmed no collision (both resolve to 0). */
#include <mach/mach_types.h>

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
/*
 * findings.md Section 87: rtl8192_tx_ring, BE_QUEUE, rtl_pcidev(),
 * and rtl_pcipriv() (used by _rtlwifi_be_ring() below) all live in
 * real rtlwifi's pci.h, not wifi.h — CONFIRMED wifi.h does not
 * include pci.h itself (grep against real wifi.h for "include.*pci.h"
 * returns zero hits).
 *
 * NOT a plain #include "pci.h" — CONFIRMED by a real failed compile
 * this session: quote-form #include searches the including file's
 * own directory (src/compat/) first, then falls through the -I list
 * in order; src/compat/linux (this port's OWN unrelated compat-shim
 * pci.h, zero rtlwifi symbols in it) is listed before either real
 * -I$(LINUX_SRC) path on the actual build command, so a bare
 * #include "pci.h" silently picked up the wrong file and produced
 * "incomplete type"/"undeclared identifier" errors that looked like a
 * missing include but were actually a *shadowed* one.
 *
 * Fixing this from inside the .c file with a clever relative path
 * isn't reliable — quote-form resolution for a compound relative path
 * like "rtl8188ee/../pci.h" isn't something to guess at either,
 * same risk as the original bug. The correct fix is a build-system
 * one: RTLWIFI_COMPAT_CFLAGS (Makefile.rtl8188ee) needs
 * -I$(LINUX_SRC) placed BEFORE -Isrc/compat/linux specifically for
 * this file's compile, so real rtlwifi's pci.h wins the shadow
 * instead of the compat shim's. Not fixed here — flagging the seam
 * per this project's own convention (see e.g. rtl8188ee_firmware.c's
 * own header comment for the same pattern of flagging an open
 * build-system decision rather than guessing a source-level
 * workaround). See findings.md Section 87 / handover for the exact
 * Makefile change needed and why.
 */
#include "pci.h"

/*
 * REG_RCR, RCR_CBSSID_DATA, RCR_CBSSID_BCN (used by
 * rtlwifi_log_rcr_state() below) live in rtl8188ee/reg.h, the
 * chip-specific register/bit-definition header — not in the generic
 * wifi.h already included above. Confirmed no shadow risk the way
 * pci.h/usb.h have above: grepped this port's own src/compat/ tree
 * for a same-named reg.h and found none, so a plain quote-include
 * resolves straight to the real rtl8188ee/reg.h via the CHIP_SRC
 * -I path (already ordered ahead of COMPAT_FLAGS per the pci.h fix
 * documented above this file's own Makefile.rtl8188ee).
 */
#include "reg.h"

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
 * g_rtlwifi_vif / g_rtlwifi_sta -- single-station bridge for
 * ieee80211_find_sta(). Mirrors g_rtlwifi_hw's own split-declaration/
 * global-pointer pattern immediately above, rather than inventing a
 * second bridging mechanism. Set from RTW88IEEE80211.cpp via
 * rtlwifi_set_vif_sta()/rtlwifi_clear_sta() (declared in
 * rtlwifi_compat.h). See ieee80211_find_sta()'s own comment below for
 * why a scalar pointer (not a list + real RCU) is the correct shape
 * here, not a simplification.
 */
static struct ieee80211_vif *g_rtlwifi_vif = NULL;
static struct ieee80211_sta *g_rtlwifi_sta = NULL;


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
    /*
     * findings.md Section 98 item 5 / Section 99.6 item 2: rx_byte_count
     * (surfaced via rtlwifi_get_stats() -> rtlpriv->stats.rxbytesunicast)
     * stayed 0 even during confirmed-active RX-drain events. Root cause:
     * real rtlwifi normally increments rxbytesunicast deep inside its own
     * base.c/core.c RX-completion accounting, which this port's direct
     * rtlwifi_do_interrupt() -> ieee80211_rx_irqsafe() dispatch (Section 52
     * -- deliberately bypasses rtlwifi's own RX entry point, same
     * bypass-mac80211 pattern as the TX side) never reaches. This is the
     * single choke point every real RX frame this port delivers passes
     * through (both ieee80211_rx_irqsafe() callers and ieee80211_rx_napi(),
     * which just forwards here), so it's the correct place to add the
     * counter rather than the interrupt handler itself. skb->len is still
     * valid at this point, before the callback consumes/frees it.
     */
    if (hw && hw->priv && skb) {
        struct rtl_priv *rtlpriv = rtl_priv(hw);
        rtlpriv->stats.rxbytesunicast += skb->len;
    }

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

    /* BUGFIX (real-hardware connect investigation, post-108): real
     * rtl_op_config() (core.c:752) sets rtlphy->current_channel from
     * hw->conf.chandef BEFORE calling switch_channel() -- the chip-
     * specific switch_channel() (e.g. rtl88e_phy_sw_chnl()) reads
     * rtlphy->current_channel directly, NOT hw->conf.chandef.chan.
     * This function set hw->conf.chandef.chan (via the caller's
     * setConnectedChandef()) but never set rtlphy->current_channel,
     * so switch_channel() was silently switching to whatever stale
     * channel was already there instead of the target BSS's channel.
     * Confirmed on real hardware: [authrxdiag] showed zero frames ever
     * arriving from the target BSSID during the auth window, while
     * beacons from other, unrelated APs kept arriving normally --
     * i.e. we never actually left the old channel. */
    if (hw->conf.chandef.chan && rtlpriv->cfg->ops->switch_channel) {
        struct rtl_phy *rtlphy = &rtlpriv->phy;
        rtlphy->current_channel = hw->conf.chandef.chan->hw_value;
        rtlpriv->cfg->ops->switch_channel(hw);
    }

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

/* ------------------------------------------------------------------ */
/* RTW88PCIDevice.cpp's own unported-symbol gap (findings.md Section  */
/* 87, Section 77's original catalogue corrected: 6 of the original   */
/* 17 names were stale comment text with no compiled call site, and   */
/* 3 more already had real bridging infrastructure sitting unused —   */
/* see rtlwifi_compat.h for the full per-name evidence.                */
/* ------------------------------------------------------------------ */

/*
 * Real definitions for globals real rtlwifi/dma-mapping.h and pci.h
 * already declare `extern` and already read from internally
 * (rtw88_pci_io_ops used throughout linux/pci.h's inline wrappers,
 * rtw88_dma_ops throughout linux/dma-mapping.h's) — RTW88PCIDevice.cpp
 * already assigns to both by these exact names (start()/teardown()),
 * so no renaming is needed, only a single real (non-extern) definition
 * linked into the kext, same pattern as g_rtlwifi_hw.
 */
struct pci_ops_rtw88 *rtw88_pci_io_ops = NULL;
struct rtw88_dma_alloc_ops *rtw88_dma_ops = NULL;

/* CONFIRMED no-ops — see rtlwifi_compat.h for full rationale (no real
 * rtlwifi module-level init/exit exists to mirror; the workqueues
 * this port's own call-site comment references are static storage,
 * always valid, no creation step to call). */
void rtlwifi_compat_init(void)
{
}

void rtlwifi_compat_exit(void)
{
}

/* CONFIRMED no-op — see rtlwifi_compat.h. rtl88e_get_btc_status()
 * (real RTL8188EE chip source, compiled as-is) already unconditionally
 * returns false, so real rtlwifi's own init path already always takes
 * the wifi-only branch for this chip. Nothing to force. */
void rtlwifi_force_wifi_only(void)
{
}

/*
 * Shared BE-ring lookup for rtlwifi_be_tx_avail()/
 * _debug_dump_tx_state(). Returns NULL if hw isn't registered yet
 * (mirrors this file's existing null-safety pattern elsewhere, e.g.
 * rtlwifi_get_hw()'s own callers).
 */
static struct rtl8192_tx_ring *_rtlwifi_be_ring(void)
{
    struct ieee80211_hw *hw = rtlwifi_get_hw();

    /*
     * hw->priv IS the rtl_priv allocation (rtl_priv(hw) is just
     * `(struct rtl_priv *)(hw)->priv`) — this is the one real guard
     * needed. There is no separate rtlpriv->priv to check afterward:
     * real rtl_priv's own `priv[]` field (wifi.h:2751) is a C99
     * flexible array member at the struct's tail, part of the same
     * allocation, not a second pointer — checking it for null is
     * checking "is this array's own address non-null," which is
     * always true once rtl_priv itself exists (confirmed: clang
     * flagged an earlier version of this check as dead code,
     * -Wpointer-bool-conversion, correctly).
     */
    if (!hw || !hw->priv)
        return NULL;

    struct rtl_pci *rtlpci = rtl_pcidev(rtl_pcipriv(hw));

    return &rtlpci->tx_ring[BE_QUEUE];
}

/*
 * CONFIRMED real mechanism: rtl8188ee has no get_available_desc
 * implementation (zero grep hits), so this mirrors the real fallback
 * rtlwifi itself uses internally, _rtl_pci_tx_chk_waitq()
 * (pci.c:417-419): entries minus current queue length, for the BE
 * ring specifically (BE_QUEUE == 1).
 */
unsigned int rtlwifi_be_tx_avail(void)
{
    struct rtl8192_tx_ring *ring = _rtlwifi_be_ring();

    if (!ring)
        return 0;

    return ring->entries - skb_queue_len(&ring->queue);
}

void rtlwifi_debug_dump_tx_state(void)
{
    struct rtl8192_tx_ring *ring = _rtlwifi_be_ring();

    if (!ring) {
        IOLog("rtlwifi: debug_dump_tx_state: hw not ready\n");
        return;
    }

    IOLog("rtlwifi: BE ring: entries=%u queued=%u avail=%u wp=%u rp=%u\n",
          ring->entries, skb_queue_len(&ring->queue),
          ring->entries - skb_queue_len(&ring->queue),
          ring->cur_tx_wp, ring->cur_tx_rp);
}

/*
 * CONFIRMED real hook point: real rtlwifi's own TX-complete path
 * (_rtl_pci_tx_isr, pci.c:450) signals "ring slots freed" via
 * ieee80211_wake_queue(hw, queue) (pci.c:540) — a real mac80211 API
 * this port's mac80211.h already declares (line 1429) but, unlike its
 * three siblings just below, never defines. Storing the callback here
 * and firing it from ieee80211_wake_queue() (defined right after this
 * block) makes that the actual, correct wiring rather than a bespoke
 * parallel mechanism.
 */
static void (*g_tx_resume_cb)(void) = NULL;

void rtlwifi_set_tx_resume_cb(void (*cb)(void))
{
    g_tx_resume_cb = cb;
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

/*
 * ieee80211_wake_queue() — UNLIKE its three siblings above, this one
 * is not a deliberate no-op: it's the real per-queue "TX has room
 * again" signal real rtlwifi's own _rtl_pci_tx_isr() calls after
 * freeing ring slots (pci.c:540, confirmed live read). RTW88PCIDevice
 * .cpp's own flow-control mechanism (IOGatedOutputQueue +
 * kIOReturnOutputStall, resumeTxIfStalled()) is this port's IOKit-side
 * reimplementation of exactly the same concept — so this real
 * mac80211 API is the correct, real hook to fire it from, not a
 * contradiction of the no-op siblings' bypass rationale (that
 * rationale is about mac80211's own *outbound* tx-scheduling queue,
 * a different concern from this *inbound* hardware-ready signal).
 */
void ieee80211_wake_queue(struct ieee80211_hw *hw, int queue)
{
    (void)hw;
    (void)queue;
    if (g_tx_resume_cb)
        g_tx_resume_cb();
}

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
 * rtlwifi_add_tx_bytes() -- see rtlwifi_compat.h for the full
 * findings.md Section 100 rationale (rx_byte_count's identical bug,
 * root cause, and why tx_byte_count needs a bridge function instead
 * of a single shared choke point the way the RX side has one).
 * Called once from RTW88IEEE80211::txDataFrame(), right after
 * _hw->ops->tx() returns, with the frame's data-payload length.
 */
void rtlwifi_add_tx_bytes(u32 len)
{
    if (!g_rtlwifi_hw || !g_rtlwifi_hw->priv)
        return;

    struct rtl_priv *rtlpriv = (struct rtl_priv *)g_rtlwifi_hw->priv;
    rtlpriv->stats.txbytesunicast += len;
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
    if (!t)
        return;

    if (t->done_lock) { IOLockLock(t->done_lock); t->running = 1; IOLockUnlock(t->done_lock); }

    if (t->function)
        t->function(t);

    if (t->done_lock) {
        IOLockLock(t->done_lock);
        t->running = 0;
        IOLockWakeup(t->done_lock, (void *)&t->running, false);
        IOLockUnlock(t->done_lock);
    }
}

void timer_setup(struct timer_list *timer,
                 void (*func)(struct timer_list *t),
                 unsigned int flags)
{
    (void)flags;
    timer->function = func;
    timer->data     = 0;
    timer->expires  = 0;
    timer->active    = 0;
    timer->running   = 0;
    timer->done_lock = IOLockAlloc();
    timer->call      = thread_call_allocate(rtlwifi_timer_thread_call_trampoline,
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
        if (!timer->done_lock)
            timer->done_lock = IOLockAlloc();
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

    /* thread_call_cancel_wait() is com.apple.kpi.private -- rejected at
     * kmutil load time for third-party kexts ("Linking com.apple.kpi.private
     * requires an Apple kext", confirmed via a real load attempt). Reproduce
     * its cancel-then-wait-for-in-flight contract using only public KPI:
     * thread_call_cancel() first; if that catches it before it started, done;
     * otherwise block on done_lock/running, set by the trampoline above. */
    was_pending = thread_call_cancel(timer->call) ? 1 : 0;

    if (!was_pending && timer->done_lock) {
        IOLockLock(timer->done_lock);
        while (timer->running)
            IOLockSleep(timer->done_lock, (void *)&timer->running, 0);
        IOLockUnlock(timer->done_lock);
    }

    timer->active = 0;
    return was_pending;
}

/* timer_delete_sync() -- Linux 6.x renamed del_timer_sync() to
 * this; rtlwifi's base.c uses the new name directly
 * (base.c:182,184,476). Same cancel-and-wait contract, just
 * aliased to the existing implementation rather than
 * duplicated. */
int timer_delete_sync(struct timer_list *timer)
{
    return del_timer_sync(timer);
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
    if (!w)
        return;

    w->pending = 0;
    if (w->done_lock) { IOLockLock(w->done_lock); w->running = 1; IOLockUnlock(w->done_lock); }

    if (w->func)
        w->func(w);

    if (w->done_lock) {
        IOLockLock(w->done_lock);
        w->running = 0;
        IOLockWakeup(w->done_lock, (void *)&w->running, false);
        IOLockUnlock(w->done_lock);
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
        if (!work->done_lock)
            work->done_lock = IOLockAlloc();
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
        if (!dwork->timer.done_lock)
            dwork->timer.done_lock = IOLockAlloc();
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

    /* see del_timer_sync() above for why this isn't thread_call_cancel_wait() */
    was_pending = thread_call_cancel(work->call) ? true : false;

    if (!was_pending && work->done_lock) {
        IOLockLock(work->done_lock);
        while (work->running)
            IOLockSleep(work->done_lock, (void *)&work->running, 0);
        IOLockUnlock(work->done_lock);
    }

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

    /* see del_timer_sync() above for why this isn't thread_call_cancel_wait() */
    was_pending = thread_call_cancel(dwork->timer.call) ? true : false;

    if (!was_pending && dwork->timer.done_lock) {
        IOLockLock(dwork->timer.done_lock);
        while (dwork->timer.running)
            IOLockSleep(dwork->timer.done_lock, (void *)&dwork->timer.running, 0);
        IOLockUnlock(dwork->timer.done_lock);
    }

    dwork->work.pending  = 0;
    dwork->timer.active  = 0;
    return was_pending;
}

void flush_work(struct work_struct *work)
{
    /* Real flush_work() waits for in-flight execution WITHOUT cancelling a
     * not-yet-run item -- unlike the old thread_call_cancel_wait()-based
     * version, this wait-only form matches that contract exactly, as a side
     * effect of no longer having a cancel+wait primitive available at all
     * (kpi.private). No call site in DRIVER_SRCS/PCI_SRCS uses flush_work()
     * (confirmed by grep), so still currently unreachable in practice. */
    if (work && work->call && work->done_lock) {
        IOLockLock(work->done_lock);
        while (work->running)
            IOLockSleep(work->done_lock, (void *)&work->running, 0);
        IOLockUnlock(work->done_lock);
    }
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

/*
 * ---------------------------------------------------------------------
 * Logging: rtw88_printk / rtw88_log_level / rtw88_read_log
 * ---------------------------------------------------------------------
 * Confirmed missing at kext-load time, not just a naming leftover: grep
 * across src/ found declarations only (src/compat/linux/kernel.h,
 * src/kext/RTW88UserClient.cpp) and zero definitions anywhere in this
 * tree. pr_err/pr_warn/pr_info/pr_debug/printk/WARN/WARN_ON/BUG all
 * expand to rtw88_printk() (kernel.h) and are called throughout the
 * real, compiled-in rtlwifi source (base.c/core.c/pci.c/ps.c/etc. all
 * use pr_*), so this needs a real implementation, not a stub — a
 * no-op body would silently swallow every diagnostic message the
 * driver produces, which is exactly the wrong failure mode for a
 * still-unverified-on-real-hardware port.
 *
 * Kept the rtw88_-prefixed names from kernel.h/RTW88UserClient.cpp
 * as-is here (rather than renaming to rtlwifi_) since both call sites
 * are in files reused verbatim from Feixiao and neither name collides
 * with anything real rtlwifi source defines — renaming would only
 * matter if this project ever also linked rtw88_compat.c, which it
 * does not (findings.md Section 57: separate Makefile, separate
 * build/ tree, never combined into one link).
 *
 * Design: small fixed-size ring buffer, IOSimpleLock-protected
 * (interrupt-safe — pr_err/WARN/BUG are reachable from the ISR and
 * tasklets, not just process context, so this cannot use IOLock,
 * which can block). Every message is also mirrored to IOLog()
 * unconditionally, so console/log output works even before
 * RTW88UserClient's sGetLog() is ever called — the ring buffer exists
 * for the userclient debug-drain path specifically, not as the only
 * way to see driver output.
 */

int rtw88_log_level = KERN_INFO;

#define RTW88_LOG_RING_SIZE 16384

static char            s_rtw88_log_ring[RTW88_LOG_RING_SIZE];
static uint32_t        s_rtw88_log_head;   /* next byte to write */
static uint32_t        s_rtw88_log_count;  /* valid bytes, <= ring size */
static IOSimpleLock    *s_rtw88_log_lock;  /* lazily allocated, see below */

static IOSimpleLock *rtw88_log_lock_get(void)
{
    /* Lazily allocated because this file has no single init call site
     * guaranteed to run before the first pr_*()/WARN() — chip probe,
     * module start, and even early error paths can all call in first.
     * IOSimpleLockAlloc() itself is not interrupt-safe to race with
     * itself, but the realistic first caller is kext start() on a
     * single thread before any IRQ is wired up (Section 46/49: IRQ
     * registration happens late in probe, after hw registration) —
     * flagged rather than silently assumed watertight under true
     * concurrent first-use. */
    if (!s_rtw88_log_lock)
        s_rtw88_log_lock = IOSimpleLockAlloc();
    return s_rtw88_log_lock;
}

void rtw88_printk(int level, const char *fmt, ...)
{
    char        line[256];
    va_list     args;
    int         len;
    IOSimpleLock *lock;

    if (level > rtw88_log_level)
        return;

    va_start(args, fmt);
    len = vsnprintf(line, sizeof(line), fmt, args);
    va_end(args);
    if (len < 0)
        return;
    if ((size_t)len >= sizeof(line))
        len = sizeof(line) - 1; /* vsnprintf truncated; keep what fit */

    /* Always mirror to the real kernel log regardless of ring-buffer
     * state — this is the primary output path or a serial/verbose
     * boot; the ring buffer below is secondary (RTW88UserClient's
     * on-demand drain). */
    IOLog("%s", line);

    lock = rtw88_log_lock_get();
    if (!lock) {
        /* Allocation failed (or raced) — IOLog above already ran, so
         * the message isn't lost, just not available via the
         * userclient ring-buffer drain for this one call. */
        return;
    }

    IOSimpleLockLock(lock);
    for (int i = 0; i < len; i++) {
        s_rtw88_log_ring[s_rtw88_log_head] = line[i];
        s_rtw88_log_head = (s_rtw88_log_head + 1) % RTW88_LOG_RING_SIZE;
        if (s_rtw88_log_count < RTW88_LOG_RING_SIZE)
            s_rtw88_log_count++;
    }
    IOSimpleLockUnlock(lock);
}

uint32_t rtw88_read_log(char *out_buf, uint32_t max_len)
{
    IOSimpleLock *lock;
    uint32_t     to_copy;
    uint32_t     start;

    if (!out_buf || max_len == 0)
        return 0;

    lock = rtw88_log_lock_get();
    if (!lock)
        return 0;

    IOSimpleLockLock(lock);

    to_copy = s_rtw88_log_count;
    if (to_copy > max_len)
        to_copy = max_len;

    start = (s_rtw88_log_head + RTW88_LOG_RING_SIZE - s_rtw88_log_count)
            % RTW88_LOG_RING_SIZE;

    for (uint32_t i = 0; i < to_copy; i++) {
        out_buf[i] = s_rtw88_log_ring[(start + i) % RTW88_LOG_RING_SIZE];
    }

    s_rtw88_log_count -= to_copy;

    IOSimpleLockUnlock(lock);

    return to_copy;
}

/*
 * ---------------------------------------------------------------------
 * rtw88_trigger_interrupt — debug-only manual IRQ trigger
 * ---------------------------------------------------------------------
 * Called from RTW88IEEE80211.cpp/RTW88PCIDevice.cpp (both reused
 * verbatim from Feixiao) as a debug/diagnostic hook. Confirmed no
 * equivalent exists anywhere in this tree under any name. Left as an
 * explicit no-op rather than guessing at a real MMIO trigger sequence
 * — a wrong guess here risks writing to an undefined register on real
 * hardware, which is worse than "the debug trigger does nothing yet."
 */
void rtw88_trigger_interrupt(void)
{
    /* no-op: no confirmed software-IRQ-trigger register for RTL8188EE.
     * Left silent -- this fires on every real hardware interrupt, so a
     * per-call IOLog here reproduces the same flood this function used
     * to cause. See docs/findings.md Section 88.3. */
}

/*
 * ---------------------------------------------------------------------
 * rtw88_module_start / rtw88_module_stop — kext entry points
 * ---------------------------------------------------------------------
 * Referenced by src/kext/kmod_info.c (KMOD_EXPLICIT_DECL, _realmain,
 * _antimain) — these ARE the kext's start()/stop() entry points the
 * kernel calls on load/unload, not optional. Confirmed no definition
 * existed anywhere in this tree. Bodies are deliberately minimal: real
 * per-device bring-up happens later via IOKit's own probe/start() on
 * RTW88PCIDevice, not here.
 */
kern_return_t rtw88_module_start(kmod_info_t *ki, void *data)
{
    (void)ki;
    (void)data;
    IOLog("rtw88: kext module loaded (com.rtlwifi.rtl8188ee)\n");
    return KERN_SUCCESS;
}

kern_return_t rtw88_module_stop(kmod_info_t *ki, void *data)
{
    (void)ki;
    (void)data;
    IOLog("rtw88: kext module unloading (com.rtlwifi.rtl8188ee)\n");
    return KERN_SUCCESS;
}


/*
 * rtlwifi_set_vif_sta() / rtlwifi_clear_sta() -- see g_rtlwifi_vif's
 * declaration comment above for the full rationale. Called from
 * RTW88IEEE80211.cpp: once (with sta==NULL) right after _vif is
 * allocated, and again once _sta is fully populated inside
 * doAssociate()'s sta_add block. rtlwifi_clear_sta() is called from
 * releaseSta() on teardown/disconnect.
 */
void rtlwifi_set_vif_sta(struct ieee80211_vif *vif, struct ieee80211_sta *sta)
{
    g_rtlwifi_vif = vif;
    g_rtlwifi_sta = sta;
}

void rtlwifi_clear_sta(void)
{
    g_rtlwifi_sta = NULL;
}

/*
 * ieee80211_find_sta() -- real implementation, not a stub. Every
 * rcu_read_lock()/rcu_read_unlock() call site in the compiled driver
 * (base.c, core.c, stats.c, rtl8188ee/dm.c -- confirmed by direct grep
 * against only the files actually in DRIVER_SRCS/CHIP_SRCS, not the
 * other-chip files that also live in the vendored rtlwifi tree) exists
 * solely to bracket a call into this function, via wifi.h's inline
 * rtl_find_sta()/get_sta() wrappers or directly (core.c).
 *
 * No real RCU implementation exists anywhere in this port
 * (src/compat/linux/rcupdate.h's rcu_read_lock/unlock are deliberate
 * no-ops). Since this driver only ever tracks one associated station
 * (RTW88IEEE80211.hpp's _sta is scalar, not a list -- confirmed by
 * direct header read, no AP-mode support), a real list-walk-under-RCU
 * emulation would be solving a problem that doesn't exist here. A
 * direct address compare against the one tracked station, with no
 * locking beyond the pointer read itself, is the correct, honest
 * substitute -- not a guessed-at RCU replacement standing in for
 * unverified behavior.
 */
struct ieee80211_sta *ieee80211_find_sta(struct ieee80211_vif *vif,
                                          const u8 *addr)
{
    if (!g_rtlwifi_sta || vif != g_rtlwifi_vif)
        return NULL;
    if (memcmp(g_rtlwifi_sta->addr, addr, ETH_ALEN) != 0)
        return NULL;
    return g_rtlwifi_sta;
}

/*
 * rtlwifi_do_interrupt() -- real ISR body, port of _rtl_pci_interrupt()
 * (pci.c:835) + _rtl_pci_rx_interrupt()'s pdesc-only branch (pci.c:647).
 * See findings.md Section 96.4-96.6 for the full trace that led here:
 * RTW88PCIDevice::handleInterrupt() previously called only the no-op
 * rtw88_trigger_interrupt(), so no real PCI interrupt ever reached the
 * chip's ISR register at all -- the BSS list was structurally
 * guaranteed to stay empty, and (96.5) naively wiring the real
 * interrupt path through with no ISR-clear logic causes the chip's IRQ
 * line to never de-assert (500k+ interrupts in ~2 minutes, confirmed
 * on hardware). This function is the missing ISR-clear + RX-drain
 * logic; the two calls a future handleInterrupt() must bracket around
 * it are still rtl88ee_enable/disable_interrupt() at the two call
 * sites, exactly as real pci.c does.
 *
 * rtl8188ee always takes the `use_new_trx_flow == false` branch --
 * confirmed this session: that field (wifi.h:2735) is never set
 * anywhere under rtl8188ee/*.c, so it zero-inits false. Only the
 * legacy pdesc-based ring walk is ported here; the newer
 * buffer_desc/rx_desc_buff_remained_cnt path (later 8822B/8821C-class
 * chips) is out of scope for this chip and intentionally not written.
 *
 * Returns true if any RX frame was recognized this call (RTL_IMR_ROK
 * or RTL_IMR_RDU set in intvec.inta) -- purely informational for the
 * caller's own logging; the real work already happened by return.
 */
/*
 * rtlwifi_mark_interface_started() -- see declaration comment in
 * rtlwifi_compat.h for the full trace of why this is needed.
 */
void rtlwifi_mark_interface_started(void)
{
    struct ieee80211_hw *hw = rtlwifi_get_hw();

    if (!hw || !hw->priv)
        return;

    struct rtl_priv *rtlpriv = rtl_priv(hw);
    set_bit(RTL_STATUS_INTERFACE_START, &rtlpriv->status);
}

/*
 * rtlwifi_log_rcr_state() -- TEMPORARY DIAGNOSTIC (2026-08-26).
 *
 * findings.md Section 97.3: the interrupt->recognize->drain->deliver
 * pipeline is proven correct end-to-end (one real frame drained cleanly),
 * mask (IMR_ROK|IMR_RDU) and power-state (rfpwr_state == ERFON) are both
 * confirmed correct, yet RX events are far too rare given a confirmed-
 * dense RF environment. Next suspect per that section: whether this
 * port's compat build of hw_init() actually reaches the same RX-filter
 * (REG_RCR) configuration real rtl88ee_hw_init() does, the same way
 * 97.2 found RTL_STATUS_INTERFACE_START was being silently skipped.
 *
 * Real rtl88ee_hw_init() (hw.c) writes REG_RCR from rtlpci->receive_config
 * THREE times total across init: once in _rtl88ee_init_mac() (hw.c:884,
 * the sw.c:78-90 wide-open default -- AM|AB|ACF|ADF|AAP-family bits, no
 * BSSID filtering), once again right after rtl88e_phy_mac_config() (hw.c:
 * 1096-1097, a corrective &= ~(RCR_ACRC32|RCR_AICV) -- the *authoritative*
 * value, per that function's own comment: phy_mac_config silently touches
 * RCR internally, so this second write exists specifically to fix it back
 * up), and conditionally again via rtl88ee_set_check_bssid() (hw.c:1262,
 * toggles RCR_CBSSID_DATA|RCR_CBSSID_BCN) -- called only from
 * rtl88ee_set_network_type() (hw.c:1290-1298), itself only reachable from
 * real core.c's rtl_op_add_interface()/rtl_op_bss_info_changed(). This
 * port's RTW88IEEE80211::start() does call hw->ops->add_interface() (line
 * ~835) before hw->ops->start() (line ~842), and rtl_pci_probe() (called
 * even earlier, before both) is what runs init_sw_vars() and first sets
 * rtlpci->receive_config's default -- so ordering looks correct on paper.
 * This function exists to confirm that on real hardware rather than by
 * source-reading alone: it reads back the LIVE register value actually
 * sitting in hardware right now via the same MMIO path real
 * rtl_read_dword()/rtl_write_dword() use, and logs it next to the
 * software's own believed value (rtlpci->receive_config). If they match
 * and look like the wide-open sw.c default (no CBSSID bits), RCR is fully
 * cleared as a suspect and the investigation moves to IQK/LC calibration
 * instead. If they diverge, or CBSSID bits are unexpectedly set/unset,
 * that is the smoking gun this section's "single next step" was looking
 * for. Call once, right after hw->ops->start() returns in
 * RTW88IEEE80211::start() -- not in a hot path, no rate-limiting needed.
 * Strip once RCR is confirmed correct or the real bug is found here,
 * per this project's own standing rule about not leaving permanent
 * unconditional log spam (95.4/96.1 precedent).
 */
void rtlwifi_log_rcr_state(void)
{
    struct ieee80211_hw *hw = rtlwifi_get_hw();

    if (!hw || !hw->priv) {
        rtw88_printk(0, "rtw88: rcr-diag: no hw/priv yet, skipping\n");
        return;
    }

    struct rtl_priv *rtlpriv = rtl_priv(hw);
    struct rtl_pci *rtlpci = rtl_pcidev(rtl_pcipriv(hw));

    if (!rtlpriv || !rtlpci) {
        rtw88_printk(0, "rtw88: rcr-diag: rtlpriv/rtlpci not ready, skipping\n");
        return;
    }

    u32 sw_believed = rtlpci->receive_config;
    u32 hw_live = rtl_read_dword(rtlpriv, REG_RCR);

    rtw88_printk(0,
        "rtw88: rcr-diag: sw_receive_config=0x%08x hw_live_REG_RCR=0x%08x "
        "cbssid_data=%d cbssid_bcn=%d match=%d\n",
        sw_believed, hw_live,
        !!(hw_live & RCR_CBSSID_DATA), !!(hw_live & RCR_CBSSID_BCN),
        sw_believed == hw_live);
}

/*
 * rtlwifi_log_iqk_lc_state() -- TEMPORARY DIAGNOSTIC (2026-08-26).
 *
 * findings.md Section 98: RCR is now fully ruled out (rcr-diag showed
 * sw_receive_config == live REG_RCR, match=1, wide-open filter as
 * intended). Per real rtl88ee_hw_init() (hw.c, confirmed against
 * upstream this session -- see 98's citation), the next step in that
 * function after the RCR/BB/RF config block, gated behind
 * ppsc->rfpwr_state == ERFON, is:
 *
 *     if (rtlphy->iqk_initialized)
 *         rtl88e_phy_iq_calibrate(hw, true);
 *     else {
 *         rtl88e_phy_iq_calibrate(hw, false);
 *         rtlphy->iqk_initialized = true;
 *     }
 *     rtl88e_dm_check_txpower_tracking(hw);
 *     rtl88e_phy_lc_calibrate(hw);
 *
 * If IQ/LC calibration silently fails or never runs, the RF front-end
 * can be mistuned (wrong LO frequency, poor image rejection) even
 * though every software-side register write this port makes is
 * correct -- which would explain rare RX despite a confirmed-dense
 * RF environment without RCR, IMR, or the ring-drain logic being at
 * fault.
 *
 * struct rtl_phy (real wifi.h, confirmed against upstream mirrors
 * this session) exposes iqk_initialized (bool, set true once the
 * *first* full calibration completes -- does not by itself mean that
 * calibration succeeded, only that it ran), lck_inprogress (bool, LC
 * calibration's own busy/in-progress flag -- should read false by
 * the time hw_init() returns, since LC calibrate is called
 * synchronously with no async completion path in real phy.c), and
 * the eight power-tracking registers reg_e94/e9c/ea4/eac/eb4/ebc/
 * ec4/ecc, which mirror exactly the vendor driver's own IQK debug
 * output style (e.g. "RegE94=103 RegE9C=f ... IQK: final_candidate is
 * 0" from real-hardware vendor logs) -- non-zero/non-default values
 * here are the closest signal available to a pass/fail result without
 * re-deriving real phy.c's internal IQK math, which is not vendored
 * in this port and out of scope for a diagnostic.
 *
 * This function reads all of the above back from the live rtlphy
 * struct -- not a register read via MMIO like rcr-diag, since IQK/LC
 * state lives in software bookkeeping in rtl_phy, not a single
 * hardware register -- and logs it once, same call site and same
 * one-shot-after-start() discipline as rtlwifi_log_rcr_state().
 * Catching this log line requires the same verbose-boot capture
 * workflow documented in 98 (dmesg/log show both proved unreliable
 * for this kext's one-shot early-boot IOLog output).
 *
 * Strip once IQK/LC is confirmed correct or the real bug is found
 * here, per this project's own standing rule about not leaving
 * permanent unconditional log spam (95.4/96.1 precedent).
 */
void rtlwifi_log_iqk_lc_state(void)
{
    struct ieee80211_hw *hw = rtlwifi_get_hw();

    if (!hw || !hw->priv) {
        rtw88_printk(0, "rtw88: iqklc-diag: no hw/priv yet, skipping\n");
        return;
    }

    struct rtl_priv *rtlpriv = rtl_priv(hw);

    if (!rtlpriv) {
        rtw88_printk(0, "rtw88: iqklc-diag: rtlpriv not ready, skipping\n");
        return;
    }

    struct rtl_phy *rtlphy = &rtlpriv->phy;

    rtw88_printk(0,
        "rtw88: iqklc-diag: iqk_initialized=%d lck_inprogress=%d "
        "regE94=%d regE9C=%d regEA4=%d regEAC=%d "
        "regEB4=%d regEBC=%d regEC4=%d regECC=%d\n",
        !!rtlphy->iqk_initialized, !!rtlphy->lck_inprogress,
        rtlphy->reg_e94, rtlphy->reg_e9c, rtlphy->reg_ea4, rtlphy->reg_eac,
        rtlphy->reg_eb4, rtlphy->reg_ebc, rtlphy->reg_ec4, rtlphy->reg_ecc);
}

/*
 * rtlwifi_pci_tx_isr() -- port of real rtlwifi's _rtl_pci_tx_isr()
 * (pci.c:450, static/non-exported -- confirmed via live source read,
 * so it cannot be called directly and must be reimplemented here).
 *
 * findings.md "net not working" investigation (post-110): [txdatadiag]/
 * [rxdatadiag] captures showed a real, working connection (correct
 * CCMP keying, real replies with real -- if initially poor -- RTT)
 * that worked for an initial burst right after connect, then went
 * completely silent on the TX side (zero [txdatadiag] lines) while RX
 * of unrelated broadcast traffic continued normally. Traced to:
 * rtlwifi_do_interrupt() (this file, below) only ever checks
 * RTL_IMR_ROK/RTL_IMR_RDU (RX bits) -- confirmed via its own docstring
 * and body, neither of which mention or call any TX-related interrupt
 * bit or _rtl_pci_tx_isr(). rtlwifi_be_tx_avail() (used by both
 * RTW88PCIDevice::outputPacket()'s stall check and
 * resumeTxIfStalled()'s un-stall check) reads
 * `ring->entries - skb_queue_len(&ring->queue)` -- the exact same
 * ring->queue that real rtlwifi's TX submission path (inside
 * hw->ops->tx() -> real rtl_pci_tx(), already correctly linked in per
 * the Twentieth Update's "0 undefined symbols" milestone) pushes onto,
 * but which only _rtl_pci_tx_isr() ever pops from. With that function
 * never called, ring->queue's length only ever grows, so available
 * space monotonically shrinks to the kRTW88TxStallAvail threshold and
 * never recovers -- exactly matching "works for a burst, then stops
 * forever," since outputPacket() stalls the queue once threshold is
 * crossed and nothing ever calls ieee80211_wake_queue() to un-stall
 * it (that call only exists inside the missing function).
 *
 * This port only ever submits on the BE queue (txDataFrame() hardcodes
 * skb_set_queue_mapping(skb, IEEE80211_AC_BE); mgmt frames go through
 * a separate, simpler path -- txMgmtFrame() -- not ported here since
 * mgmt-frame TX was already proven working end-to-end all session via
 * auth/assoc/EAPOL without needing this), so only BE_QUEUE is drained.
 * If mgmt-queue (or other AC) TX-ring exhaustion is ever observed,
 * extend the caller to also call this for MGNT_QUEUE/other prio
 * values -- the logic below is prio-generic, matching real code.
 *
 * Ported faithfully rather than simplified, including the nullfunc/
 * power-save and HT-SMPS-action-frame special cases and the
 * ring->entries-skb_queue_len(&ring->queue) <= 4 hysteresis threshold
 * before waking the queue (both exactly as real pci.c:450-540) --
 * cutting corners here is exactly the class of gap that caused this
 * bug in the first place. rtlpriv->use_new_trx_flow's branch is
 * omitted: confirmed false unconditionally for rtl8188ee (never set
 * under rtl8188ee/*.c, same finding rtlwifi_do_interrupt()'s own
 * docstring already relies on for its own RX-side omission of that
 * flow).
 */
static void rtlwifi_pci_tx_isr(struct ieee80211_hw *hw, int prio)
{
    struct rtl_priv *rtlpriv = rtl_priv(hw);
    struct rtl_pci  *rtlpci  = rtl_pcidev(rtl_pcipriv(hw));
    struct rtl8192_tx_ring *ring = &rtlpci->tx_ring[prio];

    while (skb_queue_len(&ring->queue)) {
        struct sk_buff *skb;
        struct ieee80211_tx_info *info;
        __le16 fc;
        u8 tid;
        u8 *entry = (u8 *)(&ring->desc[ring->idx]);

        if (!rtlpriv->cfg->ops->is_tx_desc_closed(hw, prio, ring->idx))
            return;
        ring->idx = (ring->idx + 1) % ring->entries;

        skb = __skb_dequeue(&ring->queue);
        dma_unmap_single(&rtlpci->pdev->dev,
                          rtlpriv->cfg->ops->get_desc(hw, entry,
                              true, HW_DESC_TXBUFF_ADDR),
                          skb->len, DMA_TO_DEVICE);

        if (prio == TXCMD_QUEUE) {
            dev_kfree_skb(skb);
            continue;
        }

        fc = rtl_get_fc(skb);
        if (ieee80211_is_nullfunc(fc)) {
            if (ieee80211_has_pm(fc)) {
                rtlpriv->mac80211.offchan_delay = true;
                rtlpriv->psc.state_inap = true;
            } else {
                rtlpriv->psc.state_inap = false;
            }
        }
        if (ieee80211_is_action(fc)) {
            struct ieee80211_mgmt *action_frame = (void *)skb->data;
            /* Minimum size to safely read category + the flat
             * action_code byte -- avoiding IEEE80211_MIN_ACTION_SIZE
             * here since it did not resolve as a visible macro from
             * this translation unit at this point (build error:
             * "use of undeclared identifier"), despite mac80211.h
             * defining it and being included via rtlwifi_compat.h.
             * Not investigated further; this hand-computed offset is
             * self-evidently correct against the real struct layout
             * documented just above ieee80211_mgmt's action member
             * (mac80211.h) and doesn't depend on that macro at all. */
            size_t min_action_sz =
                offsetof(struct ieee80211_mgmt, u.action.action_code) +
                sizeof(action_frame->u.action.action_code);
            if (skb->len >= min_action_sz &&
                action_frame->u.action.action_code == WLAN_HT_ACTION_SMPS) {
                dev_kfree_skb(skb);
                continue;
            }
        }

        tid = rtl_get_tid(skb);
        if (tid <= 7)
            rtlpriv->link_info.tidtx_inperiod[tid]++;

        info = IEEE80211_SKB_CB(skb);

        if (likely(!ieee80211_is_nullfunc(fc))) {
            ieee80211_tx_info_clear_status(info);
            info->flags |= IEEE80211_TX_STAT_ACK;
            ieee80211_tx_status_irqsafe(hw, skb);
        } else {
            /* Real code calls rtl_tx_ackqueue(hw, skb) here -- a
             * separate real-rtlwifi ACK-tracking queue this port
             * doesn't use elsewhere (txNullFunc() sends nullfunc
             * frames for power-save signaling but nothing in this
             * port consumes rtl_tx_ackqueue's tracking). Plain free
             * instead: correct for ring-slot accounting (the only
             * thing this function needs to fix), just doesn't feed a
             * consumer this port never built. */
            dev_kfree_skb(skb);
        }

        if ((ring->entries - skb_queue_len(&ring->queue)) <= 4) {
            ieee80211_wake_queue(hw, IEEE80211_AC_BE);
        }
    }
}


bool rtlwifi_do_interrupt(void)
{
    struct ieee80211_hw *hw = rtlwifi_get_hw();

    if (!hw || !hw->priv)
        return false;

    struct rtl_priv *rtlpriv = rtl_priv(hw);
    struct rtl_pci  *rtlpci  = rtl_pcidev(rtl_pcipriv(hw));
    struct rtl_int   intvec  = {0};

    if (!rtlpci->irq_enabled)
        return false;

    /* disable -> read-and-clear ISR -> (RX drain) -> re-enable, exactly
     * the real _rtl_pci_interrupt() bracket (pci.c:835). Real code also
     * takes rtlpriv->locks.irq_th_lock here; this port has no SMP
     * concurrent-ISR concern (IOInterruptEventSource serializes calls
     * on the workloop thread), so the lock is intentionally omitted,
     * not forgotten. */
    rtlpriv->cfg->ops->disable_interrupt(hw);
    rtlpriv->cfg->ops->interrupt_recognized(hw, &intvec);

    /* Section 108 cleanup: removed the top-of-ISR call-count diagnostic
     * (inta/intb/irq_enabled/rfpwr_state) and the RX-flagged-entry/
     * ring-idx/own-bit diagnostics that used to live in this function.
     * Both answered their questions long ago -- interrupt delivery,
     * irq_mask, RCR, IQK/LC, and power-state were all independently
     * confirmed healthy (Sections 97-99) -- and stayed pending a real
     * cleanup pass (tracked since Section 99.6 item 3). The actual bug
     * (Section 108: missing dma_sync_single_for_cpu() on the RX bounce-
     * buffer path) was upstream of anything this logging could show,
     * since it read descriptor/interrupt state, not skb payload
     * content -- removing it now that root cause is fixed, not before. */

    /* Shared IRQ or HW disappeared -- real pci.c's own bail-out check. */
    if (!intvec.inta || intvec.inta == 0xffff) {
        rtlpriv->cfg->ops->enable_interrupt(hw);
        return false;
    }

    bool got_rx = (intvec.inta & rtlpriv->cfg->maps[RTL_IMR_ROK]) ||
                  (intvec.inta & rtlpriv->cfg->maps[RTL_IMR_RDU]);

    /* TX-complete: see rtlwifi_pci_tx_isr()'s own header comment (above
     * this function) for the full trace of why this was missing and
     * what it broke. Only BE_QUEUE is drained -- see that comment for
     * why, and what to extend if other queues are ever found to need
     * it. Runs regardless of got_rx, same as real pci.c's own
     * unconditional TX-related interrupt block (both RX and TX bits
     * are read from the same intvec.inta/intb before either is
     * handled, so there is no ordering dependency between them). */
    if (intvec.inta & rtlpriv->cfg->maps[RTL_IMR_BEDOK]) {
        rtlpriv->link_info.num_tx_inperiod++;
        rtlwifi_pci_tx_isr(hw, BE_QUEUE);
    }

    if (got_rx) {
        int rxring_idx = RTL_PCI_RX_MPDU_QUEUE;
        unsigned int count = rtlpci->rxringcount;

        while (count--) {
            struct rtl_rx_desc *pdesc =
                &rtlpci->rx_ring[rxring_idx].desc[rtlpci->rx_ring[rxring_idx].idx];
            struct sk_buff *skb =
                rtlpci->rx_ring[rxring_idx].rx_buf[rtlpci->rx_ring[rxring_idx].idx];
            struct sk_buff *new_skb;
            struct ieee80211_rx_status rx_status = {0};
            struct rtl_stats stats = { .signal = 0, .rate = 0 };
            u8 own;
            u16 len;

            own = (u8)rtlpriv->cfg->ops->get_desc(hw, (u8 *)pdesc, false, HW_DESC_OWN);
            if (own)
                break; /* no more data filled by hardware -- ring drained */

            /* Section 108: this port's RX DMA path is a bounce-buffer
             * design (RTW88PCIDevice.cpp's compat_dma_map() allocates a
             * separate physically-contiguous buffer for the hardware to
             * write into, tracked in a DMAEntry; compat_dma_sync_cpu()
             * copies bounce -> the original skb->data buffer). That sync
             * step was never called anywhere in this RX loop -- skb->data
             * was being read directly, straight past the driver's own
             * bounce-buffer indirection, so it only ever contained
             * whatever alloc_skb()/kmalloc() initialized it to, never the
             * real received bytes. This fully explains the fc=0x0000-on-
             * every-frame symptom chased through Sections 103-107: every
             * descriptor-level field (len, own, drvinfo_size, crc,
             * hwerror) is read over MMIO from real DMA'd ring memory
             * (RTW88PCIDevice::allocCoherent(), confirmed correct), so
             * those always looked sane, while the actual payload bytes
             * were never copied out of the bounce buffer at all.
             *
             * Real pci.c calls dma_sync_single_for_cpu(DMA_FROM_DEVICE)
             * before touching skb->data, then dma_unmap_single() once
             * it's done with the mapping -- matching real code's own
             * "AAAAAAttention" comment that order matters at this exact
             * point. Fix: added the missing sync call, immediately
             * before the existing dma_unmap_single(), so the bounce
             * buffer's contents are copied into skb->data (via
             * RTW88PCIDevice::syncBounceForCpu()) before compat_dma_unmap()
             * frees the DMAEntry that memcpy needs to find the bounce
             * buffer by physical address. Everything from query_rx_desc()
             * onward now reads real copied-back data instead of the
             * skb's original, never-written-to allocation. */
            dma_sync_single_for_cpu(&rtlpci->pdev->dev, *((dma_addr_t *)skb->cb),
                                     rtlpci->rxbuffersize, DMA_FROM_DEVICE);
            dma_unmap_single(&rtlpci->pdev->dev, *((dma_addr_t *)skb->cb),
                              rtlpci->rxbuffersize, DMA_FROM_DEVICE);

            new_skb = dev_alloc_skb(rtlpci->rxbuffersize);
            if (!new_skb) {
                /* Real code re-arms with the old skb (best-effort, drops
                 * this one packet) rather than stalling the ring. */
                goto rearm;
            }

            rtlpriv->cfg->ops->query_rx_desc(hw, &stats, &rx_status,
                                              (u8 *)pdesc, skb);

            len = (u16)rtlpriv->cfg->ops->get_desc(hw, (u8 *)pdesc, false,
                                                    HW_DESC_RXPKT_LEN);

            /* Section 108 cleanup: removed the [rxdesc] diagnostic
             * (len/drvinfo_size/bufshift/crc/hwerror). It answered its
             * question this session: those descriptor-level fields all
             * read sane (drvinfo_size=32, crc=0, hwerror=0), which
             * correctly narrowed the bug away from descriptor parsing
             * and toward the RX payload delivery path itself -- see the
             * dma_sync_single_for_cpu() fix above. */

            if (skb->end - skb->tail > len) {
                skb_put(skb, len);
                skb_reserve(skb, stats.rx_drvinfo_size + stats.rx_bufshift);
            } else {
                IOLog("rtlwifi: rx desc len %u exceeds skb tailroom, dropping\n",
                      (unsigned int)len);
                dev_kfree_skb_any(skb);
                goto rearm;
            }

            /* Section 107: real pci.c's _rtl_pci_rx_interrupt() (confirmed
             * against the person's own local copy, not just public source)
             * has a check this port's RX loop was missing entirely: C2H
             * (command-to-host) packets share the same RX descriptor ring
             * as real 802.11 frames, but are firmware status/report
             * payloads, not 802.11 frames -- real code detects this via
             * stats.packet_report_type (set by query_rx_desc()) and
             * reroutes them away from normal frame processing instead of
             * treating the payload as an ieee80211_hdr. This is a real,
             * independent correctness fix (kept as a permanent guard, not
             * temporary diagnostics) -- NOTE: it turned out NOT to be the
             * cause of Section 104's fc=0x0000 capture (that was Section
             * 108's missing dma_sync_single_for_cpu(); this filter never
             * matched in that capture, 0 C2H hits logged), but real C2H
             * report packets do exist on this hardware per Section 107.3's
             * enum and must not be misinterpreted as 802.11 frames.
             *
             * This port has no rtl_c2hcmd_enqueue() (real C2H command
             * processing -- e.g. firmware rate reports feeding back into
             * rate control -- was never ported, out of scope for basic
             * data-path bring-up), so these packets are dropped rather
             * than processed, matching real code's net effect on the
             * mac80211-delivery path without the full command pipeline. */
            if (stats.packet_report_type == C2H_PACKET) {
                dev_kfree_skb_any(skb);
                goto rearm;
            }

            /* FCS_LEN (wifi.h:122, already in scope via this file's own
             * #include "wifi.h") -- confirmed present, not the same
             * symbol as compat/linux/if_ether.h's ETH_FCS_LEN. */
            if (!stats.crc && !stats.hwerror && (skb->len > FCS_LEN)) {
                memcpy(IEEE80211_SKB_RXCB(skb), &rx_status, sizeof(rx_status));
                /* Deliver via the real intercept point, confirmed this
                 * session against pci.c:629/631 and base.c:1363: this is
                 * what bridges to g_hw_cbs->rx_frame -> RTW88IEEE80211::
                 * rxFrame(), already fully wired on the delivery side. */
                ieee80211_rx_irqsafe(hw, skb);
            } else {
                dev_kfree_skb_any(skb);
            }

rearm:
            /* Re-arm this descriptor slot with a fresh (or, on alloc
             * failure, no) skb -- port of _rtl_pci_init_one_rxdesc()'s
             * pdesc branch (pci.c:552), inlined here rather than
             * duplicating the static helper, since that helper is not
             * externally linkable (confirmed: static, pci.c-local). */
            {
                struct sk_buff *arm_skb = new_skb ? new_skb : dev_alloc_skb(rtlpci->rxbuffersize);
                u32 bufferaddress;
                u8 tmp_one = 1;

                if (arm_skb) {
                    *((dma_addr_t *)arm_skb->cb) =
                        dma_map_single(&rtlpci->pdev->dev, skb_tail_pointer(arm_skb),
                                       rtlpci->rxbuffersize, DMA_FROM_DEVICE);
                    bufferaddress = *((dma_addr_t *)arm_skb->cb);
                    rtlpci->rx_ring[rxring_idx].rx_buf[rtlpci->rx_ring[rxring_idx].idx] = arm_skb;
                    rtlpriv->cfg->ops->set_desc(hw, (u8 *)pdesc, false,
                                                HW_DESC_RXBUFF_ADDR, (u8 *)&bufferaddress);
                    rtlpriv->cfg->ops->set_desc(hw, (u8 *)pdesc, false,
                                                HW_DESC_RXPKT_LEN, (u8 *)&rtlpci->rxbuffersize);
                    rtlpriv->cfg->ops->set_desc(hw, (u8 *)pdesc, false,
                                                HW_DESC_RXOWN, (u8 *)&tmp_one);
                }
                /* else: leave OWN bit as-is (still hardware-owned from the
                 * failed own-check above would not reach here; this is
                 * only the alloc-failure path, where we intentionally
                 * drop one packet's worth of ring capacity rather than
                 * risk a bad re-arm). */
            }

            if (rtlpci->rx_ring[rxring_idx].idx == (unsigned int)(rtlpci->rxringcount - 1))
                rtlpriv->cfg->ops->set_desc(hw, (u8 *)pdesc, false, HW_DESC_RXERO,
                                            (u8 *)&(u8){1});

            rtlpci->rx_ring[rxring_idx].idx =
                (rtlpci->rx_ring[rxring_idx].idx + 1) % rtlpci->rxringcount;
        }
    }

    rtlpriv->cfg->ops->enable_interrupt(hw);
    return got_rx;
}

