/* EF67 SiI8620 SPI, USB switch, HDMI and charging integration. */
#include <linux/module.h>
#include <linux/spi/spi.h>
#include <linux/of_gpio.h>
#include <linux/of_platform.h>
#include <linux/gpio.h>
#include <linux/regulator/consumer.h>
#include <linux/power_supply.h>
#include <linux/usb/msm_hsusb.h>
#include <linux/completion.h>
#include <linux/delay.h>
#include <linux/interrupt.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/hrtimer.h>
#include <linux/cdev.h>
#include "si_fw_macros.h"
#include "si_infoframe.h"
#include "si_edid.h"
#include "si_mhl_defs.h"
#include "si_mhl_tx_hw_drv_api.h"
#include "mhl_linux_tx.h"
#include "si_mhl_callback_api.h"
#include "si_8620_drv.h"
#include "si_8620_regs.h"
#include "platform.h"
#include "../mdss_hdmi_mhl.h"

#define SPI_BUFFER_SIZE (LOCAL_BLK_RCV_BUFFER_SIZE + 14)
#define BLOCK_REQUEST_SIZE (LOCAL_BLK_RCV_BUFFER_SIZE + 1)
#define DISCOVERY_TIMEOUT_MS 500 /* Stock waits 50 ticks at HZ=100. */

struct ef67_mhl {
	struct spi_device *spi;
	struct regulator *vdd;
	struct platform_device *hdmi;
	struct msm_hdmi_mhl_ops hdmi_ops;
	struct mhl_drv_info drv_info;
	struct usb_ext_notification usb_notify;
	struct power_supply supply;
	struct mutex connection_lock;
	struct mutex transfer_lock;
	struct completion discovery;
	struct work_struct disconnect_work;
	struct gpio gpios[7];
	u8 tx[SPI_BUFFER_SIZE];
	u8 rx[SPI_BUFFER_SIZE];
	u8 block_buffers[NUM_BLOCK_QUEUE_REQUESTS * BLOCK_REQUEST_SIZE];
	void (*usb_online)(void *, int);
	void *usb_context;
	bool powered;
	bool detected;
	bool usb_owned;
	bool removing;
	bool hdmi_registered;
	bool irq_wake_enabled;
	int current_max;
};

/* The EF67 has one transmitter. The Silicon Image core has board-wide APIs. */
static struct ef67_mhl *transmitter;
bool use_spi = true;
bool input_dev_rap = true;
bool input_dev_rcp = true;
bool input_dev_ucp = true;
bool wait_for_user_intr;
bool force_ocbus_for_ects;
int crystal_khz = 24000; /* S0226114 crystal_khz data symbol. */
int hdcp_content_type;

enum {
	GPIO_INTERRUPT,
	GPIO_POWER,
	GPIO_RESET,
	GPIO_SELECT_1,
	GPIO_SELECT_2,
	GPIO_SWITCH_POWER,
	GPIO_FW_WAKE,
};

static const char * const gpio_properties[] = {
	"mhl-intr-gpio", "mhl-pwr-gpio", "mhl-rst-gpio",
	"mhl-switch-sel-1-gpio", "mhl-switch-sel-2-gpio",
	"mhl-switch-pwr-gpio", "mhl-fw-wake-gpio",
};

static int spi_page(u16 address)
{
	switch (address >> 8) {
	case 0x72: return 0;
	case 0x7a: return 1;
	case 0x92: return 2;
	case 0x9a: return 3;
	case 0xba: return 4;
	case 0xc8: return 5;
	case 0xb2: return 6;
	case 0xc2: return 7;
	case 0xd2: return 8;
	default: return -EINVAL;
	}
}

int mhl_tx_write_reg_block(void *context, u16 address, u16 count, u8 *values)
{
	struct ef67_mhl *mhl = transmitter;
	int page = spi_page(address), ret;

	if (page < 0 || !count || count > LOCAL_BLK_RCV_BUFFER_SIZE)
		return -EINVAL;
	mutex_lock(&mhl->transfer_lock);
	mhl->tx[0] = 0x61;
	mhl->tx[1] = page;
	mhl->tx[2] = address;
	memcpy(mhl->tx + 3, values, count);
	ret = spi_write(mhl->spi, mhl->tx, count + 3);
	mutex_unlock(&mhl->transfer_lock);
	return ret;
}

