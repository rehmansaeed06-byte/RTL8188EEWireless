# RTL8188EE macOS Port --- Findings

## Purpose

This file is the running technical record for the RTL8188EE native macOS
Wi-Fi port. It records confirmed source findings, architecture
conclusions, current hypotheses, and the next investigation steps.

The target is a **native RTL8188EE PCIe Wi-Fi kext for macOS Monterey**
using the existing Feixiao macOS/IOKit scaffold where practical.

This version extends the prior findings.md with confirmed source
evidence gathered in the current session (sections 40+). Sections 1--39
are carried forward unchanged from the prior handoff and are not
repeated in full detail here except where superseded.

------------------------------------------------------------------------

# 1--30. Prior Findings (carried forward, summarized)

- Hardware target: Realtek RTL8188EE, PCI ID `10EC:8179`, PCIe, macOS
  Monterey.
- RTL8188EE belongs to the **rtlwifi** Linux driver family, not rtw88.
- Feixiao is the macOS/IOKit scaffold, currently built against **rtw88**.
- Feixiao's existing PCI table (`rtw88_pci_chip_table[]` in
  `RTW88IEEE80211.cpp`) contains only rtw88-family device IDs; `0x8179`
  is absent.
- rtlwifi has a shared `rtl_pci_probe()` (pci.c), architecturally
  analogous to rtw88's `rtw_pci_probe()`.
- `rtl8188ee/sw.c` uses `.probe = rtl_pci_probe`, `rtl88ee_hal_cfg`
  (a `struct rtl_hal_cfg`), which points to `rtl8188ee_hal_ops`
  (a `struct rtl_hal_ops`, ~40+ function pointers).
- rtlwifi's chip-specific hardware work is concentrated in
  `rtl8188ee/{hw,phy,trx,fw,dm,rf,pwrseq,table,reg.h}`.
- rtlwifi shares common mac80211 ops (`.start`, `.add_interface`,
  `.set_key`, etc. in `core.c`), matching the pattern Feixiao's wrapper
  already expects.

------------------------------------------------------------------------

# 31--39. Prior Session Findings (carried forward, summarized)

- `struct rtl_priv` full field layout confirmed from `wifi.h`
  (lines ~2657-2752). `hw->priv` is directly `struct rtl_priv *`
  (macro `rtl_priv(hw)`).
- Private-state model is two-layer: `rtl_priv` then tail-allocated
  `rtl_pci_priv`, both sized together in one `ieee80211_alloc_hw` call
  (`rtl_pci_probe()`, pci.c lines ~2040-2280).
- Full Linux/rtlwifi API surface used by `rtl_pci_probe()` enumerated
  and tiered by porting difficulty (raw PCI/DMA/IRQ kernel APIs /
  rtlwifi-internal shared helpers / mac80211-cfg80211 surface).
- `_rtl_pci_find_adapter` and `_rtl_pci_io_handler_init` are `static`,
  pci.c-local — must be read from source directly, not assumed.
  Still unread.
- Full rtw88-dependency audit of `RTW88IEEE80211.cpp` completed:
  Category A (mechanical replace, ~14 lines), Category B (9 bridge
  functions in `rtw88_compat.c` carrying real rtw88-specific logic),
  Category C (generic mac80211/macOS glue, reusable as-is).
- RTL8188EE needs **no PCI-ID lookup table** — single chip, single
  `rtl_hal_cfg`. Net simplification vs. current Feixiao shape.
- Of the 9 Category B bridge functions: `rtw88_register_vif`,
  `rtw88_unregister_vif`, `rtw88_get_hw` are trivial one-liners.
  `rtw88_connect_hw_setup` and `rtw88_restore_connected_hw` have now
  been fully read (see Section 40). `rtw88_set_hw_callbacks` and the
  three `sw_scan_*` functions remain unread.

------------------------------------------------------------------------

# 40. CONFIRMED: `rtw88_connect_hw_setup` / `rtw88_restore_connected_hw`
     Full Bodies, and Full Verification of the LPS-Hazard Question

## 40.1 What the two functions do

Source: `Feixiao/src/compat/rtw88_compat.c`, lines 1139-1223.

Both functions are near-identical (`restore_connected_hw` is
`connect_hw_setup` minus logging). Both bypass mac80211's
`ops->config` / `ops->bss_info_changed` entirely and, under
`rtwdev->mutex`, perform four steps:

1. **Channel switch** — `rtw_set_channel(rtwdev)`, called directly.
2. **BSSID write** — `memcpy` into `rtwvif->bssid` (a per-vif field),
   then `rtw_vif_port_config(rtwdev, rtwvif, PORT_SET_BSSID)`.
3. **RF calibration** — `rtwdev->need_rfk = true;` then
   `rtw_chip_prepare_tx(rtwdev)` to force IQK/DPK/GAPK. Comment in
   source explains this exists because the port bypasses mac80211, so
   `mgd_prepare_tx()` (stock trigger for this calibration) never
   fires; omitting it left TX uncalibrated on the 8822C, causing
   throughput collapse after a handful of frames.
4. **Rate adaptation** — `rtwdev->dm_info.fix_rate = 0xFF;` to let
   firmware RA run freely across the full MCS set (replaces an old
   5GHz 6M-OFDM pin that capped throughput, no longer needed once
   HT/VHT caps are properly advertised).

## 40.2 Why the bypass exists (rtw88-specific reason, source comment)

`rtw_ops_config()` calls `rtw_leave_lps_deep()` →
`__rtw_fw_leave_lps_check_reg()` — **an MMIO-read polling loop**
(`rtw_read32_mask` on `REG_TCR`). If the chip is unresponsive
immediately post-HW-scan (firmware still in an internal scan-exit
critical section), those reads stall the CPU core indefinitely →
PCIe timeout → system freeze. `rtw_ops_bss_info_changed()` has the
same issue. This is why Feixiao routes connect/reconnect around both
ops callbacks entirely for rtw88.

## 40.3 rtlwifi verification — full call chain traced, source-read end to end

Traced the equivalent rtlwifi path start to finish across four files
to determine whether the same hazard exists:

**`rtl_op_bss_info_changed`** (core.c:1008-1150) — on the connect
branch (`BSS_CHANGED_ASSOC`, `vif->cfg.assoc` true), does NOT call
`rtl_lps_leave()` at all. `rtl_lps_leave()` only appears in the
**disassociate** branch. rtw88's hazard is specifically about LPS-exit
stalling during the post-scan *connect* sequence — if rtlwifi's own
connect path never calls the LPS-leave function in the first place,
the hazard's precondition doesn't exist here. BSSID write on connect
is a plain `memcpy(mac->bssid, bss_conf->bssid, ETH_ALEN)` plus
`rtlpriv->cfg->ops->linked_set_reg(hw)` — no per-vif struct, no
port-config indirection.

**`rtl_lps_set_psmode`** (ps.c:325-388) — when leaving LPS
(`EACTIVE`), performs one dispatch:
`rtlpriv->cfg->ops->set_hw_reg(hw, HW_VAR_FW_LPS_ACTION, ...)` — a
single register-write dispatch through the chip ops table. No
register-read polling loop. Structurally different from rtw88's
`__rtw_fw_leave_lps_check_reg()`.

**`rtl_lps_leave_core`** (ps.c:431-456) — locks `lps_mutex`,
optionally disables ASPM, calls `rtl_lps_set_psmode(hw, EACTIVE)`.
No polling.

**`rtl_op_config`** (core.c:569-763), `IEEE80211_CONF_CHANGE_CHANNEL`
branch — channel-switch path uses one bounded, conditional
`mdelay(50)` (fires only when `offchan_delay` is set during scan),
then three chip-ops dispatches: `switch_channel(hw)`,
`set_channel_access(hw)`, `set_bw_mode(hw, channel_type)`. No
indefinite polling.

**Chip-ops resolution for RTL8188EE** (`rtl8188ee/sw.c:193-235`,
`rtl8188ee_hal_ops` table):
- `linked_set_reg` is **not implemented for this chip** (absent from
  the ops table) — `core.c` guards every call with
  `if (rtlpriv->cfg->ops->linked_set_reg)`, so these calls are simply
  skipped for RTL8188EE specifically.
- `.switch_channel = rtl88e_phy_sw_chnl` (phy.c) — the last remaining
  candidate for a blocking hazard.

**`rtl88e_phy_sw_chnl` / `rtl88e_phy_sw_chnl_callback`**
(rtl8188ee/phy.c:1153-1230) — a scripted step-by-step channel-switch
command sequence (`_rtl88e_phy_sw_chnl_step_by_step`, driving
precommon/rfdepend/postcommon command arrays such as
`CMDID_SET_TXPOWEROWER_LEVEL`), advanced via a `do {} while(true)`
loop gated on a boolean return value with a **fixed `mdelay(delay)`**
between steps. Not a register-read polling loop waiting on hardware
state — a bounded, scripted delay sequence.

## 40.4 Conclusion (source-verified, not inferred)

The entire rtlwifi connect-path call chain —
`rtl_op_config` → `rtl_op_bss_info_changed` →
`rtl_lps_leave`/`rtl_lps_set_psmode` →
`switch_channel`/`set_bw_mode` → `rtl88e_phy_sw_chnl` →
`sw_chnl_callback` — was read end to end and contains **no
equivalent of rtw88's MMIO-read polling hazard**. rtw88's problem is a
specific pattern (poll an MMIO register with no bound, waiting for
firmware to leave a critical section); rtlwifi's equivalents use fixed
delays and single dispatched register writes instead.

**Practical conclusion:** `rtw88_connect_hw_setup` and
`rtw88_restore_connected_hw` most likely do **not** need to be ported
as custom bypass functions for RTL8188EE. The stock `ops->config`
(`IEEE80211_CONF_CHANGE_CHANNEL`) + `ops->bss_info_changed`
(`BSS_CHANGED_ASSOC`) path appears safe to call directly, removing the
need to reauthor this bridge function's channel/BSSID logic. This is a
real simplification versus the original Category B assumption that
all six unread bridge functions would need rtlwifi-equivalent
rewrites.

**Caveats (not blockers, but not yet closed):**

1. This conclusion is based on static source reading across
   core.c/ps.c/phy.c, not a runtime trace on real hardware. Treat as a
   strong, source-verified hypothesis — confirm once a kext boots
   against real firmware.
2. RF calibration (step 3 in Section 40.1 — rtw88's
   `need_rfk`/`rtw_chip_prepare_tx`) has no confirmed equivalent
   trigger yet in what's been read. rtlwifi's IQK for this chip is
   likely handled elsewhere (candidates: `dm.c` watchdog —
   `rtl88e_dm_watchdog` is in the ops table — or inside
   `rtl88ee_hw_init`). Needs a dedicated check that channel changes
   actually re-trigger calibration on RTL8188EE, so TX doesn't end up
   uncalibrated the way rtw88's did pre-fix. This is now a standalone
   open question (see Section 41).

------------------------------------------------------------------------

# 40.5 CONFIRMED: RF Calibration (IQK) Trigger Points for RTL8188EE
     (Resolves Section 41.5 open question)

## 40.5.1 Method

Full call chain traced source-to-source via `grep`/`sed` across
`dm.c`, `hw.c`, and `sw.c`:

`need_rfk`/`iqk`/`phy_iq_calibrate`/`dpk` grepped in
`rtl8188ee/{dm.c,hw.c}` → traced both hit clusters to their
enclosing functions → traced those functions' callers → traced
those callers' registration in the `rtl_hal_ops` table (`sw.c`).

## 40.5.2 Confirmed IQK trigger points

Exactly two call sites trigger IQK-related work for RTL8188EE, and
neither is the channel-switch path:

1. **`rtl88ee_hw_init()`** (`hw.c:1031`, registered as `.hw_init =
   rtl88ee_hw_init` in `sw.c`) — called once at hardware bring-up
   (interface start), dispatched generically through rtlwifi core,
   not mac80211's per-channel `ops->config`. Inside it:
   - `hw.c:1135-1139` — unconditional one-time IQK, gated by
     `rtlphy->iqk_initialized`: full `rtl88e_phy_iq_calibrate(hw,
     false)` if not yet initialized, else a lighter
     `iq_calibrate(hw, true)` path.
   - `hw.c:1441-1442` — `iqk_initialized` is reset to `false` after
     power-off, with an explicit comment: "after power off we should
     do iqk again" — confirming IQK re-runs after a power cycle, not
     on ordinary channel switches while powered on.
   - `hw.c:1142` also calls `rtl88e_dm_check_txpower_tracking(hw)`
     directly (see below).

2. **`rtl88e_dm_watchdog()`** (`dm.c:1756`, registered as
   `.dm_watchdog = rtl88e_dm_watchdog` in `sw.c:220`) — dispatched
   periodically by rtlwifi core's DM timer (same generic ops-table
   mechanism as `.hw_init`, confirmed by identical registration
   pattern). Calls `rtl88e_dm_check_txpower_tracking(hw)`
   (`dm.c:1778`), which conditionally calls
   `dm_txpower_track_cb_therm(hw)` (`dm.c:1112`), which performs IQK
   only if thermal drift exceeds a threshold:
   `delta_iqk >= 8` (`dm.c:1067-1069`).

`rtl88e_dm_check_txpower_tracking` has exactly these two callers
(`hw.c:1142`, `dm.c:1778`) confirmed via `grep -rn` across the whole
`rtl8188ee/` directory — no third call site exists.

## 40.5.3 Conclusion

IQK/RF calibration on RTL8188EE is driven by:
- **Init/power-on-time bring-up** (`rtl88ee_hw_init`), and
- **Periodic thermal-drift polling** (`rtl88e_dm_watchdog`, timer-driven).

It is **never triggered by channel switch itself** — no call to any
IQK-related function exists in `rtl88e_phy_sw_chnl`,
`rtl88e_phy_sw_chnl_callback`, or `_rtl88e_phy_sw_chnl_step_by_step`
(consistent with Section 40.3's channel-switch trace, which read
those functions fully and found no calibration calls).

This is structurally different from rtw88's model, where
`rtw88_connect_hw_setup`/`rtw88_restore_connected_hw` had to force
`need_rfk = true` + `rtw_chip_prepare_tx()` synchronously after
reconnect specifically because mac80211's `mgd_prepare_tx()` (the
stock trigger) never fires when mac80211's connect path is bypassed.
RTL8188EE's calibration is not gated on `mgd_prepare_tx()` or any
per-connect/per-channel hook at all — it is (a) unconditional at
`hw_init` and (b) periodic via watchdog regardless of connection
state. Both of those continue to fire normally whether or not the
stock `ops->config`/`ops->bss_info_changed` path is used, since
neither depends on mac80211 callbacks that Section 40 found were
being bypassed for rtw88.

**Practical result:** the calibration side-effect of
`rtw88_connect_hw_setup`/`rtw88_restore_connected_hw` has no needed
rtlwifi equivalent. Combined with Section 40.4, both functions
appear to have no rtlwifi-side reason to exist as custom bypasses —
the stock mac80211 path is expected to keep working, including
calibration, without a rewritten equivalent.

**Caveat (unchanged in kind from Section 40.4):** this is a
source-verified static conclusion, not a runtime one. The one
remaining risk this doesn't cover: whether the periodic watchdog's
thermal-drift threshold (`delta_iqk >= 8`) is responsive enough in
practice after a fresh channel change to avoid a transient
uncalibrated-TX window analogous to what rtw88 hit — this can only
be assessed by measurement on real hardware, not by source reading.
Recommend treating this as a boot-test checklist item (watch TX
throughput/quality in the first few seconds after a channel change
or reconnect) rather than a blocker to further source work.

# 40.6 CONFIRMED: All Remaining Category B Bridge Functions Read
     (resolves the last open items from Section 41.2/41.9's bridge-
     function list)

## 40.6.1 `rtw88_set_hw_callbacks` (rtw88_compat.c, ~line 491)

Read in full. Trivial: stores two globals (`g_hw_cbs`, `g_kext_hw`)
and back-fills `hw->kext_hw` so IOKit-side callbacks can find the
wrapper object. No register access, no rtw88 hardware/firmware
logic, no LPS/scan-state interaction. Pure macOS-side callback
plumbing between the mac80211 shim and the IOKit kext object —
reusable as-is, same category as the other generic glue functions
around it (`ieee80211_rx_irqsafe`, `ieee80211_tx_status_irqsafe`,
`ieee80211_scan_completed`, the `iterate_active_interfaces` stubs,
etc., all also in this range and all similarly generic/non-chip-
specific).

## 40.6.2 `rtw88_sw_scan_start` / `rtw88_sw_scan_switch_channel` /
     `rtw88_sw_scan_complete` (rtw88_compat.c, lines ~1063-1096)

**`rtw88_sw_scan_start`** — mutex-locks, calls
`rtw_core_scan_start(rtwdev, rtwvif, vif->addr, false)`, clears
`BIT_CBSSID_BCN` in RCR, writes it. `rtw_core_scan_start`'s body
(rtw88-stable/main.c:1471) was read and its **first action is
`rtw_leave_lps(rtwdev)`** — i.e. it inherits the exact same
LPS-exit hazard pattern documented in Section 40.2
(`rtw_leave_lps` → `rtw_leave_lps_deep()` →
`__rtw_fw_leave_lps_check_reg()`, an unbounded MMIO-read poll).

**`rtw88_sw_scan_switch_channel`** — just calls `rtw_set_channel`,
same call already covered by Section 40.1 step 1.

**`rtw88_sw_scan_complete`** — restores `BIT_CBSSID_BCN`, writes
RCR, calls `rtw_core_scan_complete`. Body read
(rtw88-stable/main.c:1500) — no LPS call at all; DIG reset, flag
clear, coex notify, conditional idle-work queue only.

## 40.6.3 rtlwifi-side verification for the scan-start LPS concern

Traced whether rtlwifi's scan-start path has the same hazard:

- `rtl_op_sw_scan_start` (core.c:1410-1440, registered as
  `.sw_scan_start` at core.c:1905) — on the `MAC80211_LINKED`
  branch, calls `rtl_lps_leave(hw, true)`.
- `rtl_lps_leave(hw, bool may_block)` (ps.c:667) — when
  `may_block == true` (the value passed here), routes directly to
  `rtl_lps_leave_core(hw)` — **the same function already fully
  traced and confirmed safe in Section 40.3**: locks `lps_mutex`,
  optionally disables ASPM, calls `rtl_lps_set_psmode(hw, EACTIVE)`
  → single dispatched `HW_VAR_FW_LPS_ACTION` register write. No
  polling loop. (When `may_block == false`, it instead defers via
  `schedule_work`, which is not the branch taken here.)

## 40.6.4 Conclusion

None of the three `sw_scan_*` bridge functions appear to need a
custom rtlwifi rewrite:
- The rtw88-side hazard that motivated `sw_scan_start`'s existence
  has a rtlwifi-side equivalent that was already source-verified
  safe (same underlying dispatched-write path as the connect/
  reconnect functions).
- `sw_scan_switch_channel` duplicates an already-cleared call
  (Section 40.3/40.1).
- `sw_scan_complete` has nothing to compare against — no hazard
  pattern in the source at all.

