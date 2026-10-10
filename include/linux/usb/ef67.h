/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_USB_EF67_H
#define _LINUX_USB_EF67_H

#include <linux/types.h>

#ifdef CONFIG_USB_ANDROID_EF67_MANAGER
void ef67_usb_vbus(bool present);
#else
static inline void ef67_usb_vbus(bool present) { }
#endif

#endif
