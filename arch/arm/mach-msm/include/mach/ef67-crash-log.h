#ifndef __MACH_EF67_CRASH_LOG_H
#define __MACH_EF67_CRASH_LOG_H

#include <linux/init.h>

#ifdef CONFIG_EF67_CRASH_LOG
void __init ef67_crash_log_init(void);
#else
static inline void ef67_crash_log_init(void) { }
#endif

#endif