Stock rtlwifi ops (`rtl_op_sw_scan_start` and rtlwifi's own
scan-complete equivalent, name not yet confirmed but presumed to
exist alongside it in core.c near line ~1905's ops table) should be
usable directly, matching the pattern established in Section 40 for
the connect/reconnect bridge functions.

**Net result:** all nine originally-identified Category B bridge
functions (Section 31-39 audit) have now been read in full across
this and the prior session. None appear to require a rtlwifi-
specific rewrite; all six previously-unread ones collapse to "use
stock rtlwifi ops" the same way the three trivial ones already did.
This is a significant simplification of the bridge-layer scope
versus the original assumption. Same caveat as throughout: static-
analysis result, pending runtime boot/connect/scan test.

# 40.7 CONFIRMED: Full rtlwifi TX Path, Source-Traced End to End
     (resolves Section 41 item 3)

## 40.7.1 Method

Traced from the mac80211 ops-table entry through to the hardware
kick, across `core.c`, `pci.c`, and `rtl8188ee/trx.c`.

## 40.7.2 Confirmed call chain

```
mac80211
  -> rtl_op_tx (core.c:174, registered .tx = rtl_op_tx at core.c:1886)
       - gate: drop frame if hal stopped or rfpwr_state != ERFON
       - gate: drop frame if RTL_STATUS_INTERFACE_START not set
       - dispatch through rtlpriv->intf_ops (bus-transport ops table,
         separate from rtlpriv->cfg->ops chip-behavior table):
         -> intf_ops->waitq_insert = rtl_pci_tx_chk_waitq_insert
              (pci.c:1482, optional software queueing check)
         -> intf_ops->adapter_tx  = rtl_pci_tx (pci.c:1521)
              - spin_lock_irqsave(&rtlpriv->locks.irq_th_lock, ...)
              - select ring: rtlpci->tx_ring[hw_queue]; index by
                ring->cur_tx_wp (if use_new_trx_flow) or
                (ring->idx + skb_queue_len) % ring->entries (older flow)
              - ring-full check: cfg->ops->get_desc(..., HW_DESC_OWN)
                — classic DMA ownership-bit pattern; if hardware still
                owns the slot, drop frame back to mac80211
                (return skb->len, not a hard error)
              - cfg->ops->fill_tx_desc(...)
                  = rtl88ee_tx_fill_desc (rtl8188ee/trx.c:476)
                  — chip-specific descriptor content; DMA address
                  setup presumed to live here (not visible in
                  rtl_pci_tx itself)
              - cfg->ops->set_desc(..., HW_DESC_OWN, ...) — hands the
                descriptor to hardware
              - backpressure: ieee80211_stop_queue() if fewer than 2
                free descriptors remain (and not BEACON_QUEUE)
              - spin_unlock_irqrestore(...)
              - cfg->ops->tx_polling(hw, hw_queue)
                  = rtl88ee_tx_polling (rtl8188ee/trx.c:823)
                  — unconditional, once per rtl_pci_tx call; this is
                  the hardware doorbell/kick
```

Chip-specific pieces (`rtl88ee_tx_fill_desc`, `rtl88ee_tx_polling`)
are dispatched through the same `cfg->ops` mechanism already seen for
`hw_init`, `dm_watchdog`, `switch_channel`, etc. — a single consistent
pattern across the whole driver. `rtl88ee_tx_fill_cmddesc`
(trx.c:668) was found by the original grep but not yet placed in the
call chain — presumed to be used for command-type descriptors
(beacons/management), likely called from a different path (beacon
tasklet or similar), not the data-TX path traced here. Not yet
confirmed.

## 40.7.3 Side findings (not yet needed, noted for later)

- `rtl_pci_flush` (queue drain on stop) polls `skb_queue_len` with a
  bounded `msleep(20)` loop, capped at ~200 iterations (~4s) or an
  immediate return if RF is off — bounded, not a hazard, consistent
  with every other polling pattern found in rtlwifi so far (see
  Section 40.3/40.4's conclusion that rtlwifi generally avoids
  unbounded polling where rtw88 sometimes doesn't).
- `rtl_pci_deinit` shows the interrupt/tasklet teardown sequence
  (`synchronize_irq`, two `tasklet_kill` calls, `cancel_work_sync`)
  — relevant context for the eventual IOKit interrupt-handling
  design (Tier 1 risk), not analyzed further yet.

## 40.7.4 Conclusion

The TX path is now fully mapped source-to-source. Two ops tables are
involved (`cfg->ops` for chip behavior, `intf_ops` for bus/transport
mechanics) — a cleaner separation than initially assumed, and useful
for the eventual macOS port: `intf_ops` is exactly the layer that
will need a full IOKit/PCIe rewrite (DMA ring management, descriptor
ownership, doorbell), while `cfg->ops`'s chip-specific fill/polling
functions are comparatively narrow, self-contained pieces of logic
that can likely be ported closer to verbatim.

**Open sub-item:** which `use_new_trx_flow` branch RTL8188EE actually
uses (older `ring->idx`-based indexing, or newer `cur_tx_wp`-based) —
not yet confirmed from `rtl8188ee/sw.c` or `hw.c`. This determines
which of the two indexing schemes the macOS port needs to replicate.

# 40.8 CONFIRMED: PCI BAR Mapping and MMIO Register Access Primitives
     (partially resolves Section 41 item 1 — `_rtl_pci_io_handler_init`
     is fully read; `_rtl_pci_find_adapter` is still unread)

## 40.8.1 `_rtl_pci_io_handler_init` (pci.c:344-357)

Read in full — 13 lines. Wires `rtlpriv->io.{read,write}{8,16,32}`
function pointers to `pci_{read,write}{8,16,32}_{sync,async}`
(defined `pci.h:243-273`, `static inline`). No BAR mapping happens
here — purely function-pointer assignment.

## 40.8.2 The actual MMIO primitives (pci.h:243-273)

```c
pci_read8_sync(rtlpriv, addr)   -> readb(pci_mem_start + addr)
pci_read16_sync/read32_sync     -> readw/readl, same pattern
pci_write8_async(rtlpriv, addr, val) -> writeb(val, pci_mem_start + addr)
pci_write16_async/write32_async -> writew/writel, same pattern
```

**Correction to in-conversation working assumption:** "sync"/"async"
in these names is standard Linux MMIO terminology, not
threading/queuing — `writel` is a posted write that doesn't wait for
bus completion ("async"), `readl` inherently blocks until the read
completes ("sync"). No actual asynchronous dispatch, locking, or
queuing exists in these primitives. This is about as simple as MMIO
access gets: base pointer (`rtlpriv->io.pci_mem_start`) + offset,
straight through the standard kernel `readb`/`writeb`-family
accessors.

## 40.8.3 Where `pci_mem_start` is set — the actual BAR mapping
     (`rtl_pci_probe`, pci.c:~2140-2170)

Confirmed sequence inside `rtl_pci_probe`:

1. `pci_request_regions(pdev, KBUILD_MODNAME)` — claim PCI resource
   regions; failure → `goto fail1`.
2. `pci_resource_start/len/flags(pdev, rtlpriv->cfg->bar_id)` — BAR
   index (`bar_id`) is **chip-specific**, pulled from `rtlpriv->cfg`
   (the same `rtl_hal_cfg` struct holding `rtl88ee_hal_ops` etc.), not
   hardcoded in `pci.c`. RTL8188EE's actual `bar_id` value not yet
   confirmed — check `rtl8188ee/sw.c` when read.
3. `pci_iomap(pdev, bar_id, pmem_len)` → `rtlpriv->io.pci_mem_start`
   — the actual BAR mapping call. `0` is the failure sentinel
   (`goto fail2`).
4. Two direct **PCI config-space** writes (separate access path from
   the BAR-mapped MMIO region — goes through `pci_write_config_byte`,
   not the `readb`/`writeb` primitives above):
   `pci_write_config_byte(pdev, 0x81, 0)` (disable Clock Request /
   ASPM-related) and `pci_write_config_byte(pdev, 0x44, 0)` (leave D3
   power state).

Teardown: `pci_iounmap(pdev, pci_mem_start)`, called on both the
`rtl_pci_probe` error path (pci.c:2247) and normal device teardown
(pci.c:2301).

## 40.8.4 Conclusion

The full PCI BAR/MMIO chain is now traced end-to-end: PCI resource
claim → chip-specific BAR index lookup → `pci_iomap` → base pointer
→ `readb`/`writeb`-family register access used throughout the rest
of the driver via `rtlpriv->io.read*`/`write*` function pointers.
This is a clean, low-risk translation target for macOS/IOKit:
`pci_request_regions`+`pci_iomap` maps onto `IOPCIDevice::open()` +
BAR mapping (e.g. `mapDeviceMemoryWithIndex`), and the
`readb`/`writeb`-style accessors map onto `IOMemoryMap`'s
`readRegister8/16/32`/`writeRegister8/16/32`-style methods. No
locking or queuing needs to be replicated — the Linux primitives
don't have any at this layer either.

The two PCI-config-space writes (Clock Request disable, D3 exit) are
worth flagging separately since they go through `IOPCIDevice`'s
config-space accessors (`configWrite8`), not the memory-mapped BAR
accessors — a different macOS API surface than the rest of register
access.

**Still open (not yet read):** `_rtl_pci_find_adapter` (pci.c:1798),
the other of the two originally-flagged static helpers. Given
`rtl_pci_probe` already shown to include chip-specific `bar_id`
lookup via `rtlpriv->cfg`, `_rtl_pci_find_adapter` likely handles
chip identification/EEPROM-or-efuse-based hardware detection —
worth reading before considering Section 41 item 1 fully closed.

# 40.9 CONFIRMED: `_rtl_pci_find_adapter` Fully Read — Resolves
     Section 40.7's `use_new_trx_flow` Open Sub-Item and Closes
     Section 41 Item 1

## 40.9.1 Function purpose (pci.c:1798-1993)

`_rtl_pci_find_adapter` is a PCI device-ID dispatch table. It reads
`pdev->vendor`/`pdev->device`/config-space revision ID, and sets
`rtlhal->hw_type` via an if/else chain matching `deviceid` against
per-chip-family `RTL_PCI_*_DID` constants (RTL8192E/SE/CE/DE,
RTL8723AE/BE, RTL8188EE, RTL8192EE, RTL8821AE, RTL8812AE, RTL8822BE).
Confirmed match: `deviceid == RTL_PCI_8188EE_DID` →
`rtlhal->hw_type = HARDWARE_TYPE_RTL8188EE`. This is how the single
shared `rtl_pci_probe()` determines which chip family it's actually
talking to at runtime — necessary since `pci.c` is genuinely
multi-chip. No hazard-relevant content (no register polling, no
LPS/calibration touch) — pure PCI-config-space ID matching.

## 40.9.2 RESOLVES Section 40.7's open sub-item: `use_new_trx_flow`

Immediately after `hw_type` is set:

```c
switch (rtlhal->hw_type) {
case HARDWARE_TYPE_RTL8192EE:
case HARDWARE_TYPE_RTL8822BE:
    rtlpriv->use_new_trx_flow = true;
    break;
default:
    rtlpriv->use_new_trx_flow = false;
    break;
}
```

`HARDWARE_TYPE_RTL8188EE` is not in the `true` list, so it falls to
`default`. **Confirmed: RTL8188EE uses `use_new_trx_flow = false`**
— the older `ring->idx`-based descriptor indexing scheme in
`rtl_pci_tx` (Section 40.7.2), not the newer `cur_tx_wp`-based one.
This fully closes the TX-path open item.

## 40.9.3 Other content (bus/bridge topology, not chased further)

- Bus/device/function numbers recorded from `pdev` directly.
- PCI-bridge vendor lookup (`pcibridge_vendors[]` table) with a
  defensive null-check for ARM systems lacking a bridge device —
  feeds ASPM/power-quirk handling for specific chipsets (e.g. an
  AMD-specific "L1 patch" via `rtl_pci_get_amd_l1_patch`). Not
  hazard-relevant to anything tracked so far; noted as PCI-topology-
  dependent logic the macOS port will need to either replicate or
  safely stub, since IOKit exposes PCI bus/device/function
  information through a different API surface.
- Ends with a call to `rtl_pci_parse_configuration(pdev, hw)`
  (unread — presumed ASPM/link-control register parsing) and
  `return true`.

## 40.9.4 New lead surfaced, not yet chased

Immediately following this function in the file is
`rtl_pci_intr_mode_msi` — MSI interrupt registration
(`pci_enable_msi`, `request_irq(..., _rtl_pci_interrupt, ...)`).
This is directly relevant to the Tier-1 "IRQ registration" risk item
already flagged in the handover but was not one of the two originally
-targeted static helpers. Logged as a new candidate read for later,
not pursued this session to keep to the one-thing-at-a-time workflow.

## 40.9.5 Conclusion

Both originally-flagged pci.c static helpers
(`_rtl_pci_io_handler_init`, Section 40.8; `_rtl_pci_find_adapter`,
this section) are now fully read. **Section 41 item 1 is closed.**
Combined with Section 40.8, the entire PCI bring-up chain relevant to
BAR/MMIO and chip identification is now mapped source-to-source, with
no hazards found in either — this continues the pattern established
across LPS (40.4), calibration (40.5), bridge functions (40.6), and
TX (40.7): rtlwifi's actual mechanisms have consistently turned out
simpler and lower-risk than the original assumption, once traced
rather than inferred.

# 40.10 CONFIRMED: `rtl8188ee/sw.c` Lines 1-193 Read — Firmware
     Identity, IRQ Masks, Band/PHY Mode, BT-Coexist Lead

Read: `rtl88e_init_aspm_vars`, `rtl88e_init_sw_vars`,
`rtl88e_deinit_sw_vars`, `rtl88e_get_btc_status` (all of `sw.c`
before the `rtl8188ee_hal_ops` table, which was already read in
Section 40.3).

## 40.10.1 Firmware identity (partially resolves Section 41's
     firmware item)

- Filename: `"rtlwifi/rtl8188efw.bin"`.
- Buffer allocation: `vzalloc(0x8000)` (32KB), `max_fw_size = 0x8000`.
- Loaded via `request_firmware_nowait(THIS_MODULE, 1, fw_name,
  rtlpriv->io.dev, GFP_KERNEL, hw, rtl_fw_cb)` — standard Linux
  async firmware-loader API, callback `rtl_fw_cb` (not yet read).
- **Still open:** the Linux `request_firmware` subsystem has no
  macOS equivalent — the port will need to either embed the `.bin`
  as a kext resource or otherwise load it explicitly, then feed it
  into whatever `rtl_fw_cb`-equivalent parsing/validation logic
  exists (not yet read). This is a genuine translation gap, not
  just a "read the file" task.

## 40.10.2 IRQ masks and MSI support (Tier-1 IRQ lead, ties to
     Section 40.9.4)

- `rtlpci->irq_mask[0]`, `irq_mask[1]`, `rtlpci->sys_irq_mask` are
  hardcoded bitmask constants set here (PSTIMEOUT, C2HCMD, HIGHDOK,
  MGNTDOK, BKDOK, BEDOK, VIDOK, VODOK, RDU, ROK, RXFOVW, etc.) — the
  actual set of interrupt sources RTL8188EE enables.
- `rtlpci->msi_support = rtlpriv->cfg->mod_params->msi_support` —
  **MSI usage is a runtime module parameter, not hardcoded.** The
  port needs to either replicate this decision point or pick one
  mode deliberately (relevant once `rtl_pci_intr_mode_msi`, Section
  40.9.4's lead, is read).

## 40.10.3 Band/PHY mode (new context)

`current_bandtype = BAND_ON_2_4G`, `bandset = BAND_ON_2_4G`,
`macphymode = SINGLEMAC_SINGLEPHY`. Confirms RTL8188EE is
single-band (2.4GHz only) and single-MAC/single-PHY — simpler than
some other chips this same shared `pci.c`/`core.c` code handles
(e.g. RTL8192DE's dual-MAC logic seen in `_rtl_pci_find_adapter`,
Section 40.9). Reduces port scope — no dual-band/dual-interface
logic needed.

## 40.10.4 BT-coexist lead (relevant to Section 41's btcoexist item)

`rtl88e_get_btc_status()` is:
```c
static bool rtl88e_get_btc_status(void)
{
	return false;
}
```
Hardcoded, unconditional — not gated on a module param or hardware
detection. This is the same `cfg->ops->get_btc_status()` checked in
`rtl_op_sw_scan_start` (Section 40.6.2) to decide which BT-coexist
notify path to take; since it's always `false` for RTL8188EE, that
branch is dead code for this chip in upstream Linux already.

**Not yet conclusive:** `rtl8188ee_bt_reg_init(hw)` is still called
unconditionally at the top of `rtl88e_init_sw_vars` — its body is
unread, so it may still perform real hardware/state initialization
independent of the status flag. This is a real signal toward
"btcoexist can likely be stubbed for RTL8188EE" but not a full
confirmation until `rtl8188ee_bt_reg_init` itself is read.

## 40.10.5 Other init-time details (context, not yet needed further)

- `rtlpci->transmit_config = CFENDFORM | BIT(15)`.
- RCR (`receive_config`) flags — the same register touched in
  `rtl88ee_hw_init` (Section 40.5.2) and TX-mode toggles seen in the
  scan bridge functions (Section 40.6.2); this is where the default
  RCR value is first established.
- LPS-related module params read here: `psc.inactiveps`,
  `psc.swctrl_lps`, `psc.fwctrl_lps`; `psc.reg_fwctrl_lps = 3` →
  `fwctrl_psmode = FW_PS_DTIM_MODE` by default. Ties to the LPS work
  in Sections 40.2-40.4 but doesn't change any prior conclusion.
- ASPM constants (`rtl88e_init_aspm_vars`) — PCIe power-management
  policy values, not chased further; relevant context for macOS
  `IOPCIDevice` power-state handling but not blocking anything
  currently tracked.
- Per-TID (8 total) `skb_waitq` initialization, early-mode config
  (`earlymode_enable = false`, `max_earlymode_num = 10`) — ties to
  the early-mode TX aggregation logic seen in
  `_rtl_update_earlymode_info` (Section 40.8.2 area, `pci.c:359`).

## 40.10.6 Still not found in this range: `bar_id`

RTL8188EE's actual `bar_id` value (referenced by
`_rtl_pci_find_adapter`/`rtl_pci_probe`, Section 40.8.3) was not in
lines 1-193. It's a field of `rtl_hal_cfg`, not `rtl_hal_ops` — the
ops table already read in Section 40.3 is the wrong struct to find
it in. Likely defined alongside the `rtl_hal_cfg` struct itself,
possibly further down in `sw.c` or in `def.h`. Still open.

# 40.11 CONFIRMED: `rtl8188ee/sw.c` Fully Read — `bar_id`, Module
     Params, PCI ID Table, Driver Struct (Closes Section 41's
     `sw.c`/`bar_id`/PCI-ID-table items)

Read lines 193-393 (`rtl8188ee_hal_ops` table, `rtl88ee_mod_params`,
`rtl88ee_hal_cfg`, register-map table, `pci_device_id` table, module
metadata, `pci_driver` struct) — the whole file is now read.

## 40.11.1 `bar_id` — resolved

`rtl88ee_hal_cfg.bar_id = 2`. Directly the value consumed by
`pci_resource_start/len/flags(pdev, rtlpriv->cfg->bar_id)` and
`pci_iomap(pdev, bar_id, pmem_len)` traced in Section 40.8.3. RTL8188EE
maps PCI BAR index 2. Also confirmed: `.write_readback = true`,
`.name = "rtl88e_pci"`.

## 40.11.2 `rtl88ee_mod_params` — concrete defaults

```c
static struct rtl_mod_params rtl88ee_mod_params = {
    .sw_crypto = false, .inactiveps = true, .swctrl_lps = false,
    .fwctrl_lps = false, .msi_support = true, .aspm_support = 1,
    .debug_level = 0, .debug_mask = 0,
};
```

- **`msi_support = true` by default** — resolves the practical
  question from Section 40.10.2: MSI is the path a typical bring-up
  actually exercises, not a fallback. `rtl_pci_intr_mode_msi`
  (Section 40.9.4 lead) is the relevant code path.
- **`swctrl_lps = false`, `fwctrl_lps = false` by default** — the LPS
  leave/enter mechanism traced in Sections 40.2-40.4 may not even
  engage on a first bring-up unless explicitly enabled. Doesn't
  change the safety conclusions already reached (those were about
  the mechanism itself, not its default enablement) — but is
  practical context for what an initial boot test will and won't
  exercise.
- **Discrepancy found, flagged rather than silently resolved:** the
  `MODULE_PARM_DESC` comment for `fwlps` claims "(default 1)", but
  the struct literal sets `fwctrl_lps = false` (i.e. 0). Since
  `module_param_named` exposes the struct field rather than
  overriding its compile-time initializer, the actual default is
  `false` — the doc-string appears to be stale/incorrect in the
  upstream kernel source, not evidence of a different real value.
  Noted for awareness; not something to resolve differently without
  further reason.

## 40.11.3 PCI ID table and driver registration — resolved

```c
static const struct pci_device_id rtl88ee_pci_ids[] = {
    {RTL_PCI_DEVICE(PCI_VENDOR_ID_REALTEK, 0x8179, rtl88ee_hal_cfg)},
    {},
};
```
Ties `0x8179` directly to `rtl88ee_hal_cfg` via the `RTL_PCI_DEVICE`
macro. Driver struct:
```c
static struct pci_driver rtl88ee_driver = {
    .name = KBUILD_MODNAME, .id_table = rtl88ee_pci_ids,
    .probe = rtl_pci_probe, .remove = rtl_pci_disconnect,
    .driver.pm = &rtlwifi_pm_ops,
};
```
Confirms `.probe = rtl_pci_probe` (shared, already known) and PM ops
wired via `SIMPLE_DEV_PM_OPS(rtlwifi_pm_ops, rtl_pci_suspend,
rtl_pci_resume)`.

## 40.11.4 Conclusion

The entire `rtl8188ee/sw.c` file is now read. All items originally
tracked against it (Section 41: `bar_id`, PCI ID table entry, module
init) are closed. Combined with Sections 40.8-40.10, the full PCI
bring-up → chip-ID dispatch → BAR/MMIO → sw-vars-init chain is now
mapped source-to-source for RTL8188EE specifically, not just
generically across rtlwifi.

------------------------------------------------------------------------

# 40.12 CONFIRMED: rtlwifi's `.sw_scan_complete` op (core.c:1444)

`rtl_op_sw_scan_complete` — scan-state cleanup only: resets
`act_scanning`/`skip_scan`/`n_channels`, updates btcoexist AP count,
toggles p2p-scan heuristic, clears `load_imrandiqk_setting_for2g`.
If link was `MAC80211_LINKED_SCANNING` and opmode is station, makes
one dispatched call: `cfg->ops->set_network_type(hw, mac->opmode)`.
No polling/blocking pattern. Closes the last Section 40.6 loose end —
all three `sw_scan_*` bridge functions and their rtlwifi equivalents
are now fully clean on both sides of the bridge.

------------------------------------------------------------------------

# 40.13 CONFIRMED: MSI Interrupt Registration and Handler
     (`rtl_pci_intr_mode_msi`, `_rtl_pci_interrupt`, pci.c)

Source: `linux-kernel/drivers/net/wireless/realtek/rtlwifi/pci.c`,
lines 835 (`_rtl_pci_interrupt`) and 1997 (`rtl_pci_intr_mode_msi`).

## Interrupt handler — `_rtl_pci_interrupt` (pci.c:835)

Standard shared-IRQ top-half pattern, not chip-specific:

1. Early-out if `rtlpci->irq_enabled == 0`.
2. Takes `rtlpriv->locks.irq_th_lock` (spinlock, IRQ-safe).
3. `cfg->ops->disable_interrupt(hw)` then
   `cfg->ops->interrupt_recognized(hw, &intvec)` reads the ISR
   (4/8 bytes) into a local `struct rtl_int`.
4. Shared-IRQ/hardware-gone guard: if `!intvec.inta || intvec.inta ==
   0xffff`, skip to cleanup (`goto done`) — the `0xffff` case is the
   standard Linux idiom for "device was hot-unplugged / link down,
   register reads returning all-ones."
5. Remaining logic is a flat sequence of `if (intvec.inta & mask)`
   bit tests against `rtlpriv->cfg->maps[...]` — beacon
   ok/err/interrupt, then TX-queue-complete bits, each either logging
   or calling `_rtl_pci_tx_isr(hw, QUEUE)` (per-queue TX-complete
   processing) or `tasklet_schedule(&rtlpriv->works.irq_prepare_bcn_tasklet)`
   for beacon prep. All deferred work is via `tasklet_schedule`, not
   done inline in the ISR.

No register-read polling loop anywhere in the handler — it reads the
ISR once, branches on the bits, and returns. This is the same
single-dispatch-per-condition pattern already confirmed safe
elsewhere in rtlwifi (Section 40.3).

## MSI setup — `rtl_pci_intr_mode_msi` (pci.c:1997)

```c
static int rtl_pci_intr_mode_msi(struct ieee80211_hw *hw)
{
    ...
    ret = pci_enable_msi(rtlpci->pdev);
    if (ret < 0)
        return ret;

    ret = request_irq(rtlpci->pdev->irq, &_rtl_pci_interrupt,
                       IRQF_SHARED, KBUILD_MODNAME, hw);
    if (ret < 0) {
        pci_disable_msi(rtlpci->pdev);
        return ret;
    }

    rtlpci->using_msi = true;
    ...
    return 0;
}
```

Textbook Linux MSI setup: `pci_enable_msi()` then `request_irq()`
against the same `_rtl_pci_interrupt` handler used for legacy
pin-based IRQs (i.e. **one handler serves both interrupt modes** —
mode only changes how the IRQ line itself is wired up, not what
processes it). On failure at either step it cleanly unwinds
(`pci_disable_msi` on `request_irq` failure) and returns an error.

`rtl_pci_intr_mode_decide` (pci.c, adjacent) is the caller: tries MSI
first when `rtlpci->msi_support` is set (confirmed default `true` for
RTL8188EE per Section 40.11.2), falls back to
`rtl_pci_intr_mode_legacy` (plain `request_irq`, `IRQF_SHARED`, no
MSI) if MSI setup fails. So MSI is preferred-with-fallback, not an
unconditional requirement — relevant for a macOS port if
`IOPCIDevice` MSI setup fails on a given system, since the upstream
driver already treats that as a recoverable path rather than fatal.

## Conclusion

No hazard found in either function — no polling, no indefinite wait,
clean error unwind. Both are a fairly direct conceptual match for
macOS `IOPCIDevice`/`IOInterruptEventSource` MSI + legacy-pin
interrupt setup (`IOPCIDevice::configWrite`/`kIOPCIMSIInterruptType`
family). The one thing to preserve in the port: **the fallback
behavior** (MSI attempt → legacy on failure), not just the MSI path
alone, and the fact that a single dispatch handler processes both
modes' interrupts identically once wired up. This closes the last
Tier-1 IRQ-registration lead from Section 40.9.4.

------------------------------------------------------------------------

# 40.14 CONFIRMED: `rtl8188ee_bt_reg_init` / `rtl8188ee_bt_hw_init` —
     Closes the Btcoexist-Stubbing Question

Source: `linux-kernel/drivers/net/wireless/realtek/rtlwifi/rtl8188ee/hw.c`,
lines 2462 (`rtl8188ee_bt_reg_init`) and 2472 (`rtl8188ee_bt_hw_init`).

## `rtl8188ee_bt_reg_init` (hw.c:2462)

Trivial — sets exactly two `rtl_priv` config fields and returns:

