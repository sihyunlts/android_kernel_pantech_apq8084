/*
 * Pantech EF67 ES705 firmware, voice presets and SLIMbus audio paths.
 * Protocol and power sequence follow the S0226114 kernel.
 */
#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/firmware.h>
#include <linux/fs.h>
#include <linux/gpio.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of_gpio.h>
#include <linux/regulator/consumer.h>
#include <linux/slab.h>
#include <linux/slimbus/slimbus.h>
#include <linux/tty.h>
#include <linux/workqueue.h>
#include <sound/pcm_params.h>
#include "es705-ef67.h"
#include "es705-ef67-routes.h"

#define ES705_FIRMWARE "audience/es705/audience-es705-fw.bin"
#define ES705_WRITE_OFFSET 0x800
#define ES705_READ_OFFSET 0x804
#define ES705_SYNC 0x80000000
#define ES705_BOOT 0x00000001
#define ES705_BOOT_ACK 0x01010101
#define ES705_SLEEP 0x90100001
#define ES705_FW_FIRST_CHAR 0x80200000
#define ES705_FW_NEXT_CHAR 0x80210000
#define ES705_WAKE_UART "/dev/ttyHS10"

/* AP playback 1, AP capture 1, AP playback 2; WCD9330 DAI IDs 0/1/2. */
struct ef67_es705_group {
	u16 handle;
	u32 ports[2];
	unsigned int connected;
	bool defined;
};

struct ef67_es705_stream {
	unsigned int channels[2];
	unsigned int channel_count;
	unsigned int rate;
	bool enabled;
	struct ef67_es705_group frontend;
	struct ef67_es705_group backend;
};

static const unsigned int es705_rx_channels[] = {152, 153, 154, 155, 134, 135};
static const unsigned int es705_tx_channels[] = {156, 157, 144, 145, 149, 150};

struct ef67_es705 {
	struct slim_device *slim;
	struct clk *clock;
	struct regulator *supply;
	struct work_struct boot_work;
	struct mutex lock;
	int reset_gpio;
	int wakeup_gpio;
	bool uart_wakeup;
	bool clock_enabled;
	bool supply_enabled;
	bool removing;
	bool bus_available;
	bool firmware_ready;
	bool asleep;
	bool channels_allocated;
	u16 rx_handles[6];
	u16 tx_handles[6];
	struct ef67_es705_stream streams[3];
	unsigned int internal_route;
	unsigned int internal_rate;
	unsigned int ap_tx_channels;
	char firmware_version[256];
};

/* EF67 has one Audience chip; serialize card calls against its removal. */
static DEFINE_MUTEX(es705_device_lock);
static struct ef67_es705 *es705_device;

static int ef67_es705_set_preset(struct ef67_es705 *es705,
	unsigned int route, unsigned int rate);
static bool ef67_es705_in_use(struct ef67_es705 *es705);
static int ef67_es705_allocate_channels(struct ef67_es705 *es705);
static void ef67_es705_release_channels(struct ef67_es705 *es705);
static void ef67_es705_stop_streams(struct ef67_es705 *es705);

static int ef67_es705_write(struct ef67_es705 *es705,
			    const u8 *data, size_t length)
{
	struct slim_ele_access access = {
		.start_offset = ES705_WRITE_OFFSET,
		.num_bytes = 4,
	};
	size_t offset;
	int ret, attempt;

	for (offset = 0; offset < length; offset += 4) {
		if (!es705->slim->reported)
			return -ENODEV;
		ret = slim_get_logical_addr(es705->slim, es705->slim->e_addr,
					   6, &es705->slim->laddr);
		if (ret)
			return ret;
		/* The stock transport retries a failed element three times. */
		for (attempt = 0; attempt < 3; attempt++) {
			ret = slim_change_val_element(es705->slim, &access,
						      (u8 *)data + offset,
						      min_t(size_t, 4,
							    length - offset));
			if (!ret)
				break;
			usleep_range(1000, 1000);
		}
		if (ret)
			return ret;
	}
	return 0;
}

static int ef67_es705_read(struct ef67_es705 *es705, __le32 *response)
{
	struct slim_ele_access access = {
		.start_offset = ES705_READ_OFFSET,
		.num_bytes = 4,
	};
	int ret, attempt;

	ret = slim_get_logical_addr(es705->slim, es705->slim->e_addr,
				   6, &es705->slim->laddr);
	if (ret)
		return ret;
	for (attempt = 0; attempt < 3; attempt++) {
		ret = slim_request_val_element(es705->slim, &access,
					       (u8 *)response, sizeof(*response));
		if (!ret)
			return 0;
		usleep_range(1000, 1000);
	}
	return ret;
}

