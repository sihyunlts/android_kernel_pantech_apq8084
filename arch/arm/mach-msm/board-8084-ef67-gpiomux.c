/* SPDX-License-Identifier: GPL-2.0-only */
/* Pantech EF67 GPIO configuration. */

#include <linux/bug.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <mach/gpiomux.h>

static struct gpiomux_setting gpio_in_2ma_pull_up __initdata = {
	.func = GPIOMUX_FUNC_GPIO,
	.drv = GPIOMUX_DRV_2MA,
	.pull = GPIOMUX_PULL_UP,
	.dir = GPIOMUX_IN,
};

static struct gpiomux_setting gpio_in_2ma_pull_down __initdata = {
	.func = GPIOMUX_FUNC_GPIO,
	.drv = GPIOMUX_DRV_2MA,
	.pull = GPIOMUX_PULL_DOWN,
	.dir = GPIOMUX_IN,
};

static struct gpiomux_setting func1_8ma_no_pull __initdata = {
	.func = GPIOMUX_FUNC_1,
	.drv = GPIOMUX_DRV_8MA,
	.pull = GPIOMUX_PULL_NONE,
	.dir = GPIOMUX_IN,
};

static struct gpiomux_setting func3_2ma_no_pull __initdata = {
	.func = GPIOMUX_FUNC_3,
	.drv = GPIOMUX_DRV_2MA,
	.pull = GPIOMUX_PULL_NONE,
	.dir = GPIOMUX_IN,
};

static struct gpiomux_setting func2_16ma_no_pull __initdata = {
	.func = GPIOMUX_FUNC_2,
	.drv = GPIOMUX_DRV_16MA,
	.pull = GPIOMUX_PULL_NONE,
	.dir = GPIOMUX_IN,
};

static struct gpiomux_setting func1_6ma_no_pull __initdata = {
	.func = GPIOMUX_FUNC_1,
	.drv = GPIOMUX_DRV_6MA,
	.pull = GPIOMUX_PULL_NONE,
	.dir = GPIOMUX_IN,
};

static struct gpiomux_setting func2_8ma_no_pull __initdata = {
	.func = GPIOMUX_FUNC_2,
	.drv = GPIOMUX_DRV_8MA,
	.pull = GPIOMUX_PULL_NONE,
	.dir = GPIOMUX_IN,
};

static struct gpiomux_setting gpio_out_high_8ma_no_pull __initdata = {
	.func = GPIOMUX_FUNC_GPIO,
	.drv = GPIOMUX_DRV_8MA,
	.pull = GPIOMUX_PULL_NONE,
	.dir = GPIOMUX_OUT_HIGH,
};

static struct gpiomux_setting gpio_in_8ma_no_pull __initdata = {
	.func = GPIOMUX_FUNC_GPIO,
	.drv = GPIOMUX_DRV_8MA,
	.pull = GPIOMUX_PULL_NONE,
	.dir = GPIOMUX_IN,
};

static struct gpiomux_setting gpio_in_6ma_pull_up __initdata = {
	.func = GPIOMUX_FUNC_GPIO,
	.drv = GPIOMUX_DRV_6MA,
	.pull = GPIOMUX_PULL_UP,
	.dir = GPIOMUX_IN,
};

static struct gpiomux_setting gpio_in_6ma_pull_down __initdata = {
	.func = GPIOMUX_FUNC_GPIO,
	.drv = GPIOMUX_DRV_6MA,
	.pull = GPIOMUX_PULL_DOWN,
	.dir = GPIOMUX_IN,
};

static struct gpiomux_setting gpio_in_8ma_pull_down __initdata = {
	.func = GPIOMUX_FUNC_GPIO,
	.drv = GPIOMUX_DRV_8MA,
	.pull = GPIOMUX_PULL_DOWN,
	.dir = GPIOMUX_IN,
};

static struct gpiomux_setting gpio_out_high_2ma_pull_down __initdata = {
	.func = GPIOMUX_FUNC_GPIO,
	.drv = GPIOMUX_DRV_2MA,
	.pull = GPIOMUX_PULL_DOWN,
	.dir = GPIOMUX_OUT_HIGH,
};