```c
void rtl8188ee_bt_reg_init(struct ieee80211_hw *hw)
{
    struct rtl_priv *rtlpriv = rtl_priv(hw);

    rtlpriv->btcoexist.reg_bt_iso = 2;   /* 0:Low, 1:High, 2:From Efuse */
    rtlpriv->btcoexist.reg_bt_sco = 0;
}
```

No register access, no hardware I/O, no polling — pure struct
initialization. Effectively free to keep as-is in a port.

## `rtl8188ee_bt_hw_init` (hw.c:2472)

This is the function that actually touches hardware registers
(`rtl_write_byte`/`rtl_read_byte` on `REG_GPIO_MUXCFG` and `0x4fd`),
but the entire body is gated behind one condition:

```c
if (rtlpriv->btcoexist.bt_coexistence &&
    ((rtlpriv->btcoexist.bt_coexist_type == BT_CSR_BC4) ||
      rtlpriv->btcoexist.bt_coexist_type == BT_CSR_BC8)) {
    ...
}
```

Combined with Section 40.10.4's finding that `rtl88e_get_btc_status()`
is hardcoded `return false` for RTL8188EE, this register-touching
path is already dead code on this chip in upstream Linux — nothing
sets `bt_coexistence` true for RTL8188EE through the normal init
path.

## Conclusion

The btcoexist-stubbing question is now **fully closed**. A macOS port
can either keep `rtl8188ee_bt_reg_init` (cheap, harmless struct init)
or omit it entirely, and can stub `rtl8188ee_bt_hw_init` outright (or
just leave `bt_coexistence` unset/false) without losing any behavior
RTL8188EE actually exercises upstream — the hardware-touching path
was already conditionally dead for this chip.

------------------------------------------------------------------------

# 40.15 CONFIRMED: `rtl_fw_cb` / `rtl_fw_do_work` — Firmware
     Load-Completion Callback (core.c:61, core.c:105)

Source: `linux-kernel/drivers/net/wireless/realtek/rtlwifi/core.c`,
lines 61 (`rtl_fw_do_work`) and 105 (`rtl_fw_cb`).

`rtl_fw_cb` is a one-line wrapper: `rtl_fw_do_work(firmware, context,
false)`. The real logic — shared across every rtlwifi chip, not
RTL8188EE-specific — is in `rtl_fw_do_work`:

1. **Null-firmware fallback.** If `firmware == NULL` (the standard
   `request_firmware_nowait` failure signal), attempts a
   **synchronous** `request_firmware()` against
   `rtlpriv->cfg->alt_fw_name`. If that also fails: sets
   `max_fw_size = 0`, jumps to exit.
2. **Size guard.** Rejects (`release_firmware` + exit) if
   `firmware->size > rtlpriv->max_fw_size` — ties to the `0x8000`
   (32KB) buffer confirmed in Section 40.10.1.
3. **Copy into pre-allocated buffer.** `memcpy` into
   `rtlhal->pfirmware` (normal path — what `rtl_fw_cb`/RTL8188EE
   uses) or `rtlhal->wowlan_firmware` (WoWLAN path, via the sibling
   `rtl_wowlan_fw_cb`/`is_wow=true` — not exercised by RTL8188EE's
   plain firmware load).
4. **Always releases** the firmware object once copied.
5. **Always signals completion** — `complete(&rtlpriv
   ->firmware_loading_complete)` fires on every path, including both
   failure branches. This is a `struct completion`.

No hazard — no polling, no indefinite loop.

## 40.15.1 CORRECTION: `wait_for_completion` is NOT a startup gate

The prior revision of this section speculated that something in
`hw_init`/`adapter_start` blocks on
`wait_for_completion(&rtlpriv->firmware_loading_complete)` before
proceeding. **This is wrong** — traced and corrected via
`grep -rn "firmware_loading_complete" rtlwifi/`.

Both PCI-path `wait_for_completion` call sites (`pci.c:2242`,
`pci.c:2269`) are **error-cleanup / teardown safety waits, not
startup gates**:

- `pci.c:2242` — inside `rtl_pci_probe()`'s `fail3:` label, reached
  only if IRQ registration (`rtl_pci_intr_mode_decide`) fails. Waits
  for any in-flight firmware callback before `deinit_sw_vars(hw)`
  runs, so the completion object isn't torn down mid-callback.
- `pci.c:2269` — in `rtl_pci_disconnect` (module/device removal),
  with an explicit source comment: `/* just in case driver is
  removed before firmware callback */`. Same purpose.

**Neither is on the normal success path.** `rtl_pci_probe()` returns
success without waiting for firmware to actually finish loading —
`request_firmware_nowait()` fires, probe continues immediately, and
`rtl_fw_cb`'s memcpy into `pfirmware` happens later, asynchronously,
whenever the kernel's firmware-loader workqueue gets to it.

**Practical implication:** whatever actually *consumes* the firmware
buffer (presumed `rtl88ee_hw_init`, invoked later from `rtl_op_start`
when the interface is brought up, not during probe) must have its
own readiness check — either it re-checks `rtlhal->fwsize`/
`pfirmware`, or there's a separate synchronization point not yet
located. **Not yet confirmed** — worth a follow-up read of
`rtl88ee_hw_init` specifically for how it handles the case where
firmware hasn't finished loading yet.

## Port-relevant conclusion (revised)

This confirms the *shape* of the load-and-hand-off mechanism the
macOS port needs to replace (`request_firmware_nowait` itself has no
macOS equivalent, per Section 40.10.1) — **corrected from the prior
revision**:

- **Async load, not synchronous, and NOT blocking at probe time** —
  probe/attach succeeds immediately; firmware lands later via
  callback. This is more permissive than initially assumed, but
  pushes the readiness question onto whatever consumes the firmware
  buffer at actual hardware bring-up.
- **Bounded pre-allocated destination buffer** (32KB) — size-checked
  before copy, not resized.
- **Completion object exists only for teardown safety** — to avoid
  destroying it mid-callback during error cleanup or device removal,
  not as an init-path synchronization primitive.
- **Open follow-up:** how `rtl88ee_hw_init` (or whatever actually
  programs firmware into hardware) handles a not-yet-loaded firmware
  buffer — this is now the real synchronization question for the
  port, not a blocking wait at probe.
- A macOS equivalent needs: an async firmware-read (e.g. from the
  kext bundle's Resources, or a matching in-kernel load path),
  landing in an equivalently-sized buffer — with the port's own
  IOKit `start()`/`probe()` free to succeed without waiting, mirroring
  upstream's actual (not previously assumed) behavior, PROVIDED the
  later hardware-init step has its own readiness check to add.

------------------------------------------------------------------------

# 40.16 CONFIRMED: `rtl_pci_parse_configuration` (pci.c:308)

```c
static void rtl_pci_parse_configuration(struct pci_dev *pdev,
                                          struct ieee80211_hw *hw)
{
    ...
    pcie_capability_read_word(pdev, PCI_EXP_LNKCTL, &linkctrl_reg);
    pcipriv->ndis_adapter.linkctrl_reg = (u8)linkctrl_reg;

    pcie_capability_set_word(pdev, PCI_EXP_DEVCTL2,
                              PCI_EXP_DEVCTL2_COMP_TMOUT_DIS);

    tmp = 0x17;
    pci_write_config_byte(pdev, 0x70f, tmp);
}
```

Matches the presumption from Section 40.9/40.11: pure PCIe
config-space parsing/tweaking, no MMIO, no polling.

1. **Reads** the PCIe Link Control register (`PCI_EXP_LNKCTL`) and
   stashes it in `pcipriv->ndis_adapter.linkctrl_reg` — read for
   later reference (used elsewhere for ASPM state), not acted on
   here.
2. **Sets** `PCI_EXP_DEVCTL2_COMP_TMOUT_DIS` in the Device Control 2
   register — disables PCIe Completion Timeout. Standard Realtek
   workaround: avoids the chip triggering a completion-timeout error
   under normal link-power-state transitions.
3. **Writes** a fixed byte (`0x17`) to vendor-specific config offset
   `0x70f` — undocumented in the kernel comment; a known Realtek
   magic-register write with no explanation in-tree (consistent with
   other Realtek vendor-specific PCI quirk writes elsewhere in this
   codebase).

All three operations are `pci_*_config_*`/`pcie_capability_*` calls —
PCI **configuration space** access, not BAR-mapped MMIO. This is the
same "different macOS API" class already flagged in Section 40.8
(`IOPCIDevice::configRead`/`configWrite`, not `IOMemoryMap`).

No hazard. Closes the last open pci.c item — both originally-flagged
static helpers plus this configuration-parsing function are now all
read; pci.c is fully closed out.

------------------------------------------------------------------------

# 40.17 CONFIRMED: Firmware Readiness at `rtl88ee_hw_init` — Closes
     the Section 40.15.1 Open Follow-Up

Source: `linux-kernel/drivers/net/wireless/realtek/rtlwifi/rtl8188ee/hw.c`
(line 1031, `rtl88ee_hw_init`) and
`linux-kernel/drivers/net/wireless/realtek/rtlwifi/rtl8188ee/fw.c`
(line 107, `rtl88e_download_fw`).

## The mechanism: fail-fast check, not a wait

No `wait_for_completion`, no polling, no synchronization primitive of
any kind guards firmware readiness on the RTL8188EE hw-init path.
Instead:

1. `rtl88ee_hw_init` sets `rtlhal->fw_ready = false` up front
   (hw.c:1055).
2. Calls `rtl88e_download_fw(hw, false)` (hw.c:1075) —
   **synchronous**, not deferred.
3. `rtl88e_download_fw` (fw.c:107-118) immediately checks
   `if (!rtlhal->pfirmware) return 1;` — if the async `rtl_fw_cb`
   callback (Section 40.15) hasn't populated the buffer yet, this
   returns failure **instantly**, no blocking.
4. If firmware IS present: writes it into hardware
   (`_rtl88e_write_fw`) and confirms with `_rtl88e_fw_free_to_go`.
5. Back in `hw_init`: nonzero return → logs `"Failed to download FW.
   Init HW without FW now.."`, sets `err = 1`, jumps to `exit`
   (hw.c:1076-1080). **Hardware init fails cleanly and explicitly —
   it does not hang or retry.**
6. Only on success does `hw_init` set `rtlhal->fw_ready = true`
   (hw.c:1082) and continue fw-dependent setup. `fw_ready` is then
   checked at hw.c:108, 178, 1388 to gate further fw-dependent ops
   elsewhere in the file.

## Why this is safe without explicit synchronization

`hw_init` isn't called from `probe()` — it's invoked later, from
`rtl_op_start` (core.c), which mac80211 calls when the interface is
actually brought up (typically well after `rtl_pci_probe()` returned
and the async `request_firmware_nowait()` had time to complete on the
kernel's firmware-loader workqueue). The design relies on that
practical timing gap rather than an explicit wait — if firmware
genuinely isn't ready yet, `hw_init` fails fast and cleanly instead of
blocking indefinitely.

## Conclusion — fully closes the Section 40.15.1 open question

This is the simplest possible pattern: **null-check-and-fail-fast**,
no polling, no hazard. Port-relevant takeaway: the macOS equivalent
of `hw_init` needs the same shape — check whether the firmware buffer
is populated, proceed if so, fail explicitly and report a clean
error if not (never block/spin waiting). This also confirms probing/
attach and hardware bring-up are appropriately decoupled in the
original design — mirroring that decoupling (rather than forcing
firmware load to complete during IOKit `probe()`/`start()`) is the
correct translation, with the same fail-fast check at the point
firmware is actually consumed.

**This closes the last open rtlwifi-source question from Section 41.
rtlwifi source mapping is now complete — no further open items remain
that require reading more Linux kernel source; everything left is
either Feixiao-side work or runtime verification.**

------------------------------------------------------------------------

# 41. What Is NOT Yet Confirmed (supersedes/extends Section 36)

**Consolidation note:** as of this revision, every rtlwifi-source
question from the original investigation plan is resolved — see
Sections 40.5 through 40.17 for detail. **All of pci.c and
rtl8188ee/{sw.c, hw.c firmware path} are fully read. No further
Linux kernel source reading is required for the core architectural
questions this investigation set out to answer.** Genuinely open
items only, none of which are answerable by more rtlwifi source
reading:

1. Whether Feixiao's mac80211/cfg80211 compat shim matches rtlwifi's
   expected struct sizes/signatures, or only rtw88's — this requires
   reading Feixiao's own source, not more rtlwifi.
2. Runtime stability on macOS Monterey — unchanged, still unknown;
   inherently untestable by source reading alone.
3. Runtime boot-test verification of the LPS-hazard (Section 40.4)
   and RF-calibration watchdog-responsiveness (Section 40.5)
   conclusions — both are static-analysis results pending an actual
   connect/scan/channel-change test.

------------------------------------------------------------------------

# 42. Updated Recommended Next Steps

**rtlwifi source mapping is now COMPLETE.** Every question from the
original investigation plan has been traced to a source-verified
answer across Sections 40.5–40.17: RF calibration, all nine bridge
functions, TX path, all three pci.c static/config helpers, the
entire `rtl8188ee/sw.c` file, scan-complete op, MSI interrupt
registration, btcoexist stubbing, firmware identity/load-callback,
and firmware-readiness-at-hw-init. **pci.c and the relevant parts of
rtl8188ee/{sw.c, hw.c, fw.c} are all fully read.**

There is no more rtlwifi source reading queued. What remains:

1. **Feixiao compat-shim audit** — whether Feixiao's existing
   mac80211/cfg80211 code matches rtlwifi's expected struct
   sizes/signatures (vs. only rtw88's). This is the real next phase —
   architecture/design work against Feixiao's own source, not rtlwifi.
2. **Runtime verification** (LPS-hazard, RF-calibration timing,
   general Monterey stability) — not answerable by more source
   reading; needs an actual boot test once porting work has produced
   something bootable.

**Recommended next phase:** shift from "read rtlwifi source" to
auditing Feixiao's actual compat-shim code (`RTW88IEEE80211.cpp`,
`rtw88_compat.c`) against everything now known about rtlwifi's
requirements — struct field usage, `hw->priv` casting, ops-table
expectations. This is the item standing between "we understand
rtlwifi" and "we can start writing the port," per the handover's
"Do not do yet" constraints (still no large rewrites, no internal
EFI/boot-config testing until verified).

------------------------------------------------------------------------

# 43. Important Development Rule (unchanged, carried forward)

Do not yet:

- add `0x8179` to the existing rtw88 table and expect it to work
  (also known to be architecturally unnecessary)
- copy the entire rtlwifi tree without checking dependencies
- rewrite large sections of Feixiao
- modify the stable internal EFI
- test an unverified kext using the stable boot configuration
- **assume** `rtw88_connect_hw_setup`/`rtw88_restore_connected_hw` can
  simply be deleted — the Section 40.4 conclusion is source-verified
  but not yet runtime-verified, and the RF calibration question
  (Section 41.5) is still open

The current stage is **source mapping and architecture verification**.

------------------------------------------------------------------------

# 44. Current Conclusion (supersedes Section 39)

The project remains a genuine **rtlwifi → macOS** driver port.
**rtlwifi source mapping is now complete** — every architectural
question raised across this and prior sessions has been traced to a
source-verified answer:

- **LPS/channel-switch hazard** (the original reason `rtw88_connect_
  hw_setup`/`rtw88_restore_connected_hw` existed for rtw88): does
  **not exist** in rtlwifi's equivalent path (Section 40.3–40.4).
  Both bridge functions appear to have no rtlwifi-side reason to
  exist as custom bypasses.
- **RF calibration (IQK)**: driven by `rtl88ee_hw_init` (once) and
  `rtl88e_dm_watchdog` (periodic, thermal-drift-gated) — never by
  channel switch (Section 40.5).
- **All nine Category B bridge functions**: read in full; none
  require a rtlwifi-specific rewrite (Section 40.6). The bridge layer
  collapses toward "use stock rtlwifi mac80211 ops directly."
- **Full TX path**: traced `rtl_op_tx` → `intf_ops->adapter_tx` →
  `cfg->ops->fill_tx_desc`/`tx_polling` (Section 40.7). Clean
  `intf_ops` (bus/transport, needs IOKit rewrite) vs `cfg->ops` (chip
  behavior, narrow/self-contained) split. `use_new_trx_flow = false`
  confirmed for RTL8188EE (Section 40.9.2).
- **PCI BAR/MMIO, device-ID dispatch, and config-space parsing**: all
  three pci.c static helpers read (Sections 40.8, 40.9, 40.16) —
  **pci.c is fully closed**. Low-risk `IOPCIDevice`/`IOMemoryMap`
  (MMIO) and `configRead`/`configWrite` (config-space) translation
  targets.
- **`rtl8188ee/sw.c`**: fully read (Sections 40.10–40.11) —
  `bar_id = 2`, PCI ID table entry, module-param defaults all
  confirmed.
- **Scan-complete op, MSI interrupt registration, btcoexist
  stubbing**: all confirmed clean, no hazards (Sections 40.12–40.14).
  Btcoexist can be safely stubbed entirely.
- **Firmware load/hand-off mechanism, fully traced end-to-end**:
  identity, buffer size, the `rtl_fw_cb`/`rtl_fw_do_work` async
  callback (Section 40.15), and firmware-readiness at hw-init
  (Section 40.17) are all confirmed. **Correction from an earlier
  revision** (Section 40.15.1): the completion object is NOT a
  startup gate — `rtl_pci_probe()` succeeds without waiting for
  firmware to finish loading. **Fully resolved** (Section 40.17):
  `rtl88ee_hw_init` handles this with a simple synchronous
  null-check-and-fail-fast in `rtl88e_download_fw`
  (`if (!rtlhal->pfirmware) return 1;`) — no polling, no wait, clean
  explicit failure if firmware isn't ready by the time hw-init runs.
  The core translation gap — no macOS equivalent to
  `request_firmware_nowait` — remains, but its complete *shape* is
  now known end-to-end: async load, bounded pre-allocated buffer,
  non-blocking probe, fail-fast check at the point of actual use.

**All source questions from the original investigation plan are now
closed. rtlwifi source mapping is complete** — pci.c and the relevant
parts of `rtl8188ee/{sw.c, hw.c, fw.c}` are all fully read. What
remains is entirely outside further rtlwifi source reading:

1. Auditing Feixiao's own compat-shim code against these findings —
   the actual gating item before writing any port code.
2. Runtime verification of conclusions that source reading alone
   cannot settle (LPS-hazard, RF-calibration timing, general
   Monterey stability) — needs a real boot test once something is
   bootable.

The natural next phase is shifting entirely from "read rtlwifi
source" to auditing Feixiao's own source, or beginning the mechanical
architecture work described in the handover — per the handover's
"Do not do yet" constraints.

------------------------------------------------------------------------

# 45. CORRECTION — MAJOR: Feixiao Compiles Unmodified Upstream rtw88
     Source Directly. The Port Is NOT a Hand-Translation Task.

**This section corrects the working assumption behind Sections 1–44.**
Everything in those sections about rtlwifi's internal behavior (LPS
hazards, IQK triggers, TX path, MSI setup, firmware hand-off) remains
factually accurate as a description of what rtlwifi does — but it was
gathered under the assumption that porting RTL8188EE means **hand-
translating rtlwifi's C source into new macOS/IOKit-native code**,
function by function. That assumption is wrong, and it changes what
"port RTL8188EE" actually means as an engineering task.

## 45.1 Method

Audited Feixiao's actual source tree directly (not just the handover's
prose description of it):

1. `find` across `Feixiao/src` for all `.c/.cpp/.h/.hpp` files —
   confirmed `src/compat/linux/*` is a ~38-header LinuxKPI-style shim
   layer (types, atomic, spinlock, mutex, completion, workqueue,
   skbuff, dma-mapping, pci, firmware, interrupt, etc.), not a
   one-off rtw88-specific stub set.
2. Read `src/compat/rtw88_compat.h` in full (209 lines).
3. Grepped all top-level function definitions in
   `src/compat/rtw88_compat.c` (1274 lines).
4. Found `extern` declarations referencing `struct rtw_dev *`/
   `struct rtw_pci *`-typed functions (`rtw88_be_ring_avail`,
   `rtw88_get_be_bd`, `rtw_pci_enable_interrupt`,
   `rtw88_get_be_ring_state`) annotated `/* pci.c */`, called from
   `rtw88_compat.c` as real external functions, not stubs.
5. Located the actual `pci.c` on disk:
   `find .. -name "pci.c"` → both
   `../rtw88-stable/drivers/net/wireless/realtek/rtw88/pci.c` and
   `../linux-kernel/drivers/net/wireless/realtek/rtlwifi/pci.c`
   exist as sibling checkouts. The `/* pci.c */`-annotated externs
   resolve to the **rtw88-stable one** — real upstream Linux driver
   source, not a Feixiao-authored file of the same name.
6. Confirmed via `Feixiao/Makefile` (line 24):
   `LINUX_SRC := $(PROJ_ROOT)/../rtw88-stable/drivers/net/wireless/realtek/rtw88`
   — and `DRIVER_SRCS`/`CHIP_SRCS` (Makefile lines 106–150) list
   **every core and chip-specific rtw88 `.c` file** (`main.c`, `mac.c`,
   `phy.c`, `fw.c`, `tx.c`, `rx.c`, `sec.c`, `efuse.c`, `coex.c`,
   `ps.c`, `regd.c`, `bf.c`, `sar.c`, `util.c`, `pci.c`, `usb.c`,
   `sdio.c`, `mac80211.c`, plus all `rtw88xx*`/`*_table.c` chip files)
   compiled straight from `$(LINUX_SRC)`, unmodified paths, no local
   copy or patch step visible in the Makefile.

## 45.2 The actual architecture

```
Real upstream rtw88-stable/.../rtw88/*.c   (compiled AS-IS, unmodified)
        |
        | -include rtw88_compat.h forces every linux/*.h and net/*.h
        | #include to resolve against src/compat/linux/*, src/compat/net/*
        v
src/compat/linux/*.h + src/compat/net/mac80211.h   (LinuxKPI-style shim:
        |                                            emulates enough of the
        |                                            Linux kernel API surface
        |                                            for the real driver to
        |                                            compile and link)
        v
src/compat/rtw88_compat.c   (implements the shim primitives, PLUS the small
        |                     number of genuinely rtw88-specific bridge
        |                     functions: rtw88_connect_hw_setup,
        |                     rtw88_sw_scan_*, rtw88_register_vif, etc. —
        |                     all cast hw->priv -> struct rtw_dev * directly,
        |                     matching rtw88's own hw->priv convention)
        v
src/kext/RTW88PCIDevice.cpp, RTW88IEEE80211.cpp, RTW88Kext.cpp   (actual
        IOKit C++ classes: PCI matching/MMIO/IRQ registration, driving the
        compiled-in rtw88 core via its real entry points, e.g. presumably
        calling rtw_pci_probe() equivalent-path logic)
```

Confirmed from `Makefile` lines 61–99:
- `DRIVER_CFLAGS` includes `-I$(COMPAT_DIR) -I$(COMPAT_DIR)/linux
  -include $(COMPAT_DIR)/rtw88_compat.h -I$(LINUX_SRC) -D__KERNEL__`
  plus **chip-family config gates**
  (`-DCONFIG_RTW88_8822BE=1`, `-DCONFIG_RTW88_8822CE=1`, etc.,
  lines 76–84) — these map directly onto `#ifdef CONFIG_RTW88_xxx`
  guards inside the real rtw88 upstream source, controlling which
  chip-specific translation units are meaningfully compiled in.