/* SLIMbus carries the command bytes in little-endian order. */
static int ef67_es705_command(struct ef67_es705 *es705, u32 command,
			      u32 *result)
{
	__le32 request = cpu_to_le32(command);
	__le32 response;
	int ret, attempt;

	ret = ef67_es705_write(es705, (u8 *)&request, sizeof(request));
	if (ret || (command & BIT(28)))
		return ret;

	/* S0226114 polls up to six times with a 4-4.5 ms interval. */
	for (attempt = 0; attempt < 6; attempt++) {
		usleep_range(4000, 4500);
		ret = ef67_es705_read(es705, &response);
		if (ret)
			return ret;
		if ((le32_to_cpu(response) & 0xffff0000) == 0xffff0000)
			return -EINVAL;
		if (response) {
			*result = le32_to_cpu(response);
			return 0;
		}
	}
	return -ETIMEDOUT;
}

static int ef67_es705_read_version(struct ef67_es705 *es705)
{
	u32 command = ES705_FW_FIRST_CHAR, response;
	unsigned int i;
	int ret;

	for (i = 0; i < sizeof(es705->firmware_version); i++) {
		ret = ef67_es705_command(es705, command, &response);
		if (ret)
			return ret;
		if ((response & 0xffff0000) != command)
			return -EIO;
		es705->firmware_version[i] = response & 0xff;
		if (!es705->firmware_version[i])
			return 0;
		command = ES705_FW_NEXT_CHAR;
	}
	return -EOVERFLOW;
}

static void ef67_es705_power_off(struct ef67_es705 *es705)
{
	gpio_set_value_cansleep(es705->reset_gpio, 0);
	es705->firmware_ready = false;
	es705->asleep = false;
	if (es705->clock_enabled) {
		clk_disable_unprepare(es705->clock);
		es705->clock_enabled = false;
	}
	if (es705->supply_enabled) {
		regulator_disable(es705->supply);
		es705->supply_enabled = false;
	}
}

/* Called before SLIMbus assigns the generic device's logical address. */
static int ef67_es705_power_on(struct ef67_es705 *es705)
{
	int ret;

	gpio_set_value_cansleep(es705->reset_gpio, 0);
	es705->firmware_ready = false;
	if (!es705->supply_enabled) {
		ret = regulator_enable(es705->supply);
		if (ret)
			return ret;
		es705->supply_enabled = true;
	}
	if (!es705->clock_enabled) {
		ret = clk_prepare_enable(es705->clock);
		if (ret) {
			ef67_es705_power_off(es705);
			return ret;
		}
		es705->clock_enabled = true;
	}
	usleep_range(1000, 1000);
	gpio_set_value_cansleep(es705->reset_gpio, 1);
	usleep_range(10000, 10000);
	return 0;
}

static int ef67_es705_sleep(struct ef67_es705 *es705)
{
	int ret;

	ret = ef67_es705_command(es705, ES705_SLEEP, NULL);
	if (ret)
		return ret;
	msleep(30);
	clk_disable_unprepare(es705->clock);
	es705->clock_enabled = false;
	es705->asleep = true;
	return 0;
}

/* S0226114 wakes the chip by writing 'A' through the board's UART10. */
static int ef67_es705_uart_wake(void)
{
	struct file *file;
	struct tty_file_private *tty_private;
	char wakeup = 'A';
	ssize_t written;
	int ret = 0;

	file = filp_open(ES705_WAKE_UART, O_RDWR | O_NOCTTY | O_NONBLOCK, 0);
	if (IS_ERR(file))
		return PTR_ERR(file);
	tty_private = file->private_data;
	if (!tty_private || !tty_private->tty) {
		ret = -ENODEV;
		goto close;
	}
	written = kernel_write(file, &wakeup, sizeof(wakeup), 0);
	if (written != sizeof(wakeup)) {
		ret = written < 0 ? written : -EIO;
		goto close;
	}
	tty_wait_until_sent(tty_private->tty, msecs_to_jiffies(100));
	if (tty_chars_in_buffer(tty_private->tty))
		ret = -ETIMEDOUT;
close:
	filp_close(file, NULL);
	return ret;
}

/* UART or GPIO wake is selected by the stock board description. */
static int ef67_es705_wake(struct ef67_es705 *es705)
{
	u32 response;
	int ret;

	if (!es705->asleep)
		return 0;
	ret = clk_prepare_enable(es705->clock);
	if (ret)
		return ret;
	es705->clock_enabled = true;
	msleep(1);
	if (es705->uart_wakeup) {
		ret = ef67_es705_uart_wake();
		if (ret)
			goto disable_clock;
	} else {
		gpio_set_value_cansleep(es705->wakeup_gpio, 0);
		usleep_range(1000, 1000);
		gpio_set_value_cansleep(es705->wakeup_gpio, 1);
	}
	msleep(30);
	ret = ef67_es705_command(es705, ES705_SYNC, &response);
	if (!ret && response != ES705_SYNC)
		ret = -EIO;
	if (ret)
		goto disable_clock;
	es705->asleep = false;
	return 0;
disable_clock:
	/* A failed wake must not acquire another clock reference on retry. */
	clk_disable_unprepare(es705->clock);
	es705->clock_enabled = false;
	return ret;
}

