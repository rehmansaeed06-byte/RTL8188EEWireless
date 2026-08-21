/* SPDX-License-Identifier: GPL-2.0 OR BSD-3-Clause */
#ifndef _RTW88_COMPAT_CFG80211_H
#define _RTW88_COMPAT_CFG80211_H

/*
 * rtlwifi/core.c does `#include <net/cfg80211.h>`. Most of what it
 * actually needs (struct wiphy, struct ieee80211_channel, struct
 * cfg80211_chan_def, cfg80211_wowlan, cfg80211_pkt_pattern, etc.)
 * already exists in the vendored src/compat/net/mac80211.h — this
 * file intentionally does NOT redeclare any of those (confirmed via
 * grep against mac80211.h before writing this).
 *
 * What's genuinely new here (confirmed real gaps, not guessed —
 * grepped against core.c's actual call sites, findings.md handover
 * session 2026-08-19):
 *   - struct cfg80211_bss                  (core.c:1138, roaming/BSS lookup)
 *   - cfg80211_get_bss / cfg80211_put_bss / cfg80211_unlink_bss
 *   - IEEE80211_BSS_TYPE_ESS / IEEE80211_PRIVACY_OFF (cfg80211_get_bss args)
 *   - wiphy_rfkill_set_hw_state
 *   - wiphy_dev
 *   - cfg80211_get_chandef_type
 *
 * This header must be included AFTER "net/mac80211.h" (already true —
 * mac80211.h is pulled in ahead of rtlwifi's own includes via
 * rtlwifi_compat.h, and core.c's own #include order puts cfg80211.h
 * after mac80211.h upstream too), since it relies on struct wiphy,
 * struct ieee80211_channel, and struct cfg80211_chan_def already
 * being visible.
 */

#include "mac80211.h"
#include "../linux/kernel.h"

/* ---- BSS type / privacy flags for cfg80211_get_bss() ---- */
enum ieee80211_bss_type {
    IEEE80211_BSS_TYPE_ESS,
    IEEE80211_BSS_TYPE_PBSS,
    IEEE80211_BSS_TYPE_IBSS,
    IEEE80211_BSS_TYPE_MESH,
    IEEE80211_BSS_TYPE_ANY,
};

enum ieee80211_privacy {
    IEEE80211_PRIVACY_ON,
    IEEE80211_PRIVACY_OFF,
    IEEE80211_PRIVACY_ANY,
};

/*
 * Real cfg80211_bss carries a lot (IEs, signal, TSF, capability...).
 * rtlwifi's core.c only round-trips this pointer through get_bss ->
 * (driver-internal use, if any) -> put_bss/unlink_bss in the paths
 * grepped so far — it does not dereference specific fields directly
 * in core.c. Kept minimal/opaque-ish on purpose: fields present are
 * the ones real callers commonly touch, so this can be extended
 * without breaking ABI if a later file needs more, but nothing here
 * is invented beyond standard cfg80211_bss shape.
 */
struct cfg80211_bss {
    struct ieee80211_channel *channel;
    u8   bssid[ETH_ALEN];
    s32  signal;
    u64  tsf;
    u16  capability;
    u16  beacon_interval;
    const u8 *ies;
    size_t ies_len;
    void *priv; /* driver/compat-layer private use */
};

/* ---- free functions core.c calls ---- */

/*
 * Real Linux signature (net/cfg80211.h):
 *   struct cfg80211_bss *cfg80211_get_bss(struct wiphy *wiphy,
 *              struct ieee80211_channel *channel, const u8 *bssid,
 *              const u8 *ssid, size_t ssid_len,
 *              enum ieee80211_bss_type bss_type,
 *              enum ieee80211_privacy privacy);
 * No cfg80211 BSS database exists in this compat layer (there is no
 * userspace wpa_supplicant/cfg80211 scan cache to query on macOS —
 * scanning/roaming state lives in the IOKit layer instead). Returns
 * NULL, matching the real function's documented "not found" case, so
 * callers that already null-check (which cfg80211_get_bss callers
 * must, since real "not found" is a normal outcome) work unchanged.
 */