- Notably permissive warning suppression:
  `-Wno-implicit-function-declaration -Wno-int-conversion
  -Wno-incompatible-pointer-types` (lines 85–87). This means the
  compat layer's coverage of the real Linux API surface used by the
  driver is **not fully type-checked at compile time** — a call to an
  unshimmed or mismatched-signature Linux function can silently
  compile (implicit declaration, wrong pointer type) rather than fail
  the build. This is a real risk surface: gaps in compat-layer
  coverage may only surface at link time or runtime, not compile
  time, and this risk is structurally the same for any new driver
  family (rtlwifi) added the same way.

## 45.3 Firmware loading — Feixiao does NOT emulate `request_firmware_nowait`

Confirmed from `Makefile` lines 156–160, 232–250 and the separate
`FW_CFLAGS` block (lines 238–242): `rtw88_firmware.c` and the
build-generated `fw_blobs.c` (from `firmware/*.bin` via
`scripts/gen_fw_blobs.py`) are compiled **without** the Linux compat
headers and **without** `-DRTW88_MACOS=1`/driver flags — i.e. firmware
loading is handled entirely outside the Linux-emulation path, via
firmware blobs embedded directly into the kext binary at build time
(zlib-compressed C arrays generated from `firmware/*.bin`), not via
any macOS equivalent of `request_firmware_nowait`.

**This resolves/reframes Section 40.15–40.17's open firmware
question.** Those sections traced, in detail, how upstream rtlwifi's
`request_firmware_nowait` → `rtl_fw_cb` → `pfirmware`/`fw_ready`
consumption chain behaves, on the correct assumption that Linux's
async firmware-loader subsystem has no macOS equivalent. That
analysis is still useful for understanding what `rtl88ee_hw_init`
expects to find in `rtlhal->pfirmware` and when — but Feixiao's actual
solution to "there's no `request_firmware_nowait` on macOS" is
**architectural bypass, not compat-layer emulation**: bake the `.bin`
into the kext, and feed it into the driver's consumption point
directly (bypassing the request/callback machinery entirely), rather
than trying to emulate `request_firmware_nowait` + the kernel
firmware-loader workqueue inside the compat layer. The rtlwifi port
should very likely follow the same pattern — embed
`rtl8188efw.bin` as a blob and populate `rtlhal->pfirmware`/
`rtlhal->fwsize`/`fw_ready`-equivalent state directly, skipping
`rtl_fw_cb`/`rtl_fw_do_work` (Section 40.15) entirely rather than
porting them.

## 45.4 Practical consequence: what "port RTL8188EE" actually means

The task is **not** "hand-translate rtlwifi's C source into new
macOS-native code, file by file" (the implicit model behind Sections
1–44's function-by-function behavioral tracing). It is much closer to:

1. **Extend `src/compat/linux/*`** to cover whatever Linux kernel API
   surface `linux-kernel/.../rtlwifi/{pci.c, core.c, ps.c, base.c,
   efuse.c, ...}` use that the current rtw88-targeted shim doesn't
   already provide. This is a coverage-diffing exercise (rtw88's
   Linux API usage vs. rtlwifi's), not a rewrite of driver logic —
   and given both are mainline Linux wireless drivers of a similar
   era, a large fraction of what's already shimmed (workqueues,
   timers, mutexes, skbuff, dma-mapping, pci config/BAR access,
   completion) is likely directly reusable as-is.
2. **Compile the real, unmodified `linux-kernel/.../rtlwifi/*.c`
   and `rtlwifi/rtl8188ee/*.c` files** against that extended compat
   layer — analogous to `DRIVER_SRCS`/`CHIP_SRCS` in the current
   Makefile, pointed at a new `LINUX_SRC`.
3. **Write a new, small `rtlwifi_compat.c`** — the rtlwifi-family
   equivalent of `rtw88_compat.c` — implementing the genuinely
   OS-specific bridge functions (IRQ registration glue, scan-state
   bridging, and whatever connect/reconnect bypass is actually needed
   — Section 40.4's conclusion that rtlwifi's own connect path has no
   LPS-poll hazard suggests this file may need **fewer** custom bypass
   functions than `rtw88_compat.c` has, not a 1:1 port of the same
   nine). Every rtw88-specific `(struct rtw_dev *)hw->priv` cast in
   the existing file becomes `(struct rtl_priv *)hw->priv` in the new
   one — consistent with the handover's original mechanical-conversion
   note, now confirmed as the literal, actual pattern in use, not a
   guess.
4. **Add a new IOKit driving layer** — either a new
   `RTL8188EEIEEE80211.cpp`/`RTL8188EEPCIDevice.cpp` pair, or a
   generalized version of the existing `RTW88IEEE80211.cpp`/
   `RTW88PCIDevice.cpp` — that drives the compiled-in rtlwifi core via
   its real entry points (`rtl_pci_probe()` etc.) instead of rtw88's.
5. **Embed `rtl8188efw.bin`** the same way `firmware/*.bin` is
   embedded for rtw88 (Section 45.3), bypassing
   `request_firmware_nowait` entirely rather than porting it.

## 45.5 What Sections 1–44 are still worth, under this correction

Not wasted — reframed:
- **Behavioral/hazard knowledge remains directly useful**: knowing that
  rtlwifi's connect path never calls the LPS-poll-hazard function
  (40.3–40.4), that IQK is driven by `hw_init`/watchdog and not
  channel switch (40.5), that RTL8188EE uses `use_new_trx_flow =
  false` (40.9.2), that `bar_id = 2` (40.11.1), that MSI is
  preferred-with-fallback (40.13), that btcoexist is dead code for
  this chip (40.14) — all of this tells us **what the compiled-in
  rtlwifi source will actually do at runtime**, which is exactly the
  knowledge needed to write a correct, minimal `rtlwifi_compat.c`
  bridge layer (step 3 above), even though the underlying C files
  themselves don't need hand-translation.
- **What changes is the framing of "why" this knowledge matters**: not
  "so we can faithfully reimplement this logic in IOKit C++," but "so
  we know which of rtlwifi's real, compiled-in behaviors are safe to
  leave alone and which need a bypass function in the new compat.c,"
  mirroring exactly what `rtw88_compat.c`'s
  `rtw88_connect_hw_setup`/`rtw88_sw_scan_*` do for rtw88 today.
- The firmware chain tracing (40.15–40.17) is reframed by 45.3 above:
  useful for knowing what state to populate directly, not useful as a
  guide to porting the load mechanism itself.

## 45.6 New open items surfaced by this correction (supersedes/extends
     Section 41)

1. **Compat-layer coverage diff**: does `src/compat/linux/*` (as
   written for rtw88) already cover rtlwifi's Linux API usage, or
   what's missing? Needs a systematic grep/diff of `#include`s and
   Linux API calls across `linux-kernel/.../rtlwifi/*.c` against
   what's implemented in `src/compat/linux/*.h` and
   `rtw88_compat.c`. Not yet started.
2. **`rtw88_compat.c`'s non-bridge content**: the function inventory
   (Section 45.1 step 3) shows this file also implements a large
   fraction of the **mac80211 API surface itself**
   (`ieee80211_rx_irqsafe`, `ieee80211_tx_status`,
   `ieee80211_iterate_active_interfaces`, `ieee80211_beacon_get_tim`,
   `ieee80211_nullfunc_get`, etc. — roughly lines 502–881 of the
   1274-line file) — this is generic mac80211-shim work, not
   rtw88-specific, and should be directly reusable/shared for
   rtlwifi rather than reimplemented. Needs confirmation that
   rtlwifi's mac80211 API usage doesn't require anything beyond what's
   already implemented here.
3. **`rtw88_is_scanning()`** (declared `rtw88_compat.h` line 184,
   defined `rtw88_compat.c` line 1049): a scan-state query function
   not among the nine bridge functions Sections 31–40 tracked. Its
   doc comment ("used by the kext to wait for post-scan MMIO cleanup
   before connecting") indicates a synchronization dependency between
   the IOKit layer and driver scan state that hasn't been analyzed for
   an rtlwifi equivalent need. Open.
4. **`rtw88_set_hw_callbacks` / `rtw88_get_hw`**: declared/defined in
   `rtw88_compat.c` (`rtw88_set_hw_callbacks` line 491,
   `rtw88_register_hw`/`rtw88_get_hw` lines 807/812) but **not
   declared in `rtw88_compat.h`** — meaning they're not part of the
   header's public bridge-function API surface the way the other seven
   are. Findings.md Sections 31–40 treated all nine as equivalent-tier
   bridge functions; this asymmetry (two of nine absent from the
   header) hasn't been explained and should be checked against how
   `RTW88IEEE80211.cpp`/`RTW88PCIDevice.cpp` actually call them.
5. **`RTW88PCIDevice.cpp`/`RTW88IEEE80211.cpp` not yet read.** These
   are the actual IOKit classes driving the compiled-in rtw88 core —
   the layer that would need the most direct rtlwifi-equivalent work
   per item 4 in Section 45.4. Not yet audited at all; sizes confirmed
   (978 and 3169 lines respectively) but no content read yet.
6. **Whether the same "compile real upstream source + thin compat
   bridge" strategy is even fully intended/correct for rtlwifi, or
   whether rtlwifi's older/different kernel-API usage patterns (e.g.
   different DMA/DMA-mapping idioms, DMA ring management differences
   already flagged in the original handover's Tier-1 risk list) make
   direct compilation harder than it was for rtw88.** This is a
   reasonable working hypothesis given the confirmed pattern, not yet
   a certainty for the rtlwifi case specifically.

------------------------------------------------------------------------

# 46. CONFIRMED: Full IOKit Driving Sequence, Read End-to-End
     (`RTW88PCIDevice::start` → `RTW88IEEE80211::create`/`init`/`start`)
     — Resolves Section 45.6 Items 4 and 5 (Partially)

## 46.1 Method

Read `RTW88PCIDevice::start()` (RTW88PCIDevice.cpp, lines 293-403) and
`RTW88IEEE80211::create()`/`init()`/`start()` (RTW88IEEE80211.cpp,
lines 534-651 and 733-827) in full, following the actual call sequence
from IOKit `start()` through to `hw->ops->start(hw)`.

## 46.2 Confirmed full driving sequence

```
RTW88PCIDevice::start(provider)
  - cast provider to IOPCIDevice, retain
  - setBusMasterEnable(true), setMemoryEnable(true)
  - _mmioMap = mapDeviceMemoryWithRegister(kIOPCIConfigBaseAddress2)
      [BAR index HARDCODED to 2 in this C++ layer itself — not read from
       any chip-info struct; see Section 46.3]
  - install globals: g_pci_dev_instance, rtw88_pci_io_ops, rtw88_dma_ops
  - rtw88_compat_init()  [workqueue/timer runtime bring-up]
  - hand-build _compatPciDev (struct pci_dev*, IOMallocZero'd):
      ->vendor/->device from IOPCIDevice configRead16(0x00/0x02)
      ->kext_dev = this (back-pointer)
      ->resource[2] = mmio base, ->resource_len[2] = mmio length
  - set up IOWorkLoop + IOCommandGate
  - setupInterrupt()  [IOInterruptEventSource bound to handleInterrupt]
  - rtw88_find_fw_dir()
  - _ieee80211 = RTW88IEEE80211::create(this, _compatPciDev)
      -> init(): alloc locks/workloop/timers, RX A-MPDU reorder timer,
         rtw88_set_hw_callbacks(&cbs, this)  [rx_frame/tx_status/scan_done
         — this IS where this function is actually called; it is NOT
         declared in rtw88_compat.h because callers are meant to be
         internal kext code, not general compat-layer API — resolves
         Section 45.6 item 4]
      [does NOT call rtw_pci_probe yet — that happens in start(), below]
  - rtw88_force_wifi_only()  [eFuse btcoex override]
  - _ieee80211->start()
      -> look up chip in LOCAL, FILE-SCOPED rtw88_pci_chip_table[]
         (RTW88IEEE80211.cpp) by PCI device ID — this table is
         Feixiao-authored, separate from any real upstream
         pci_driver.id_table (Section 46.4)
      -> hand-build a STACK-ALLOCATED struct pci_device_id (not from
         any real PCI subsystem dispatch — driver_data = chip ptr)
      -> rtw_pci_probe(_pcidev, &fake_id)   <-- REAL upstream Linux
         entry point, called as a direct, ordinary C function call —
         NOT through any pci_driver/module registration machinery
         (Section 46.4)
      -> _hw = rtw88_get_hw()  [retrieves the ieee80211_hw* that got
         registered as a side effect somewhere inside the compiled-in
         driver's ieee80211_alloc_hw() path — mechanism confirmed in
         shape, not yet traced to its exact call site; see 46.5]
      -> _rtwdev = (struct rtw_dev *)_hw->priv   [the exact hw->priv
         cast pattern already seen throughout rtw88_compat.c]
      -> MAC address read from _hw->wiphy->perm_addr (populated by
         SET_IEEE80211_PERM_ADDR() during rtw_register_hw(), inside
         rtw_pci_probe's real body)
      -> hand-build struct ieee80211_vif (IOMallocZero'd, +128 bytes
         tail padding), NL80211_IFTYPE_STATION, bss_conf.bssid pointed
         at a buffer (must never be NULL — dereferenced by iterators
         on every RX frame even pre-association)
      -> _hw->ops->add_interface(_hw, _vif)
      -> rtw88_register_vif(_vif)
      -> _hw->ops->start(_hw)    <-- the mac80211 ops-table ->start
         entry point. THIS is the call site that, for rtlwifi, would
         invoke rtl_op_start (core.c) -> rtl88ee_hw_init (Section
         40.17's traced fail-fast firmware-readiness check). Confirms
         the probe/hw-bring-up decoupling findings.md Section 40.15.1/
         40.17 predicted from rtlwifi source reading alone is exactly
         how the real, working macOS driving sequence is structured.
  - attachDevice()  [attachInterface, publishHardwareIdentity, medium
     dict, output queue]
  - rtw88_set_tx_resume_cb(rtw88_tx_resume_trampoline)
  - enable _intrSrc
  - debug timer (BE-ring/HISR/HIMR polling, 1s interval, diagnostic only)
  - registerService()
```

## 46.3 BAR index is hardcoded in the IOKit layer, not read from chip config

```cpp
/* Map BAR2 — rtw88 driver hardcodes bar_id=2 in pci.c */
_mmioMap = _pciDev->mapDeviceMemoryWithRegister(kIOPCIConfigBaseAddress2);
```

This mapping happens in `RTW88PCIDevice::start()`, **before** any
chip-specific code runs — the comment's framing ("rtw88 driver
hardcodes bar_id=2") is true of every chip currently in
`rtw88_pci_chip_table`, but this C++ layer does not read `bar_id` from
any `rtw_chip_info`/`rtl_hal_cfg` struct dynamically. It is a compile-
time constant (`kIOPCIConfigBaseAddress2`) in the IOKit code itself.

**For RTL8188EE this is a non-issue**: Section 40.11.1 independently
confirmed (from real rtlwifi source) `rtl88ee_hal_cfg.bar_id = 2` — the
same value. A port that reuses this hardcoded mapping will work
correctly for RTL8188EE specifically. This is flagged for accuracy,
not as a blocker: the existing code is chip-family-generic in practice
(all currently-supported rtw88 chips + RTL8188EE happen to share
`bar_id = 2`) but not chip-generic in design (no dynamic lookup exists).
A future multi-chip rtlwifi port (if ever needed) would need to make
this a real lookup; this single-chip RTL8188EE port does not.

## 46.4 `rtw_pci_probe` is called directly, bypassing all Linux PCI
     driver-registration machinery — major simplification

The kext does **not** emulate `pci_register_driver`/`pci_driver.probe`
dispatch, module loading, or any part of the Linux driver-registration
subsystem. It calls `rtw_pci_probe(pdev, id)` as an ordinary, direct C
function call, with:
- `pdev` = the hand-built `_compatPciDev` (populated in
  `RTW88PCIDevice::start`, Section 46.2)
- `id` = a **stack-allocated, synthetic** `struct pci_device_id`
  (`driver_data` field carrying the `rtw_chip_info*` pointer), built
  fresh in `RTW88IEEE80211::start()` — not looked up from any real
  `pci_driver.id_table`.

Chip selection (which `rtw_chip_info*` to pass) happens via a
**Feixiao-authored, file-local lookup table**
(`rtw88_pci_chip_table[]` in `RTW88IEEE80211.cpp`, distinct from and
parallel to the real per-chip `pci_device_id` tables that exist inside
the actual compiled-in `rtw88xxae.c` files per-chip, per the Makefile's
`CHIP_SRCS`) — i.e. **there are two separate PCI-ID tables in play**:
the real upstream one (compiled in, presumably unused for dispatch
purposes since IOKit's own matching already selected this driver
before any of this code runs) and this Feixiao-authored one that
exists purely to hand the right `rtw_chip_info*` to the manually-
constructed `pci_device_id`.

**Practical implication for RTL8188EE, updating Section 45.4 step 4**:
no PCI-ID table is needed at all (already anticipated in the original
handover and findings.md Section 40 — "RTL8188EE needs no PCI-ID
lookup table, single chip, single rtl_hal_cfg" — now confirmed as
categorically correct, since even Feixiao's multi-chip table is just a
` driver_data` pointer selector, not something the real PCI subsystem
touches). The rtlwifi equivalent of `RTW88IEEE80211::start()` can
skip the table-lookup step entirely and construct the synthetic
`pci_device_id` with `driver_data = (unsigned long)&rtl88ee_hal_cfg`
directly, unconditionally, then call
`rtl_pci_probe(_compatPciDev, &fake_id)`.

## 46.5 `rtw88_get_hw()`/`rtw88_register_hw()` mechanism — resolves
     Section 45.6 item 4 (partially)

`RTW88IEEE80211::start()`'s own comment explains the design intent
directly: `rtw88_get_hw()` is described as *"the external-linkage
accessor for the static g_rtw88_hw variable"*, deliberately used
instead of either (a) dereferencing `rtwdev` to recover `hw` (called
"fragile...double-dereference" in the source comment) or (b) an
`extern` declaration on a `static` variable across translation units
(called undefined behavior in the source comment).

This confirms: somewhere inside the real, compiled-in Linux driver
code path invoked by `rtw_pci_probe()` — almost certainly inside
`ieee80211_alloc_hw()`, which per the master compat header
(`rtw88_compat.h`) is one of the many `net/mac80211.h`-declared
functions the compat layer must implement — `rtw88_register_hw(hw)`
(compat.c line 807: `void rtw88_register_hw(struct ieee80211_hw *hw)
{ g_rtw88_hw = hw; }`) gets called as a side effect, storing the newly
allocated `ieee80211_hw*` in a static global for the IOKit layer to
retrieve afterward. **Not yet directly confirmed by reading
`ieee80211_alloc_hw`'s own implementation** (not yet located/read) —
inferred from the calling pattern and the function's existence/naming,
but not yet source-verified the way other findings in this document
are. Flagged as a small remaining open item, not a blocker: the
*mechanism* is clear even if the exact call site inside the shim's
`ieee80211_alloc_hw()` hasn't been read line-by-line.

## 46.6 Manual `ieee80211_vif` construction — a real, hand-written
     compat behavior worth carrying forward as-is

`RTW88IEEE80211::start()` does not rely on any mac80211
`ieee80211_add_virtual_intf`-style negotiation — it directly
`IOMallocZero`s a `struct ieee80211_vif` (deliberately over-allocated
by +128 bytes, presumably to cover a chip-family's private tail struct
that mac80211 normally allocates contiguously after the public `vif`
struct) and hand-populates `type`/`addr`/`bss_conf.bssid`, then calls
`hw->ops->add_interface()` directly. The source comment flags a real,
previously-hit bug class: `bss_conf.bssid` must point at a valid
buffer, never `NULL`, because RX-path iterators dereference it
unconditionally even pre-association. This is exactly the kind of
detail that would NOT surface from reading rtlwifi/rtw88 Linux source
alone (it's a macOS-port-specific workaround for the absence of
mac80211's own vif-lifecycle management) and must be replicated
carefully in any rtlwifi-driving equivalent, including getting the
tail-padding size right for `struct rtl_priv`'s analogous
per-vif private structure (if rtlwifi's `ieee80211_vif` usage has an
equivalent tail-allocation pattern — not yet confirmed, worth checking
when writing the rtlwifi-side equivalent).

## 46.7 Conclusion

The full IOKit-to-Linux-driver driving sequence for rtw88 is now
traced end-to-end, source-verified, from `IOPCIDevice` BAR mapping
through to the mac80211 `->start()` ops call. This is the direct,
concrete template for the equivalent rtlwifi-driving code:

- No Linux PCI driver-registration emulation needed (Section 46.4) —
  direct function calls only.
- No dynamic BAR lookup needed for a single-chip RTL8188EE target
  (Section 46.3) — the existing hardcoded BAR2 mapping is already
  correct for this chip.
- No PCI-ID table needed for a single-chip target (Section 46.4) —
  simpler than even Feixiao's own multi-chip table.
- The `hw->priv` cast pattern, MAC-address-from-perm_addr read, and
  manual `ieee80211_vif` construction (Section 46.6) are all direct,
  mechanical templates to replicate with `struct rtl_priv *` in place
  of `struct rtw_dev *`.
- `_hw->ops->start(_hw)` is confirmed as the real call site
  corresponding to `rtl_op_start`/`rtl88ee_hw_init` in the rtlwifi
  model built in Sections 1-44 — strong cross-validation between the
  two halves of this investigation (rtlwifi behavioral tracing, and
  Feixiao mechanism tracing) landing on a consistent picture.

**Updated remaining open items** (supersedes Section 45.6 items 4/5,
which are now resolved/superseded by this section):

1. Compat-layer Linux-API coverage diff (Section 45.6 item 1) — still
   open, unchanged.
2. `rtw88_compat.c`'s generic mac80211-stub portion (Section 45.6
   item 2) — still open, unchanged.
3. `rtw88_is_scanning()` (Section 45.6 item 3) — still open,
   unchanged.
4. Exact call site of `rtw88_register_hw()` inside the compat layer's
   `ieee80211_alloc_hw()` implementation — not yet read line-by-line
   (Section 46.5), minor remaining gap.
5. `rtw_pci_probe`'s own body (real upstream `rtw88-stable/.../pci.c`)
   — not yet read; would confirm exactly how `_compatPciDev`'s
   hand-populated fields (`resource[2]`, `resource_len[2]`, etc.) get
   consumed, and cross-check against findings.md Section 40.8's
   rtlwifi-side `rtl_pci_probe` trace for structural parity.
6. RX/TX data-path wiring — `compat_rx_frame`/`RTW88PCIDevice::
   outputPacket()` → `rtw_tx()` — not yet traced. This is the
   counterpart, on the mac80211/ethernet-interface side, to the
   rtlwifi TX-path tracing already done in Section 40.7 (which
   covered only the driver-internal `rtl_op_tx` → `intf_ops` → hw-kick
   chain, not how packets get into that chain from IOKit's
   `IOEthernetController` output-queue side).

------------------------------------------------------------------------

# 47. CONFIRMED: Compat-Layer Linux-API Coverage Diff (rtw88-built
     shim vs. real rtlwifi source's actual usage) — Resolves Section
     45.6 Item 1 / Section 46.7 Item 1

## 47.1 Method

1. Grepped all `#include` directives across every `.c` file in
   `linux-kernel/.../rtlwifi/*.c` and `rtlwifi/rtl8188ee/*.c`
   directly.
2. Found this undercounts real usage — many `linux/*.h` includes are
   pulled in transitively via rtlwifi's own internal headers
   (`wifi.h`, `base.h`, `pci.h`, `core.h`, `ps.h`, `efuse.h`), not
   included directly in the `.c` files. Re-ran the grep against those
   headers specifically to get the full picture.
3. Cross-referenced the combined include set against
   `src/compat/linux/*` and `src/compat/net/mac80211.h`'s actual
   file inventory.
4. For each gap found, traced the actual usage site in rtlwifi source
   to assess real risk (not just "header missing" — what specifically
   from that header is used, and how hard is it to shim).

## 47.2 Result: full include-set comparison

Confirmed already covered by the existing (rtw88-built) compat layer,
directly:
`linux/bitfield.h`, `linux/completion.h`, `linux/etherdevice.h`,
`linux/firmware.h`, `linux/interrupt.h`, `linux/pci.h`,
`linux/usb.h`, `linux/vmalloc.h`, `linux/module.h`,
`net/mac80211.h` (confirmed substantial — 252 references to
cfg80211/wiphy/ieee80211 symbols within the shim, not a stub).

Missing, assessed low-risk (macro-only headers, no real behavior to
emulate):
- `linux/export.h` — `EXPORT_SYMBOL`-family macros, normally no-ops
  in a monolithic (non-modular) kext build.
- `linux/moduleparam.h` — `module_param_named`-family macros, same
  category.

Missing, assessed low-risk after tracing actual usage:
- `linux/sched.h` — pulled in via a single generic
  `#include <linux/sched.h>` in `wifi.h` (rtlwifi's own master
  header, analogous to how rtw88 pulls in its own dependencies).
  Not yet traced to its exact symbol-level usage inside rtlwifi
  source (only the include site is confirmed, not which
  scheduling primitives — `task_struct`, `current`, etc. — are
  actually referenced). Given rtlwifi is architecturally similar in
  age/style to rtw88, and no equivalent gap was flagged for rtw88's
  own build (which presumably also needs *some* sched.h-adjacent
  primitives, likely already covered by another compat header or
  simply unneeded in a kext context), this is treated as low-risk
  but not fully closed — worth a quick symbol-level check before
  writing the port's compat header, not urgent enough to block
  other work.
- `linux/ip.h`, `linux/udp.h` — traced to a single call site:
  `rtl_is_special_data()` in `base.c` (a shared, non-chip-specific
  core file), lines ~1495-1512. Function classifies outgoing/
  incoming frames as DHCP (UDP ports 67/68 BOOTP client/server),
  ARP, or EAPOL (802.1X handshake) traffic, feeding into power-save
  special-casing and BT-coexist 4-way-handshake tracking
  (`setup_special_tx`, `in_4way`/`in_4way_ts`). Usage of
  `struct iphdr`/`struct udphdr` is **read-only, manual field/byte-
  offset access on raw skb data** (`ip->protocol`, `ip->ihl`,
  hand-indexed byte offsets into the UDP header) — no real Linux
  networking-stack machinery involved (no routing, no socket layer,
  no checksum offload, no `ip_hdr()`/`udp_hdr()` helper macros used,
  just direct pointer casts). A compat shim needs only two
  fixed-layout structs (RFC 791/768 header layouts) with correct
  field ordering — a data-layout task, not a behavioral-emulation
  task.

## 47.3 Conclusion

The Linux-API coverage gap between Feixiao's existing (rtw88-built)
compat layer and what real rtlwifi source actually requires is
**small and low-risk** — materially narrower than the original
handover's Tier-1 risk framing implied ("the largest unknown,"
genuine-translation-gap language written before this diff was done).
Concretely: two small header-only struct definitions
(`iphdr`/`udphdr`) isolated to one non-critical classification
function in one shared file, plus a small number of macro-only
headers (`export.h`, `moduleparam.h`) with no real behavior to
emulate. `linux/sched.h`'s exact symbol usage remains a minor,
non-blocking loose end (Section 47.2).