static struct gpiomux_setting gpio_out_high_2ma_no_pull __initdata = {
	.func = GPIOMUX_FUNC_GPIO,
	.drv = GPIOMUX_DRV_2MA,
	.pull = GPIOMUX_PULL_NONE,
	.dir = GPIOMUX_OUT_HIGH,
};

static struct gpiomux_setting func1_2ma_pull_down __initdata = {
	.func = GPIOMUX_FUNC_1,
	.drv = GPIOMUX_DRV_2MA,
	.pull = GPIOMUX_PULL_DOWN,
	.dir = GPIOMUX_IN,
};

static struct gpiomux_setting gpio_out_low_2ma_pull_down __initdata = {
	.func = GPIOMUX_FUNC_GPIO,
	.drv = GPIOMUX_DRV_2MA,
	.pull = GPIOMUX_PULL_DOWN,
	.dir = GPIOMUX_OUT_LOW,
};

static struct gpiomux_setting gpio_out_low_8ma_no_pull __initdata = {
	.func = GPIOMUX_FUNC_GPIO,
	.drv = GPIOMUX_DRV_8MA,
	.pull = GPIOMUX_PULL_NONE,
	.dir = GPIOMUX_OUT_LOW,
};

static struct gpiomux_setting func1_2ma_pull_up __initdata = {
	.func = GPIOMUX_FUNC_1,
	.drv = GPIOMUX_DRV_2MA,
	.pull = GPIOMUX_PULL_UP,
	.dir = GPIOMUX_IN,
};

static struct gpiomux_setting func1_16ma_pull_down __initdata = {
	.func = GPIOMUX_FUNC_1,
	.drv = GPIOMUX_DRV_16MA,
	.pull = GPIOMUX_PULL_DOWN,
	.dir = GPIOMUX_IN,
};

static struct gpiomux_setting gpio_out_high_8ma_pull_up __initdata = {
	.func = GPIOMUX_FUNC_GPIO,
	.drv = GPIOMUX_DRV_8MA,
	.pull = GPIOMUX_PULL_UP,
	.dir = GPIOMUX_OUT_HIGH,
};

static struct gpiomux_setting gpio_in_8ma_pull_up __initdata = {
	.func = GPIOMUX_FUNC_GPIO,
	.drv = GPIOMUX_DRV_8MA,
	.pull = GPIOMUX_PULL_UP,
	.dir = GPIOMUX_IN,
};

static struct gpiomux_setting gpio_out_high_16ma_pull_up __initdata = {
	.func = GPIOMUX_FUNC_GPIO,
	.drv = GPIOMUX_DRV_16MA,
	.pull = GPIOMUX_PULL_UP,
	.dir = GPIOMUX_OUT_HIGH,
};

static struct gpiomux_setting gpio_in_2ma_no_pull __initdata = {
	.func = GPIOMUX_FUNC_GPIO,
	.drv = GPIOMUX_DRV_2MA,
	.pull = GPIOMUX_PULL_NONE,
	.dir = GPIOMUX_IN,
};

static struct gpiomux_setting func1_2ma_no_pull __initdata = {
	.func = GPIOMUX_FUNC_1,
	.drv = GPIOMUX_DRV_2MA,
	.pull = GPIOMUX_PULL_NONE,
	.dir = GPIOMUX_IN,
};

static struct gpiomux_setting gpio_out_low_2ma_no_pull __initdata = {
	.func = GPIOMUX_FUNC_GPIO,
	.drv = GPIOMUX_DRV_2MA,
	.pull = GPIOMUX_PULL_NONE,
	.dir = GPIOMUX_OUT_LOW,
};

static struct gpiomux_setting func2_2ma_pull_up __initdata = {
	.func = GPIOMUX_FUNC_2,
	.drv = GPIOMUX_DRV_2MA,
	.pull = GPIOMUX_PULL_UP,
	.dir = GPIOMUX_IN,
};

static struct gpiomux_setting gpio_in_8ma_keeper __initdata = {
	.func = GPIOMUX_FUNC_GPIO,
	.drv = GPIOMUX_DRV_8MA,
	.pull = GPIOMUX_PULL_KEEPER,
	.dir = GPIOMUX_IN,
};

