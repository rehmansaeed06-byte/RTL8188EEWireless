/* SPDX-License-Identifier: GPL-2.0 OR BSD-3-Clause */
#ifndef _RTW88_COMPAT_INTERRUPT_H
#define _RTW88_COMPAT_INTERRUPT_H

#include "types.h"

#define IRQF_SHARED     0x00000080
#define IRQF_DISABLED   0x00000020

typedef int irqreturn_t;
#define IRQ_NONE        0
#define IRQ_HANDLED     1
#define IRQ_WAKE_THREAD 2

typedef irqreturn_t (*irq_handler_t)(int, void *);

#ifndef KBUILD_MODNAME
#define KBUILD_MODNAME "rtw88"
#endif

/* In the kext, interrupts are registered via IOInterruptEventSource.
 * These stubs satisfy compilation of driver C files.
 */
struct device;

static inline int request_irq(unsigned int irq,
                               irqreturn_t (*handler)(int, void *),
                               unsigned long flags,
                               const char *name, void *dev)
{
    /* Handled by kext IOInterruptEventSource */
    return 0;
}

static inline void free_irq(unsigned int irq, void *dev_id) {}

void rtw88_devm_free_irq(struct device *dev, unsigned int irq, void *dev_id);
#define devm_free_irq rtw88_devm_free_irq

static inline void enable_irq(unsigned int irq) {}
static inline void disable_irq(unsigned int irq) {}
static inline void disable_irq_nosync(unsigned int irq) {}
static inline void synchronize_irq(unsigned int irq) {}

/* tasklet — simplified to direct call in our context.
 *
 * Real upstream (post use_callback/tasklet_setup merge) supports two
 * calling conventions on the same struct: the old
 * tasklet_init(t, func, data) where func takes `unsigned long`, and
 * the new tasklet_setup(t, callback) where callback takes
 * `struct tasklet_struct *`. use_callback picks which union member
 * tasklet_schedule() invokes. This project's actual rtlwifi/pci.c
 * (confirmed by direct grep — findings.md Section 71.10) only calls
 * tasklet_setup()/from_tasklet(), never tasklet_init(), but both are
 * kept here rather than only adding the new one, since other files
 * in the vendored driver tree were not similarly checked and may
 * still use the old API.
 */
struct tasklet_struct {
    bool use_callback;
    union {
        void (*func)(unsigned long);
        void (*callback)(struct tasklet_struct *);
    };
    unsigned long data;
};

static inline void tasklet_init(struct tasklet_struct *t,
                                 void (*func)(unsigned long),
                                 unsigned long data)
{
    t->use_callback = false;
    t->func = func;
    t->data = data;
}

static inline void tasklet_setup(struct tasklet_struct *t,
                                  void (*callback)(struct tasklet_struct *))
{
    t->use_callback = true;
    t->callback = callback;
    t->data = 0;
}

/* Real upstream: container_of(callback_tasklet, typeof(*var), tasklet_fieldname).
 * Confirmed verbatim against torvalds/linux's real include/linux/interrupt.h,
 * not guessed (findings.md Section 71.10). */
#define from_tasklet(var, callback_tasklet, tasklet_fieldname) \
    container_of(callback_tasklet, typeof(*var), tasklet_fieldname)

static inline void tasklet_schedule(struct tasklet_struct *t)
{
    if (t->use_callback) {
        if (t->callback) t->callback(t);
    } else {
        if (t->func) t->func(t->data);
    }
}

static inline void tasklet_kill(struct tasklet_struct *t) {}

/* NAPI — not used directly; kext handles RX scheduling */
struct net_device;
struct napi_struct {
    struct net_device *dev;
    int (*poll)(struct napi_struct *, int);
    int weight;
    int running;
    void *thread_call;
};

void rtw88_netif_napi_add(struct net_device *dev, struct napi_struct *napi, int (*poll_fn)(struct napi_struct *, int));
#define netif_napi_add rtw88_netif_napi_add

static inline void napi_enable(struct napi_struct *napi) {}
static inline void napi_disable(struct napi_struct *napi) {}

void rtw88_napi_schedule(struct napi_struct *napi);
#define napi_schedule rtw88_napi_schedule

static inline void napi_complete(struct napi_struct *napi) {}
static inline int napi_reschedule(struct napi_struct *napi) { return 0; }
static inline void napi_synchronize(struct napi_struct *napi) {}

void rtw88_netif_napi_del(struct napi_struct *napi);
#define netif_napi_del rtw88_netif_napi_del

static inline int napi_complete_done(struct napi_struct *napi, int work) { return 1; }

extern irq_handler_t g_irq_handler;
extern irq_handler_t g_irq_thread_fn;
extern void *g_irq_dev_id;

int rtw88_devm_request_threaded_irq(struct device *dev, unsigned int irq,
        irq_handler_t handler, irq_handler_t thread_fn,
        unsigned long flags, const char *name, void *dev_id);
#define devm_request_threaded_irq rtw88_devm_request_threaded_irq

#endif /* _RTW88_COMPAT_INTERRUPT_H */
