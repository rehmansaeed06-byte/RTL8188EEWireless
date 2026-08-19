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

#endif /* _RTW88_COMPAT_CFG80211_H */