static struct gpiomux_setting gpio_in_2ma_keeper __initdata = {
	.func = GPIOMUX_FUNC_GPIO,
	.drv = GPIOMUX_DRV_2MA,
	.pull = GPIOMUX_PULL_KEEPER,
	.dir = GPIOMUX_IN,
};

static struct msm_gpiomux_config ef67_led_configs[] __initdata = {
	{
		.gpio = 92,
		.settings = {
			[GPIOMUX_ACTIVE] = &gpio_in_2ma_pull_up,
			[GPIOMUX_SUSPENDED] = &gpio_in_2ma_pull_up,
		},
	},
	{
		.gpio = 103,
		.settings = {
			[GPIOMUX_ACTIVE] = &gpio_in_2ma_pull_down,
			[GPIOMUX_SUSPENDED] = &gpio_in_2ma_pull_down,
		},
	},
};

static struct msm_gpiomux_config ef67_blsp_configs[] __initdata = {
	{
		.gpio = 0,
		.settings = {
			[GPIOMUX_SUSPENDED] = &func1_8ma_no_pull,
		},
	},
	{
		.gpio = 1,
		.settings = {
			[GPIOMUX_SUSPENDED] = &func1_8ma_no_pull,
		},
	},
	{
		.gpio = 3,
		.settings = {
			[GPIOMUX_SUSPENDED] = &func1_8ma_no_pull,
		},
	},
	{
		.gpio = 2,
		.settings = {
			[GPIOMUX_SUSPENDED] = &func1_8ma_no_pull,
		},
	},
	{
		.gpio = 4,
		.settings = {
			[GPIOMUX_SUSPENDED] = &func1_8ma_no_pull,
		},
	},
	{
		.gpio = 5,
		.settings = {
			[GPIOMUX_SUSPENDED] = &func1_8ma_no_pull,
		},
	},
	{
		.gpio = 6,
		.settings = {
			[GPIOMUX_SUSPENDED] = &func1_8ma_no_pull,
		},
	},
	{
		.gpio = 7,
		.settings = {
			[GPIOMUX_SUSPENDED] = &func1_8ma_no_pull,
		},
	},
	{
		.gpio = 10,
		.settings = {
			[GPIOMUX_SUSPENDED] = &func3_2ma_no_pull,
		},
	},
	{
		.gpio = 11,
		.settings = {
			[GPIOMUX_SUSPENDED] = &func3_2ma_no_pull,
		},
	},
	{
		.gpio = 41,
		.settings = {
			[GPIOMUX_SUSPENDED] = &func3_2ma_no_pull,
		},
	},
	{
		.gpio = 42,
		.settings = {
			[GPIOMUX_SUSPENDED] = &func3_2ma_no_pull,
		},
	},
	{
		.gpio = 51,
		.settings = {
			[GPIOMUX_SUSPENDED] = &func2_16ma_no_pull,
		},
	},
	{
		.gpio = 52,
		.settings = {
			[GPIOMUX_SUSPENDED] = &func2_16ma_no_pull,
		},
	},
	{
		.gpio = 61,
		.settings = {
			[GPIOMUX_SUSPENDED] = &func3_2ma_no_pull,
		},
	},
	{
		.gpio = 62,
		.settings = {
			[GPIOMUX_SUSPENDED] = &func3_2ma_no_pull,
		},
	},
	{
		.gpio = 65,
		.settings = {
			[GPIOMUX_SUSPENDED] = &func3_2ma_no_pull,
		},
	},
	{
		.gpio = 66,
		.settings = {
			[GPIOMUX_SUSPENDED] = &func3_2ma_no_pull,
		},
	},
	{
		.gpio = 116,
		.settings = {
			[GPIOMUX_SUSPENDED] = &func1_6ma_no_pull,
		},
	},
	{
		.gpio = 130,
		.settings = {
			[GPIOMUX_ACTIVE] = &func2_8ma_no_pull,
			[GPIOMUX_SUSPENDED] = &gpio_in_2ma_pull_down,
		},
	},
	{
		.gpio = 131,
		.settings = {
			[GPIOMUX_ACTIVE] = &func2_8ma_no_pull,
			[GPIOMUX_SUSPENDED] = &gpio_in_2ma_pull_down,
		},
	},
	{
		.gpio = 132,
		.settings = {
			[GPIOMUX_ACTIVE] = &func2_8ma_no_pull,
			[GPIOMUX_SUSPENDED] = &gpio_in_2ma_pull_down,
		},
	},
	{
		.gpio = 133,
		.settings = {
			[GPIOMUX_ACTIVE] = &func2_8ma_no_pull,
			[GPIOMUX_SUSPENDED] = &gpio_in_2ma_pull_down,
		},
	},
};

