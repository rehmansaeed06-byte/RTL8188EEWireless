/* SPDX-License-Identifier: GPL-2.0 OR BSD-3-Clause */
#ifndef _RTL8188EE_COMPAT_DEVICE_H
#define _RTL8188EE_COMPAT_DEVICE_H

#include "types.h"

/*
 * dev_warn() -- genuinely missing, no existing dev_* logging in this
 * file. Bridges to the same rtl8188ee_printk mechanism kernel.h's
 * pr_err()/pr_info() already use, rather than inventing a second
 * logging path. Real call site: efuse.c. The struct device * arg is
 * accepted for signature compatibility but not used in the message
 * (this compat layer has no per-device log prefix mechanism).
 */
#include "kernel.h"
#define dev_warn(dev, fmt, ...) \
    rtl8188ee_printk(KERN_WARN, fmt, ##__VA_ARGS__)

struct device {
    const char *name;
    void *parent;
    void *kext_dev;
};

static inline const char *dev_name(const struct device *dev)
{
    return dev ? dev->name : "(null)";
}

/* pm_runtime stubs */
static inline void pm_runtime_enable(struct device *dev) {}
static inline void pm_runtime_disable(struct device *dev) {}
static inline void pm_runtime_allow(struct device *dev) {}
static inline void pm_runtime_forbid(struct device *dev) {}
static inline int  pm_runtime_get_sync(struct device *dev) { return 0; }
static inline void pm_runtime_put_autosuspend(struct device *dev) {}
static inline void pm_runtime_put_sync(struct device *dev) {}
static inline void pm_runtime_set_autosuspend_delay(struct device *dev, int d) {}
static inline void pm_runtime_use_autosuspend(struct device *dev) {}
static inline int  pm_runtime_suspended(struct device *dev) { return 0; }
static inline void pm_runtime_mark_last_busy(struct device *dev) {}

/* Forward declaration so devm_kzalloc below doesn't create an implicit external one */
static inline void *kzalloc(size_t size, gfp_t flags);

/* devres — devm_kzalloc forwards to kzalloc (no per-device resource tracking) */
static inline void *devm_kzalloc(struct device *dev, size_t size, gfp_t flags)
{
    return kzalloc(size, flags);
}

/* devm_kmemdup / devm_kmemdup_array — managed copies, implemented in rtl8188ee_compat.c */
void *devm_kmemdup(struct device *dev, const void *src, size_t len, gfp_t gfp);
void *devm_kmemdup_array(struct device *dev, const void *src, size_t n,
                          size_t size, gfp_t gfp);

/* dev_get_drvdata / dev_set_drvdata */
static inline void *dev_get_drvdata(const struct device *dev) { return dev ? dev->kext_dev : NULL; }
static inline void  dev_set_drvdata(struct device *dev, void *data) { if (dev) dev->kext_dev = data; }

#endif /* _RTL8188EE_COMPAT_DEVICE_H */