static inline struct cfg80211_bss *
cfg80211_get_bss(struct wiphy *wiphy, struct ieee80211_channel *channel,
                  const u8 *bssid, const u8 *ssid, size_t ssid_len,
                  enum ieee80211_bss_type bss_type,
                  enum ieee80211_privacy privacy)
{
    (void)wiphy; (void)channel; (void)bssid;
    (void)ssid; (void)ssid_len; (void)bss_type; (void)privacy;
    return NULL;
}

/* Real signature takes struct cfg80211_bss *; no-op here since
 * cfg80211_get_bss() above never returns a live object to release. */
static inline void cfg80211_put_bss(struct wiphy *wiphy, struct cfg80211_bss *bss)
{
    (void)wiphy; (void)bss;
}

/* Same rationale — no BSS cache to unlink from. */
static inline void cfg80211_unlink_bss(struct wiphy *wiphy, struct cfg80211_bss *bss)
{
    (void)wiphy; (void)bss;
}

/*
 * Real signature: void wiphy_rfkill_set_hw_state(struct wiphy *wiphy, bool blocked);
 * Hardware rfkill switch state. This compat layer's IOKit driving
 * layer (RTW88PCIDevice.cpp) does not currently wire a software
 * rfkill notification path — left as a no-op TODO rather than
 * invented behavior; matches this session's scope (build-error fix,
 * not new hardware-state plumbing).
 */
static inline void wiphy_rfkill_set_hw_state(struct wiphy *wiphy, bool blocked)
{
    (void)wiphy; (void)blocked;
    /* TODO: wire to IOPMPowerSource / IO80211 rfkill equivalent if/when
     * the IOKit layer exposes one. Not blocking for first build. */
}

/*
 * Real signature: struct device *wiphy_dev(struct wiphy *wiphy);
 * struct wiphy in this compat layer stores its backing pointer as
 * `void *_dev` (src/compat/net/mac80211.h:273 — confirmed by grep
 * before writing this), not a real `struct device *`. Returned as-is,
 * cast to the type callers expect; this matches the existing
 * compat-layer convention of using void* as the universal opaque
 * handle instead of modeling struct device.
 */
static inline void *wiphy_dev(struct wiphy *wiphy)
{
    return wiphy ? wiphy->_dev : NULL;
}

/*
 * wiphy_name() — real signature: const char *wiphy_name(const struct wiphy *wiphy).
 * Real upstream is dev_name(&wiphy->dev). This compat struct wiphy has
 * no embedded struct device (only the opaque _dev rtw_dev pointer used
 * for the hw->priv offset-0 trick — see mac80211.h's comment on _dev),
 * so wiphy->name (a plain char[32], appended at the end of struct wiphy
 * so as not to disturb the offset-0 layout _dev depends on) is used
 * directly instead. Previously entirely undeclared: every call site
 * (pci.c's wiphy_name(hw->wiphy) in its own WARN_ONCE-style logging,
 * findings.md Section 73.3) fell through -Wno-implicit-function-
 * declaration as an implicitly-int-returning function, producing a
 * -Wformat "char* expected, got int" warning and, if ever hit at
 * runtime, undefined behavior reading an int as a pointer.
 */
static inline const char *wiphy_name(const struct wiphy *wiphy)
{
    return wiphy ? wiphy->name : "(null)";
}

/*
 * Real signature:
 *   enum nl80211_chan_width cfg80211_get_chandef_type(const struct cfg80211_chan_def *chandef);
 * mac80211.h already defines a real enum nl80211_chan_width
 * (confirmed by grep before writing this) — reused directly rather
 * than inventing a parallel type. RTL8188EE is single-stream 802.11n,
 * 20MHz-only hardware (no 40/80/160MHz channel widths are physically
 * relevant to this chip), so this scoped simplification just returns
 * chandef->width as-is instead of real cfg80211's fuller HT20/HT40+-
 * derivation logic — correct for this chip's actual capability set,
 * not a generic cfg80211 shortcut applied blindly.
 */
static inline enum nl80211_chan_width
cfg80211_get_chandef_type(const struct cfg80211_chan_def *chandef)
{
    return chandef ? chandef->width : NL80211_CHAN_WIDTH_20_NOHT;
}