static struct msm_gpiomux_config ef67_es705_configs[] __initdata = {
	{
		.gpio = 59,
		.settings = {
			[GPIOMUX_SUSPENDED] = &func2_8ma_no_pull,
		},
	},
	{
		.gpio = 60,
		.settings = {
			[GPIOMUX_SUSPENDED] = &func2_8ma_no_pull,
		},
	},
	{
		.gpio = 119,
		.settings = {
			[GPIOMUX_ACTIVE] = &gpio_out_high_8ma_no_pull,
			[GPIOMUX_SUSPENDED] = &gpio_out_high_8ma_no_pull,
		},
	},
	{
		.gpio = 121,
		.settings = {
			[GPIOMUX_ACTIVE] = &gpio_in_8ma_no_pull,
			[GPIOMUX_SUSPENDED] = &gpio_in_8ma_no_pull,
		},
	},
	{
		.gpio = 120,
		.settings = {
			[GPIOMUX_ACTIVE] = &gpio_in_8ma_no_pull,
			[GPIOMUX_SUSPENDED] = &gpio_in_8ma_no_pull,
		},
	},
};

static struct msm_gpiomux_config ef67_uart6_configs[] __initdata = {
	{
		.gpio = 43,
		.settings = {
			[GPIOMUX_SUSPENDED] = &func2_16ma_no_pull,
		},
	},
	{
		.gpio = 44,
		.settings = {
			[GPIOMUX_SUSPENDED] = &func2_16ma_no_pull,
		},
	},
	{
		.gpio = 45,
		.settings = {
			[GPIOMUX_SUSPENDED] = &func2_16ma_no_pull,
		},
	},
	{
		.gpio = 46,
		.settings = {
			[GPIOMUX_SUSPENDED] = &func2_16ma_no_pull,
		},
	},
};

static struct msm_gpiomux_config ef67_touch_configs[] __initdata = {
	{
		.gpio = 143,
		.settings = {
			[GPIOMUX_ACTIVE] = &gpio_in_6ma_pull_up,
			[GPIOMUX_SUSPENDED] = &gpio_in_6ma_pull_down,
		},
	},
	{
		.gpio = 145,
		.settings = {
			[GPIOMUX_ACTIVE] = &gpio_in_6ma_pull_up,
			[GPIOMUX_SUSPENDED] = &gpio_in_6ma_pull_down,
		},
	},
};

static struct msm_gpiomux_config ef67_modem_configs[] __initdata = {
	{
		.gpio = 110,
		.settings = {
			[GPIOMUX_SUSPENDED] = &gpio_in_8ma_no_pull,
		},
	},
	{
		.gpio = 109,
		.settings = {
			[GPIOMUX_SUSPENDED] = &gpio_in_8ma_pull_down,
		},
	},
	{
		.gpio = 111,
		.settings = {
			[GPIOMUX_SUSPENDED] = &gpio_in_8ma_pull_down,
		},
	},
	{
		.gpio = 112,
		.settings = {
			[GPIOMUX_SUSPENDED] = &gpio_in_8ma_no_pull,
		},
	},
	{
		.gpio = 128,
		.settings = {
			[GPIOMUX_SUSPENDED] = &gpio_in_8ma_no_pull,
		},
	},
	{
		.gpio = 108,
		.settings = {
			[GPIOMUX_SUSPENDED] = &gpio_in_8ma_pull_down,
		},
	},
	{
		.gpio = 113,
		.settings = {
			[GPIOMUX_SUSPENDED] = &gpio_in_8ma_no_pull,
		},
	},
};

