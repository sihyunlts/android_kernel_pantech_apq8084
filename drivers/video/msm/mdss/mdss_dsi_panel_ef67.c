/*
 * EF67 R63419 panel power and backlight control.
 * GPIO order and DCS packets follow IM-A930S S0226114.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2.
 */

#include <linux/delay.h>
#include <linux/gpio.h>
#include <linux/of_gpio.h>
#include <linux/slab.h>

#include "mdss_dsi.h"
#include "mdss_dsi_panel_ef67.h"

enum ef67_panel_gpio {
	EF67_BACKLIGHT,
	EF67_RESET,
	EF67_VDDIO,
	EF67_VCIP,
	EF67_VCIN,
	EF67_GPIO_COUNT,
};

struct mdss_dsi_ef67_panel {
	struct mutex lock;
	int gpio[EF67_GPIO_COUNT];
	bool powered;
	bool dimming_enabled;
};

static const char * const ef67_gpio_properties[] = {
	"qcom,mdss-bl-en-gpio",
	"qcom,mdss-reset-gpio",
	"qcom,mdss-vddio-reg-gpio",
	"qcom,mdss-vcip-en-gpio",
	"qcom,mdss-vcin-en-gpio",
};

int mdss_dsi_ef67_init(struct device *dev, struct device_node *node,
		struct mdss_dsi_ctrl_pdata *ctrl)
{
	struct mdss_dsi_ef67_panel *panel;
	int i, rc;

	/* Other panels in an EF67 build still use the generic CAF path. */
	if (!of_find_property(node, "qcom,mdss-vcip-en-gpio", NULL))
		return 0;
	if (!ctrl->on_cmds.cmd_cnt || !ctrl->off_cmds.cmd_cnt)
		return -EINVAL;

	panel = devm_kzalloc(dev, sizeof(*panel), GFP_KERNEL);
	if (!panel)
		return -ENOMEM;
	mutex_init(&panel->lock);

	ctrl->disp_en_gpio = -EINVAL;
	ctrl->rst_gpio = -EINVAL;
	ctrl->bklt_en_gpio = -EINVAL;
	ctrl->mode_gpio = -EINVAL;
	ctrl->disp_te_gpio = -EINVAL;

	/* Both DSI nodes describe one physical panel. DSI0 owns its pins. */
	if (ctrl->panel_data.panel_info.pdest != DISPLAY_1) {
		ctrl->ef67_panel = panel;
		return 0;
	}

	for (i = 0; i < EF67_GPIO_COUNT; i++) {
		panel->gpio[i] = of_get_named_gpio(node,
						ef67_gpio_properties[i], 0);
		if (!gpio_is_valid(panel->gpio[i]))
			return panel->gpio[i] < 0 ? panel->gpio[i] : -EINVAL;
		rc = devm_gpio_request(dev, panel->gpio[i],
						ef67_gpio_properties[i]);
		if (rc)
			return rc;
	}

	ctrl->disp_te_gpio = of_get_named_gpio(node, "qcom,mdss-te-gpio", 0);
	if (!gpio_is_valid(ctrl->disp_te_gpio))
		return ctrl->disp_te_gpio < 0 ? ctrl->disp_te_gpio : -EINVAL;
	rc = devm_gpio_request(dev, ctrl->disp_te_gpio, "ef67_display_te");
	if (rc)
		return rc;
	rc = gpio_tlmm_config(GPIO_CFG(ctrl->disp_te_gpio, 1, GPIO_CFG_INPUT,
			GPIO_CFG_PULL_DOWN, GPIO_CFG_2MA), GPIO_CFG_ENABLE);
	if (rc)
		return rc;
	rc = gpio_direction_input(ctrl->disp_te_gpio);
	if (rc)
		return rc;

	panel->powered = ctrl->panel_data.panel_info.cont_splash_enabled;
	/* Requesting pins must not interrupt bootloader power sequencing. */
	ctrl->ef67_panel = panel;
	return 0;
}

static void ef67_panel_power_off(struct mdss_dsi_ef67_panel *panel)
{
	gpio_set_value(panel->gpio[EF67_BACKLIGHT], 0);
	gpio_set_value(panel->gpio[EF67_RESET], 0);
	msleep(1);
	gpio_set_value(panel->gpio[EF67_VCIN], 0);
	msleep(1);
	gpio_set_value(panel->gpio[EF67_VCIP], 0);
	msleep(120);
	gpio_set_value(panel->gpio[EF67_VDDIO], 0);
	panel->powered = false;
	panel->dimming_enabled = false;
}

