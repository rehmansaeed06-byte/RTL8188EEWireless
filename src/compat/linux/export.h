/* SPDX-License-Identifier: GPL-2.0 OR BSD-3-Clause */
#ifndef _RTW88_COMPAT_EXPORT_H
#define _RTW88_COMPAT_EXPORT_H

/*
 * Real rtlwifi source files sometimes #include <linux/export.h> directly
 * (rather than pulling EXPORT_SYMBOL in transitively via <linux/module.h>).
 * There is no separate kernel module symbol table in this single-binary
 * kext build, so EXPORT_SYMBOL / EXPORT_SYMBOL_GPL are already defined as
 * no-ops in module.h. Redirect here instead of redefining them, so this
 * header is safe to include alongside module.h in the same translation
 * unit without a duplicate-macro-definition error.
 */
#include "module.h"

#endif /* _RTW88_COMPAT_EXPORT_H */