/*
 * GPIO26/96/97 belong to the panel power sequence. Do not install gpiomux
 * settings for them: request/free would otherwise raise BL/reset/VDDIO
 * outside that sequence. Keep bootloader levels until the panel takes over.
 */
static struct msm_gpiomux_config ef67_display_configs[] __initdata = {
	{
		.gpio = 86,
		.settings = {
			[GPIOMUX_SUSPENDED] = &gpio_in_2ma_pull_down,
		},
	},
};

static struct msm_gpiomux_config ef67_display_power_configs[] __initdata = {
	{
		.gpio = 93,
		.settings = {
			[GPIOMUX_ACTIVE] = &gpio_out_high_2ma_no_pull,
			[GPIOMUX_SUSPENDED] = &gpio_out_high_2ma_pull_down,
		},
	},
};

static struct msm_gpiomux_config ef67_auxpcm_configs[] __initdata = {
	{
		.gpio = 87,
		.settings = {
			[GPIOMUX_ACTIVE] = &func1_8ma_no_pull,
			[GPIOMUX_SUSPENDED] = &func1_2ma_pull_down,
		},
	},
	{
		.gpio = 88,
		.settings = {
			[GPIOMUX_ACTIVE] = &func1_8ma_no_pull,
			[GPIOMUX_SUSPENDED] = &func1_2ma_pull_down,
		},
	},
	{
		.gpio = 89,
		.settings = {
			[GPIOMUX_ACTIVE] = &func1_8ma_no_pull,
			[GPIOMUX_SUSPENDED] = &func1_2ma_pull_down,
		},
	},
	{
		.gpio = 90,
		.settings = {
			[GPIOMUX_ACTIVE] = &func1_8ma_no_pull,
			[GPIOMUX_SUSPENDED] = &func1_2ma_pull_down,
		},
	},
};

static struct msm_gpiomux_config ef67_hsic_configs[] __initdata = {
	{
		.gpio = 134,
		.settings = {
			[GPIOMUX_ACTIVE] = &func1_8ma_no_pull,
			[GPIOMUX_SUSPENDED] = &gpio_out_low_2ma_pull_down,
		},
	},
	{
		.gpio = 135,
		.settings = {
			[GPIOMUX_ACTIVE] = &func1_8ma_no_pull,
			[GPIOMUX_SUSPENDED] = &gpio_out_low_2ma_pull_down,
		},
	},
	{
		.gpio = 107,
		.settings = {
			[GPIOMUX_ACTIVE] = &gpio_in_8ma_pull_down,
			[GPIOMUX_SUSPENDED] = &gpio_in_2ma_pull_down,
		},
	},
	{
		.gpio = 106,
		.settings = {
			[GPIOMUX_ACTIVE] = &gpio_out_low_8ma_no_pull,
			[GPIOMUX_SUSPENDED] = &gpio_out_low_2ma_pull_down,
		},
	},
	{
		.gpio = 108,
		.settings = {
			[GPIOMUX_SUSPENDED] = &gpio_in_8ma_pull_down,
		},
	},
};

static struct msm_gpiomux_config ef67_hdmi_configs[] __initdata = {
	{
		.gpio = 31,
		.settings = {
			[GPIOMUX_ACTIVE] = &func1_2ma_pull_up,
			[GPIOMUX_SUSPENDED] = &gpio_in_2ma_pull_down,
		},
	},
	{
		.gpio = 32,
		.settings = {
			[GPIOMUX_ACTIVE] = &func1_2ma_pull_up,
			[GPIOMUX_SUSPENDED] = &gpio_in_2ma_pull_down,
		},
	},
	{
		.gpio = 33,
		.settings = {
			[GPIOMUX_ACTIVE] = &func1_2ma_pull_up,
			[GPIOMUX_SUSPENDED] = &gpio_in_2ma_pull_down,
		},
	},
	{
		.gpio = 34,
		.settings = {
			[GPIOMUX_ACTIVE] = &func1_16ma_pull_down,
			[GPIOMUX_SUSPENDED] = &gpio_in_2ma_pull_down,
		},
	},
	{
		.gpio = 83,
		.settings = {
			[GPIOMUX_ACTIVE] = &gpio_out_high_8ma_pull_up,
			[GPIOMUX_SUSPENDED] = &gpio_in_2ma_pull_down,
		},
	},
	{
		.gpio = 85,
		.settings = {
			[GPIOMUX_ACTIVE] = &gpio_out_low_2ma_pull_down,
			[GPIOMUX_SUSPENDED] = &gpio_in_2ma_pull_down,
		},
	},
};