**This closes Section 45.6 item 1 / Section 46.7 item 1.** Combined
with Section 46's full IOKit driving-sequence trace, the two largest
open items identified after the Section 45 architecture correction are
now both resolved to "small, well-scoped, low-risk" outcomes rather
than open unknowns.

## 47.4 Updated remaining open items

1. `linux/sched.h` exact symbol-level usage — minor, not yet traced
   to specific call sites (Section 47.2).
2. `rtw88_compat.c`'s generic mac80211-stub portion — still needs
   confirming rtlwifi's mac80211 API usage doesn't exceed what's
   already implemented there (carried forward from Section 45.6
   item 2 / 46.7 item 2).
3. `rtw88_is_scanning()` — still open (carried forward from Section
   45.6 item 3 / 46.7 item 3).
4. `rtw_pci_probe`'s own body, and RX/TX data-path wiring
   (`outputPacket()`/`compat_rx_frame` → `rtw_tx()`) — still unread
   (carried forward from Section 46.7 items 5-6).
5. Runtime boot-test verification (LPS-hazard, RF-calibration
   timing, general Monterey stability) — unchanged, not answerable
   by source reading.

------------------------------------------------------------------------

# 48. CONFIRMED: `rtw_pci_probe`'s Full Body Read — Resolves Section
     46.7 Item 5, Cross-Validates the rtlwifi Private-Data Model,
     Surfaces Two New Rtlwifi-Side Loose Ends

## 48.1 Method

Read `rtw_pci_probe()` in full — real upstream
`rtw88-stable/drivers/net/wireless/realtek/rtw88/pci.c`, lines
1829-1921.

## 48.2 Confirmed sequence

```c
int rtw_pci_probe(struct pci_dev *pdev, const struct pci_device_id *id)
{
    drv_data_size = sizeof(struct rtw_dev) + sizeof(struct rtw_pci);
    hw = ieee80211_alloc_hw(drv_data_size, &rtw_ops);
    rtwdev = hw->priv;
    rtwdev->hw = hw;
    rtwdev->dev = &pdev->dev;
    rtwdev->chip = (struct rtw_chip_info *)id->driver_data;
    rtwdev->hci.ops = &rtw_pci_ops;
    rtwdev->hci.type = RTW_HCI_TYPE_PCIE;
    rtwpci = (struct rtw_pci *)rtwdev->priv;   /* tail-allocated */
    atomic_set(&rtwpci->link_usage, 1);

    rtw_core_init(rtwdev);
    rtw_pci_claim(rtwdev, pdev);
    rtw_pci_setup_resource(rtwdev, pdev);
    rtw_pci_napi_init(rtwdev);
    rtw_chip_info_setup(rtwdev);
        /* chip-specific ASPM-L1 quirk for 8821C on Intel bridge, here */
    rtw_pci_phy_cfg(rtwdev);
    rtw_register_hw(rtwdev, hw);        /* <-- registration happens HERE,
                                            not inside ieee80211_alloc_hw */
    rtw_pci_request_irq(rtwdev, pdev);  /* <-- IRQ setup happens LAST,
                                            AFTER hw registration */
    return 0;
    /* clean, ordered error-unwind chain on every failure path */
}
```

## 48.3 Resolves Section 46.5 — `rtw88_register_hw()` call site

Corrects Section 46.5's inference. `rtw88_register_hw(hw)` (which
sets the static `g_rtw88_hw` the IOKit layer later retrieves via
`rtw88_get_hw()`) is **not** called inside `ieee80211_alloc_hw()`.
It is called via `rtw_register_hw(rtwdev, hw)`, a separate, later step
in `rtw_pci_probe()` itself — almost certainly a compat-layer-provided
function (or an intercepted/wrapped version of upstream rtw88's own
`rtw_register_hw`) that performs the normal
`ieee80211_register_hw()`/`SET_IEEE80211_PERM_ADDR()` work AND, as an
add-on for the macOS port, calls `rtw88_register_hw(hw)` as a side
effect. **Not yet directly confirmed by reading `rtw_register_hw`'s
own implementation** — the call site and its position in the sequence
are now confirmed; the exact body is not yet read. Minor remaining
gap, mechanism otherwise clear.

## 48.4 Cross-validates the two-layer private-data model

```c
drv_data_size = sizeof(struct rtw_dev) + sizeof(struct rtw_pci);
hw = ieee80211_alloc_hw(drv_data_size, &rtw_ops);
rtwdev = hw->priv;
rtwpci = (struct rtw_pci *)rtwdev->priv;
```

This is the exact two-layer private-data pattern the original
handover documented for rtlwifi from source reading alone
(`rtl_priv` then tail-allocated `rtl_pci_priv`, both sized together in
one `ieee80211_alloc_hw` call — see the handover's "Confirmed
private-data model" section, and findings.md's earlier notes on
`rtl_pci_probe`, pci.c lines ~2040-2280). Independent confirmation
from the rtw88 side: both driver families use an identical structural
pattern (single combined allocation; `hw->priv` = primary struct;
a `->priv` tail field *inside* that primary struct holds the
bus-specific struct). This is strong cross-family validation, not
just single-source inference, and confirms the rtlwifi port's
allocation call is a direct, mechanical translation:
```c
drv_data_size = sizeof(struct rtl_priv) + sizeof(struct rtl_pci_priv);
hw = ieee80211_alloc_hw(drv_data_size, &rtl_ops /* rtlwifi's mac80211
                                                    ops struct name,
                                                    not yet confirmed */);
```

## 48.5 Two new loose ends surfaced on the RTLWIFI side (not the
     Feixiao side) — gaps in the earlier Sections 1-44 tracing

Reading rtw88's real `rtw_pci_probe()` surfaced two structural
questions about rtlwifi's own `rtl_pci_probe()` that the original
Sections 1-44 investigation did not explicitly check:

1. **Chip-specific behavioral quirks embedded inside the shared probe
   function itself.** `rtw_pci_probe()` contains a hardcoded
   `if (rtwdev->chip->id == RTW_CHIP_TYPE_8821C && ...)` branch
   (ASPM-L1 workaround, Intel-bridge-gated) directly inline, not
   dispatched through the chip's ops table. Section 40.9's
   `_rtl_pci_find_adapter` trace covered rtlwifi's `hw_type`
   ID-matching dispatch table, but did NOT specifically check whether
   `rtl_pci_probe()` itself (pci.c, the function `_rtl_pci_find_adapter`
   is called from) contains similar inline `hw_type ==`-gated
   behavioral branches beyond ID matching and `use_new_trx_flow`
   selection (which Section 40.9.2 did cover). **Open**: re-check
   `rtl_pci_probe`'s full body for any inline chip-conditional logic
   beyond what's already documented.
2. **IRQ registration ordering relative to hw registration.**
   Confirmed for rtw88: `rtw_register_hw()` (mac80211/cfg80211
   registration) happens BEFORE `rtw_pci_request_irq()` (IRQ setup).
   Section 40.13 traced `rtl_pci_intr_mode_msi`/`_rtl_pci_interrupt`'s
   internal mechanics in detail but did not explicitly document where
   IRQ setup falls in `rtl_pci_probe`'s overall sequence relative to
   any hw-registration-equivalent step. **Open**: check whether
   rtlwifi's `rtl_pci_probe` orders these the same way (register hw,
   then wire IRQs) or differently — relevant because if ordering
   differs, it could affect assumptions about when the device is
   "live" to mac80211 relative to when interrupts can fire, which
   matters for a race-condition-sensitive IOKit port.

## 48.6 Updated remaining open items

1. Two new rtlwifi-side loose ends from Section 48.5 (chip-quirk
   branches inside `rtl_pci_probe`; IRQ-vs-hw-registration ordering).
2. `rtw_register_hw`'s own body — not yet read (Section 48.3).
3. RX/TX data-path wiring (`outputPacket()`/`compat_rx_frame` →
   `rtw_tx()`) — still unread (carried forward).
4. `rtw88_is_scanning()` — still open (carried forward).
5. `linux/sched.h` exact symbol usage — still open, minor (carried
   forward).
6. `rtw88_compat.c`'s mac80211-stub coverage sufficiency for rtlwifi
   — still open (carried forward).
7. Runtime boot-test verification — unchanged, not answerable by
   source reading.

------------------------------------------------------------------------

# 49. CONFIRMED: `rtl_pci_probe()` Full Body Read — Resolves Both
     Section 48.5 Open Items

## 49.1 Method

Read `rtl_pci_probe()` in full — real source, direct terminal read via
`sed -n`, not summarized or paraphrased —
`linux-kernel/drivers/net/wireless/realtek/rtlwifi/pci.c`,
lines 2071-2258 (function body; `EXPORT_SYMBOL(rtl_pci_probe)` at 2258
confirms the closing boundary).

This directly answers the two open questions Section 48.5 surfaced
after reading rtw88's `rtw_pci_probe()`.

## 49.2 Confirmed sequence

```c
int rtl_pci_probe(struct pci_dev *pdev, const struct pci_device_id *id)
{
    pci_enable_device(pdev);
    /* DMA mask setup: 64-bit if cfg->mod_params->dma64, else 32-bit */

    pci_set_master(pdev);

    hw = ieee80211_alloc_hw(sizeof(struct rtl_pci_priv) +
                             sizeof(struct rtl_priv), &rtl_ops);

    rtlpriv = hw->priv;
    rtlpriv->hw = hw;
    pcipriv = (void *)rtlpriv->priv;        /* tail-allocated */
    pcipriv->dev.pdev = pdev;
    init_completion(&rtlpriv->firmware_loading_complete);

    rtlpriv->rtlhal.interface = INTF_PCI;
    rtlpriv->cfg = (struct rtl_hal_cfg *)(id->driver_data);
    rtlpriv->intf_ops = &rtl_pci_ops;
    rtl_efuse_ops_init(hw);

    pci_request_regions(pdev, KBUILD_MODNAME);
    /* BAR map via rtlpriv->cfg->bar_id */
    rtlpriv->io.pci_mem_start = (unsigned long)pci_iomap(...);

    /* Disable Clk Request; leave D3 mode -- generic, NOT chip-gated */
    pci_write_config_byte(pdev, 0x81, 0);
    pci_write_config_byte(pdev, 0x44, 0);
    pci_write_config_byte(pdev, 0x04, 0x06);
    pci_write_config_byte(pdev, 0x04, 0x07);

    _rtl_pci_find_adapter(pdev, hw);
    _rtl_pci_io_handler_init(&pdev->dev, hw);

    rtlpriv->cfg->ops->read_eeprom_info(hw);
    rtlpriv->cfg->ops->init_sw_vars(hw);
    rtl_init_sw_leds(hw);

    rtl_pci_init_aspm(hw);          /* <-- ASPM handling: a dispatched
                                          function call, NOT inline
                                          chip-conditional logic */

    rtl_init_core(hw);              /* Init mac80211 sw */
    rtl_pci_init(hw, pdev);         /* Init PCI sw */

    ieee80211_register_hw(hw);      /* <-- hw registration happens HERE */
    rtlpriv->mac80211.mac80211_registered = 1;

    rtl_debug_add_one(hw);
    rtl_init_rfkill(hw);

    rtlpci = rtl_pcidev(pcipriv);
    rtl_pci_intr_mode_decide(hw);   /* <-- IRQ setup happens AFTER
                                          hw registration */
    rtlpci->irq_alloc = 1;

    set_bit(RTL_STATUS_INTERFACE_START, &rtlpriv->status);
    return 0;

    /* clean, ordered error-unwind chain on every failure path;
       fail3: label calls wait_for_completion(&rtlpriv->
       firmware_loading_complete) before deinit_sw_vars -- teardown
       DOES block on firmware-load completion if init_core/pci_init
       fails after firmware load was kicked off */
}
```

## 49.3 Resolves Section 48.5 Item 1 — chip-conditional quirk branches

**Confirmed: none.** `rtl_pci_probe()` contains no inline
`hw_type ==`, `rtlhal->hw_type`, or `cfg->... == RTL8188EE`-style
conditional branches anywhere in its body. The only quirk-adjacent
lines are the four `pci_write_config_byte()` calls (Clk-Request
disable, D3-exit sequence) and the `rtl_pci_init_aspm(hw)` call — both
run **unconditionally for every rtlwifi chip**, not gated on chip ID.

ASPM handling in particular is delegated to a separate dispatched
function (`rtl_pci_init_aspm`), architecturally different from rtw88's
inline `if (rtwdev->chip->id == RTW_CHIP_TYPE_8821C && ...)` branch
found directly inside `rtw_pci_probe()`'s body (Section 48.5 item 1).
rtlwifi keeps probe-level PCI/DMA/D3/ASPM setup chip-agnostic and
pushes any real chip-specific behavior out through the `cfg->ops`
dispatch table (`read_eeprom_info`, `init_sw_vars`, etc.) instead.
**No hidden per-chip logic to account for when porting.**

## 49.4 Resolves Section 48.5 Item 2 — IRQ vs. hw-registration ordering

**Confirmed: same relative ordering as rtw88.**

```
ieee80211_register_hw(hw)         (line ~2213)
  -> rtl_debug_add_one(hw)
  -> rtl_init_rfkill(hw)
  -> rtl_pci_intr_mode_decide(hw) (line ~2233, IRQ setup)
```

`ieee80211_register_hw()` runs before `rtl_pci_intr_mode_decide()`.
This matches rtw88's `rtw_register_hw()` → `rtw_pci_request_irq()`
ordering documented in Section 48.2 — in both driver families, the
device becomes visible to mac80211/cfg80211 *before* interrupts are
wired up. rtlwifi interleaves `rtl_debug_add_one`/`rtl_init_rfkill`
between the two steps, which rtw88 does not, but the core ordering
(hw-register, then IRQ) is identical.

**Port implication:** no ordering asymmetry between the two driver
families to reconcile. The IOKit driving layer (Section 46) can follow
either family's probe sequence for this specific ordering question
without introducing a race-condition mismatch.

## 49.5 New finding, not previously tracked — firmware-load teardown blocking

The `fail3:` error-unwind label calls:

```c
fail3:
    wait_for_completion(&rtlpriv->firmware_loading_complete);
    rtlpriv->cfg->ops->deinit_sw_vars(hw);
```

