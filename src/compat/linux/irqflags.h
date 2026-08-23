/* src/compat/linux/irqflags.h
 *
 * local_save_flags()/local_irq_enable()/local_irq_restore() compat shim.
 *
 * Real rtlwifi usage (confirmed: rtl8188ee/hw.c, _rtl88ee_hw_init(), the
 * only CHIP_SRCS/DRIVER_SRCS call site):
 *
 *     local_save_flags(flags);
 *     local_irq_enable();
 *     ... up to ~350ms of firmware-init work with interrupts re-enabled ...
 *     local_irq_restore(flags);
 *
 * i.e. genuinely just "remember whether interrupts were enabled, force
 * them on for a long operation, then put them back exactly as found" --
 * not a full IRQ-masking/priority-level primitive. XNU's
 * ml_*_interrupts_enabled pair (MacKernelSDK/Headers/i386/machine_routines.h)
 * is the direct, correct equivalent: ml_get_interrupts_enabled() returns
 * the current state without changing it, ml_set_interrupts_enabled(BOOL)
 * sets it and returns the *previous* state.
 *
 * NOTE: unlike Linux's local_irq_save() (which both reads AND disables in
 * one call), rtlwifi here only ever calls local_save_flags() (read-only)
 * immediately followed by local_irq_enable() (unconditional enable) -- so
 * this shim intentionally does NOT need to replicate local_irq_save()'s
 * combined semantics, only this exact real usage.
 */

#ifndef _RTLWIFI_COMPAT_IRQFLAGS_H_
#define _RTLWIFI_COMPAT_IRQFLAGS_H_

#include <i386/machine_routines.h>

typedef boolean_t rtlwifi_irq_flags_t;

/* Linux's local_save_flags(flags) takes 'flags' by macro (unsigned long
 * flags; local_save_flags(flags);) not by pointer -- match that shape so
 * real call sites (`local_save_flags(flags);`) need no rewriting. */
#define local_save_flags(flags) \
    do { (flags) = (unsigned long)ml_get_interrupts_enabled(); } while (0)

#define local_irq_enable() \
    ((void)ml_set_interrupts_enabled(TRUE))

#define local_irq_restore(flags) \
    ((void)ml_set_interrupts_enabled((boolean_t)(flags)))

#endif /* _RTLWIFI_COMPAT_IRQFLAGS_H_ */