static bool ef67_es705_in_use(struct ef67_es705 *es705)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(es705->streams); i++)
		if (es705->streams[i].enabled ||
		    es705->streams[i].frontend.defined ||
		    es705->streams[i].backend.defined)
			return true;
	return false;
}

static int ef67_es705_write_commands(struct ef67_es705 *es705,
				     const u32 *commands)
{
	u32 response;
	int ret;

	while (*commands) {
		ret = ef67_es705_command(es705, *commands++, &response);
		if (ret)
			return ret;
	}
	return 0;
}

static int ef67_es705_set_preset(struct ef67_es705 *es705,
	unsigned int route, unsigned int rate)
{
	const struct ef67_es705_route *preset = &ef67_es705_routes[route];
	int ret;

	ret = ef67_es705_write_commands(es705, preset->preset);
	if (ret)
		return ret;
	usleep_range(10000, 10000);
	/* Stock routes 0/21 omit the rate preset. Other NULL lists do too. */
	if (route == 0 || route == 21 || !(preset->rate_mask & BIT(rate)))
		return 0;
	return ef67_es705_write_commands(es705, preset->rate[rate]);
}

static int ef67_es705_allocate_channels(struct ef67_es705 *es705)
{
	unsigned int i;
	int ret;

	if (es705->channels_allocated)
		return 0;
	for (i = 0; i < ARRAY_SIZE(es705_rx_channels); i++) {
		ret = slim_query_ch(es705->slim, es705_rx_channels[i],
				    &es705->rx_handles[i]);
		if (ret)
			goto release_rx;
	}
	for (i = 0; i < ARRAY_SIZE(es705_tx_channels); i++) {
		ret = slim_query_ch(es705->slim, es705_tx_channels[i],
				    &es705->tx_handles[i]);
		if (ret)
			goto release_tx;
	}
	es705->channels_allocated = true;
	return 0;
release_tx:
	while (i)
		slim_dealloc_ch(es705->slim, es705->tx_handles[--i]);
	i = ARRAY_SIZE(es705_rx_channels);
release_rx:
	while (i)
		slim_dealloc_ch(es705->slim, es705->rx_handles[--i]);
	return ret;
}

static void ef67_es705_release_channels(struct ef67_es705 *es705)
{
	unsigned int i;

	if (!es705->channels_allocated)
		return;
	for (i = 0; i < ARRAY_SIZE(es705_rx_channels); i++) {
		slim_dealloc_ch(es705->slim, es705->rx_handles[i]);
		slim_dealloc_ch(es705->slim, es705->tx_handles[i]);
	}
	es705->channels_allocated = false;
}

static int ef67_es705_group_stop(struct ef67_es705 *es705,
				 struct ef67_es705_group *group)
{
	int ret, disconnect_ret;

	if (!group->defined)
		return 0;
	/* REMOVE requires an active channel; a failed start may only be defined. */
	ret = slim_get_ch_state(es705->slim, group->handle) >= SLIM_CH_ACTIVE ?
		slim_control_ch(es705->slim, group->handle, SLIM_CH_REMOVE, true) : 0;
	disconnect_ret = group->connected ?
		slim_disconnect_ports(es705->slim, group->ports, group->connected) : 0;
	if (!disconnect_ret)
		group->connected = 0;
	if (!ret && !disconnect_ret)
		memset(group, 0, sizeof(*group));
	return ret ? ret : disconnect_ret;
}

static void ef67_es705_stop_streams(struct ef67_es705 *es705)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(es705->streams); i++) {
		ef67_es705_group_stop(es705, &es705->streams[i].frontend);
		ef67_es705_group_stop(es705, &es705->streams[i].backend);
	}
}

