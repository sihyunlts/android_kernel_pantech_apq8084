/* Copyright (c) 2013-2014, The Linux Foundation. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */
#include <linux/module.h>
#include <linux/export.h>
#include "msm_led_flash.h"

#define FLASH_NAME "ti,lm3559"

static struct msm_led_flash_ctrl_t fctrl;
static struct i2c_driver lm3559_i2c_driver;

/* EF67S S0226114 uses software strobe with FLASH_NOW held low. */
static struct msm_camera_i2c_reg_array lm3559_init_array[] = {
	{0x10, 0x18},
};

static struct msm_camera_i2c_reg_array lm3559_off_array[] = {
	{0x10, 0x00},
};

static struct msm_camera_i2c_reg_array lm3559_release_array[] = {
	{0x10, 0x00},
};

static struct msm_camera_i2c_reg_array lm3559_low_array[] = {
	{0x10, 0x1A},
	{0xA0, 0x1B},
};

static struct msm_camera_i2c_reg_array lm3559_high_array[] = {
	{0x10, 0x1B},
	{0xB0, 0x55},
};

static void __exit msm_flash_lm3559_i2c_remove(void)
{
	i2c_del_driver(&lm3559_i2c_driver);
	return;
}

static const struct of_device_id lm3559_trigger_dt_match[] = {
	{.compatible = "ti,lm3559", .data = &fctrl},
	{}
};

MODULE_DEVICE_TABLE(of, lm3559_trigger_dt_match);

static const struct i2c_device_id lm3559_i2c_id[] = {
	{FLASH_NAME, (kernel_ulong_t)&fctrl},
	{ }
};

static int msm_flash_lm3559_i2c_probe(struct i2c_client *client,
		const struct i2c_device_id *id)
{
	if (!id) {
		pr_err("msm_flash_lm3559_i2c_probe: id is NULL");
		id = lm3559_i2c_id;
	}

	return msm_flash_i2c_probe(client, id);
}

static struct i2c_driver lm3559_i2c_driver = {
	.id_table = lm3559_i2c_id,
	.probe  = msm_flash_lm3559_i2c_probe,
	.remove = __exit_p(msm_flash_lm3559_i2c_remove),
	.driver = {
		.name = FLASH_NAME,
		.owner = THIS_MODULE,
		.of_match_table = lm3559_trigger_dt_match,
	},
};

static int msm_flash_lm3559_platform_probe(struct platform_device *pdev)
{
	const struct of_device_id *match;
	match = of_match_device(lm3559_trigger_dt_match, &pdev->dev);
	if (!match)
		return -EFAULT;
	return msm_flash_probe(pdev, match->data);
}

static struct platform_driver lm3559_platform_driver = {
	.probe = msm_flash_lm3559_platform_probe,
	.driver = {
		.name = "ti,lm3559",
		.owner = THIS_MODULE,
		.of_match_table = lm3559_trigger_dt_match,
	},
};

static int __init msm_flash_lm3559_init_module(void)
{
	int32_t rc = 0;
	rc = platform_driver_register(&lm3559_platform_driver);
	if (!rc)
		return rc;
	pr_debug("%s:%d rc %d\n", __func__, __LINE__, rc);
	return i2c_add_driver(&lm3559_i2c_driver);
}

static void __exit msm_flash_lm3559_exit_module(void)
{
	if (fctrl.pdev)
		platform_driver_unregister(&lm3559_platform_driver);
	else
		i2c_del_driver(&lm3559_i2c_driver);
}

static struct msm_camera_i2c_client lm3559_i2c_client = {
	.addr_type = MSM_CAMERA_I2C_BYTE_ADDR,
};

static struct msm_camera_i2c_reg_setting lm3559_init_setting = {
	.reg_setting = lm3559_init_array,
	.size = ARRAY_SIZE(lm3559_init_array),
	.addr_type = MSM_CAMERA_I2C_BYTE_ADDR,
	.data_type = MSM_CAMERA_I2C_BYTE_DATA,
	.delay = 0,
};

static struct msm_camera_i2c_reg_setting lm3559_off_setting = {
	.reg_setting = lm3559_off_array,
	.size = ARRAY_SIZE(lm3559_off_array),
	.addr_type = MSM_CAMERA_I2C_BYTE_ADDR,
	.data_type = MSM_CAMERA_I2C_BYTE_DATA,
	.delay = 0,
};

static struct msm_camera_i2c_reg_setting lm3559_release_setting = {
	.reg_setting = lm3559_release_array,
	.size = ARRAY_SIZE(lm3559_release_array),
	.addr_type = MSM_CAMERA_I2C_BYTE_ADDR,
	.data_type = MSM_CAMERA_I2C_BYTE_DATA,
	.delay = 0,
};

static struct msm_camera_i2c_reg_setting lm3559_low_setting = {
	.reg_setting = lm3559_low_array,
	.size = ARRAY_SIZE(lm3559_low_array),
	.addr_type = MSM_CAMERA_I2C_BYTE_ADDR,
	.data_type = MSM_CAMERA_I2C_BYTE_DATA,
	.delay = 0,
};

static struct msm_camera_i2c_reg_setting lm3559_high_setting = {
	.reg_setting = lm3559_high_array,
	.size = ARRAY_SIZE(lm3559_high_array),
	.addr_type = MSM_CAMERA_I2C_BYTE_ADDR,
	.data_type = MSM_CAMERA_I2C_BYTE_DATA,
	.delay = 0,
};

static struct msm_led_flash_reg_t lm3559_regs = {
	.init_setting = &lm3559_init_setting,
	.off_setting = &lm3559_off_setting,
	.low_setting = &lm3559_low_setting,
	.high_setting = &lm3559_high_setting,
	.release_setting = &lm3559_release_setting,
};

static struct msm_flash_fn_t lm3559_func_tbl = {
	.flash_get_subdev_id = msm_led_i2c_trigger_get_subdev_id,
	.flash_led_config = msm_led_i2c_trigger_config,
	.flash_led_init = msm_flash_led_init,
	.flash_led_release = msm_flash_led_release,
	.flash_led_off = msm_flash_led_off,
	.flash_led_low = msm_flash_led_low,
	.flash_led_high = msm_flash_led_high,
};

static struct msm_led_flash_ctrl_t fctrl = {
	.flash_i2c_client = &lm3559_i2c_client,
	.reg_setting = &lm3559_regs,
	.func_tbl = &lm3559_func_tbl,
	.use_i2c_strobe = true,
};

module_init(msm_flash_lm3559_init_module);
module_exit(msm_flash_lm3559_exit_module);
MODULE_DESCRIPTION("lm3559 FLASH");
MODULE_LICENSE("GPL v2");
