/*
 * SPDX-License-Identifier: Apache-2.0 OR GPL-2.0-or-later
 * See LICENSE, LICENSE.Apache-2.0, and LICENSE.GPL-2.0-or-later.
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>

extern const char *PLUGIN_NAME;
extern const char *PLUGIN_VERSION;

void obs_log(int log_level, const char *format, ...);
extern void blogva(int log_level, const char *format, va_list args);

#ifdef __cplusplus
}
#endif