static int ef67_es705_group_start(struct ef67_es705 *es705,
	struct ef67_es705_group *group, bool source, const unsigned int *channels,
	unsigned int count, unsigned int rate)
{
	const unsigned int *map = source ? es705_tx_channels : es705_rx_channels;
	u16 *handles = source ? es705->tx_handles : es705->rx_handles;
	struct slim_ch properties = {
		.prot = SLIM_AUTO_ISO,
		.baser = SLIM_RATE_4000HZ,
		.dataf = SLIM_CH_DATAF_NOT_DEFINED,
		.auxf = SLIM_CH_AUXF_NOT_APPLICABLE,
		.sampleszbits = 16,
	};
	u16 channel_handles[2];
	u32 ports[2];
	unsigned int i, index;
	int ret;

	if (!count || count > ARRAY_SIZE(ports) || group->defined ||
	    !rate || rate % 4000)
		return -EINVAL;
	for (i = 0; i < count; i++) {
		for (index = 0; index < ARRAY_SIZE(es705_rx_channels); index++)
			if (channels[i] == map[index])
				break;
		if (index == ARRAY_SIZE(es705_rx_channels))
			return -EINVAL;
		ret = slim_get_slaveport(es705->slim->laddr,
			index + (source ? 10 : 0), &ports[i],
			source ? SLIM_SRC : SLIM_SINK);
		if (ret)
			return ret;
		channel_handles[i] = handles[index];
	}
	properties.ratem = rate / 4000;
	ret = slim_define_ch(es705->slim, &properties, channel_handles,
			     count, true, &group->handle);
	if (ret)
		return ret;
	group->defined = true;
	for (i = 0; i < count; i++) {
		ret = source ? slim_connect_src(es705->slim, ports[i],
					       channel_handles[i]) :
			slim_connect_sink(es705->slim, &ports[i], 1,
					  channel_handles[i]);
		if (ret)
			goto failed;
		group->ports[group->connected++] = ports[i];
	}
	ret = slim_control_ch(es705->slim, group->handle, SLIM_CH_ACTIVATE, true);
	if (!ret)
		return 0;
failed:
	ef67_es705_group_stop(es705, group);
	return ret;
}

/* Caller owns the device mutex until unlock; teardown cannot free this chip. */
static struct ef67_es705 *ef67_es705_lock_device(void)
{
	struct ef67_es705 *es705;

	mutex_lock(&es705_device_lock);
	es705 = es705_device;
	if (es705)
		mutex_lock(&es705->lock);
	return es705;
}

static void ef67_es705_unlock_device(struct ef67_es705 *es705)
{
	if (es705)
		mutex_unlock(&es705->lock);
	mutex_unlock(&es705_device_lock);
}

bool ef67_es705_route_enabled(unsigned int id)
{
	struct ef67_es705 *es705 = ef67_es705_lock_device();
	bool enabled = es705 && id < ARRAY_SIZE(es705->streams) &&
		es705->streams[id].enabled;

	ef67_es705_unlock_device(es705);
	return enabled;
}
EXPORT_SYMBOL_GPL(ef67_es705_route_enabled);

int ef67_es705_get_channel_map(unsigned int id, unsigned int *tx_num,
	unsigned int *tx_slot, unsigned int *rx_num, unsigned int *rx_slot)
{
	if (id == 1) {
		if (!tx_num || !tx_slot)
			return -EINVAL;
		*tx_num = 2;
		tx_slot[0] = es705_tx_channels[0];
		tx_slot[1] = es705_tx_channels[1];
	} else if (id == 0 || id == 2) {
		if (!rx_num || !rx_slot)
			return -EINVAL;
		*rx_num = 2;
		rx_slot[0] = es705_rx_channels[id];
		rx_slot[1] = es705_rx_channels[id + 1];
	} else {
		return -EINVAL;
	}
	return 0;
}
EXPORT_SYMBOL_GPL(ef67_es705_get_channel_map);

int ef67_es705_set_channel_map(unsigned int id, unsigned int tx_num,
	unsigned int *tx_slot, unsigned int rx_num, unsigned int *rx_slot)
{
	struct ef67_es705 *es705 = ef67_es705_lock_device();
	unsigned int count = id == 1 ? tx_num : rx_num;
	unsigned int *slots = id == 1 ? tx_slot : rx_slot;
	const unsigned int *map = id == 1 ? es705_tx_channels : es705_rx_channels;
	unsigned int offset = id == 2 ? 2 : 0;
	unsigned int i;
	int ret = -ENODEV;

	if (!es705)
		goto out;
	ret = -EINVAL;
	if (id >= ARRAY_SIZE(es705->streams) || !count || count > 2 || !slots)
		goto out;
	if (es705->streams[id].frontend.defined || es705->streams[id].backend.defined) {
		ret = -EBUSY;
		goto out;
	}
	for (i = 0; i < count; i++)
		if (slots[i] != map[offset] && slots[i] != map[offset + 1])
			goto out;
	if (count == 2 && slots[0] == slots[1])
		goto out;
	memcpy(es705->streams[id].channels, slots, count * sizeof(*slots));
	es705->streams[id].channel_count = count;
	ret = 0;
out:
	ef67_es705_unlock_device(es705);
	return ret;
}
EXPORT_SYMBOL_GPL(ef67_es705_set_channel_map);

