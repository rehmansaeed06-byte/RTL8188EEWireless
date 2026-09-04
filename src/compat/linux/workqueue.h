/* SPDX-License-Identifier: GPL-2.0 OR BSD-3-Clause */
#ifndef _RTL8188EE_COMPAT_WORKQUEUE_H
#define _RTL8188EE_COMPAT_WORKQUEUE_H

#include "types.h"
#include "spinlock.h"
#include "timer.h"
#include "../iokit_shim.h"

struct work_struct;
struct delayed_work;

typedef void (*work_func_t)(struct work_struct *work);

struct work_struct {
    work_func_t      func;
    struct list_head entry;
    unsigned long    pending;
    thread_call_t    call;   /* XNU thread_call backing this work item;
                               * mirrors delayed_work's timer.call so
                               * cancel_work_sync()/flush_work() have
                               * something real to cancel/wait on. */
    /* same cancel+wait completion tracking as timer_list, see timer.h --
     * thread_call_cancel_wait() is com.apple.kpi.private and unusable by a
     * third-party kext. */
    IOLock          *done_lock;
    volatile int     running;
};

struct delayed_work {
    struct work_struct  work;
    struct timer_list   timer;  /* defined in timer.h */
};

struct workqueue_struct {
    thread_t         thread;
    IOLock          *lock;
    struct list_head queue;
    int              running;
    volatile int     done;
    char             name[64];
};

#define INIT_WORK(_work, _func) \
    do { (_work)->func = (_func); \
         INIT_LIST_HEAD(&(_work)->entry); \
         (_work)->pending    = 0; \
         (_work)->call       = NULL; \
         (_work)->done_lock  = NULL; \
         (_work)->running    = 0; } while (0)

#define INIT_DELAYED_WORK(_dwork, _func) \
    INIT_WORK(&(_dwork)->work, _func)

extern struct workqueue_struct *system_wq;
extern struct workqueue_struct *system_long_wq;

/*
 * alloc_workqueue — CORRECTED this session: real upstream Linux
 * defines this as a variadic printf-style MACRO
 * (`fmt, flags, max_active, ...args`), not a plain 3-arg function —
 * confirmed by the real call site (base.c's _rtl_init_deferred_work,
 * grepped this session): `alloc_workqueue("%s", WQ_UNBOUND, 0,
 * rtlpriv->cfg->name)`, 4 arguments against what was previously a
 * fixed 3-param declaration ("too many arguments to function call,
 * expected 3, have 4" — real compiler error, not a hypothetical).
 * Only this one call site exists in the driver code this build
 * compiles (DRIVER_SRCS), so the varargs are handled generically
 * (vsnprintf into the workqueue's fixed name[64] buffer) rather than
 * specially-cased for "%s" + one string arg, in case other rtlwifi
 * source not yet hit by a build error uses a different format.
 */
struct workqueue_struct *alloc_workqueue(const char *fmt, unsigned int flags,
                                          int max_active, ...);
/*
 * NOTE — separate from the arity fix above: alloc_workqueue() (and
 * queue_work/destroy_workqueue/etc. below) are declared here but have
 * NO implementation anywhere in this project yet (confirmed by grep,
 * this session — rtlwifi_compat.c is COMPAT_SRCS's only compiled .c,
 * and none of the workqueue functions have a body there). This is a
 * pre-existing gap, not something this round's arity fix introduced
 * or needs to solve — the build hasn't reached the link stage yet
 * (still hitting real compile errors first, base.c/rc.c this round).
 * Flagged here so it isn't mistaken for "done" once compile errors
 * stop: a real thread_call/IOLock-backed workqueue implementation is
 * still needed in rtlwifi_compat.c before this build will link.
 */
struct workqueue_struct *alloc_ordered_workqueue(const char *name,
                                                  unsigned int flags);
void destroy_workqueue(struct workqueue_struct *wq);

bool queue_work(struct workqueue_struct *wq, struct work_struct *work);
bool queue_delayed_work(struct workqueue_struct *wq,
                        struct delayed_work *dwork, unsigned long delay);
void flush_workqueue(struct workqueue_struct *wq);
bool cancel_work_sync(struct work_struct *work);
bool cancel_delayed_work_sync(struct delayed_work *dwork);
bool cancel_delayed_work(struct delayed_work *dwork);
void flush_work(struct work_struct *work);
bool schedule_work(struct work_struct *work);
bool schedule_delayed_work(struct delayed_work *dwork, unsigned long delay);
void flush_scheduled_work(void);

#define WQ_HIGHPRI     0
#define WQ_UNBOUND     0
#define WQ_MEM_RECLAIM 0
#define WQ_FREEZABLE   0
#define WQ_BH          0
#define WQ_PERCPU      0

static inline bool queue_work_on(int cpu, struct workqueue_struct *wq,
                                  struct work_struct *work)
{
    return queue_work(wq, work);
}

static inline bool mod_delayed_work(struct workqueue_struct *wq,
                                     struct delayed_work *dwork,
                                     unsigned long delay)
{
    cancel_delayed_work(dwork);
    return queue_delayed_work(wq, dwork, delay);
}

int  rtw88_workqueue_init(void);
void rtw88_workqueue_exit(void);

#endif /* _RTL8188EE_COMPAT_WORKQUEUE_H */
