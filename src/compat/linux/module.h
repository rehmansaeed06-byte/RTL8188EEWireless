/* SPDX-License-Identifier: GPL-2.0 OR BSD-3-Clause */
#ifndef _RTL8188EE_COMPAT_MODULE_H
#define _RTL8188EE_COMPAT_MODULE_H

/* Module stubs — not needed in kext context */
#define MODULE_AUTHOR(x)
#define MODULE_DESCRIPTION(x)
#define MODULE_LICENSE(x)
#define MODULE_FIRMWARE(x)
#define MODULE_VERSION(x)
#define MODULE_DEVICE_TABLE(type, name)
#define MODULE_PARM_DESC(name, desc)
#define EXPORT_SYMBOL(sym)
#define EXPORT_SYMBOL_GPL(sym)

#define module_param(name, type, perm)
#define module_param_named(name, var, type, perm)
#define module_param_string(name, str, len, perm)
#define module_param_array(name, type, nump, perm)

#define THIS_MODULE ((struct module *)0)

struct module;
struct device_driver { const char *name; };
struct bus_type { const char *name; };

#define module_driver(__driver, __register, __unregister, ...) \
    static int __init __driver##_init(void) \
    { return __register(&(__driver), ##__VA_ARGS__); } \
    static void __exit __driver##_exit(void) \
    { __unregister(&(__driver), ##__VA_ARGS__); }

#define module_pci_driver(__pci_driver) \
    module_driver(__pci_driver, pci_register_driver, pci_unregister_driver)

#define module_usb_driver(__usb_driver) \
    module_driver(__usb_driver, usb_register, usb_deregister)

/*
 * module_init/module_exit — CONFIRMED real, missing entirely (not a
 * struct/macro-arg issue): base.c ends with
 * `module_init(rtl_core_module_init); module_exit(rtl_core_module_exit);`
 * With no macro defined, `module_init(rtl_core_module_init);` parsed
 * as a bare function-style declaration with no return type — hence
 * the real compiler error was "a parameter list without types is only
 * allowed in a function definition", not a plainer "undeclared
 * identifier" (same class of misleading-error-from-missing-macro as
 * skb_queue_walk in Section 67.6).
 *
 * Real upstream Linux registers these as the module's load/unload
 * entry points, invoked by the kernel's module loader. This is a
 * macOS kext, not a loadable .ko — there is no Linux module loader to
 * register with, and findings.md's build-system work (kmod_info.c,
 * IOKit driver lifecycle) already established the real load/unload
 * path is IOKit's own start()/stop(), not these. No-op stubs — same
 * "not needed in kext context" rationale as every other macro already
 * in this file — but the two named functions (rtl_core_module_init,
 * rtl_core_module_exit) still need to actually compile as ordinary
 * static functions in base.c even though nothing calls them via this
 * path; that's fine, they simply become dead code the linker can
 * discard, not a build error.
 */
#define module_init(fn)
#define module_exit(fn)

#define __init
#define __exit
#define __devinit
#define __devexit
#define __devexit_p(f) (f)

#endif /* _RTL8188EE_COMPAT_MODULE_H */