static struct msm_gpiomux_config ef67_haptic_configs[] __initdata = {
	{
		.gpio = 48,
		.settings = {
			[GPIOMUX_ACTIVE] = &gpio_in_8ma_pull_up,
			[GPIOMUX_SUSPENDED] = &gpio_in_2ma_pull_down,
		},
	},
};

static struct msm_gpiomux_config ef67_wlan_configs[] __initdata = {
	{
		.gpio = 82,
		.settings = {
			[GPIOMUX_ACTIVE] = &gpio_out_high_16ma_pull_up,
			[GPIOMUX_SUSPENDED] = &gpio_out_high_16ma_pull_up,
		},
	},
};

static struct msm_gpiomux_config ef67_sd_detect_configs[] __initdata = {
	{
		.gpio = 122,
		.settings = {
			[GPIOMUX_ACTIVE] = &gpio_in_2ma_no_pull,
			[GPIOMUX_SUSPENDED] = &gpio_in_2ma_pull_up,
		},
	},
};

static struct msm_gpiomux_config ef67_camera_configs[] __initdata = {
	{
		.gpio = 15,
		.settings = {
			[GPIOMUX_ACTIVE] = &func1_2ma_no_pull,
			[GPIOMUX_SUSPENDED] = &func1_2ma_pull_down,
		},
	},
	{
		.gpio = 17,
		.settings = {
			[GPIOMUX_ACTIVE] = &func1_2ma_no_pull,
			[GPIOMUX_SUSPENDED] = &func1_2ma_pull_down,
		},
	},
	{
		.gpio = 19,
		.settings = {
			[GPIOMUX_ACTIVE] = &func1_2ma_no_pull,
			[GPIOMUX_SUSPENDED] = &gpio_in_2ma_no_pull,
		},
	},
	{
		.gpio = 20,
		.settings = {
			[GPIOMUX_ACTIVE] = &func1_2ma_no_pull,
			[GPIOMUX_SUSPENDED] = &gpio_in_2ma_no_pull,
		},
	},
	{
		.gpio = 21,
		.settings = {
			[GPIOMUX_ACTIVE] = &func1_2ma_no_pull,
			[GPIOMUX_SUSPENDED] = &gpio_in_2ma_no_pull,
		},
	},
	{
		.gpio = 22,
		.settings = {
			[GPIOMUX_ACTIVE] = &func1_2ma_no_pull,
			[GPIOMUX_SUSPENDED] = &gpio_in_2ma_no_pull,
		},
	},
	{
		.gpio = 25,
		.settings = {
			[GPIOMUX_ACTIVE] = &gpio_in_2ma_no_pull,
			[GPIOMUX_SUSPENDED] = &gpio_out_low_2ma_no_pull,
		},
	},
	{
		.gpio = 36,
		.settings = {
			[GPIOMUX_ACTIVE] = &gpio_in_2ma_no_pull,
			[GPIOMUX_SUSPENDED] = &gpio_out_low_2ma_no_pull,
		},
	},
	{
		.gpio = 80,
		.settings = {
			[GPIOMUX_ACTIVE] = &gpio_in_2ma_no_pull,
			[GPIOMUX_SUSPENDED] = &gpio_out_low_2ma_no_pull,
		},
	},
	{
		.gpio = 83,
		.settings = {
			[GPIOMUX_ACTIVE] = &gpio_in_2ma_no_pull,
			[GPIOMUX_SUSPENDED] = &gpio_out_low_2ma_no_pull,
		},
	},
	{
		.gpio = 125,
		.settings = {
			[GPIOMUX_ACTIVE] = &gpio_in_2ma_no_pull,
			[GPIOMUX_SUSPENDED] = &gpio_out_low_2ma_no_pull,
		},
	},
	{
		.gpio = 126,
		.settings = {
			[GPIOMUX_ACTIVE] = &gpio_in_2ma_no_pull,
			[GPIOMUX_SUSPENDED] = &gpio_out_low_2ma_no_pull,
		},
	},
};