This confirms, at an exact call site, Section 47's earlier
characterization of the completion object as "teardown-safety only."
If `rtl_init_core()` or `rtl_pci_init()` fails *after* async firmware
load was already kicked off (firmware load itself happens later,
inside `read_eeprom_info`/chip init paths dispatched earlier in probe,
per Section 40's firmware-hand-off tracing), probe teardown **does
block** on `wait_for_completion` before calling `deinit_sw_vars`. This
was previously inferred from call-site *locations* (both
`wait_for_completion` sites being in error-cleanup paths); this read
confirms `fail3:` is one of those exact sites, with its position in
the unwind chain now directly visible.

**Port implication:** the IOKit teardown/detach path must replicate
this block-on-firmware-completion behavior specifically in the
equivalent of the `fail3:` unwind branch (i.e., failure of the
core/PCI-init steps after firmware load has started), not just at
final detach.

## 49.6 Updated remaining open items

1. `rtw_register_hw`'s own body — not yet read (Section 48.3).
2. RX/TX data-path wiring (`outputPacket()`/`compat_rx_frame` →
   `rtw_tx()`) — still unread (carried forward).
3. `rtw88_is_scanning()` — still open (carried forward).
4. `linux/sched.h` exact symbol usage — still open, minor (carried
   forward).
5. `rtw88_compat.c`'s mac80211-stub coverage sufficiency for rtlwifi
   — still open (carried forward).
6. IOKit teardown path must account for the Section 49.5
   block-on-firmware-completion behavior in the `fail3:`-equivalent
   unwind branch — new, scoped implementation note rather than an
   open question.
7. Runtime boot-test verification — unchanged, not answerable by
   source reading.

------------------------------------------------------------------------

# 50. CONFIRMED: `rtw_register_hw()` Full Body Read, and CORRECTION of
     the `rtw88_register_hw()`/`g_rtw88_hw` Call-Site Claim from
     Sections 46/48

## 50.1 Method

Two direct source reads this session:
1. `rtw_register_hw()` — real upstream, unmodified —
   `rtw88-stable/drivers/net/wireless/realtek/rtw88/main.c`,
   lines 2259-2352 (`EXPORT_SYMBOL` at 2352 confirms boundary).
2. The actual call site of `rtw88_register_hw(hw)` — Feixiao's own
   compat layer — `Feixiao/src/compat/net/mac80211.h`, lines 990-1020
   (found via grep across the whole Feixiao tree, then read in
   context).

## 50.2 `rtw_register_hw()` — confirmed contents

Pure upstream rtw88 code, unmodified. No macOS-specific or
RTL8188EE-relevant logic. Performs, in order:

1. TX headroom sizing (`chip->tx_pkt_desc_sz`, +SDIO alignment if
   applicable — not relevant to a PCIe target).
2. `hw->queues`, `txq_data_size`, `sta_data_size`, `vif_data_size`
   struct-size wiring.
3. A block of `ieee80211_hw_set()` capability flags (SIGNAL_DBM,
   RX_INCLUDES_FCS, AMPDU_AGGREGATION, MFP_CAPABLE,
   REPORTS_TX_ACK_STATUS, SUPPORTS_PS, SUPPORTS_DYNAMIC_PS,
   SUPPORT_FAST_XMIT, conditionally SUPPORTS_AMSDU_IN_AMPDU,
   HAS_RATE_CONTROL, TX_AMSDU, SINGLE_SCAN_ON_ALL_BANDS).
4. `wiphy` interface-mode bitmask (STATION/AP/ADHOC), antenna counts,
   TDLS flags, scan-randomization feature bit, max SSID/IE-len for
   scanning.
5. **One inline chip-conditional branch**:
   `if (rtwdev->chip->id == RTW_CHIP_TYPE_8822C) { ... iface_combinations
   ... }` — sets `wiphy->iface_combinations` only for the 8822C. Not
   relevant to RTL8188EE/rtlwifi, but confirms rtw88 does have
   per-chip conditional branches scattered at multiple call sites (not
   just inside `rtw_pci_probe`, contra the narrower framing in Section
   48.5/49.3 which only checked the probe function itself).
6. Extended NL80211 feature bits, optional `CONFIG_PM`/wowlan wiring,
   `rtw_set_supported_band()`, `SET_IEEE80211_PERM_ADDR()`,
   `hw->wiphy->sar_capa`.
7. `rtw_regd_init()`, `rtw_led_init()`.
8. **`ieee80211_register_hw(hw)`** — the real mac80211 registration
   call.
9. `rtw_regd_hint()`, `rtw_debugfs_init()`, beamforming counter reset.
10. Clean single-label error unwind (`led_deinit:` →
    `rtw_led_deinit()`).

**Confirmed: no call to `rtw88_register_hw()` and no reference to
`g_rtw88_hw` anywhere in this function.** This directly contradicts
Section 48.3's inference that `rtw88_register_hw(hw)` fires "as a side
effect" of `rtw_register_hw()`.

## 50.3 CORRECTION: where `rtw88_register_hw()` is actually called

Grep across the whole Feixiao tree for `rtw88_register_hw|g_rtw88_hw`
found the real call site is in the **compat shim's own
`ieee80211_alloc_hw()` implementation** —
`src/compat/net/mac80211.h`, inside the function body, immediately
before `return hw;`:

```c
static inline struct ieee80211_hw *ieee80211_alloc_hw(size_t priv_data_len,
                                                        const struct ieee80211_ops *ops)
{
    ...
    struct ieee80211_hw *hw = (struct ieee80211_hw *)
        kzalloc(sizeof(*hw) + priv_data_len, GFP_KERNEL);
    if (!hw) return NULL;
    hw->priv = (u8 *)hw + sizeof(*hw);
    hw->wiphy = (struct wiphy *)kzalloc(sizeof(struct wiphy), GFP_KERNEL);
    if (!hw->wiphy) { kfree(hw); return NULL; }
    hw->wiphy->_dev = hw->priv;
    hw->ops = ops;
    hw->conf.chandef.chan   = &s_default_chan;
    hw->conf.chandef.width  = NL80211_CHAN_WIDTH_20_NOHT;
    rtw88_register_hw(hw);     /* belt: global fallback */
    return hw;
}
```

**This is a different function entirely from what Sections 46/48
assumed.** `g_rtw88_hw` is populated inside the compat layer's
`ieee80211_alloc_hw()` — the very first Linux-API call in
`rtw_pci_probe()` (Section 48.2) — not inside `rtw_register_hw()`.

## 50.4 Corrected timeline

```
rtw_pci_probe()
  hw = ieee80211_alloc_hw(...)   <-- g_rtw88_hw = hw happens HERE,
                                      via the compat shim, before
                                      anything else in probe runs
  rtwdev = hw->priv
  ...
  rtw_core_init(rtwdev)
  rtw_pci_claim / setup_resource / napi_init / chip_info_setup / phy_cfg
  rtw_register_hw(rtwdev, hw)    <-- real mac80211 registration;
                                      does NOT touch g_rtw88_hw
  rtw_pci_request_irq(rtwdev, pdev)
```

`g_rtw88_hw` is therefore live from the earliest possible point in
probe — before `rtw_core_init`, before any chip setup, before
`ieee80211_register_hw()` proper, before IRQs. This is consistent with
the `/* belt: global fallback */` comment: it's not a registration
side effect, it's an unconditional, as-early-as-possible population so
that `rtw88_get_hw()` is safe to call even from early probe
failure/error-unwind paths, not just after full registration succeeds.

The header comment at `mac80211.h:974-975` (seen in the grep output)
references a `rtw88_get_hw()` "external-linkage accessor for the
static `g_rtw88_hw` pointer" and explicitly warns against declaring
`extern struct ieee80211_hw *g_rtw88_hw` directly ("would be UB") —
consistent with `rtw88_compat.c` defining `g_rtw88_hw` as `static` in
two places (lines 372 and 805 per grep; likely one is a stale/dead
duplicate — **not yet checked, new minor loose end**).

## 50.5 "Belt: global fallback" — implies a second (suspenders) lookup path

The comment wording implies `rtw88_get_hw()` is a fallback for a
primary lookup mechanism keyed on something more specific than a
single global (most likely `wiphy`, given `rtw88_compat.c:823`'s
`if (!wiphy) return g_rtw88_hw;` fallback pattern seen in the earlier
grep). **Not yet read directly** — the wiphy-keyed primary lookup
function's own body is an open item (see 50.7).

## 50.6 Corrects Sections 46.5 / 48.3

Section 46.5's original inference ("`rtw88_register_hw(hw)`... called
inside `ieee80211_alloc_hw()`") was actually **closer to correct** than
Section 48.3's revision, which moved the call to `rtw_register_hw()`
based on the call-site name alone without reading either function
body. Section 48.3's correction is now itself corrected: the call is
inside the compat shim's `ieee80211_alloc_hw()`, matching Section
46.5's original guess in substance (right function), though 46.5 did
not have this session's full source confirmation.

## 50.7 Updated remaining open items

1. `rtw88_compat.c` defines `g_rtw88_hw` as `static` at both line 372
   and line 805 (per Section 50.3's grep) — likely one is dead/stale
   code, not yet checked. New, minor.
2. The wiphy-keyed primary lookup function implied by "belt: global
   fallback" (Section 50.5) — not yet read.
3. RX/TX data-path wiring (`outputPacket()`/`compat_rx_frame` →
   `rtw_tx()`) — still unread (carried forward).
4. `rtw88_is_scanning()` — still open (carried forward).
5. `linux/sched.h` exact symbol usage — still open, minor (carried
   forward).
6. `rtw88_compat.c`'s mac80211-stub coverage sufficiency for rtlwifi
   — still open (carried forward).
7. IOKit teardown path must account for the Section 49.5
   block-on-firmware-completion behavior — implementation note,
   carried forward.
8. Runtime boot-test verification — unchanged, not answerable by
   source reading.

------------------------------------------------------------------------

# 51. CONFIRMED: Both Section 50.7 Loose Ends Resolved —
     `g_rtw88_hw` "Duplicate" Explained; `wiphy_to_ieee80211_hw()`
     Full Body Read

## 51.1 Method

Two direct source reads, `Feixiao/src/compat/rtw88_compat.c`:
1. Lines 360-375, to check whether the two `g_rtw88_hw` declarations
   (Section 50.3's grep: lines 372 and 805) are a real duplicate-symbol
   issue or something benign.
2. Lines 813-828, the full body of `wiphy_to_ieee80211_hw()`, the
   function containing line 823's `if (!wiphy) return g_rtw88_hw;`
   fallback identified in Section 50.5.

## 51.2 Resolves Section 50.7 item 1 — `g_rtw88_hw` "duplicate"

**Not a bug, not dead code.** Line 372 is a file-scope C tentative
definition, explicitly commented as intentional:

```c
/* Forward declaration — defined later in ieee80211_alloc_hw section */
static struct ieee80211_hw *g_rtw88_hw;
```

Line 805 is the actual defining declaration with initializer:

```c
static struct ieee80211_hw *g_rtw88_hw = NULL;
```

Standard C allows a `static` tentative definition (no initializer)
followed later in the same translation unit by a full definition; both
refer to the same single variable. This lets earlier code in the file
(e.g. the callback-registration logic around line 498, which reads
`g_rtw88_hw` before its "real" definition appears later) reference the
variable without a header-level `extern`. **Confirmed intentional,
correct C, no action needed.**

## 51.3 Resolves Section 50.7 item 2 — the "belt/suspenders" primary
     lookup function

`wiphy_to_ieee80211_hw()`, full body:

```c
struct ieee80211_hw *wiphy_to_ieee80211_hw(struct wiphy *wiphy)
{
    /*
     * wiphy->_dev (offset 0 in struct wiphy) holds rtwdev.
     * struct ieee80211_hw::priv is also at offset 0.
     * Casting wiphy to ieee80211_hw* lets callers do hw->priv
     * and get rtwdev directly from wiphy memory — always valid
     * as long as wiphy itself is alive (which it is during probe).
     */
    if (!wiphy) return g_rtw88_hw;  /* fallback: global */
    return (struct ieee80211_hw *)wiphy;
}
```

**This is not a real lookup/search — it's a pointer-reinterpretation
trick relying on struct-layout coincidence.** The compat layer's
`ieee80211_alloc_hw()` (Section 50.3) sets `hw->wiphy->_dev =
hw->priv;`, placing `rtwdev` at offset 0 of `struct wiphy`. Since
`ieee80211_hw::priv` is also at offset 0 of `struct ieee80211_hw`,
reinterpreting a `wiphy*` as an `ieee80211_hw*` and reading `->priv`
off it lands on the same memory as the real `rtwdev` pointer, purely
by construction of both structs' layouts — not via any actual mapping
table or lookup.

"Belt: global fallback" (Section 50.3's comment on the
`ieee80211_alloc_hw()` call site) now fully explained: `wiphy_to_
ieee80211_hw()` is the "suspenders" (primary, used whenever a valid
`wiphy*` is on hand), `g_rtw88_hw`/`rtw88_get_hw()` is the "belt"
(fallback, used when no `wiphy*` is available — e.g., very early in
probe or from contexts with no natural wiphy access).

## 51.4 Port implication — fragility note, not a blocker

This offset-0 struct-layout-coincidence pattern is fragile in general
(breaks silently if either struct's field layout changes, or if
`wiphy->_dev` assignment is ever moved/removed) but is internally
consistent with every other confirmed mechanism this session, and low
risk in practice for this specific port: single target chip, single
`wiphy`, single `ieee80211_hw` instance, no multi-adapter case to
confuse the global fallback. The rtlwifi-side `rtlwifi_compat.c`
equivalent should replicate the same offset-0 trick (or document
choosing not to) rather than needing a different mechanism — no new
rtlwifi-specific design question raised here.

## 51.5 Updated remaining open items

------------------------------------------------------------------------

# 52. CONFIRMED: Full RX/TX Data-Path Wiring Read End to End — Resolves
     the Long-Standing "RX/TX data-path wiring... still unread" Item
     (Sections 46.7, 48.6, 50.7, 51.5)

## 52.1 Method

Read the complete TX and RX call chains directly from source, in
Feixiao's IOKit layer and compat shim:
- `Feixiao/src/kext/RTW88PCIDevice.cpp` (`outputPacket` entry, ~line
  597).
- `Feixiao/src/kext/RTW88IEEE80211.cpp` — `outputPacket()` (~912),
  `txDataFrame()` (2856-2976), `rxFrame()` (~934), `processRxMgmt()`
  (~958), `processRxData()` (1260-1289), `deliverDataFrame()`
  (1291-1349), `deliverEthernet()` (1354+), `rtw88_make_packet_mbuf()`
  (2999+), `compat_rx_frame()`/`compat_tx_status()`/`compat_scan_done()`
  (511-527).
- `Feixiao/src/compat/rtw88_compat.c` — `rtw88_set_hw_callbacks()`,
  `ieee80211_rx_irqsafe()` (490-510).

## 52.2 Confirmed TX chain

```
RTW88PCIDevice::outputPacket(mbuf_t m, void *param)   [PCIDevice.cpp:597]
  -> _ieee80211->outputPacket(m)                      [PCIDevice.cpp:618]

RTW88IEEE80211::outputPacket(mbuf_t m)                [IEEE80211.cpp:912]
  - gates on connection state (_state == CONNECTED, or SCANNING with
    a saved CONNECTED return-state and on-home-channel)
  - drops (mbuf_freem + kIOReturnOutputDropped) if not connected or
    missing _rtwdev/_hw/_vif/_sta
  -> txDataFrame(m)                                    [IEEE80211.cpp:2856]

txDataFrame(mbuf_t m)                                  [IEEE80211.cpp:2856-2976]
  - manually builds a full 802.11 data frame from the raw Ethernet
    mbuf: 24-byte (or 26-byte QoS) 802.11 header, optional 8-byte CCMP
    header (encryption in progress, PN increment), 8-byte RFC1042
    LLC/SNAP, then the IP payload copied via mbuf_copydata
  - sets skb_set_queue_mapping(skb, IEEE80211_AC_BE) and
    ieee80211_tx_info flags (FIRST_FRAGMENT, AMPDU when a BA agreement
    is active, band, vif/sta, hw_key)
  -> _hw->ops->tx(_hw, &ctrl, skb)                     [IEEE80211.cpp:2972]
```

`_hw->ops->tx` is a **direct function-pointer call into real,
unmodified rtw88's own `tx` mac80211 op** (i.e. what upstream registers
as `rtw_ops_tx`, which itself calls `rtw_tx()` — consistent with the
`rtw_tx` forward declaration seen at IEEE80211.cpp:28 and the
comment reference at line 2961). **mac80211's own TX queueing/txq
machinery is bypassed entirely** — the IOKit layer hand-builds the
frame and hands it straight to the driver's tx entry point, matching
the same "bypass mac80211 ops, call driver entry points directly"
pattern already confirmed for connect/reconnect in Section 40.

## 52.3 Confirmed RX chain

```
[real, unmodified rtw88 driver RX code, deep in mac.c/rx.c/pci.c,
 not read this session — calls the Linux/mac80211 API function
 ieee80211_rx_irqsafe(), which the compat layer intercepts]

ieee80211_rx_irqsafe(hw, skb)                    [rtw88_compat.c:501-510]
  - ctx = hw->kext_hw, falling back to g_kext_hw if unset
  -> g_hw_cbs->rx_frame(ctx, skb)                [rtw88_compat.c:508]

RTW88IEEE80211::compat_rx_frame(kext_hw, skb)    [IEEE80211.cpp:511]
  -> self->rxFrame(skb)

RTW88IEEE80211::rxFrame(skb)                     [IEEE80211.cpp:~934]
  - reads rx_status (signal/RSSI) via IEEE80211_SKB_RXCB
  - dispatches by frame type: mgmt -> processRxMgmt(), data ->
    processRxData(), else kfree_skb

processRxData(skb)                               [IEEE80211.cpp:1260]
  - gates on connection state (CONNECTED / HANDSHAKING / SCANNING
    with a CONNECTED return-state)
  - for QoS data frames with an active downlink BlockAck agreement on
    that TID: routes into a per-TID reorder buffer
    (rxReorderInput(tid, skb, sn), takes ownership) instead of
    delivering immediately -- explicitly to keep A-MPDU subframes and
    late retransmissions in order (comment: out-of-order delivery
    "collapses TCP and trips CCMP replay drops")
  - otherwise -> deliverDataFrame(skb) immediately

deliverDataFrame(skb)                            [IEEE80211.cpp:1291]
  - strips the 802.11 header (+ CCMP IV if present -- rtw88 leaves the
    IV in the frame; mac80211 would normally have stripped it, so this
    compensates for bypassing mac80211's own RX path)
  - de-aggregates A-MSDU subframes via deAmsdu() if the QoS A-MSDU bit
    is set
  - for a single MSDU: locates the RFC1042 LLC/SNAP header (with a
    fallback heuristic for an extra 8 bytes of undecrypted CCMP header
    still present), extracts ethertype, special-cases EAPOL during
    handshake (handleEAPOL(), frame consumed, returns before general
    delivery)
  -> deliverEthernet(addr1, addr3, ethertype, payload, paylen)
       [IEEE80211.cpp:1354]

deliverEthernet(da, sa, ethertype, payload, paylen)
  - allocates via _parent->allocateInputPacket(14 + paylen) --
    NOT rtw88_make_packet_mbuf()/mbuf_allocpacket -- comment: this
    guarantees consistent m_len/pkthdr.len across every mbuf segment,
    "which the dlil input validator requires"; mbuf_allocpacket alone
    does not guarantee this
  - builds the final 14-byte Ethernet header, copies in payload
  - hands off to _parent (RTW88PCIDevice, the IONetworkController)
    for delivery into the macOS networking stack
```

`rtw88_make_packet_mbuf()` (IEEE80211.cpp:2999, using
`mbuf_allocpacket`/`mbuf_copyback`) is used **only on the TX-adjacent
side** (called from a different site at line 2596, not the RX
deliver path) — RX delivery to the network stack deliberately uses the
separate `allocateInputPacket()` route instead, per the dlil-validator
comment above. This is a meaningful implementation distinction: two
different mbuf-construction helpers exist in this file for a reason,
not interchangeably.

## 52.4 Port implications for `rtlwifi_compat.c`

1. rtlwifi's own TX path (`rtl_op_tx`, per the handover's "Full TX
   path" summary: `rtl_op_tx` → `intf_ops->adapter_tx` →
   `cfg->ops->fill_tx_desc`/`tx_polling`) will need the equivalent
   hand-built-frame-then-direct-dispatch treatment: the rtlwifi
   `rtlwifi_compat.c` IOKit driving layer should call into
   `rtl_op_tx`/`adapter_tx` directly, the same way this port calls
   `_hw->ops->tx` directly, bypassing rtlwifi's own mac80211 tx
   plumbing analogous to how rtw88's is bypassed here.
2. RX needs an rtlwifi-side equivalent of `ieee80211_rx_irqsafe()`
   interception in the compat layer — rtlwifi's RX path (traced
   separately in prior sections) will call some Linux/mac80211 RX
   delivery function that the extended compat header must intercept
   the same way `rtw88_compat.c` intercepts `ieee80211_rx_irqsafe`.
3. The CCMP-IV-left-in-frame and A-MSDU-de-aggregation handling here
   are rtw88/mac80211-bypass-specific compensations. Whether rtlwifi's
   hardware leaves the CCMP IV in decrypted frames the same way is
   **not yet verified** — new open item, since this behavior is
   chip/driver-specific, not something to assume carries over.
4. The two-different-mbuf-helper distinction (`rtw88_make_packet_mbuf`
   for one path, `allocateInputPacket` for RX-to-networking-stack
   delivery) should be preserved in the rtlwifi/RTL8188EE port for the
   same dlil-validator-correctness reason, not collapsed into one
   helper for simplicity.

## 52.5 Updated remaining open items

1. Whether RTL8188EE/rtlwifi's decrypted RX frames leave the CCMP IV
   in place the same way rtw88's do (Section 52.4 item 3) — new,
   needs checking against rtlwifi's own RX/decrypt path once read.
2. `rtw88_is_scanning()` — still open (carried forward).
3. `linux/sched.h` exact symbol usage — still open, minor (carried
   forward).
4. `rtw88_compat.c`'s mac80211-stub coverage sufficiency for rtlwifi
   — still open (carried forward).
5. IOKit teardown path must account for the Section 49.5
   block-on-firmware-completion behavior — implementation note,
   carried forward.
6. `rtlwifi_compat.c` should replicate the offset-0
   `wiphy->_dev`/`hw->priv` struct-layout trick (Section 51.3-51.4)
   for its own hw-lookup mechanism — implementation note, carried
   forward.
7. `rtlwifi_compat.c`'s TX/RX wiring should mirror the direct-dispatch
   pattern confirmed in Section 52.2-52.4 (bypass rtlwifi's own
   mac80211 tx plumbing the same way rtw88's is bypassed here;
   intercept whatever RX delivery function rtlwifi calls, analogous to
   `ieee80211_rx_irqsafe`) — implementation note/design guidance,
   carried forward as scoped guidance for writing that file.
8. Runtime boot-test verification — unchanged, not answerable by
   source reading.

------------------------------------------------------------------------

# 53. CONFIRMED: `rtw88_is_scanning()` Full Body and Call Site Read —
     Resolves the Long-Standing Open Item (Sections 45.6, 46.7, 48.6,
     50.7, 52.5)

## 53.1 Method

Read both directly, `Feixiao/src/compat/rtw88_compat.c:1049-1054`
(function body) and `Feixiao/src/kext/RTW88IEEE80211.cpp:1995-2009`
(the one call site, inside `doAuthenticate()`).

## 53.2 Confirmed function body

```c
bool rtw88_is_scanning(void)
{
    if (!g_rtw88_hw || !g_rtw88_hw->priv) return false;
    struct rtw_dev *rtwdev = (struct rtw_dev *)g_rtw88_hw->priv;
    return test_bit(RTW_FLAG_SCANNING, rtwdev->flags);
}
```

A plain accessor: null-checks `g_rtw88_hw` (Section 50/51's global),
casts `->priv` to `struct rtw_dev *`, reads the real rtw88 driver's
own `RTW_FLAG_SCANNING` bit via `test_bit()`. No polling loop, no
MMIO access — a single in-memory bitflag read.

## 53.3 Confirmed call site — `doAuthenticate()`

```c
/* Wait for RTW_FLAG_SCANNING to clear.
 * The flag is cleared inside rtw_core_scan_complete() which runs under
 * rtwdev->mutex in the c2h_work thread. */
for (int i = 0; i < 100; i++) {
    if (!rtw88_is_scanning()) break;
    IOSleep(50);
}
```

A bounded sleep-loop: up to 100 iterations × 50ms `IOSleep` = 5 second
ceiling, sleeping (not busy-polling) between checks. Per the source
comment, this exists so that authentication doesn't proceed while a
scan is still finishing — `RTW_FLAG_SCANNING` clears inside
`rtw_core_scan_complete()`, which runs under `rtwdev->mutex` in the
c2h_work thread, so this wait avoids a mutex-contention/ordering race
between scan completion and the start of authentication.

## 53.4 Hazard classification — NOT the Section 40 MMIO-polling pattern

This is a **software-flag wait with a bounded sleep-loop**, not a
hardware-register poll. It does not resemble rtw88's LPS-leave hazard
(Section 40.2: unbounded `rtw_read32_mask` MMIO polling on `REG_TCR`)
at all — it reads driver-internal memory state (`rtwdev->flags`), not
hardware registers, and is explicitly bounded (5s ceiling) with actual
sleeps rather than a spin loop. Same general category as rtlwifi's own
already-confirmed-safe bounded waits (Section 40.3: fixed `mdelay()` in
channel switch). **No hazard here; nothing to bypass or rewrite.**

## 53.5 Port implication for rtlwifi/RTL8188EE

rtlwifi's own scan-in-progress state is tracked separately from rtw88's
`RTW_FLAG_SCANNING` (different struct entirely — `struct rtl_priv`, not
`struct rtw_dev`) and has not been located/read in this investigation.
A `rtlwifi_compat.c` equivalent of `rtw88_is_scanning()` — reading
whatever flag rtlwifi's core.c/mac80211 layer uses to track an
in-progress scan (candidate: something under `rtlpriv->mac80211`,
per the field layout confirmed in Sections 31-39, though the specific
member is unconfirmed) — would need the same treatment: a small
accessor, called from the equivalent authentication/connect path with
the same bounded-sleep-loop pattern. This is a small, low-risk,
well-understood porting task now that the rtw88-side shape is fully
confirmed — not a new architectural question.

## 53.6 Updated remaining open items

1. Whether RTL8188EE/rtlwifi's decrypted RX frames leave the CCMP IV
   in place the same way rtw88's do (Section 52.4) — still open, the
   sole remaining genuine unknown from source reading.
2. The specific rtlwifi/`rtl_priv`-side scan-in-progress flag/field
   (Section 53.5) — not yet located; needed to write the
   `rtlwifi_compat.c` equivalent of `rtw88_is_scanning()`. New, minor,
   well-scoped.
3. `linux/sched.h` exact symbol usage — still open, minor (carried
   forward).
4. `rtw88_compat.c`'s mac80211-stub coverage sufficiency for rtlwifi
   — still open (carried forward).
5. IOKit teardown path must account for the Section 49.5
   block-on-firmware-completion behavior — implementation note,
   carried forward.
6. `rtlwifi_compat.c` should replicate the offset-0
   `wiphy->_dev`/`hw->priv` struct-layout trick (Section 51.3-51.4)
   for its own hw-lookup mechanism — implementation note, carried
   forward.
7. `rtlwifi_compat.c`'s TX/RX wiring should mirror the direct-dispatch
   pattern confirmed in Section 52.2-52.4 — implementation
   note/design guidance, carried forward.
8. Runtime boot-test verification — unchanged, not answerable by
   source reading.

------------------------------------------------------------------------

# 54. CONFIRMED: rtlwifi/RTL8188EE's RX Decryption-Status Handling
     Read — Original CCMP-IV Question REFRAMED, Not Cleanly Resolved

## 54.1 Method

Grepped rtlwifi's chip-specific source tree for decryption-status
handling (`swdec`, `hwdec`, `RX_FLAG_DECRYPTED`, `iv_len`) —
found only in `rtl8188ee/trx.c` among the chips relevant to this
investigation (also present in several other chip families' trx.c
files, not read). Read the RX-status-parsing function directly,
`rtl8188ee/trx.c` lines 380-450 (the block containing
`status->decrypted = !get_rx_desc_swdec(pdesc);` at line 390 and the
`RX_FLAG_DECRYPTED` handling at lines 440-450).

## 54.2 Confirmed behavior

```c
status->decrypted = !get_rx_desc_swdec(pdesc);
...
/* hw will set status->decrypted true, if it finds the
 * frame is open data frame or mgmt frame.
 * So hw will not decryption robust managment frame
 * for IEEE80211w but still set status->decrypted
 * true, so here we should set it back to undecrypted
 * for IEEE80211w frame, and mac80211 sw will help
 * to decrypt it
 */
if (status->decrypted) {
    if ((!_ieee80211_is_robust_mgmt_frame(hdr)) &&
        (ieee80211_has_protected(hdr->frame_control)))
        rx_status->flag |= RX_FLAG_DECRYPTED;
    else
        rx_status->flag &= ~RX_FLAG_DECRYPTED;
}
```

`status->decrypted` comes from a hardware descriptor bit
(`get_rx_desc_swdec`, inverted — "swdec" = software-decryption-needed,
so `!swdec` means hardware already decrypted it). `RX_FLAG_DECRYPTED`
is then set on `rx_status` (a standard mac80211
`ieee80211_rx_status` flag) — **except** for robust management frames
(802.11w/PMF), where the driver deliberately forces the flag back off
even though hardware reported the frame as decrypted, specifically so
mac80211's own software decryption path handles those frames instead
of trusting hardware's decrypt-status bit for that frame class.

## 54.3 Why this reframes rather than answers the original question

Section 52.4 item 3 asked whether RTL8188EE's hardware "leaves the
CCMP IV in place the same way rtw88's does" — a question framed around
rtw88's specific compensating behavior (Section 52.3: rtw88's port
manually strips the header and, per an explicit source comment, "rtw88
leaves the CCMP IV in the frame; mac80211 would normally strip it,"
requiring `deliverDataFrame()` to account for it directly).

