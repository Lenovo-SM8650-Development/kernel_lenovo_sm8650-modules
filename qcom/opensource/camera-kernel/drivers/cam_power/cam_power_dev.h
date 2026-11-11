/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef CAM_KIRBY_POWER_DEV_H
#define CAM_KIRBY_POWER_DEV_H

#include <linux/kconfig.h>
#include <linux/types.h>

#if IS_ENABLED(CONFIG_ARCH_KIRBY)
int cam_kirby_power_driver_init(void);
void cam_kirby_power_driver_exit(void);
int cam_kirby_ldo_power(unsigned int port, bool enable);
#endif

#endif