int ef67_es705_hw_params(unsigned int id, struct snd_pcm_hw_params *params)
{
	struct ef67_es705 *es705 = ef67_es705_lock_device();
	unsigned int rate = params_rate(params), count = params_channels(params);
	int ret = -ENODEV;

	if (!es705)
		goto out;
	ret = -EINVAL;
	if (id >= ARRAY_SIZE(es705->streams) || !count || count > 2 ||
	    params_format(params) != SNDRV_PCM_FORMAT_S16_LE ||
	    (rate != 8000 && rate != 16000 && rate != 24000 &&
	     rate != 32000 && rate != 48000))
		goto out;
	es705->streams[id].channel_count = count;
	es705->streams[id].rate = rate;
	ret = 0;
out:
	ef67_es705_unlock_device(es705);
	return ret;
}
EXPORT_SYMBOL_GPL(ef67_es705_hw_params);

int ef67_es705_stream_start(unsigned int id)
{
	struct ef67_es705 *es705 = ef67_es705_lock_device();
	struct ef67_es705_stream *stream;
	const unsigned int *backend;
	unsigned int frontend_count;
	int ret = 0;

	if (!es705 || id >= ARRAY_SIZE(es705->streams))
		goto out;
	stream = &es705->streams[id];
	if (!stream->enabled)
		goto out;
	if (!es705->firmware_ready || !es705->bus_available) {
		ret = -EAGAIN;
		goto out;
	}
	if (!stream->channel_count || !stream->rate || stream->frontend.defined ||
	    stream->backend.defined) {
		ret = -EINVAL;
		goto out;
	}
	ret = ef67_es705_wake(es705);
	if (ret)
		goto out;
	/* Capture's AP channel count and microphone count are separate in stock. */
	frontend_count = id == 1 ? es705->ap_tx_channels : stream->channel_count;
	backend = id == 1 ? &es705_rx_channels[4] :
		&es705_tx_channels[id == 0 ? 2 : 4];
	ret = ef67_es705_group_start(es705, &stream->frontend, id == 1,
		stream->channels, frontend_count, stream->rate);
	if (ret)
		goto out;
	ret = ef67_es705_group_start(es705, &stream->backend, id != 1,
		backend, stream->channel_count, stream->rate);
	if (ret)
		ef67_es705_group_stop(es705, &stream->frontend);
out:
	ef67_es705_unlock_device(es705);
	return ret;
}
EXPORT_SYMBOL_GPL(ef67_es705_stream_start);

int ef67_es705_stream_stop(unsigned int id)
{
	struct ef67_es705 *es705 = ef67_es705_lock_device();
	struct ef67_es705_stream *stream;
	int ret = 0, backend_ret;

	if (!es705 || id >= ARRAY_SIZE(es705->streams))
		goto out;
	stream = &es705->streams[id];
	ret = ef67_es705_group_stop(es705, &stream->frontend);
	backend_ret = ef67_es705_group_stop(es705, &stream->backend);
	if (!ret)
		ret = backend_ret;
	if (!ret) {
		stream->channel_count = 0;
		stream->rate = 0;
		if (es705->firmware_ready && !es705->asleep &&
		    !ef67_es705_in_use(es705))
			ret = ef67_es705_sleep(es705);
	}
out:
	ef67_es705_unlock_device(es705);
	return ret;
}
EXPORT_SYMBOL_GPL(ef67_es705_stream_stop);

enum ef67_es705_control {
	ES705_RX1_ENABLE,
	ES705_TX1_ENABLE,
	ES705_RX2_ENABLE,
	ES705_AP_TX_CHANNELS,
	ES705_INTERNAL_ROUTE,
	ES705_INTERNAL_RATE,
};

static const char * const es705_channel_names[] = {"One", "Two"};
static const char * const es705_rate_names[] = {"NB", "WB", "SWB", "FB"};

static int ef67_es705_control_info(struct snd_kcontrol *control,
				  struct snd_ctl_elem_info *info)
{
	unsigned int id = control->private_value;

	if (id == ES705_AP_TX_CHANNELS)
		return snd_ctl_enum_info(info, 1, ARRAY_SIZE(es705_channel_names),
					 es705_channel_names);
	if (id == ES705_INTERNAL_RATE)
		return snd_ctl_enum_info(info, 1, ARRAY_SIZE(es705_rate_names),
					 es705_rate_names);
	info->type = id == ES705_INTERNAL_ROUTE ? SNDRV_CTL_ELEM_TYPE_INTEGER :
		SNDRV_CTL_ELEM_TYPE_BOOLEAN;
	info->count = 1;
	info->value.integer.min = 0;
	info->value.integer.max = id == ES705_INTERNAL_ROUTE ?
		ARRAY_SIZE(ef67_es705_routes) - 1 : 1;
	return 0;
}

static int ef67_es705_control_get(struct snd_kcontrol *control,
				 struct snd_ctl_elem_value *value)
{
	struct ef67_es705 *es705 = ef67_es705_lock_device();
	unsigned int id = control->private_value;
	int ret = 0;