int mhl_tx_read_reg_block(void *context, u16 address, u16 count, u8 *values)
{
	struct ef67_mhl *mhl = transmitter;
	struct spi_transfer transfers[2] = { };
	struct spi_message message;
	int page = spi_page(address), ret;
	int dummy = address == REG_DDC_DATA ? 11 : 5;

	if (page < 0 || !count || count > LOCAL_BLK_RCV_BUFFER_SIZE)
		return -EINVAL;
	mutex_lock(&mhl->transfer_lock);
	memset(mhl->tx, 0, 3 + dummy);
	mhl->tx[0] = address == REG_DDC_DATA ? 0x62 : 0x60;
	mhl->tx[1] = page;
	mhl->tx[2] = address;
	transfers[0].tx_buf = mhl->tx;
	transfers[0].len = 3 + dummy;
	transfers[1].rx_buf = mhl->rx;
	transfers[1].len = count;
	spi_message_init(&message);
	spi_message_add_tail(&transfers[0], &message);
	spi_message_add_tail(&transfers[1], &message);
	ret = spi_sync(mhl->spi, &message);
	if (!ret)
		memcpy(values, mhl->rx, count);
	mutex_unlock(&mhl->transfer_lock);
	return ret;
}

int mhl_tx_write_reg(void *context, u16 address, u8 value)
{
	return mhl_tx_write_reg_block(context, address, 1, &value);
}

int mhl_tx_read_reg(void *context, u16 address)
{
	u8 value;
	int ret = mhl_tx_read_reg_block(context, address, 1, &value);

	return ret ? ret : value;
}

int mhl_tx_modify_reg(void *context, u16 address, u8 mask, u8 value)
{
	int ret = mhl_tx_read_reg(context, address);
	u8 updated;

	if (ret < 0)
		return ret;
	updated = (ret & ~mask) | (value & mask);
	ret = mhl_tx_write_reg(context, address, updated);
	return ret ? ret : updated;
}

void si_mhl_tx_platform_get_block_buffer_info(struct block_buffer_info_t *info)
{
	info->buffer = transmitter->block_buffers;
	info->payload_offset = 1;
	info->req_size = BLOCK_REQUEST_SIZE;
}

int mhl_tx_write_block_spi_emsc(void *context, struct block_req *req)
{
	if (req->count > LOCAL_BLK_RCV_BUFFER_SIZE)
		return -EINVAL;
	req->platform_header[0] = 0x81;
	return spi_write(transmitter->spi, req->platform_header, req->count + 1);
}

int mhl_tx_read_spi_emsc(void *context, u16 count, u8 *values)
{
	struct ef67_mhl *mhl = transmitter;
	struct spi_transfer transfers[2] = { };
	struct spi_message message;
	int ret;

	if (!count || count > LOCAL_BLK_RCV_BUFFER_SIZE)
		return -EINVAL;
	mutex_lock(&mhl->transfer_lock);
	mhl->tx[0] = 0x80;
	mhl->tx[1] = 0;
	transfers[0].tx_buf = mhl->tx;
	transfers[0].len = 2;
	transfers[1].rx_buf = mhl->rx;
	transfers[1].len = count;
	spi_message_init(&message);
	spi_message_add_tail(&transfers[0], &message);
	spi_message_add_tail(&transfers[1], &message);
	ret = spi_sync(mhl->spi, &message);
	if (!ret)
		memcpy(values, mhl->rx, count);
	mutex_unlock(&mhl->transfer_lock);
	return ret;
}

static int spi_command(u8 command)
{
	struct ef67_mhl *mhl = transmitter;
	int ret;

	mutex_lock(&mhl->transfer_lock);
	mhl->tx[0] = command;
	ret = spi_write(mhl->spi, mhl->tx, 1);
	mutex_unlock(&mhl->transfer_lock);
	return ret;
}

void mhl_tx_clear_emsc_read_err(void *context)
{
	spi_command(0x07);
}

void platform_mhl_tx_hw_reset(u32 reset_period, u32 reset_delay)
{
	gpio_set_value(transmitter->gpios[GPIO_RESET].gpio, 0);
	msleep(1);
	gpio_set_value(transmitter->gpios[GPIO_RESET].gpio, 1);
	if (reset_delay)
		msleep(reset_delay);
	spi_command(0x06);
}

void mhl_platform_fw_wake(bool awake)
{
	gpio_set_value(transmitter->gpios[GPIO_FW_WAKE].gpio, awake);
}

int is_interrupt_asserted(void)
{
	return !gpio_get_value(transmitter->gpios[GPIO_INTERRUPT].gpio);
}

u32 platform_get_flags(void)
{
	return 0;
}

enum hpd_control_mode platform_get_hpd_control_mode(void)
{
	return HPD_CTRL_PUSH_PULL;
}

