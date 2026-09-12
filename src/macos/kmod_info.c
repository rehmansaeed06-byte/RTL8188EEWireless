#include <mach/mach_types.h>

extern kern_return_t rtl8188ee_module_start(kmod_info_t *ki, void *data);
extern kern_return_t rtl8188ee_module_stop(kmod_info_t *ki, void *data);

/*
 * Identity here MUST match Info.plist's CFBundleIdentifier /
 * CFBundleVersion exactly, or the kernel/OpenCore will reject this
 * kext at load/inject time even though it compiles and links fine.
 * (Forked from ../Feixiao/src/kext/kmod_info.c, which hardcoded
 * Feixiao's own identity — com.rtl8188ee.driver / 1.1.0 — not this
 * project's.)
 */
KMOD_EXPLICIT_DECL(com.rtlwifi.rtl8188ee, "0.1.0", rtl8188ee_module_start, rtl8188ee_module_stop)

__attribute__((visibility("default"))) kmod_start_func_t *_realmain = rtl8188ee_module_start;
__attribute__((visibility("default"))) kmod_stop_func_t *_antimain = rtl8188ee_module_stop;

__attribute__((visibility("default"))) int _kext_apple_cc = __APPLE_CC__;
