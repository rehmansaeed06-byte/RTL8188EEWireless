/* SPDX-License-Identifier: GPL-2.0 OR BSD-3-Clause */
#ifndef _RTL8188EE_COMPAT_TIMER_H
#define _RTL8188EE_COMPAT_TIMER_H

#include "types.h"
#include "jiffies.h"
#include "../iokit_shim.h"

struct timer_list {
    unsigned long  expires;   /* absolute jiffies deadline */
    void (*function)(struct timer_list *t);
    unsigned long  data;      /* legacy arg (unused in modern kernels) */
    thread_call_t  call;      /* XNU thread_call backing this timer */
    int            active;
    /* completion tracking for del_timer_sync() -- thread_call_cancel_wait()
     * is com.apple.kpi.private and unusable by a third-party kext (confirmed
     * via a real kmutil load rejection), so cancel+wait is built here from
     * public IOLock primitives instead. Set/cleared by the trampoline in
     * rtlwifi_compat.c. */
    IOLock        *done_lock;
    volatile int   running;
};

#define from_timer(var, callback_timer, timer_fieldname) \
    container_of(callback_timer, __typeof__(*var), timer_fieldname)
#define timer_container_of(var, callback_timer, timer_fieldname) \
    container_of(callback_timer, __typeof__(*var), timer_fieldname)

void timer_setup(struct timer_list *timer,
                 void (*func)(struct timer_list *t),
                 unsigned int flags);

static inline void setup_timer(struct timer_list *timer,
                                void (*func)(unsigned long),
                                unsigned long data)
{
    timer->data     = data;
    timer->active   = 0;
    timer->call     = NULL;
    timer->function = (void (*)(struct timer_list *))func;
}

int mod_timer(struct timer_list *timer, unsigned long expires);
int del_timer_sync(struct timer_list *timer);
int timer_delete_sync(struct timer_list *timer);   /* Linux 6.x rename of del_timer_sync — same contract, alias */
int del_timer(struct timer_list *timer);

static inline int timer_pending(const struct timer_list *timer)
{
    return timer->active;
}

#endif /* _RTL8188EE_COMPAT_TIMER_H */