int mhl_platform_set_hpd(bool high)
{
	struct ef67_mhl *mhl = transmitter;

	return mhl->hdmi_ops.set_upstream_hpd(mhl->hdmi, high);
}

static enum power_supply_property mhl_properties[] = {
	POWER_SUPPLY_PROP_PRESENT,
	POWER_SUPPLY_PROP_ONLINE,
	POWER_SUPPLY_PROP_CURRENT_MAX,
};
static char *mhl_supplied_to[] = { "battery" };

static int mhl_get_property(struct power_supply *supply,
		enum power_supply_property property, union power_supply_propval *value)
{
	struct ef67_mhl *mhl = container_of(supply, struct ef67_mhl, supply);

	switch (property) {
	case POWER_SUPPLY_PROP_PRESENT:
		value->intval = ACCESS_ONCE(mhl->powered);
		break;
	case POWER_SUPPLY_PROP_ONLINE:
		value->intval = ACCESS_ONCE(mhl->powered) &&
			ACCESS_ONCE(mhl->current_max) > 0;
		break;
	case POWER_SUPPLY_PROP_CURRENT_MAX:
		value->intval = ACCESS_ONCE(mhl->current_max);
		break;
	default:
		return -EINVAL;
	}
	return 0;
}

void mhl_tx_vbus_control(enum vbus_power_state state)
{
	/* EF67's switch has no VBUS-output control. Charging is sink-supplied. */
	if (state == VBUS_ON) {
		transmitter->current_max = 0;
		power_supply_changed(&transmitter->supply);
	}
}

void mhl_tx_vbus_current_ctl(u16 current_ma)
{
	transmitter->current_max = current_ma * 1000;
	power_supply_changed(&transmitter->supply);
}

void mhl_platform_discovery_result(bool detected)
{
	transmitter->detected = detected;
	complete(&transmitter->discovery);
}

static void chip_power(struct ef67_mhl *mhl, bool on)
{
	gpio_set_value(mhl->gpios[GPIO_POWER].gpio, on);
	msleep(20);
	mhl_platform_fw_wake(on);
	mhl->powered = on;
}

/* connection_lock held; called outside the core's ISR semaphore. */
static void release_usb(struct ef67_mhl *mhl)
{
	if (mhl->powered) {
		disable_irq(mhl->drv_info.irq);
		mhl_handle_power_change_request(&mhl->spi->dev, false);
		mhl_platform_set_hpd(false);
		chip_power(mhl, false);
	}
	mhl->current_max = 0;
	power_supply_changed(&mhl->supply);
	if (mhl->usb_owned) {
		mhl->usb_online(mhl->usb_context, 0);
		mhl->usb_owned = false;
	}
	gpio_set_value(mhl->gpios[GPIO_SELECT_1].gpio, 0);
	gpio_set_value(mhl->gpios[GPIO_SELECT_2].gpio, 0);
}

static void disconnect_worker(struct work_struct *work)
{
	struct ef67_mhl *mhl = container_of(work, struct ef67_mhl, disconnect_work);

	mutex_lock(&mhl->connection_lock);
	release_usb(mhl);
	mutex_unlock(&mhl->connection_lock);
}

void mhl_platform_disconnected(void)
{
	mhl_platform_discovery_result(false);
	schedule_work(&transmitter->disconnect_work);
}

static int discover_mhl(void *context, int id,
		void (*online)(void *, int), void *usb_context)
{
	struct ef67_mhl *mhl = context;
	int ret = 1;

	if (id || !online)
		return id ? id : -EINVAL;
	cancel_work_sync(&mhl->disconnect_work);
	mutex_lock(&mhl->connection_lock);
	if (mhl->removing)
		goto out;
	INIT_COMPLETION(mhl->discovery);
	mhl->detected = false;
	mhl->usb_online = online;
	mhl->usb_context = usb_context;
	gpio_set_value(mhl->gpios[GPIO_SELECT_1].gpio, 1);
	gpio_set_value(mhl->gpios[GPIO_SELECT_2].gpio, 1);
	chip_power(mhl, true);
	ret = mhl_handle_power_change_request(&mhl->spi->dev, true);
	/* Probe and release_usb leave the IRQ disabled. */
	enable_irq(mhl->drv_info.irq);
	if (ret)
		goto failed;
	mhl->current_max = 500000;
	power_supply_changed(&mhl->supply);
	if (wait_for_completion_timeout(&mhl->discovery,
			msecs_to_jiffies(DISCOVERY_TIMEOUT_MS)) && mhl->detected) {
		mhl->usb_owned = true;
		online(usb_context, 1);
		ret = 0;
		goto out;
	}
failed:
	release_usb(mhl);
	ret = 1;
out:
	mutex_unlock(&mhl->connection_lock);
	return ret;
}