	if (!es705) {
		ret = -ENODEV;
		goto out;
	}
	if (id < ES705_AP_TX_CHANNELS)
		value->value.integer.value[0] = es705->streams[id].enabled;
	else if (id == ES705_AP_TX_CHANNELS)
		value->value.enumerated.item[0] = es705->ap_tx_channels - 1;
	else if (id == ES705_INTERNAL_ROUTE)
		value->value.integer.value[0] = es705->internal_route;
	else
		value->value.enumerated.item[0] = es705->internal_rate;
out:
	ef67_es705_unlock_device(es705);
	return ret;
}

static int ef67_es705_control_put(struct snd_kcontrol *control,
				 struct snd_ctl_elem_value *value)
{
	struct ef67_es705 *es705 = ef67_es705_lock_device();
	unsigned int id = control->private_value;
	unsigned int requested = (id == ES705_AP_TX_CHANNELS ||
		id == ES705_INTERNAL_RATE) ? value->value.enumerated.item[0] :
		value->value.integer.value[0];
	unsigned int previous;
	bool hardware_access = false;
	int ret = -ENODEV;

	if (!es705)
		goto out;
	ret = -EINVAL;
	if (id < ES705_AP_TX_CHANNELS) {
		if (requested > 1)
			goto out;
		previous = es705->streams[id].enabled;
		ret = 0;
		if (requested && !es705->firmware_ready) {
			ret = -EAGAIN;
			goto out;
		}
		if (requested == previous)
			goto out;
		if (requested) {
			hardware_access = true;
			ret = ef67_es705_wake(es705);
			if (ret)
				goto out;
		}
		es705->streams[id].enabled = requested;
		if (!requested && es705->firmware_ready &&
		    !es705->asleep && !ef67_es705_in_use(es705)) {
			hardware_access = true;
			ret = ef67_es705_sleep(es705);
		}
		if (!ret)
			ret = 1;
	} else if (id == ES705_AP_TX_CHANNELS) {
		if (requested >= ARRAY_SIZE(es705_channel_names))
			goto out;
		if (es705->streams[1].frontend.defined) {
			ret = -EBUSY;
			goto out;
		}
		ret = es705->ap_tx_channels != requested + 1;
		es705->ap_tx_channels = requested + 1;
	} else {
		const struct ef67_es705_route *preset;
		if ((id == ES705_INTERNAL_ROUTE &&
		     requested >= ARRAY_SIZE(ef67_es705_routes)) ||
		    (id == ES705_INTERNAL_RATE &&
		     requested >= ARRAY_SIZE(es705_rate_names)))
			goto out;
		if (!es705->firmware_ready) {
			ret = -EAGAIN;
			goto out;
		}
		preset = &ef67_es705_routes[es705->internal_route];
		if (id == ES705_INTERNAL_RATE &&
		    !(preset->rate_mask & BIT(requested)))
			goto out;
		hardware_access = true;
		ret = ef67_es705_wake(es705);
		if (ret)
			goto out;
		if (id == ES705_INTERNAL_ROUTE) {
			previous = es705->internal_route;
			ret = ef67_es705_set_preset(es705, requested,
						   es705->internal_rate);
			if (!ret)
				es705->internal_route = requested;
		} else {
			previous = es705->internal_rate;
			ret = ef67_es705_write_commands(es705, preset->rate[requested]);
			if (!ret)
				es705->internal_rate = requested;
		}
		if (!ret && !ef67_es705_in_use(es705))
			ret = ef67_es705_sleep(es705);
		if (!ret)
			ret = requested != previous;
	}
out:
	if (ret < 0 && hardware_access) {
		ef67_es705_stop_streams(es705);
		ef67_es705_power_off(es705);
	}
	ef67_es705_unlock_device(es705);
	return ret;
}

#define ES705_CONTROL(name_, id_) { \
	.iface = SNDRV_CTL_ELEM_IFACE_MIXER, .name = name_, \
	.info = ef67_es705_control_info, .get = ef67_es705_control_get, \
	.put = ef67_es705_control_put, .private_value = id_, \
}

static const struct snd_kcontrol_new es705_controls[] = {
	ES705_CONTROL("ES705 RX1 Enable", ES705_RX1_ENABLE),
	ES705_CONTROL("ES705 TX1 Enable", ES705_TX1_ENABLE),
	ES705_CONTROL("ES705 RX2 Enable", ES705_RX2_ENABLE),
	ES705_CONTROL("ES705-AP Tx Channels", ES705_AP_TX_CHANNELS),
	ES705_CONTROL("Internal Route", ES705_INTERNAL_ROUTE),
	ES705_CONTROL("Internal Rate", ES705_INTERNAL_RATE),
};

int ef67_es705_add_controls(struct snd_soc_codec *codec)
{
	return snd_soc_add_codec_controls(codec, es705_controls,
					  ARRAY_SIZE(es705_controls));
}
EXPORT_SYMBOL_GPL(ef67_es705_add_controls);