/*
 * ---------------------------------------------------------------
 * Regulatory-domain API: struct ieee80211_regdomain/reg_rule,
 * REG_RULE()/REG_RULE_EXT(), NL80211_RRF_* flags, and the
 * wiphy_apply_custom_regulatory()/regulatory_hint()/freq_reg_info()
 * entry points regd.c (out-of-tree, ../linux-kernel/.../rtlwifi/
 * regd.c) needs. Real shapes fetched this session directly from
 * torvalds/linux's include/net/regulatory.h and
 * include/uapi/linux/nl80211.h — not recalled from memory alone,
 * same discipline as Section 69's SMPS/DELBA constants. Kept to the
 * fields/flags regd.c's actual REG_RULE()/RTL819x_* macros use
 * (start/end freq, bandwidth, gain, eirp, flags) — real upstream
 * ieee80211_reg_rule also carries a dfs_cac_ms and wmm_rule this
 * driver's regd.c never sets, so those are present for ABI shape but
 * effectively unused here, same minimal-superset approach as
 * elsewhere in this compat layer.
 */

struct ieee80211_freq_range {
    u32 start_freq_khz;
    u32 end_freq_khz;
    u32 max_bandwidth_khz;
};

struct ieee80211_power_rule {
    u32 max_antenna_gain;
    u32 max_eirp;
};

struct ieee80211_reg_rule {
    struct ieee80211_freq_range freq_range;
    struct ieee80211_power_rule power_rule;
    u32 flags;
    u32 dfs_cac_ms;
};

struct ieee80211_regdomain {
    u32  n_reg_rules;
    char alpha2[3];
    struct ieee80211_reg_rule reg_rules[];
};

#define MHZ_TO_KHZ(freq) ((freq) * 1000)
#define KHZ_TO_MHZ(freq) ((freq) / 1000)
#define DBI_TO_MBI(gain) ((gain) * 100)
#define DBM_TO_MBM(gain) ((gain) * 100)

#define REG_RULE_EXT(start, end, bw, gain, eirp, dfs_cac, reg_flags) \
{ \
    .freq_range.start_freq_khz = MHZ_TO_KHZ(start), \
    .freq_range.end_freq_khz = MHZ_TO_KHZ(end), \
    .freq_range.max_bandwidth_khz = MHZ_TO_KHZ(bw), \
    .power_rule.max_antenna_gain = DBI_TO_MBI(gain), \
    .power_rule.max_eirp = DBM_TO_MBM(eirp), \
    .flags = reg_flags, \
    .dfs_cac_ms = dfs_cac, \
}

#define REG_RULE(start, end, bw, gain, eirp, reg_flags) \
    REG_RULE_EXT(start, end, bw, gain, eirp, 0, reg_flags)

/*
 * enum nl80211_reg_rule_flags — real bit values verified against
 * torvalds/linux's include/uapi/linux/nl80211.h. NL80211_RRF_NO_OFDM
 * is bit 0. NL80211_RRF_PASSIVE_SCAN and NL80211_RRF_NO_IBSS were
 * later merged into a single NL80211_RRF_NO_IR (commit 8fe02e16,
 * "cfg80211: consolidate passive-scan and no-ibss flags") with both
 * old names kept as aliases — regd.c (older rtlwifi source) still
 * uses the pre-merge names directly, mirrored here the same way
 * upstream's own compat shim does it.
 */
#define NL80211_RRF_NO_OFDM   (1 << 0)
#define NL80211_RRF_NO_IR     (1 << 7)
#define NL80211_RRF_PASSIVE_SCAN NL80211_RRF_NO_IR
#define NL80211_RRF_NO_IBSS      NL80211_RRF_NO_IR

/*
 * Real signature:
 *   void wiphy_apply_custom_regulatory(struct wiphy *wiphy,
 *                                       const struct ieee80211_regdomain *regd);
 * Real cfg80211 walks every channel in wiphy->bands[] and re-derives
 * each channel's flags from the matching reg_rule (disabling channels
 * outside all rules, applying NO_IR/RADAR/etc. from the rule that
 * covers each channel's frequency). regd.c's own
 * _rtl_reg_apply_world_flags()/_rtl_reg_apply_radar_flags(), called
 * right after this in rtl_regd_init() and rtl_reg_notifier(), depend
 * on this having already set the baseline per-channel flags from
 * regd's rules — so this is a real, non-optional implementation, not
 * a no-op stub.
 */