static int ef67_mhl_probe(struct spi_device *spi)
{
	struct ef67_mhl *mhl;
	struct device_node *hdmi_node;
	u32 voltages[2], currents[2];
	int ret, i;

	if (transmitter)
		return -EBUSY;
	mhl = devm_kzalloc(&spi->dev, sizeof(*mhl), GFP_KERNEL);
	if (!mhl)
		return -ENOMEM;
	mhl->spi = spi;
	mutex_init(&mhl->connection_lock);
	mutex_init(&mhl->transfer_lock);
	init_completion(&mhl->discovery);
	INIT_WORK(&mhl->disconnect_work, disconnect_worker);
	hdmi_node = of_parse_phandle(spi->dev.of_node, "qcom,hdmi-tx-map", 0);
	if (!hdmi_node)
		return -EINVAL;
	mhl->hdmi = of_find_device_by_node(hdmi_node);
	of_node_put(hdmi_node);
	if (!mhl->hdmi)
		return -EPROBE_DEFER;
	for (i = 0; i < ARRAY_SIZE(mhl->gpios); i++) {
		ret = of_get_named_gpio(spi->dev.of_node, gpio_properties[i], 0);
		if (ret < 0)
			goto put_hdmi;
		mhl->gpios[i].gpio = ret;
		mhl->gpios[i].label = gpio_properties[i];
		mhl->gpios[i].flags = i == GPIO_INTERRUPT ? GPIOF_IN :
			(i == GPIO_RESET || i == GPIO_SWITCH_POWER ?
			 GPIOF_OUT_INIT_HIGH : GPIOF_OUT_INIT_LOW);
	}
	ret = gpio_request_array(mhl->gpios, ARRAY_SIZE(mhl->gpios));
	if (ret)
		goto put_hdmi;
	mhl->vdd = devm_regulator_get(&spi->dev, "sii8620-vdd");
	if (IS_ERR(mhl->vdd)) {
		ret = PTR_ERR(mhl->vdd);
		goto free_gpios;
	}
	ret = of_property_read_u32_array(spi->dev.of_node,
			"qcom,vdd-voltage-level", voltages, 2);
	if (ret)
		goto free_gpios;
	ret = of_property_read_u32_array(spi->dev.of_node,
			"qcom,vdd-current-level", currents, 2);
	if (ret)
		goto free_gpios;
	ret = regulator_set_voltage(mhl->vdd, voltages[0], voltages[1]);
	if (ret)
		goto free_gpios;
	ret = regulator_set_optimum_mode(mhl->vdd, currents[1]);
	if (ret < 0)
		goto free_gpios;
	ret = regulator_enable(mhl->vdd);
	if (ret)
		goto idle_regulator;
	spi->bits_per_word = 8;
	spi->mode = SPI_MODE_0;
	ret = spi_setup(spi);
	if (ret)
		goto disable_regulator;
	ret = msm_hdmi_register_mhl(mhl->hdmi, &mhl->hdmi_ops, NULL);
	if (ret)
		ret = -EPROBE_DEFER;
	if (ret)
		goto disable_regulator;
	mhl->hdmi_registered = true;
	ret = mhl->hdmi_ops.set_mhl_max_pclk(mhl->hdmi, 297000);
	if (ret)
		goto disable_regulator;
	mhl->supply.name = "mhl";
	mhl->supply.type = POWER_SUPPLY_TYPE_USB_DCP;
	mhl->supply.properties = mhl_properties;
	mhl->supply.num_properties = ARRAY_SIZE(mhl_properties);
	mhl->supply.supplied_to = mhl_supplied_to;
	mhl->supply.num_supplicants = ARRAY_SIZE(mhl_supplied_to);
	mhl->supply.get_property = mhl_get_property;
	ret = power_supply_register(&spi->dev, &mhl->supply);
	if (ret)
		goto disable_regulator;
	mhl->drv_info.drv_context_size = sizeof(struct drv_hw_context);
	mhl->drv_info.mhl_version_support.major = 3;
	mhl->drv_info.irq = gpio_to_irq(mhl->gpios[GPIO_INTERRUPT].gpio);
	if (mhl->drv_info.irq < 0) {
		ret = mhl->drv_info.irq;
		goto unregister_supply;
	}
	mhl->drv_info.mhl_device_initialize = si_mhl_tx_chip_initialize;
	mhl->drv_info.mhl_device_isr = si_mhl_tx_drv_device_isr;
	mhl->drv_info.mhl_device_get_aksv = si_mhl_tx_drv_get_aksv;
	transmitter = mhl;
	/* The first IRQ may queue disconnect work before probe reaches standby. */
	mutex_lock(&mhl->connection_lock);
	chip_power(mhl, true);
	ret = mhl_tx_init(&mhl->drv_info, &spi->dev);
	if (ret) {
		mutex_unlock(&mhl->connection_lock);
		goto power_off;
	}
	release_usb(mhl);
	mutex_unlock(&mhl->connection_lock);
	mhl->usb_notify.ctxt = mhl;
	mhl->usb_notify.notify = discover_mhl;
	ret = msm_register_usb_ext_notification(&mhl->usb_notify);
	if (ret) {
		cancel_work_sync(&mhl->disconnect_work);
		mhl_tx_remove(&spi->dev);
		goto power_off;
	}
	device_init_wakeup(&spi->dev, true);
	dev_info(&spi->dev, "SiI8620 MHL transmitter registered\n");
	return 0;
power_off:
	cancel_work_sync(&mhl->disconnect_work);
	chip_power(mhl, false);
	transmitter = NULL;
unregister_supply:
	power_supply_unregister(&mhl->supply);
disable_regulator:
	if (mhl->hdmi_registered)
		msm_hdmi_unregister_mhl(mhl->hdmi);
	regulator_disable(mhl->vdd);
idle_regulator:
	regulator_set_optimum_mode(mhl->vdd, currents[0]);
free_gpios:
	gpio_set_value(mhl->gpios[GPIO_SWITCH_POWER].gpio, 0);
	gpio_free_array(mhl->gpios, ARRAY_SIZE(mhl->gpios));
put_hdmi:
	put_device(&mhl->hdmi->dev);
	return ret;
}