static struct msm_gpiomux_config ef67_pcie_configs[] __initdata = {
	{
		.gpio = 68,
		.settings = {
			[GPIOMUX_SUSPENDED] = &func2_2ma_pull_up,
		},
	},
	{
		.gpio = 141,
		.settings = {
			[GPIOMUX_SUSPENDED] = &func1_2ma_pull_up,
		},
	},
};

static struct msm_gpiomux_config ef67_nfc_configs[] __initdata = {
	{
		.gpio = 29,
		.settings = {
			[GPIOMUX_SUSPENDED] = &func3_2ma_no_pull,
		},
	},
	{
		.gpio = 30,
		.settings = {
			[GPIOMUX_SUSPENDED] = &func3_2ma_no_pull,
		},
	},
	{
		.gpio = 56,
		.settings = {
			[GPIOMUX_ACTIVE] = &gpio_in_8ma_keeper,
			[GPIOMUX_SUSPENDED] = &gpio_in_2ma_keeper,
		},
	},
	{
		.gpio = 57,
		.settings = {
			[GPIOMUX_ACTIVE] = &gpio_in_8ma_keeper,
			[GPIOMUX_SUSPENDED] = &gpio_in_2ma_keeper,
		},
	},
};

void __init apq8084_init_gpiomux(void)
{
	int rc;

	rc = msm_gpiomux_init_dt();
	if (rc) {
		pr_err("%s failed %d\n", __func__, rc);
		return;
	}

	/* Keep the stock installation order for pins shared by groups. */
	msm_gpiomux_install(ef67_led_configs,
			ARRAY_SIZE(ef67_led_configs));
	msm_gpiomux_install(ef67_blsp_configs,
			ARRAY_SIZE(ef67_blsp_configs));
	msm_gpiomux_install(ef67_es705_configs,
			ARRAY_SIZE(ef67_es705_configs));
	msm_gpiomux_install(ef67_uart6_configs,
			ARRAY_SIZE(ef67_uart6_configs));
	msm_gpiomux_install(ef67_touch_configs,
			ARRAY_SIZE(ef67_touch_configs));
	msm_gpiomux_install(ef67_modem_configs,
			ARRAY_SIZE(ef67_modem_configs));
	msm_gpiomux_install(ef67_display_configs,
			ARRAY_SIZE(ef67_display_configs));
	msm_gpiomux_install(ef67_display_power_configs,
			ARRAY_SIZE(ef67_display_power_configs));
	msm_gpiomux_install(ef67_auxpcm_configs,
			ARRAY_SIZE(ef67_auxpcm_configs));
	msm_gpiomux_install(ef67_hsic_configs,
			ARRAY_SIZE(ef67_hsic_configs));
	msm_gpiomux_install(ef67_hdmi_configs,
			ARRAY_SIZE(ef67_hdmi_configs));
	msm_gpiomux_install(ef67_haptic_configs,
			ARRAY_SIZE(ef67_haptic_configs));
	msm_gpiomux_install(ef67_wlan_configs,
			ARRAY_SIZE(ef67_wlan_configs));
	msm_gpiomux_install(ef67_sd_detect_configs,
			ARRAY_SIZE(ef67_sd_detect_configs));
	msm_gpiomux_install(ef67_camera_configs,
			ARRAY_SIZE(ef67_camera_configs));
	msm_gpiomux_install(ef67_pcie_configs,
			ARRAY_SIZE(ef67_pcie_configs));
	msm_gpiomux_install(ef67_nfc_configs,
			ARRAY_SIZE(ef67_nfc_configs));
}