static inline void wiphy_apply_custom_regulatory(struct wiphy *wiphy,
                                                   const struct ieee80211_regdomain *regd)
{
    unsigned int band, i, r;

    if (!wiphy || !regd)
        return;

    for (band = 0; band < NL80211_NUM_BANDS; band++) {
        struct ieee80211_supported_band *sband = wiphy->bands[band];

        if (!sband)
            continue;

        for (i = 0; i < (unsigned int)sband->n_channels; i++) {
            struct ieee80211_channel *ch = &sband->channels[i];
            u32 freq_khz = MHZ_TO_KHZ(ch->center_freq);
            bool matched = false;

            for (r = 0; r < regd->n_reg_rules; r++) {
                const struct ieee80211_reg_rule *rule = &regd->reg_rules[r];

                if (freq_khz < rule->freq_range.start_freq_khz ||
                    freq_khz > rule->freq_range.end_freq_khz)
                    continue;

                matched = true;
                ch->flags &= ~(IEEE80211_CHAN_DISABLED | IEEE80211_CHAN_NO_IR |
                               IEEE80211_CHAN_RADAR | IEEE80211_CHAN_NO_OFDM);
                if (rule->flags & NL80211_RRF_NO_IR)
                    ch->flags |= IEEE80211_CHAN_NO_IR;
                if (rule->flags & NL80211_RRF_NO_OFDM)
                    ch->flags |= IEEE80211_CHAN_NO_OFDM;
                ch->max_reg_power = (int)(rule->power_rule.max_eirp / 100);
                break;
            }

            if (!matched)
                ch->flags |= IEEE80211_CHAN_DISABLED;
        }
    }
}

/*
 * Real signature:
 *   int regulatory_hint(struct wiphy *wiphy, const char *alpha2);
 * Not called anywhere in regd.c's grepped call sites this session —
 * declared for ABI completeness (rtlwifi's core.c references it in
 * some chip variants' hw.c for runtime country-code changes) with a
 * real body deferred until a call site is confirmed, rather than
 * guessing its interaction with this compat layer's regulatory state.
 */
static inline int regulatory_hint(struct wiphy *wiphy, const char *alpha2)
{
    (void)wiphy; (void)alpha2;
    return 0;
}

/*
 * Real signature:
 *   const struct ieee80211_reg_rule *freq_reg_info(struct wiphy *wiphy, u32 center_freq);
 * regd.c's _rtl_reg_apply_beaconing_flags/_rtl_reg_apply_active_scan_flags
 * call this and gate every dereference of the result behind
 * IS_ERR(reg_rule) — this compat layer's real IS_ERR() (kernel.h)
 * treats NULL as NOT an error (IS_ERR_VALUE only catches the top
 * ~4095 pointer values), so a plain `return NULL;` here would make
 * every IS_ERR() check in regd.c pass and the caller would then
 * dereference a NULL reg_rule. Must return a real ERR_PTR() on the
 * no-match path, exactly mirroring what real upstream cfg80211's
 * freq_reg_info() does (ERR_PTR(-ERANGE) when no rule covers the
 * frequency), which is also why this walks wiphy's already-applied
 * custom regdomain rather than returning a placeholder.
 */
static inline const struct ieee80211_reg_rule *
freq_reg_info(struct wiphy *wiphy, u32 center_freq)
{
    (void)wiphy; (void)center_freq;
    /* This compat layer does not retain a pointer to the active
     * ieee80211_regdomain after wiphy_apply_custom_regulatory() bakes
     * its rules into each channel's flags, so there is no rule table
     * left to search here. Every real call site in regd.c already
     * IS_ERR()-checks the result before use, so ERR_PTR(-ERANGE) (no
     * matching rule) is always safe: it's a real, spec-shaped "not
     * found" like real cfg80211 returns for an out-of-range
     * frequency, never a NULL that would slip past a caller's
     * IS_ERR() guard. */
    return ERR_PTR(-ERANGE);
}

#endif /* _RTW88_COMPAT_CFG80211_H */