static int ef67_mhl_remove(struct spi_device *spi)
{
	struct ef67_mhl *mhl = transmitter;

	msm_register_usb_ext_notification(NULL);
	mutex_lock(&mhl->connection_lock);
	mhl->removing = true;
	release_usb(mhl);
	if (mhl->irq_wake_enabled) {
		disable_irq_wake(mhl->drv_info.irq);
		mhl->irq_wake_enabled = false;
	}
	mutex_unlock(&mhl->connection_lock);
	cancel_work_sync(&mhl->disconnect_work);
	mhl_tx_remove(&spi->dev);
	msm_hdmi_unregister_mhl(mhl->hdmi);
	device_init_wakeup(&spi->dev, false);
	power_supply_unregister(&mhl->supply);
	regulator_disable(mhl->vdd);
	regulator_set_optimum_mode(mhl->vdd, 0);
	gpio_set_value(mhl->gpios[GPIO_SWITCH_POWER].gpio, 0);
	gpio_free_array(mhl->gpios, ARRAY_SIZE(mhl->gpios));
	put_device(&mhl->hdmi->dev);
	transmitter = NULL;
	return 0;
}

static int ef67_mhl_suspend(struct device *dev)
{
	struct ef67_mhl *mhl = transmitter;
	int ret = 0;

	mutex_lock(&mhl->connection_lock);
	if (mhl->powered) {
		ret = enable_irq_wake(mhl->drv_info.irq);
		if (!ret)
			mhl->irq_wake_enabled = true;
	}
	mutex_unlock(&mhl->connection_lock);
	return ret;
}

static int ef67_mhl_resume(struct device *dev)
{
	struct ef67_mhl *mhl = transmitter;
	int ret = 0;

	mutex_lock(&mhl->connection_lock);
	if (mhl->irq_wake_enabled) {
		ret = disable_irq_wake(mhl->drv_info.irq);
		if (!ret)
			mhl->irq_wake_enabled = false;
	}
	mutex_unlock(&mhl->connection_lock);
	return ret;
}

static const struct dev_pm_ops ef67_mhl_pm = {
	.suspend = ef67_mhl_suspend,
	.resume = ef67_mhl_resume,
};
static const struct of_device_id ef67_mhl_match[] = {
	{ .compatible = "qcom,mhl-sii8620" },
	{ }
};
MODULE_DEVICE_TABLE(of, ef67_mhl_match);
static struct spi_driver ef67_mhl_driver = {
	.driver = {
		.name = "sii8620",
		.owner = THIS_MODULE,
		.suppress_bind_attrs = true,
		.of_match_table = ef67_mhl_match,
		.pm = &ef67_mhl_pm,
	},
	.probe = ef67_mhl_probe,
	.remove = ef67_mhl_remove,
};
module_spi_driver(ef67_mhl_driver);
MODULE_DESCRIPTION("EF67 SiI8620 MHL transmitter");
MODULE_LICENSE("GPL v2");
