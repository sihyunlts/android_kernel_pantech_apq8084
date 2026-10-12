/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef MDSS_DSI_PANEL_EF67_H
#define MDSS_DSI_PANEL_EF67_H

#ifdef CONFIG_MACH_APQ8084_EF67
int mdss_dsi_ef67_init(struct device *dev, struct device_node *node,
		struct mdss_dsi_ctrl_pdata *ctrl);
int mdss_dsi_ef67_reset(struct mdss_dsi_ctrl_pdata *ctrl, int enable);
void mdss_dsi_ef67_backlight(struct mdss_dsi_ctrl_pdata *ctrl, u32 level);
int mdss_dsi_ef67_panel_on(struct mdss_dsi_ctrl_pdata *ctrl);
int mdss_dsi_ef67_get_cabc(struct mdss_panel_data *pdata);
int mdss_dsi_ef67_set_cabc(struct mdss_panel_data *pdata, bool enabled);
#else
static inline int mdss_dsi_ef67_init(struct device *dev,
		struct device_node *node, struct mdss_dsi_ctrl_pdata *ctrl)
{
	return 0;
}
static inline int mdss_dsi_ef67_reset(struct mdss_dsi_ctrl_pdata *ctrl,
		int enable)
{
	return 0;
}
static inline void mdss_dsi_ef67_backlight(struct mdss_dsi_ctrl_pdata *ctrl,
		u32 level)
{
}
static inline int mdss_dsi_ef67_panel_on(struct mdss_dsi_ctrl_pdata *ctrl)
{
	return 0;
}
#endif

#endif
