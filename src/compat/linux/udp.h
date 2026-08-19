/* SPDX-License-Identifier: GPL-2.0 OR BSD-3-Clause */
#ifndef _RTW88_COMPAT_UDP_H
#define _RTW88_COMPAT_UDP_H

/*
 * Real rtlwifi/base.c usage (same call site as linux/ip.h, confirmed by
 * grep): rtl_is_special_data() casts a raw byte pointer to
 * `struct udphdr *` to read source/dest ports, right after the
 * `struct iphdr *` cast, to finish classifying DHCP (UDP/67/68) traffic
 * for TX prioritization. Same correctness bar as ip.h (findings.md
 * Section 64/65.2) — real on-wire layout, not a placeholder.
 *
 * Field names/order match Linux's <uapi/linux/udp.h> exactly. Like
 * iphdr, this struct is read directly out of packet bytes — multi-byte
 * fields (source, dest, len, check) are network-byte-order and must be
 * converted with ntohs() by the caller, same as real Linux code does.
 */

#include "types.h"

struct udphdr {
    u16  source;
    u16  dest;
    u16  len;
    u16  check;
};

#endif /* _RTW88_COMPAT_UDP_H */