static void ef67_es705_boot(struct work_struct *work)
{
	struct ef67_es705 *es705 = container_of(work, struct ef67_es705,
					      boot_work);
	const struct firmware *firmware;
	u32 response;
	int ret, attempt;

	ret = request_firmware(&firmware, ES705_FIRMWARE, &es705->slim->dev);
	if (ret) {
		dev_err(&es705->slim->dev, "firmware request failed: %d\n", ret);
		mutex_lock(&es705->lock);
		ef67_es705_power_off(es705);
		mutex_unlock(&es705->lock);
		return;
	}
	mutex_lock(&es705->lock);
	if (es705->removing || !es705->bus_available || es705->firmware_ready)
		goto out;
	if (!firmware->size || firmware->size % 4) {
		ret = -EINVAL;
		goto failed;
	}
	ret = ef67_es705_command(es705, ES705_BOOT, &response);
	if (!ret && response != ES705_BOOT_ACK)
		ret = -EIO;
	if (ret)
		goto failed;
	ret = ef67_es705_write(es705, firmware->data, firmware->size);
	if (ret)
		goto failed;
	/* Stock polling completion resends SYNC up to six times. */
	for (attempt = 0; attempt < 6; attempt++) {
		ret = ef67_es705_command(es705, ES705_SYNC, &response);
		if (!ret && response != ES705_SYNC)
			ret = -EIO;
		if (!ret)
			break;
	}
	if (ret)
		goto failed;
	ret = ef67_es705_read_version(es705);
	if (ret)
		goto failed;
	ret = ef67_es705_allocate_channels(es705);
	if (ret)
		goto failed;

	/* Restore a selected preset after a bus reset, otherwise stay asleep. */
	if (ef67_es705_in_use(es705))
		ret = ef67_es705_set_preset(es705, es705->internal_route,
					   es705->internal_rate);
	else
		ret = ef67_es705_sleep(es705);
	if (ret)
		goto failed;
	es705->firmware_ready = true;
	goto out;
failed:
	dev_err(&es705->slim->dev, "firmware initialization failed: %d\n", ret);
	ef67_es705_power_off(es705);
out:
	mutex_unlock(&es705->lock);
	release_firmware(firmware);
}

/* Read the live version; preserve active audio routes, otherwise sleep. */
static ssize_t fw_version_show(struct device *dev,
			      struct device_attribute *attr, char *buf)
{
	struct ef67_es705 *es705 = dev_get_drvdata(dev);
	ssize_t ret;

	mutex_lock(&es705->lock);
	if (es705->removing) {
		ret = -ENODEV;
		goto out;
	}
	if (!es705->firmware_ready) {
		ret = -EAGAIN;
		goto out;
	}
	ret = ef67_es705_wake(es705);
	if (!ret)
		ret = ef67_es705_read_version(es705);
	if (!ret && !ef67_es705_in_use(es705))
		ret = ef67_es705_sleep(es705);
	if (ret) {
		ef67_es705_stop_streams(es705);
		ef67_es705_power_off(es705);
		goto out;
	}
	ret = scnprintf(buf, PAGE_SIZE, "%s\n", es705->firmware_version);
out:
	mutex_unlock(&es705->lock);
	return ret;
}
static DEVICE_ATTR(fw_version, S_IRUGO, fw_version_show, NULL);

static int ef67_es705_device_up(struct slim_device *slim)
{
	struct ef67_es705 *es705 = slim_get_devicedata(slim);

	mutex_lock(&es705->lock);
	es705->bus_available = true;
	if (!es705->removing && !es705->firmware_ready)
		schedule_work(&es705->boot_work);
	mutex_unlock(&es705->lock);
	return 0;
}

static int ef67_es705_device_down(struct slim_device *slim)
{
	struct ef67_es705 *es705 = slim_get_devicedata(slim);

	mutex_lock(&es705->lock);
	es705->bus_available = false;
	mutex_unlock(&es705->lock);
	cancel_work_sync(&es705->boot_work);
	mutex_lock(&es705->lock);
	ef67_es705_stop_streams(es705);
	ef67_es705_power_off(es705);
	mutex_unlock(&es705->lock);
	return 0;
}

static int ef67_es705_reset_device(struct slim_device *slim)
{
	struct ef67_es705 *es705 = slim_get_devicedata(slim);
	int ret;

	ef67_es705_device_down(slim);
	mutex_lock(&es705->lock);
	ret = es705->removing ? -ENODEV : ef67_es705_power_on(es705);
	mutex_unlock(&es705->lock);
	return ret;
}

