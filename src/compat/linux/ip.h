/* SPDX-License-Identifier: GPL-2.0 OR BSD-3-Clause */
#ifndef _RTW88_COMPAT_IP_H
#define _RTW88_COMPAT_IP_H

/*
 * Real rtlwifi/base.c usage (confirmed by grep, not assumed):
 *   const struct iphdr *ip;
 *   ip = (struct iphdr *)((u8 *)ether_type_ptr + ...);
 * used inside rtl_is_special_data() to classify DHCP/ARP/EAPOL-type
 * frames for TX prioritization (findings.md Section 64 — this is a
 * correctness-sensitive struct, not just a build stub, so the layout
 * below is the real on-wire IPv4 header, byte-for-byte, not a
 * simplified placeholder).
 *
 * Field names/bitfield order match Linux's <uapi/linux/ip.h> exactly.
 * Endianness note: this struct is read directly out of packet bytes
 * (no host/network conversion applied to the struct itself), same as
 * upstream Linux — individual multi-byte fields (tot_len, id, frag_off,
 * check, saddr, daddr) are network-byte-order and must be converted
 * with ntohs()/ntohl() by the caller, same as real Linux code does.
 */

#include "types.h"

/* IPPROTO_UDP — CONFIRMED real: base.c's rtl_is_special_data() checks
 * `IPPROTO_UDP == ip->protocol` right after the iphdr cast below, to
 * finish DHCP classification. Standard <linux/in.h>/<uapi/linux/in.h>
 * value (17), not rtlwifi-specific — only the one value this driver
 * actually compares against is added, not the full IPPROTO_* set. */
#define IPPROTO_UDP 17

struct iphdr {
#if defined(__LITTLE_ENDIAN_BITFIELD) || 1
    /* macOS/x86_64 is little-endian; rtlwifi is only built for
     * little-endian targets in practice (x86), so this is the
     * only variant needed here — matches upstream's #ifdef choice
     * for LE without carrying the unused BE branch. */
    u8   ihl:4,
         version:4;
#else
    u8   version:4,
         ihl:4;
#endif
    u8   tos;
    u16  tot_len;
    u16  id;
    u16  frag_off;
    u8   ttl;
    u8   protocol;
    u16  check;
    u32  saddr;
    u32  daddr;
    /* options start here — not needed, rtlwifi only reads the
     * fixed header fields above (protocol/ports for classification) */
};

#endif /* _RTW88_COMPAT_IP_H */
