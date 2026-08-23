/* SPDX-License-Identifier: GPL-2.0 OR BSD-3-Clause */
#ifndef _RTW88_COMPAT_FIRMWARE_H
#define _RTW88_COMPAT_FIRMWARE_H

#include "types.h"
#include "kernel.h"
#include "slab.h"

struct firmware {
    size_t    size;
    const u8 *data;
    size_t    _alloc_size;  /* private — do not use in driver code */
};

struct module;
struct device;

/*
 * findings.md Section 87 / rtl8188ee_firmware.c's own header comment
 * (lines 10-29): this header previously declared/wired the rtw88_-
 * prefixed names (request_firmware_nowait, release_firmware,
 * rtw88_load_firmware_sync), which have NO definition anywhere in
 * this tree — rtw88_compat.c doesn't exist in this project (Section
 * 77.1). Only the real rtl8188ee_-prefixed equivalents in
 * rtl8188ee_firmware.c exist. CONFIRMED via grep: zero non-declaration
 * hits for rtw88_load_firmware_sync anywhere under src/.
 *
 * Fixed to declare/wire the real rtl8188ee_ names below — this is a
 * naming-mismatch fix, not new logic; the real implementations were
 * already correct and already flagged this exact seam as open.
 */
int  rtl8188ee_request_firmware_nowait(struct module *module, int uevent,
                                        const char *name, struct device *device,
                                        gfp_t gfp, void *context,
                                        void (*cont)(const struct firmware *fw, void *ctx));

void rtl8188ee_release_firmware(const struct firmware *fw);

int rtl8188ee_load_firmware_sync(const char *name, const struct firmware **fw_out);

void rtl8188ee_set_fw_dir(const char *dir);
void rtl8188ee_find_fw_dir(void);

static inline int request_firmware(const struct firmware **fw_out,
                                    const char *name, struct device *dev)
{
    (void)dev;
    return rtl8188ee_load_firmware_sync(name, fw_out);
}

static inline int firmware_request_nowarn(const struct firmware **fw,
                                           const char *name, struct device *dev)
{
    return request_firmware(fw, name, dev);
}

static inline int request_firmware_nowait(struct module *module, int uevent,
                                           const char *name, struct device *device,
                                           gfp_t gfp, void *context,
                                           void (*cont)(const struct firmware *fw, void *ctx))
{
    return rtl8188ee_request_firmware_nowait(module, uevent, name, device,
                                              gfp, context, cont);
}

static inline void release_firmware(const struct firmware *fw)
{
    rtl8188ee_release_firmware(fw);
}

#endif /* _RTW88_COMPAT_FIRMWARE_H */