**rtlwifi's mechanism is structurally different.** `RX_FLAG_DECRYPTED`
is a real, standard mac80211-facing signal meant to be consumed by
mac80211's own RX processing pipeline (which strips/handles the IV
based on this flag, among other things) — not something rtlwifi's own
driver code inspects to decide whether to manually skip IV bytes
itself. This function does not directly say whether the IV bytes are
physically still present in the skb after hardware decryption; it only
sets a flag that mac80211 would normally interpret.

Since this RTL8188EE port is expected to bypass mac80211's RX
processing the same way the rtw88 port does (Section 45's core
architecture conclusion), the port's own RX handler will need to:
1. Read `RX_FLAG_DECRYPTED` from the status this function sets (a
   concrete, confirmed data point).
2. Independently determine whether the IV bytes are still physically
   present in the skb after hardware decryption on RTL8188EE hardware
   — **not answered by this reading.** The robust-mgmt-frame carve-out
   confirms hardware decryption status is trustworthy for the flag's
   *intended* mac80211 consumer, but says nothing about IV-byte
   presence/absence in the buffer itself.

## 54.4 New, narrower open item

Whether RTL8188EE hardware physically strips the CCMP IV during
hardware decryption (matching what "hardware decrypted" typically
implies) or leaves it in the skb (matching rtw88's documented
behavior) is **not resolved by this reading** and would require
either: (a) reading the actual skb-manipulation/header-adjustment code
around this RX-status function (not yet located — likely
elsewhere in `trx.c` or in a shared rtlwifi RX path in `base.c`/
`core.c`), or (b) empirical/runtime confirmation once something is
bootable. This is a narrower, more precisely scoped version of the
original Section 52.4 item 3 question, not a clean resolution of it.

## 54.5 Updated remaining open items

1. Whether RTL8188EE hardware physically leaves the CCMP IV bytes in
   the skb after hardware decryption (narrowed from Section 52.4 item
   3, per Section 54.3-54.4) — still open, now more precisely scoped;
   the RX_FLAG_DECRYPTED mechanism itself is fully understood
   (Section 54.2), but IV-byte physical presence is a separate,
   unanswered question.
2. The specific rtlwifi/`rtl_priv`-side scan-in-progress flag/field
   needed to write the `rtlwifi_compat.c` equivalent of
   `rtw88_is_scanning()` (Section 53.5) — not yet located.
3. `linux/sched.h` exact symbol usage — still open, minor (carried
   forward).
4. `rtw88_compat.c`'s mac80211-stub coverage sufficiency for rtlwifi
   — still open (carried forward).
5. IOKit teardown path must account for the Section 49.5
   block-on-firmware-completion behavior — implementation note,
   carried forward.
6. `rtlwifi_compat.c` should replicate the offset-0
   `wiphy->_dev`/`hw->priv` struct-layout trick (Section 51.3-51.4)
   for its own hw-lookup mechanism — implementation note, carried
   forward.
7. `rtlwifi_compat.c`'s TX/RX wiring should mirror the direct-dispatch
   pattern confirmed in Section 52.2-52.4 — implementation
   note/design guidance, carried forward.
8. `rtlwifi_compat.c`'s RX handling must independently determine and
   handle IV-byte presence/absence per item 1 above, since it cannot
   rely on mac80211's own IV-stripping behavior (which normally
   consumes `RX_FLAG_DECRYPTED`) the same way rtlwifi's real driver
   code implicitly does — new implementation note arising from
   Section 54.3's architecture mismatch.
9. Runtime boot-test verification — unchanged, not answerable by
   source reading.

------------------------------------------------------------------------

# 55. CONFIRMED: `rtlwifi_compat.c`/`rtlwifi_compat.h` Skeleton Written,
     and Four Sections 53-54 Open Items Resolved Against the Real
     rtlwifi Source Tree (Not Feixiao Alone)

## 55.1 Context — a second source tree became available this session

All prior sections (1-54) were produced from Feixiao alone plus
reasoning/inference about rtlwifi's likely shape. This session, the
actual `linux-kernel/` sparse checkout referenced by the handover doc
(`linux-kernel/ — sparse Linux kernel checkout containing rtlwifi`,
handover line 74) was confirmed present on disk, sibling to `Feixiao/`,
under project root `rtl8188ee-port/`:

```
rtl8188ee-port/
├── Feixiao/
├── linux-kernel/          <- rtlwifi source, confirmed present
└── rtw88-stable/
```

This closes the standing ambiguity in Sections 1-54 about whether the
rtlwifi kernel source was available to grep directly — it is, and was
used directly (via `grep`/`sed -n` in a live terminal session) for
everything in this section. Nothing below is inferred from Feixiao
patterns alone the way some of Sections 45-54 necessarily were.

## 55.2 RESOLVED: rtlwifi's scan-in-progress flag (closes Section
     53.5/54.5 item 2 — "not yet located")

Confirmed via direct read, `linux-kernel/drivers/net/wireless/realtek/
rtlwifi/wifi.h` and `core.c`:

```c
// wifi.h:2664 — field inside struct rtl_priv
struct rtl_mac mac80211;

// wifi.h:1467 — field inside struct rtl_mac
bool act_scanning;

// wifi.h:2755 — accessor macro, same idiom as rtl_priv(hw)
#define rtl_mac(rtlpriv) (&((rtlpriv)->mac80211))
```

Set/cleared at the real mac80211 ops entry points:

```c
// core.c:1418, inside rtl_op_sw_scan_start() (.sw_scan_start op)
mac->act_scanning = true;

// core.c:1451, inside rtl_op_sw_scan_complete() (.sw_scan_complete op)
mac->act_scanning = false;
```

Read at 7 call sites across `base.c`, `core.c`, `pci.c`, `ps.c`, `rc.c`
(confirmed via `grep -rn "act_scanning" *.c`) — the driver's
general-purpose "is a scan in progress" gate, structurally the same
role `RTW_FLAG_SCANNING` plays for rtw88 (Section 53.2), implemented as
a plain `bool` rather than a bitflag-in-a-word.

**Explicitly ruled out**: `MAC80211_LINKED_SCANNING` (wifi.h ~line
931), one candidate visible in an early grep, is NOT the right field —
it's one value of a 4-state `rtl_link_state` connection-state enum
(`MAC80211_NOLINK`/`_LINKING`/`_LINKED`/`_LINKED_SCANNING`), and only
applies when a scan starts while already connected (`link_state` only
transitions to it from `MAC80211_LINKED`, per `core.c:1428-1429`).
`act_scanning` is unconditionally true/false around every scan
regardless of link state, matching the general-purpose semantics
needed here.

**`rtlwifi_compat.c` change**: `rtlwifi_is_scanning()`'s hard `#error`
compile-time stop (written in Section 55.3 below, this session's
initial skeleton draft) was replaced with:

```c
bool rtlwifi_is_scanning(void)
{
    if (!g_rtlwifi_hw || !g_rtlwifi_hw->priv)
        return false;
    struct rtl_priv *rtlpriv = (struct rtl_priv *)g_rtlwifi_hw->priv;
    return rtl_mac(rtlpriv)->act_scanning;
}
```

## 55.3 `rtlwifi_compat.c`/`rtlwifi_compat.h` skeleton written

Before the resolutions in this section, a skeleton compat file was
written mirroring `rtw88_compat.c`'s confirmed structure end-to-end:
globals (`g_rtlwifi_hw`, split tentative/defining declaration per
Section 51.2's idiom), `ieee80211_alloc_hw()` (two-layer alloc per
Section 48.4/49.2, offset-0 `wiphy->_dev` trick per Section 51.3),
`wiphy_to_ieee80211_hw()`/`rtlwifi_get_hw()` (Section 51.3 lookup
pair), and a scan-flag accessor deliberately gated behind `#error`
rather than a guessed field name or a silently-wrong `return false;`
stub, so it would fail loudly at compile time instead of misbehaving
at runtime — resolved in 55.2 above once the real field was found.

## 55.4 RESOLVED: `rtw88_set_hw_callbacks()` signature and the full
     callback-cluster API surface (closes the open callback-struct gap
     carried since Section 45.6/52.1)

Confirmed via direct read, `Feixiao/src/compat/rtw88_compat.c:491-548`
(not inferred from call-site shape, as an earlier draft this session
had done before this read):

```c
void rtw88_set_hw_callbacks(struct rtw88_hw_callbacks *cbs, void *kext_hw)
{
    g_hw_cbs  = cbs;
    g_kext_hw = kext_hw;
    if (g_rtw88_hw)
        g_rtw88_hw->kext_hw = kext_hw;
}
```

No leading `struct ieee80211_hw *hw` parameter (an earlier draft this
session had guessed one, inferring only from the
`ieee80211_rx_irqsafe()` call-site shape). The function DOES write
`hw->kext_hw`, but reaches the hw pointer via the `g_rtw88_hw` global
(populated earliest-possible in `ieee80211_alloc_hw()`, Section
50.3-50.4) rather than via a parameter — this corrects a second,
different wrong guess made about the same function within this same
session, before the real body was read. Two plausible-looking
inferences from partial evidence, in sequence, both wrong; only the
direct read settled it. Noted here as a concrete illustration of why
Sections 1-54's "inferred, not confirmed" caveats exist.

The full struct (`src/compat/rtw88_compat.c:363-365`):

```c
struct rtw88_hw_callbacks {
    void (*rx_frame)(void *kext_hw, struct sk_buff *skb);
    void (*tx_status)(void *kext_hw, struct sk_buff *skb);
    void (*scan_done)(void *kext_hw, bool aborted);
};
```

And the full cluster of real mac80211-API intercept functions this
struct backs (`src/compat/rtw88_compat.c:501-548`), all confirmed by
direct read:

- `ieee80211_rx_irqsafe(hw, skb)` — dispatches to `cbs->rx_frame`;
  `kfree_skb(skb)` in the else-branch when no callback is registered
  (an earlier draft this session omitted this else-branch entirely,
  which would have silently leaked every skb received before
  `rtlwifi_set_hw_callbacks()` runs — caught and fixed against the real
  source, not caught by inspection).
- `ieee80211_rx_napi(hw, sta, skb, napi)` — thin forwarding wrapper to
  `ieee80211_rx_irqsafe()`, extra args dropped.
- `ieee80211_tx_status_irqsafe(hw, skb)` — same ctx/fallback shape,
  dispatches to `cbs->tx_status`, `kfree_skb` on no-callback.
- `ieee80211_tx_status(hw, skb)` — thin forwarding wrapper to
  `ieee80211_tx_status_irqsafe()`.
- `ieee80211_free_txskb(hw, skb)` — plain `kfree_skb(skb)`, not a
  callback intercept at all.
- `ieee80211_scan_completed(hw, info)` — dispatches to `cbs->scan_done`
  with `info->aborted`; confirmed to have NO kfree-equivalent
  else-branch (no skb involved), intentional, not an omission.
- `ieee80211_stop_queues`/`ieee80211_wake_queues`/`ieee80211_stop_queue`
  — deliberate no-ops, consistent with Section 52.2's confirmed finding
  that mac80211's own TX queueing is bypassed entirely in this port.

All nine functions were mirrored into `rtlwifi_compat.c` under the
same names (required — see Section 55.6 below), adapted only in the
scan-flag accessor's internals (`rtl_mac(rtlpriv)->act_scanning`
instead of `test_bit(RTW_FLAG_SCANNING, rtwdev->flags)`).

## 55.5 RESOLVED (with caveats): CCMP-IV question (narrows Section
     54.4's "not resolved by this reading" open item)

Found via direct read, `linux-kernel/drivers/net/wireless/realtek/
rtlwifi/base.c`, function `rtl_skb_ether_type_ptr()`:

```c
static const u8 *rtl_skb_ether_type_ptr(struct ieee80211_hw *hw,
                                          struct sk_buff *skb, bool is_enc)
{
    struct rtl_priv *rtlpriv = rtl_priv(hw);
    u8 mac_hdr_len = ieee80211_get_hdrlen_from_skb(skb);
    u8 encrypt_header_len = 0;
    u8 offset;

    switch (rtlpriv->sec.pairwise_enc_algorithm) {
    case WEP40_ENCRYPTION:
    case WEP104_ENCRYPTION:
        encrypt_header_len = 4;  /*WEP_IV_LEN*/
        break;
    case TKIP_ENCRYPTION:
        encrypt_header_len = 8;  /*TKIP_IV_LEN*/
        break;
    case AESCCMP_ENCRYPTION:
        encrypt_header_len = 8;  /*CCMP_HDR_LEN;*/
        break;
    default:
        break;
    }

    offset = mac_hdr_len + SNAP_SIZE;
    if (is_enc)
        offset += encrypt_header_len;

    return skb->data + offset;
}
```

Called from `rtl_is_special_data()`, with an explicit source comment
directly above it: `/*should call before software enc*/`.