static int ef67_panel_output(struct mdss_dsi_ef67_panel *panel,
		enum ef67_panel_gpio pin, int value)
{
	int rc;

	/* Set the intended level at its sequence step, never from input readback. */
	rc = gpio_direction_output(panel->gpio[pin], value);
	if (rc)
		return rc;

	/* These three active pin settings were previously applied by gpiomux. */
	if (pin <= EF67_VDDIO)
		return gpio_tlmm_config(GPIO_CFG(panel->gpio[pin], 0,
				GPIO_CFG_OUTPUT, GPIO_CFG_NO_PULL,
				pin == EF67_RESET ? GPIO_CFG_8MA : GPIO_CFG_2MA),
				GPIO_CFG_ENABLE);

	/* Bias enables retain the bootloader pin mux, pull and drive settings. */
	return 0;
}

int mdss_dsi_ef67_reset(struct mdss_dsi_ctrl_pdata *ctrl, int enable)
{
	struct mdss_dsi_ef67_panel *panel = ctrl->ef67_panel;
	int rc = 0;

	if (ctrl->panel_data.panel_info.pdest != DISPLAY_1)
		return 0;

	mutex_lock(&panel->lock);
	if (enable && !panel->powered) {
		rc = ef67_panel_output(panel, EF67_BACKLIGHT, 0);
		if (rc)
			goto power_off;
		rc = ef67_panel_output(panel, EF67_RESET, 0);
		if (rc)
			goto power_off;
		rc = ef67_panel_output(panel, EF67_VDDIO, 1);
		if (rc)
			goto power_off;
		usleep_range(2000, 2000);
		rc = ef67_panel_output(panel, EF67_VCIP, 1);
		if (rc)
			goto power_off;
		usleep_range(1000, 1000);
		rc = ef67_panel_output(panel, EF67_VCIN, 1);
		if (rc)
			goto power_off;
		usleep_range(2000, 2000);
		gpio_set_value(panel->gpio[EF67_RESET], 1);
		usleep_range(12000, 15000);
		panel->powered = true;
		ctrl->ctrl_state &= ~CTRL_STATE_PANEL_INIT;
	} else if (!enable && panel->powered) {
		ef67_panel_power_off(panel);
	}
	goto out;
power_off:
	/* A partial enable must not leave the bias rails powered. */
	ef67_panel_power_off(panel);
out:
	mutex_unlock(&panel->lock);
	return rc;
}

static int ef67_backlight_command(struct mdss_dsi_ctrl_pdata *ctrl,
		char command, u8 value, u8 type)
{
	char payload[2] = { command, value };
	struct dsi_cmd_desc cmd = {
		{ type, 1, 0, 0, 1, sizeof(payload) }, payload,
	};
	struct dcs_cmd_req req = {
		.cmds = &cmd,
		.cmds_cnt = 1,
		.flags = CMD_REQ_COMMIT | CMD_CLK_CTRL,
	};

	/* COMMIT consumes this packet synchronously under cmd_mutex. */
	usleep_range(1000, 1000);
	return mdss_dsi_cmdlist_put(ctrl, &req);
}

void mdss_dsi_ef67_backlight(struct mdss_dsi_ctrl_pdata *ctrl, u32 level)
{
	struct mdss_dsi_ef67_panel *panel = ctrl->ef67_panel;
	int rc = 0;

	if (ctrl->panel_data.panel_info.pdest != DISPLAY_1)
		return;
	level = min_t(u32, level, ctrl->bklt_max);

	mutex_lock(&panel->lock);
	if (!panel->powered)
		goto out;

	rc = gpio_direction_output(panel->gpio[EF67_BACKLIGHT], !!level);
	if (rc)
		goto out;
	/* Stock sends panel control packets through DSI0. */
	if (!panel->dimming_enabled) {
		rc = ef67_backlight_command(ctrl, 0x53, 0x0c,
						DTYPE_DCS_WRITE1);
		if (rc)
			goto out;
		panel->dimming_enabled = true;
	}
	rc = ef67_backlight_command(ctrl, 0x51, level, DTYPE_DCS_LWRITE);
out:
	if (rc) {
		gpio_set_value(panel->gpio[EF67_BACKLIGHT], 0);
		pr_err("EF67 backlight update failed: %d\n", rc);
	}
	mutex_unlock(&panel->lock);
}

void mdss_dsi_ef67_panel_on(struct mdss_dsi_ctrl_pdata *ctrl)
{
	/* Stock initialization sets 0x53=0x0c and CABC 0x55=0x03. */
	ctrl->ef67_panel->dimming_enabled = true;
}
