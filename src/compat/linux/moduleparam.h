/* SPDX-License-Identifier: GPL-2.0 OR BSD-3-Clause */
#ifndef _RTW88_COMPAT_MODULEPARAM_H
#define _RTW88_COMPAT_MODULEPARAM_H

/*
 * Real rtlwifi source files sometimes #include <linux/moduleparam.h>
 * directly for module_param()/module_param_named()/etc. There is no
 * insmod-time parameter mechanism in this kext build, so these are
 * already defined as no-ops in module.h. Redirect here instead of
 * redefining them, so this header is safe to include alongside
 * module.h in the same translation unit without a duplicate-macro-
 * definition error.
 */
#include "module.h"

#endif /* _RTW88_COMPAT_MODULEPARAM_H */