**What this confirms**: rtlwifi's own driver code treats the CCMP
header (8 bytes — PN/ExtIV) as a distinct, explicitly-tracked offset
that must be skipped to reach payload past it — the identical 8-byte
magic number and the identical mental model as rtw88's documented
compensation (Section 52.3: "rtw88 leaves the CCMP IV in the frame...
`deliverDataFrame()` accounts for it directly").

**What this does NOT fully confirm**: the `is_enc` parameter and the
"should call before software enc" comment tie this specific function
to the pre-hardware-decryption case, not necessarily every RX code
path after hardware decryption completes. This is evidence that
rtlwifi's internal model matches rtw88's (same 8-byte accounting
convention exists in the codebase), not a byte-for-byte trace of the
post-hw-decrypt skb layout the way Section 54.2's `RX_FLAG_DECRYPTED`
read was a direct trace of its own narrower question.

**Port decision made on this evidence**: skip header + 8 bytes CCMP at
the IOKit-side `deliverDataFrame()`-equivalent, matching both rtw88's
confirmed behavior and rtlwifi's own offset arithmetic. Documented in
`rtlwifi_compat.c` as a provisional-but-evidenced decision, not a
blind guess — still flagged for boot-test confirmation, consistent
with the original plan (Section 54.5 item 1 / open item 8) of not
blocking further work on this, now with meaningfully stronger grounding
than "might not be determinable from source at all."

## 55.6 NEW FINDING: single-target build produces a duplicate-symbol
     problem — not previously tracked anywhere in Sections 1-54

Confirmed via direct inspection:

```
Feixiao/Makefile
Feixiao/rtw88.xcodeproj      <- single target, no RTL8188EE variant
```

```bash
grep -rn "RTW88\|RTL8188\|CHIP_FAMILY\|#ifdef" Feixiao/src/compat/rtw88_compat.c
# -> zero chip-family conditional guards found anywhere in the file
```

Every function name confirmed in Section 55.4
(`ieee80211_rx_irqsafe`, `ieee80211_tx_status_irqsafe`,
`ieee80211_scan_completed`, etc.) is a real mac80211 API name already
declared in `src/compat/net/mac80211.h` and already DEFINED in
`rtw88_compat.c`. `rtlwifi_compat.c` necessarily defines the same nine
symbols (mirroring the same API surface, adapted for rtlwifi). If both
files are ever compiled into the same build target, this is a
guaranteed duplicate-symbol link error — not a possibility to guard
against defensively, but the default outcome of naively adding
`rtlwifi_compat.c` to the existing single-target project as-is.

**Not resolved this session** — this is a build-system/project-
structure decision (separate Xcode target excluding `rtw88_compat.c`
from its source list; a renamed-symbol scheme; or some other explicit
mutual-exclusion mechanism), not a source-reading question. Flagged
directly in `rtlwifi_compat.h`'s header comment so it can't be missed
when this file is eventually wired into a build.

## 55.7 Updated remaining open items

1. **NEW (Section 55.6)** — build-target duplicate-symbol conflict
   between `rtw88_compat.c` and `rtlwifi_compat.c` under Feixiao's
   current single-target (`rtw88.xcodeproj`) structure. Needs a
   project-structure decision, not source reading. Highest-priority
   open item — blocks ever compiling this file into the real project
   as-is.
2. CCMP-IV question (Section 55.5) — narrowed from "not resolved by
   source reading" to "evidenced decision made, boot-test
   confirmation still pending" per the original plan. No longer a
   coin-flip; residual uncertainty is normal pre-boot-test residue,
   not an open investigation thread.
3. Whether rtlwifi's real RX code calls `ieee80211_rx_irqsafe()` vs.
   `ieee80211_rx_napi()` (or something else) at its actual RX delivery
   call site — not yet grepped against `linux-kernel/.../rtlwifi/`
   specifically (both wrapper functions are now written in
   `rtlwifi_compat.c`, so either call shape works once found, but the
   real call site itself hasn't been located).
4. Whether `rtl_op_sw_scan_complete()` (core.c, confirmed in 55.2 above
   as the function that clears `act_scanning`) itself calls
   `ieee80211_scan_completed()`, or whether rtlwifi's actual
   scan-completion signal reaches mac80211's API through a different
   call chain — not yet grepped.
5. `linux/sched.h` exact symbol usage — still open, minor (carried
   forward since Section 47.2, unchanged this session).
6. `rtw88_compat.c`'s remaining mac80211-stub coverage beyond the
   callback cluster resolved in 55.4 (i.e. whether rtlwifi's broader
   mac80211 API usage — beyond RX/TX-status/scan-done — exceeds what
   the existing compat layer implements) — still open, unchanged
   (carried forward since Section 45.6).
7. IOKit teardown path must account for the Section 49.5
   block-on-firmware-completion behavior — implementation note,
   unchanged, carried forward.
8. Runtime boot-test verification — unchanged, not answerable by
   source reading. Now covers a narrower, more-evidenced CCMP-IV
   question (item 2 above) than it did at the start of this session.

------------------------------------------------------------------------

## 56. Firmware loading path — resolved, new files drafted

Live `cat`/`grep` against the real repo this session (not inference):

- **`fw_blobs.h`'s `struct rtw88_fw_blob` is chip-agnostic** — plain
  `name`/`data`/`compressed_size`/`original_size`, nothing rtw88-specific
  in the struct itself. Directly reusable for rtl8188ee; only the
  *array name* (`rtw88_fw_blobs[]`) is chip-family-specific, not the
  struct shape. Resolves Section 55.7-adjacent open item from the
  handover ("fw_blobs.h's actual struct shape needs checking").
- **`request_firmware()` is synchronous**, not async-with-callback
  despite living in `linux/firmware.h` alongside
  `request_firmware_nowait()`. Confirmed by direct read: it's a
  `static inline` that calls straight through to
  `rtw88_load_firmware_sync()`. `firmware_request_nowarn()` is a plain
  alias for the same. rtlwifi's own code calls `request_firmware()`
  (per the earlier grep in this session), so the nowait/async path is
  likely not exercised for this chip family at all — worth keeping in
  mind against Section 49.5's `fail3:` unwind note (which assumed
  async completion-blocking semantics; those may not actually apply
  here since the load is synchronous under the hood).
- **Stale comment caught and disproven**: `linux/firmware.h` comments
  claim `request_firmware_nowait`/`rtw88_load_firmware_sync` are
  "Implemented in `rtw88_compat.c`" — confirmed FALSE by direct grep
  (zero matches in that file). The real implementations are in
  `rtw88_firmware.c`. Flagging this explicitly since acting on the
  comment instead of the grep would have pointed new code at the wrong
  file.
- **`gen_fw_blobs.py` hardcodes the emitted array name** to
  `rtw88_fw_blobs` on a single `lines.append(...)` line — not derived
  from the `fw_dir`/output-path CLI args. Confirmed by direct read of
  the full script. This means it cannot be reused unmodified for a
  second chip family without a link-time duplicate-symbol collision.

**Decision made (not a source-reading finding, a project-structure
choice, consistent with Section 55.6's still-open build-target
question and the handover's "avoid unnecessarily modifying the
original rtw88 implementation" guidance): fork, don't parameterize.**
`gen_fw_blobs.py` stays untouched; a new `gen_fw_blobs_rtl8188ee.py`
hardcodes `rtl8188ee_fw_blobs` instead. Trade-off acknowledged: two
near-identical scripts to keep in sync by hand if the blob format ever
changes, accepted as the better fit for the project's stated
separation goal than editing Feixiao's existing script.

### New files drafted this session (not yet added to any build target):

- `gen_fw_blobs_rtl8188ee.py` — forked generator, emits
  `rtl8188ee_fw_blobs[]` from a separate `firmware-rtl8188ee/` dir.
- `fw_blobs_rtl8188ee.h` — declares its own `struct rtl8188ee_fw_blob`
  (same field layout as `rtw88_fw_blob`, but a distinct type name, not
  just a distinct array name) rather than including `fw_blobs.h`, so
  this header has no dependency on Feixiao's rtw88-side tree; declares
  `extern const struct rtl8188ee_fw_blob rtl8188ee_fw_blobs[]`.
- `rtl8188ee_firmware.c` — mirrors `rtw88_firmware.c` function-for-
  function (same zlib decompress/allocator pattern, same private
  `struct firmware` layout), but every public symbol is prefixed
  `rtl8188ee_` instead of `rtw88_` (`rtl8188ee_request_firmware_nowait`,
  `rtl8188ee_release_firmware`, `rtl8188ee_load_firmware_sync`,
  `rtl8188ee_set_fw_dir`, `rtl8188ee_find_fw_dir`) specifically to dodge
  the same duplicate-symbol class of problem as Section 55.6, pending
  the same still-unresolved build-target decision. **Not a fix for
  55.6** — a workaround scoped to this one file, flagged as such in
  the file's own header comment. (Earlier draft of this file used an
  `rtlwifi_` prefix instead — corrected to `rtl8188ee_` to match the
  actual delivered files and avoid a second, chip-family-ambiguous
  naming scheme alongside `rtlwifi_compat.c`'s already-established
  `rtlwifi_` prefix for the *mac80211-API* shim layer.)
- `rtl8188ee.kext/Info.plist` — standalone plist, own
  `CFBundleIdentifier` (`com.rtlwifi.rtl8188ee`), one
  `IOKitPersonalities` entry for PCI ID `10EC:8179`
  (`IOPCIMatch` = `0x817910EC`, following the exact `<device><vendor>`
  byte-order pattern confirmed from rtw88.kext's existing entries).
  **`IOClass` is set to `RTL8188EEPCIDevice` (a new, distinct class
  name) with an explicit TODO comment** — this is NOT a confirmation
  that `RTW88PCIDevice` reuse is unsafe, only that it hasn't been
  confirmed safe; a new class name was chosen as the conservative
  default until open item 2 below is actually resolved by reading
  `RTW88PCIDevice.cpp`/`RTW88IEEE80211.cpp` against `rtl_priv`'s real
  layout (handover item 2 / Section 55.7 item 6, still open, still
  unread).

## 56.1 Updated remaining open items

1. Build-target duplicate-symbol conflict (Section 55.6) — still the
   top-priority open item, now with a second file
   (`rtl8188ee_firmware.c`) also depending on its eventual resolution,
   via the same `rtlwifi_`-prefix workaround rather than a real fix.
2. RTW88PCIDevice.cpp/RTW88IEEE80211.cpp reuse — still unread, still
   unresolved (carried forward unchanged; now also blocks finalizing
   `rtl8188ee.kext/Info.plist`'s `IOClass` value).
3. `rtw88_load_firmware_sync()`'s real body — declared in
   `linux/firmware.h`, implementation confirmed to live in
   `rtw88_firmware.c` (this session), but the function body itself
   (i.e. `load_fw()`'s exact call chain) was read this session and
   matches what's now mirrored in `rtl8188ee_firmware.c` — this item
   is CLOSED, listed here only for cross-reference.
4. rtlwifi's own compat `firmware.h` (parallel to
   `src/compat/linux/firmware.h`) does not exist yet. When written, it
   must call `rtl8188ee_load_firmware_sync()` (this session's new file),
   NOT `rtw88_load_firmware_sync()` — flagged explicitly in
   `rtl8188ee_firmware.c`'s header comment so this isn't silently
   wired to the wrong blob table later.
5. Whether rtlwifi's `fail3:` unwind's block-on-
   `firmware_loading_complete` behavior (Section 49.5) still applies
   given firmware loading is confirmed synchronous under the hood
   (this section) — not reconciled, carried forward as a refinement of
   existing open item 7 (Section 55.7).
6. Items 3-8 from Section 55.7 otherwise unchanged and carried forward
   as-is (RX delivery call site, scan-completion call chain,
   `linux/sched.h` usage, broader mac80211-stub coverage, IOKit
   teardown path, runtime boot-test verification).

------------------------------------------------------------------------

## 57. CORRECTION: build-target conflict was framed wrong — it's a
Makefile, not an Xcode target-membership problem; `Makefile.rtl8188ee`
drafted for real

Live `cat`/`grep` against the real repo this session (not inference):

- **`rtw88.xcodeproj` only tracks 8 hand-written files** (`rtw88_compat.c`,
  `rtw88_firmware.c`, and 6 `src/kext/*.cpp`/`.c` files) — confirmed via
  full `PBXFileReference` listing. Zero references to `rtw88-stable`
  anywhere in the project file (`grep -c "rtw88-stable"` → 0). The real
  compiled-in upstream driver source (18 `DRIVER_SRCS` + 22 `CHIP_SRCS`
  files) is pulled in entirely by `Feixiao/Makefile`, not the Xcode
  project — Xcode is used for IDE editing of the hand-written files only;
  the actual kext build is `make`-driven.
- **This means Section 55.6/handover item 25's "single Xcode target, no
  chip-family build guards, guaranteed link error" framing was itself
  incomplete/wrong** — there is no single build *target* at all in the
  Xcode sense to add a second one to. The real single-build-unit is
  `Feixiao/Makefile`'s `ALL_OBJS` → one linked `rtw88.kext` binary, and
  the actual multi-chip-family mechanism is **compile-time
  `-DCONFIG_RTW88_*` defines** (nine chip configs enabled simultaneously
  in one `DRIVER_CFLAGS` block), not Xcode build phases.
- The behavioral finding from Section 55.6 (`rtw88_compat.c` defines
  `ieee80211_rx_irqsafe` etc. with zero chip-family `#ifdef` guards) is
  still accurate — only the *mechanism by which it becomes a problem*
  was misdiagnosed. It's a problem for anything sharing `Feixiao/`'s
  `make` invocation and `ALL_OBJS`, not for "the Xcode target."

**Resolution: a second, fully parallel Makefile
(`Makefile.rtl8188ee`), not a second Xcode target.** Confirmed this
actually resolves the duplicate-symbol conflict by construction, not
just by naming convention: `make -f Makefile.rtl8188ee` and Feixiao's
own `make` produce two entirely separate `build/` object trees and two
separately linked kext binaries (`rtl8188ee.kext` vs `rtw88.kext`) that
are never combined into one linker invocation. The earlier
`rtlwifi_`/`rtl8188ee_` symbol-prefix workaround (Sections 55.3, 56) is
therefore **not load-bearing for this to work** — it was a defensive
choice made before this Makefile existed and can stay as extra margin
(e.g. against some future decision to statically link both kexts
together) without being the actual fix.

### `Makefile.rtl8188ee` drafted this session, mirroring the real
`Feixiao/Makefile` structure exactly:

- `LINUX_SRC` → `../linux-kernel/drivers/net/wireless/realtek/rtlwifi`
  (confirmed path, matches the earlier `find ... wifi.h` result).
- `CHIP_SRCS` → the same 10 rtl8188ee files confirmed against the real
  Kbuild list earlier in this project (dm/fw/hw/led/phy/pwrseq/rf/sw/
  table/trx) — unchanged, still solid.
- `DRIVER_SRCS` (rtlwifi's shared core) → **NOT independently
  re-confirmed this session** against `linux-kernel/.../rtlwifi/Makefile`'s
  real `rtlwifi-objs` list the way `CHIP_SRCS` was earlier. Drafted from
  this project's earlier general reading (base/cam/core/debug/efuse/ps/
  rc/regd/pci.c) — flagged as a TODO inside the Makefile itself rather
  than presented as confirmed.
- `COMPAT_SRCS`/`FIRMWARE_SRCS` → this session's `rtlwifi_compat.c` and
  `rtl8188ee_firmware.c`/`fw_blobs_rtl8188ee.c` (already drafted,
  Sections 55.3 and 56).
- `KEXT_SRCS` → reuses `RTW88Kext.cpp`, `RTW88PCIDevice.cpp`,
  `RTW88IEEE80211.cpp`, `RTW88UserClient.cpp` **directly from Feixiao/,
  unmodified, unconfirmed** — this is a placeholder so the Makefile is
  runnable-in-principle only. Does NOT resolve the still-open
  PCIDevice-reuse question (handover item 2 / Section 55.7 item 6). A
  clean compile of these files would NOT be evidence the reuse is
  correct — only that the C++ syntax is valid against rtl8188ee's
  compat headers, not that `hw->priv`'s assumed layout matches
  `rtl_priv`.
- Two genuinely new open items surfaced by writing this file:
  1. Whether `MacKernelSDK` and `src/compat/linux/*.h` shims should be
     reused directly from `Feixiao/` (this draft assumes so, via
     relative `-I../Feixiao/...` paths) or vendored into
     `rtl8188ee-macos/` for full independence, per the original
     project-organization goal ("final project should be maintainable
     independently of Feixiao where practical"). Not decided — flagged
     as a TODO in the Makefile itself.
  2. `rtlwifi_compat.h`'s own comment says it "assumes" Feixiao's
     existing `src/compat/linux/*` headers are available — this
     Makefile's `-I` path is the first place that assumption is
     actually cashed out as a real build dependency, which makes
     resolving open item 1 above more urgent than it looked before.

## 57.1 Updated remaining open items

1. RTW88PCIDevice.cpp/RTW88IEEE80211.cpp reuse — still unread, still
   unresolved, now the single biggest remaining gap since the build-
   target question is otherwise resolved.
2. rtlwifi's shared-core `DRIVER_SRCS` list — RESOLVED this session,
   confirmed directly against `linux-kernel/.../rtlwifi/Makefile`'s real
   `rtlwifi-objs` list, same standard the CHIP_SRCS list already met.
   Real list is 9 files: base/cam/core/debug/efuse/ps/rc/regd/stats.
   Two errors in the earlier draft caught and fixed: `stats.c` was
   missing entirely; `pci.c` was wrongly folded into this list — the
   real Kbuild puts it in a separate object (`rtl_pci.o`, gated by
   `CONFIG_RTLWIFI_PCI`), not `rtlwifi.o`. `Makefile.rtl8188ee` now
   keeps a distinct `PCI_SRCS` list for it (still compiled and linked
   into the one kext binary — the object-file separation only matters
   upstream, not for this build). rtl8188ee is PCIe-only (confirmed via
   Info.plist's `IOPCIMatch`), so `usb.c`/`CONFIG_RTLWIFI_USB` was
   deliberately not added.
3. NEW — MacKernelSDK / compat-linux-header reuse-vs-vendor decision
   (project-structure choice, not a source-reading gap): reuse from
   `Feixiao/` (current Makefile draft's assumption) vs. vendor a local
   copy into `rtl8188ee-macos/` for full independence. Not decided.
4. Firmware blob (`rtl8188efw.bin`) still not obtained — unchanged from
   Section 56.
5. Items 3-8 from Section 55.7 otherwise unchanged and carried forward
   as-is (RX delivery call site, scan-completion call chain,
   `linux/sched.h` usage, broader mac80211-stub coverage, IOKit
   teardown path, runtime boot-test verification).

------------------------------------------------------------------------

## 58. RESOLVED: rtlwifi's real `rtlwifi-objs` list confirmed, two
draft errors caught and fixed

Live `cat` of `linux-kernel/drivers/net/wireless/realtek/rtlwifi/Makefile`
this session (not inference):

```
rtlwifi-objs := base.o cam.o core.o debug.o efuse.o ps.o rc.o regd.o stats.o
obj-$(CONFIG_RTLWIFI_PCI) += rtl_pci.o
rtl_pci-objs := pci.o
```

- **9-file shared core confirmed**: `base`, `cam`, `core`, `debug`,
  `efuse`, `ps`, `rc`, `regd`, `stats`. Matches Section 57's draft on 8
  of 9 — `stats.c` was missing from that draft, now added.
- **`pci.c` is a separate object from `rtlwifi.o` upstream** (`rtl_pci.o`,
  gated by `CONFIG_RTLWIFI_PCI`), not part of `rtlwifi-objs`. Section
  57's draft had folded it into `DRIVER_SRCS` incorrectly. Fixed in
  `Makefile.rtl8188ee` with a separate `PCI_SRCS` list — functionally
  harmless for this build (everything still links into one kext binary
  either way; the object-file boundary only mattered on the upstream
  Kbuild side), but worth keeping visually distinct so the distinction
  isn't lost again if this Makefile is ever refactored.
- Also visible in the same Makefile: `obj-$(CONFIG_RTLWIFI_USB) +=
  rtl_usb.o` / `rtl_usb-objs := usb.o` — confirms `usb.c` is real but
  correctly NOT included in `Makefile.rtl8188ee`, since rtl8188ee is a
  PCIe part (10EC:8179, confirmed via this project's own Info.plist
  `IOPCIMatch` value).
- **New, unexplored detail**: this Makefile also shows
  `obj-$(CONFIG_RTL8188EE) += rtl8188ee/`, meaning real upstream Kbuild
  gates rtl8188ee's build on the actual kernel config symbol
  `CONFIG_RTL8188EE`. This project's compat flags currently define a
  different, project-invented symbol (`-DRTL8188EE_MACOS=1`) for our
  own macOS-side conditional use. Not yet checked whether any rtlwifi
  shared-core or rtl8188ee source file has `#ifdef CONFIG_RTL8188EE`
  branches that assume the *real* upstream symbol is defined — if so,
  `Makefile.rtl8188ee`'s `DRIVER_CFLAGS` needs an additional
  `-DCONFIG_RTL8188EE=1` alongside the macOS-specific define. New,
  small, well-scoped open item.

## 58.1 Updated remaining open items

1. RTW88PCIDevice.cpp/RTW88IEEE80211.cpp reuse — still unread, still
   unresolved, still the single biggest remaining gap.
2. NEW — whether any rtlwifi/rtl8188ee source references the real
   `CONFIG_RTL8188EE` kernel-config symbol (not just this project's own
   `RTL8188EE_MACOS` define) — unchecked, per 58 above.
3. MacKernelSDK / compat-linux-header reuse-vs-vendor decision — still
   undecided (Section 57).
4. Firmware blob (`rtl8188efw.bin`) still not obtained.
5. Items 3-8 from Section 55.7 otherwise unchanged and carried forward
   as-is.

------------------------------------------------------------------------

## 59. RESOLVED (with one real gap): RTW88PCIDevice.cpp/RTW88IEEE80211.cpp
reuse — handover item 2, the longest-standing open item

Live `cat` of both files (full contents) plus live `grep`/`sed` against
`linux-kernel/.../rtlwifi/core.c` and `Feixiao/src/compat/net/mac80211.h`
this session (not inference).

### 59.1 `hw->priv` cast — turns out NOT to be the real risk

Both files only touch `hw->priv` via one bare, opaque cast in
`RTW88IEEE80211::start()`: `_rtwdev = (struct rtw_dev *)_hw->priv;`.
`_rtwdev` is essentially never dereferenced directly afterward in
either file — every real access goes through `_hw->ops->*()`
function-pointer calls, `_hw->wiphy->*`, or rtw88-compat-layer helper
functions (`rtw88_get_fw_version()`, `rtw88_get_stats()`, etc., which
live in `rtw88_compat.c`, not these files). This means `struct
rtl_priv`'s field layout vs `struct rtw_dev`'s is NOT actually load-
bearing for these two files — the original open item as framed
(handover item 2, "rtl_priv's different layout") was asking the wrong
question. What actually matters is (a) whether `hw->priv` can be
substituted 1:1 as an opaque pointer, which it can, and (b) whether
every rtw88_*() compat helper these files call has an rtlwifi-side
equivalent — a compat-layer completeness question, not a struct-layout
one.

### 59.2 `RTW88PCIDevice.cpp` — reusable essentially as-is

No `hw->priv`/`ieee80211_ops` dependency at all. Confirmed by full read:
this file is PCI config space, DMA/bounce-buffer management, IOKit
lifecycle (init/start/stop/free), interrupt setup, and Ethernet-
controller glue. Chip-agnostic. Low risk to reuse directly for
rtl8188ee (still via `Makefile.rtl8188ee`'s existing placeholder
inclusion from Section 57/58 — this finding upgrades that placeholder
to "confirmed reusable," not just "runnable-in-principle").

### 59.3 `RTW88IEEE80211.cpp` — structurally reusable, ONE real
code-path gap identified

Confirmed real `struct ieee80211_ops rtl_ops` exists (`core.c:1879`),
single shared definition used by all rtlwifi chip families (not
per-chip) — good structural news, one comparison covers every chip.
Field-by-field check against every `_hw->ops->*` member this file
calls:

| Called in RTW88IEEE80211.cpp | In real `rtl_ops`? | rtlwifi function |
|---|---|---|
| `start` | yes | `rtl_op_start` |
| `stop` | yes | `rtl_op_stop` |
| `tx` | yes | `rtl_op_tx` |
| `add_interface` | yes | `rtl_op_add_interface` |
| `remove_interface` | yes | `rtl_op_remove_interface` |
| `set_key` | yes | `rtl_op_set_key` |
| `bss_info_changed` | yes | `rtl_op_bss_info_changed` |
| `sta_add` | yes | `rtl_op_sta_add` |
| `sta_remove` | yes | `rtl_op_sta_remove` |
| `hw_scan` | **NO — absent from rtl_ops** | — |
| `cancel_hw_scan` | **NO — absent from rtl_ops** | — |

Two of these nine present members' full signatures were independently
spot-checked against Feixiao's own compat `net/mac80211.h` struct
definition (not just against how the Feixiao files call them):
- `tx`: exact 3-parameter match, `(struct ieee80211_hw *, struct
  ieee80211_tx_control *, struct sk_buff *)` — identical on both
  sides.
- `start`: exact match, `(struct ieee80211_hw *)`. (First grep pass
  for this missed the compat header's copy due to a regex-escaping
  issue, not because it was actually absent — re-verified with `sed`
  before concluding.)
The other seven matching-name members' full signatures are NOT yet
independently spot-checked — names match, full parameter lists
unconfirmed. Lower-risk than the scan gap below (mac80211's
`ieee80211_ops` is a stable, well-known API surface), but still an
open item, not a closed one.

### 59.4 Real gap: rtlwifi has no `hw_scan` — different scan model
entirely, not a naming mismatch

`rtl_ops` has no `hw_scan`/`cancel_hw_scan` members at all. rtlwifi
uses `sw_scan_start`/`sw_scan_complete` instead (both present in
`rtl_ops`, confirmed) — software/mac80211-driven channel-walk
scanning, not firmware-offloaded hardware scanning. This is a
genuine architectural difference from rtw88, not a rename.

Fortunately, `RTW88IEEE80211.cpp`'s `cmdScan()` ALREADY has a fallback
path for exactly this case (`!_hw->ops->hw_scan ||
!rtw88_hw_scan_supported(_hw)` → `runManualScan()`, using
`rtw88_sw_scan_start`/`rtw88_sw_scan_switch_channel`/
`rtw88_sw_scan_complete` compat helpers). For rtlwifi, this fallback
branch becomes the ONLY branch (the `hw_scan` branch is permanently
dead code) — but the three `rtw88_sw_scan_*()` compat helpers it calls
don't have rtlwifi-side equivalents yet. **New concrete TODO**:
`rtlwifi_compat.c` needs `rtlwifi_sw_scan_start()` /
`rtlwifi_sw_scan_switch_channel()` / `rtlwifi_sw_scan_complete()`
wrapping `rtl_ops`'s real `sw_scan_start`/`sw_scan_complete` (no
per-channel-switch member was found in `rtl_ops` — `switch_channel`
may need to be driven differently; not yet checked).

### 59.5 Info.plist `IOClass` — updated from placeholder to confirmed choice

Given 59.2-59.4, reuse is real (with one identified gap, not a
structural blocker). `rtl8188ee.kext/Contents/Info.plist`'s `IOClass`
can move from a defensive `RTL8188EEPCIDevice` placeholder to actually
reusing `RTW88PCIDevice` — TODO updated accordingly (see 59.6).

## 59.6 Updated remaining open items

1. NEW — `rtlwifi_sw_scan_start/_switch_channel/_complete` compat
   helpers not yet written (Section 59.4). Concrete, well-scoped,
   blocks `cmdScan()`'s only viable path for rtlwifi.
2. NEW — 7 of 9 matching-name `ieee80211_ops` members have unconfirmed
   full signatures (names match; parameter lists not independently
   checked past `tx`/`start`). Lower risk, still open.
3. Info.plist `IOClass`: decide whether to actually rename to
   `RTW88PCIDevice` (reuse the real class) now that reuse is confirmed,
   or keep a distinct `RTL8188EEPCIDevice` name/subclass for the
   project's stated separation goal even though the underlying
   implementation would be shared — project-structure choice, not a
   source-reading gap.
4. MacKernelSDK / compat-linux-header reuse-vs-vendor decision — still
   undecided (Section 57).
5. Firmware blob (`rtl8188efw.bin`) still not obtained.
6. Items 3-8 from Section 55.7 otherwise unchanged and carried forward
   as-is.

------------------------------------------------------------------------

## 60. RESOLVED: `rtlwifi_sw_scan_start/_switch_channel/_complete()`
written, closing Section 59.4's gap

Live `sed`/`grep` against real `core.c` this session (not inference):

- `rtl_op_sw_scan_start`/`rtl_op_sw_scan_complete` full bodies read.
  Both are real `rtl_ops` members, called the same way every other
  member this port already uses (`hw->ops->sw_scan_start(...)`).
- **`rtl_op_sw_scan_complete()` never calls `ieee80211_scan_completed()`**
  — confirmed by grep against the full file, zero matches. This
  resolves the TODO that was sitting in `rtlwifi_compat.c` since the
  Section 55 session, but resolves it as "no," not "yes": rtlwifi's
  own scan-complete op only clears `mac->act_scanning` and does
  internal bookkeeping (BT coexist, LED, link-state transition). The
  actual scan-done signal for this port continues to come from
  `RTW88IEEE80211.cpp`'s `runManualScan()` calling `scanDone()`
  directly, unaffected by this finding.
- **No scan-specific channel-switch member exists in `rtl_ops`** —
  confirmed real per-channel switching happens via
  `rtlpriv->cfg->ops->switch_channel(hw)` (core.c:754), called FROM
  INSIDE `rtl_op_config()` when `changed & IEEE80211_CONF_CHANGE_CHANNEL`
  is set (full ~190-line body of `rtl_op_config` read and confirmed).
  `rtlpriv->cfg->ops` is `rtl_hal_ops` — the same internal per-chip
  vtable `sw.c`'s `set_key` belongs to (Section 59's earlier
  `rtl_hal_ops`/`ieee80211_ops` distinction) — so the new compat
  helper correctly goes through `hw->ops->config()`, never touching
  `rtlpriv->cfg->ops` directly, preserving the same abstraction
  boundary already respected elsewhere in this file.
- **RESOLVED same session, follow-up check**: `rtl_op_config`'s real
  confirmed signature is `(struct ieee80211_hw *hw, int radio_idx, u32
  changed)` — three parameters. Live grep against the actual compat
  header confirms exact match:
  `Feixiao/src/compat/net/mac80211.h:831` — `int (*config)(struct
  ieee80211_hw *hw, int radio_idx, u32 changed);`. No signature
  mismatch; `radio_idx` is already part of this compat layer's
  `ieee80211_ops` shape (not rtlwifi-specific), and
  `rtlwifi_sw_scan_switch_channel()`'s `0` default fits it correctly.
  No code changes needed — the function as written in this session is
  correct as-is.

`rtlwifi_compat.c`/`.h` updated with the three new functions
(`rtlwifi_sw_scan_start`, `rtlwifi_sw_scan_switch_channel`,
`rtlwifi_sw_scan_complete`) plus a stale-comment fix (the header's
`rtlwifi_is_scanning()` doc comment still said "gated behind #error,"
contradicting the .c file's already-resolved real implementation from
Section 55.2 — corrected while in the area).

## 60.1 Updated remaining open items

1. 7 of 9 matching-name `ieee80211_ops` members (Section 59.3) still
   have unconfirmed full signatures beyond `tx`/`start`/now-confirmed
   `config`.
2. MacKernelSDK / compat-linux-header reuse-vs-vendor decision — still
   undecided (Section 57).
3. Firmware blob (`rtl8188efw.bin`) still not obtained.
4. Items 3-8 from Section 55.7 otherwise unchanged and carried forward
   as-is.

------------------------------------------------------------------------

# End of Findings (this revision)