static int ef67_es705_probe(struct slim_device *slim)
{
	struct ef67_es705 *es705;
	u32 uart_interface = 0;
	int ret;

	es705 = devm_kzalloc(&slim->dev, sizeof(*es705), GFP_KERNEL);
	if (!es705)
		return -ENOMEM;
	es705->slim = slim;
	es705->ap_tx_channels = 2;
	es705->streams[0].channels[0] = es705_rx_channels[0];
	es705->streams[0].channels[1] = es705_rx_channels[1];
	es705->streams[1].channels[0] = es705_tx_channels[0];
	es705->streams[1].channels[1] = es705_tx_channels[1];
	es705->streams[2].channels[0] = es705_rx_channels[2];
	es705->streams[2].channels[1] = es705_rx_channels[3];
	mutex_init(&es705->lock);
	INIT_WORK(&es705->boot_work, ef67_es705_boot);
	es705->reset_gpio = of_get_named_gpio(slim->dev.of_node,
					     "adnc,reset-gpio", 0);
	if (es705->reset_gpio < 0)
		return es705->reset_gpio;
	ret = devm_gpio_request_one(&slim->dev, es705->reset_gpio,
				   GPIOF_OUT_INIT_LOW, "es705-reset");
	if (ret)
		return ret;
	of_property_read_u32(slim->dev.of_node, "adnc,enable_hs_uart_intf",
			     &uart_interface);
	es705->uart_wakeup = uart_interface != 0;
	if (!es705->uart_wakeup) {
		es705->wakeup_gpio = of_get_named_gpio(slim->dev.of_node,
					      "adnc,wakeup-gpio", 0);
		if (es705->wakeup_gpio < 0)
			return es705->wakeup_gpio;
		ret = devm_gpio_request_one(&slim->dev, es705->wakeup_gpio,
				   GPIOF_OUT_INIT_HIGH, "es705-wakeup");
		if (ret)
			return ret;
	}
	es705->supply = devm_regulator_get(&slim->dev, "vdd");
	if (IS_ERR(es705->supply))
		return PTR_ERR(es705->supply);
	es705->clock = devm_clk_get(&slim->dev, "osr_clk");
	if (IS_ERR(es705->clock))
		return PTR_ERR(es705->clock);
	/* RF_CLK2 is a fixed XO buffer; only its enable vote is programmable. */
	slim_set_clientdata(slim, es705);
	ret = ef67_es705_power_on(es705);
	if (ret)
		return ret;
	ret = device_create_file(&slim->dev, &dev_attr_fw_version);
	if (ret) {
		ef67_es705_power_off(es705);
		return ret;
	}
	mutex_lock(&es705_device_lock);
	if (es705_device) {
		ret = -EBUSY;
		device_remove_file(&slim->dev, &dev_attr_fw_version);
		ef67_es705_power_off(es705);
	} else {
		es705_device = es705;
	}
	mutex_unlock(&es705_device_lock);
	return ret;
}

static int ef67_es705_remove(struct slim_device *slim)
{
	struct ef67_es705 *es705 = slim_get_devicedata(slim);

	mutex_lock(&es705_device_lock);
	if (es705_device == es705)
		es705_device = NULL;
	mutex_unlock(&es705_device_lock);
	mutex_lock(&es705->lock);
	es705->removing = true;
	mutex_unlock(&es705->lock);
	device_remove_file(&slim->dev, &dev_attr_fw_version);
	cancel_work_sync(&es705->boot_work);
	mutex_lock(&es705->lock);
	ef67_es705_stop_streams(es705);
	ef67_es705_release_channels(es705);
	ef67_es705_power_off(es705);
	mutex_unlock(&es705->lock);
	return 0;
}

static void ef67_es705_shutdown(struct slim_device *slim)
{
	ef67_es705_remove(slim);
}

static const struct slim_device_id ef67_es705_ids[] = {
	{ "es705-codec-gen0", 0 },
	{ }
};
MODULE_DEVICE_TABLE(slim, ef67_es705_ids);

static struct slim_driver ef67_es705_driver = {
	.driver = {
		.name = "es705-ef67",
		.owner = THIS_MODULE,
	},
	.probe = ef67_es705_probe,
	.remove = ef67_es705_remove,
	.shutdown = ef67_es705_shutdown,
	.device_up = ef67_es705_device_up,
	.device_down = ef67_es705_device_down,
	.reset_device = ef67_es705_reset_device,
	.id_table = ef67_es705_ids,
};

static int __init ef67_es705_init(void)
{
	return slim_driver_register(&ef67_es705_driver);
}
module_init(ef67_es705_init);

static void __exit ef67_es705_exit(void)
{
	slim_driver_unregister(&ef67_es705_driver);
}
module_exit(ef67_es705_exit);

MODULE_DESCRIPTION("Pantech EF67 ES705 voice processing");
MODULE_LICENSE("GPL v2");
MODULE_FIRMWARE(ES705_FIRMWARE);
