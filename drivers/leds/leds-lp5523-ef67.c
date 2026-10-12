/*
 * Pantech EF67 LP5523 LEDs.
 *
 * Register sequences and programs follow IM-A930S S0226114. The stock
 * interface stages RGB/timing values before a channel8 command; channels
 * 2, 3, 7 and 8 must never be treated as physical PWM outputs.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2.
 */
#include <linux/delay.h>
#include <linux/gpio.h>
#include <linux/i2c.h>
#include <linux/jiffies.h>
#include <linux/leds.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of_gpio.h>
#include <linux/slab.h>
#include <linux/workqueue.h>
#include <soc/qcom/smem.h>

#define EF67_LED_CHANNELS	9
#define EF67_PROGRAM_SIZE	32
#define LP5523_ENABLE		0x00
#define LP5523_OP_MODE		0x01
#define LP5523_ENABLE_MSB		0x04
#define LP5523_ENABLE_LSB		0x05
#define LP5523_LED_CONTROL	0x06
#define LP5523_PWM		0x16
#define LP5523_CURRENT		0x26
#define LP5523_CONFIG		0x36
#define LP5523_STATUS		0x3a
#define LP5523_RESET		0x3d
#define LP5523_PROGRAM_START	0x4c
#define LP5523_PROGRAM_PAGE	0x4f
#define LP5523_PROGRAM_MEMORY	0x50
#define LP5523_CHIP_ENABLE	0x40

enum ef67_led_command {
	EF67_RGB_OFF,
	EF67_RGB_PWM,
	EF67_RGB_BLINK,
	EF67_RGB_FADE,
	EF67_RGB_FADE_REPEAT,
	EF67_ALL_OFF = 101,
};

struct ef67_led_chip;

struct ef67_led {
	struct led_classdev cdev;
	struct ef67_led_chip *chip;
	unsigned int channel;
	u8 led_current;
	char name[24];
};

struct ef67_led_chip {
	struct i2c_client *client;
	struct ef67_led leds[EF67_LED_CHANNELS];
	struct workqueue_struct *queue;
	struct delayed_work rgb_fade_off_work;
	struct mutex lock;
	spinlock_t state_lock;
	u8 values[EF67_LED_CHANNELS];
	u8 applied[EF67_LED_CHANNELS];
	u8 max_current;
	int enable_gpio;
	bool stopping;
};

/* Immutable snapshots keep RGB writes followed by commands in order. */
struct ef67_led_request {
	struct work_struct work;
	struct ef67_led_chip *chip;
	u8 values[EF67_LED_CHANNELS];
	unsigned int channel;
};

/* Engine 1 = red/D7, engine 2 = green/D1, engine 3 = blue/D2. */
static const u8 ef67_rgb_channels[] = { 6, 0, 1 };

/* S stock pan_led_pattern_offline_charging, 0xc12ee694. */
static const u8 ef67_offline_charging_program[EF67_PROGRAM_SIZE] = {
	0x00, 0x40, 0x9c, 0x00, 0x1e, 0xe0, 0x54, 0x00,
	0x1f, 0xe0, 0x54, 0x00, 0xa0, 0x02,
};

/* S stock pan_led_pattern_pwm_repeat, 0xc12ee6d8. */
static const u8 ef67_blink_programs[][EF67_PROGRAM_SIZE] = {
	{ 0, 0, 0x9c, 0, 0x40, 0xff, 0x40, 0, 0x40, 0, 0x40, 0,
	  0xa0, 2 },
	{ 0, 0, 0x9c, 0, 0x40, 0xff, 0x7e, 0, 0xa0, 3, 0x40, 0,
	  0x40, 0, 0x40, 0, 0xa0, 2 },
	{ 0, 0, 0x9c, 0, 0x40, 0xff, 0x40, 0, 0x40, 0, 0x7e, 0,
	  0xa0, 5, 0x40, 0, 0xa0, 2 },
	{ 0, 0, 0x9c, 0, 0x40, 0xff, 0x7e, 0, 0xa0, 3, 0x40, 0,
	  0x40, 0, 0x7e, 0, 0xa0, 3, 0x40, 0, 0xa0, 2 },
};

/* S stock pan_led_pattern_dim, 0xc12ee758. */
static const u8 ef67_fade_programs[][EF67_PROGRAM_SIZE] = {
	{ 0, 0, 0x9c, 0, 0, 0xff, 0xc0, 0 },
	{ 0, 0, 0x9c, 0, 4, 0xff, 0x7e, 0, 0xe0, 0, 5, 0, 5, 0,
	  0x40, 0, 0xe0, 0, 0xa0, 2 },
	{ 0, 0, 0x9c, 0, 4, 0xff, 0x7e, 0, 0xa0, 3, 0x7e, 0,
	  0xe0, 0, 5, 0, 0x40, 0, 0xe0, 0, 0xa0, 2 },
	{ 0, 0, 0x9c, 0, 4, 0xff, 0x7e, 0, 0xe0, 0, 5, 0, 0x7e, 0,
	  0xa0, 6, 0x40, 0, 0xe0, 0, 0xa0, 2 },
	{ 0, 0, 0x9c, 0, 4, 0xff, 0x7e, 0, 0xa0, 3, 0x7e, 0,
	  0xe0, 0, 0x40, 0, 0x7e, 0, 0xa0, 8, 0x40, 0,
	  0xe0, 0, 0xa0, 2 },
};

static int ef67_led_write(struct ef67_led_chip *chip, u8 reg, u8 value)
{
	return i2c_smbus_write_byte_data(chip->client, reg, value);
}

static int ef67_led_stop_engines(struct ef67_led_chip *chip)
{
	int ret = ef67_led_write(chip, LP5523_OP_MODE, 0);

	usleep_range(1000, 2000);
	if (ret)
		return ret;
	return ef67_led_write(chip, LP5523_ENABLE, LP5523_CHIP_ENABLE);
}

static int ef67_led_clear_rgb(struct ef67_led_chip *chip)
{
	int i, ret;

	ret = ef67_led_stop_engines(chip);
	if (ret)
		return ret;
	for (i = 0; i < ARRAY_SIZE(ef67_rgb_channels); i++) {
		ret = ef67_led_write(chip, LP5523_PWM + ef67_rgb_channels[i], 0);
		if (ret)
			return ret;
		chip->applied[ef67_rgb_channels[i]] = 0;
	}
	return 0;
}

static void ef67_led_fade_off_work(struct work_struct *work)
{
	struct ef67_led_chip *chip = container_of(to_delayed_work(work),
				struct ef67_led_chip, rgb_fade_off_work);
	int ret;

	mutex_lock(&chip->lock);
	ret = ef67_led_clear_rgb(chip);
	if (ret)
		dev_err_ratelimited(&chip->client->dev,
				   "Cannot turn RGB off after fade: %d\n", ret);
	mutex_unlock(&chip->lock);
}

static int ef67_led_load_program(struct ef67_led_chip *chip,
				 unsigned int engine, const u8 *program)
{
	int mode, ret;
	u8 mask = 0x30 >> (engine * 2);
	u8 load = 0x10 >> (engine * 2);

	mode = i2c_smbus_read_byte_data(chip->client, LP5523_OP_MODE);
	if (mode < 0)
		return mode;
	ret = ef67_led_write(chip, LP5523_OP_MODE, (mode & ~mask) | load);
	if (ret)
		return ret;
	usleep_range(1000, 2000);
	ret = ef67_led_write(chip, LP5523_PROGRAM_PAGE, engine);
	if (ret)
		return ret;
	return i2c_smbus_write_i2c_block_data(chip->client,
			LP5523_PROGRAM_MEMORY, EF67_PROGRAM_SIZE, program);
}

static int ef67_led_run_engines(struct ef67_led_chip *chip, u8 engines)
{
	int ret;

	ret = ef67_led_write(chip, LP5523_ENABLE,
			    LP5523_CHIP_ENABLE | engines);
	if (ret)
		return ret;
	usleep_range(1000, 2000);
	ret = ef67_led_write(chip, LP5523_OP_MODE, engines);
	usleep_range(1000, 2000);
	return ret;
}

/* S stock pan_led_offline_red_charging, 0xc0793968. */
static int ef67_led_start_offline_charging(struct ef67_led_chip *chip)
{
	unsigned int i;
	int ret;

	ret = ef67_led_stop_engines(chip);
	if (ret)
		return ret;
	for (i = 0; i < EF67_LED_CHANNELS; i++) {
		ret = ef67_led_write(chip, LP5523_PWM + i, 0);
		if (ret)
			return ret;
	}
	/* Stock enables logarithmic brightness except on the touchkey outputs. */
	for (i = 0; i < EF67_LED_CHANNELS; i++) {
		if (i == 4 || i == 5)
			continue;
		ret = ef67_led_write(chip, LP5523_LED_CONTROL + i, BIT(5));
		if (ret)
			return ret;
	}
	ret = ef67_led_write(chip, LP5523_PWM + 6, 31);
	if (ret)
		return ret;
	ret = ef67_led_load_program(chip, 0, ef67_offline_charging_program);
	if (ret)
		return ret;
	return ef67_led_run_engines(chip, 0x20);
}

/* Stock timing: channel2 is in 10ms units, channel3 in 100ms units. */
static u8 ef67_led_short_wait(unsigned int ticks)
{
	return 0x40 | (clamp_t(unsigned int, ticks, 1, 31) << 1);
}

static void ef67_led_long_wait(u8 *program, unsigned int offset,
			      unsigned int units, unsigned int scale,
			      unsigned int target)
{
	unsigned int loops = max_t(unsigned int, units * scale / 500000, 1);
	unsigned int remainder = units * scale / 15625 & 31;

	program[offset] = 0xa0 | ((loops >> 1) & 0x7f);
	program[offset + 1] = ((loops & 1) << 7) | target;
	program[offset + 2] = ef67_led_short_wait(remainder);
	if (scale == 100000 && units == 255) {
		/* Stock encodes the longest off interval with loop field 63. */
		program[offset] = 0xbf;
		program[offset + 1] = 0x80 | target;
		program[offset + 2] = 0x7e;
	}
}

static void ef67_led_make_blink(u8 *program, const u8 *values,
				unsigned int engine)
{
	unsigned int on = values[2], off = values[3];
	unsigned int mode = (on > 50) | ((off > 5) << 1);
	unsigned int channel = ef67_rgb_channels[engine];

	memcpy(program, ef67_blink_programs[mode], EF67_PROGRAM_SIZE);
	program[1] = BIT(channel);
	program[3] = engine * 0x10;
	program[5] = values[channel];
	if (mode & 1)
		ef67_led_long_wait(program, 8, on, 10000, 3);
	else
		program[6] = ef67_led_short_wait(on * 10000 / 15625);
	if (mode & 2) {
		/* S stock uses instruction 5 for off=255 in both layouts. */
		ef67_led_long_wait(program, mode == 2 ? 16 : 20,
				   off, 100000, mode == 2 || off == 255 ? 5 : 7);
	} else {
		program[mode == 0 ? 10 : 14] =
			ef67_led_short_wait(off * 100000 / 15625);
	}
}

/* Synchronize the shorter RGB ramps with the longest stock ramp. */
static void ef67_led_fade_trigger(u8 *program, unsigned int offset,
				 unsigned int engine, unsigned int leader,
				 u8 active)
{
	static const u8 peers[] = { 0x0c, 0x0a, 0x06 };
	u8 upper = 0xe0, lower = 0;

	if (engine == leader) {
		lower = (active << 1) & peers[engine];
	} else if (!leader) {
		lower = 0x80;
	} else if (!engine) {
		upper |= BIT(leader - 1);
	} else {
		upper |= engine == 1 ? 2 : 1;
	}
	program[offset] = upper;
	program[offset + 1] = lower;
}

static void ef67_led_make_fade_repeat(u8 *program, const u8 *values,
				      unsigned int engine, unsigned int leader,
				      u8 active)
{
	unsigned int on = values[2], off = values[3];
	unsigned int mode = (on > 50) | ((off > 5) << 1);
	u8 step = values[7] >= 1 && values[7] <= 31 ? values[7] * 2 : 4;
	unsigned int down = mode & 1 ? 14 : 10;
	unsigned int sync_on = mode & 1 ? 12 : 8;
	unsigned int sync_off = (mode & 1 ? 18 : 14) + (mode & 2 ? 4 : 0);

	memcpy(program, ef67_fade_programs[mode + 1], EF67_PROGRAM_SIZE);
	program[1] = BIT(ef67_rgb_channels[engine]);
	program[3] = engine * 0x10;
	program[4] = step;
	program[5] = values[ef67_rgb_channels[engine]];
	if (mode & 1)
		ef67_led_long_wait(program, 8, on, 10000, 3);
	else
		program[6] = ef67_led_short_wait(on * 10000 / 15625);
	program[down] = step + 1;
	program[down + 1] = values[ef67_rgb_channels[engine]];
	if (mode & 2)
		ef67_led_long_wait(program, mode == 2 ? 14 : 18,
				   off, 100000, mode == 2 ? 6 : 8);
	else
		program[down + 2] = ef67_led_short_wait(off * 100000 / 15625);
	ef67_led_fade_trigger(program, sync_on, engine, leader, active);
	ef67_led_fade_trigger(program, sync_off, engine, leader, active);
}

static int ef67_led_pattern(struct ef67_led_chip *chip, const u8 *values,
			    unsigned int command)
{
	u8 program[EF67_PROGRAM_SIZE], engines = 0, active = 0;
	unsigned int engine, channel, leader;
	int ret, delta;

	if (command == EF67_RGB_FADE)
		ret = ef67_led_stop_engines(chip);
	else
		ret = ef67_led_clear_rgb(chip);
	if (ret)
		return ret;

	/* Keep the tie-breaking order of S stock pan_rgb_led_dim_func_repeat. */
	if (values[0] > values[1] && values[6] < values[0])
		leader = 1;
	else
		leader = values[6] > values[1] ? 0 : 2;
	for (engine = 0; engine < ARRAY_SIZE(ef67_rgb_channels); engine++)
		if (values[ef67_rgb_channels[engine]])
			active |= BIT(engine);

	for (engine = 0; engine < ARRAY_SIZE(ef67_rgb_channels); engine++) {
		channel = ef67_rgb_channels[engine];
		if (command == EF67_RGB_FADE) {
			delta = values[channel] - chip->applied[channel];
			if (!delta)
				continue;
			memcpy(program, ef67_fade_programs[0], sizeof(program));
			program[1] = BIT(channel);
			program[3] = engine * 0x10;
			program[4] = values[7] >= 1 && values[7] <= 31 ?
					values[7] * 2 : 4;
			if (delta < 0)
				program[4]++;
			program[5] = abs(delta);
		} else {
			if (!values[channel])
				continue;
			if (command == EF67_RGB_BLINK)
				ef67_led_make_blink(program, values, engine);
			else
				ef67_led_make_fade_repeat(program, values, engine,
						  leader, active);
		}
		ret = ef67_led_load_program(chip, engine, program);
		if (ret)
			return ret;
		engines |= 0x20 >> (engine * 2);
	}
	ret = ef67_led_run_engines(chip, engines);
	if (!ret && command == EF67_RGB_FADE) {
		for (engine = 0; engine < ARRAY_SIZE(ef67_rgb_channels); engine++) {
			channel = ef67_rgb_channels[engine];
			chip->applied[channel] = values[channel];
		}
		if (!values[6] && !values[0] && !values[1]) {
			/* Stock clears PWM even when no fade ramp ran. */
			unsigned int delay_ms = values[7] ?
					250U * values[7] + 100 : 500;

			queue_delayed_work(chip->queue,
					   &chip->rgb_fade_off_work,
					   msecs_to_jiffies(delay_ms));
		}
	}
	return ret;
}

static int ef67_led_apply(struct ef67_led_request *request)
{
	struct ef67_led_chip *chip = request->chip;
	const u8 *values = request->values;
	unsigned int i, channel, command = values[8];
	int ret;

	if (request->channel == 4 || request->channel == 5)
		return ef67_led_write(chip, LP5523_PWM + request->channel,
				      values[request->channel]);

	switch (command) {
	case EF67_RGB_OFF:
		return ef67_led_clear_rgb(chip);
	case EF67_RGB_PWM:
		ret = ef67_led_stop_engines(chip);
		if (ret)
			return ret;
		for (i = 0; i < ARRAY_SIZE(ef67_rgb_channels); i++) {
			channel = ef67_rgb_channels[i];
			ret = ef67_led_write(chip, LP5523_PWM + channel,
					      values[channel]);
			if (ret)
				return ret;
			chip->applied[channel] = values[channel];
		}
		return 0;
	case EF67_RGB_BLINK:
	case EF67_RGB_FADE:
	case EF67_RGB_FADE_REPEAT:
		return ef67_led_pattern(chip, values, command);
	case EF67_ALL_OFF:
		ret = ef67_led_clear_rgb(chip);
		if (ret)
			return ret;
		for (i = 4; i <= 5; i++) {
			ret = ef67_led_write(chip, LP5523_PWM + i, 0);
			if (ret)
				return ret;
		}
		return 0;
	default:
		return -EINVAL;
	}
}

static void ef67_led_work(struct work_struct *work)
{
	struct ef67_led_request *request = container_of(work,
					struct ef67_led_request, work);
	struct ef67_led_chip *chip = request->chip;
	int ret;

	mutex_lock(&chip->lock);
	/* Cancel old fade-off before a new RGB command. */
	if (request->channel == 8)
		cancel_delayed_work(&chip->rgb_fade_off_work);
	ret = ef67_led_apply(request);
	if (ret) {
		/* A partly loaded RGB program must not keep executing. */
		if (request->channel == 8 && ret != -EINVAL)
			ef67_led_clear_rgb(chip);
		dev_err_ratelimited(&chip->client->dev,
			"LED channel %u value %u failed: %d\n", request->channel,
			request->values[request->channel], ret);
	}
	mutex_unlock(&chip->lock);
	kfree(request);
}

static void ef67_led_set_brightness(struct led_classdev *cdev,
				    enum led_brightness brightness)
{
	struct ef67_led *led = container_of(cdev, struct ef67_led, cdev);
	struct ef67_led_chip *chip = led->chip;
	struct ef67_led_request *request = NULL;
	unsigned long flags;

	if (led->channel == 4 || led->channel == 5 || led->channel == 8) {
		request = kzalloc(sizeof(*request), GFP_ATOMIC);
		if (!request) {
			dev_err_ratelimited(&chip->client->dev,
					    "Cannot queue LED update\n");
			return;
		}
		INIT_WORK(&request->work, ef67_led_work);
		request->chip = chip;
		request->channel = led->channel;
	}
	spin_lock_irqsave(&chip->state_lock, flags);
	if (!chip->stopping) {
		chip->values[led->channel] = brightness;
		if (request) {
			memcpy(request->values, chip->values, sizeof(request->values));
			queue_work(chip->queue, &request->work);
			request = NULL;
		}
	}
	spin_unlock_irqrestore(&chip->state_lock, flags);
	kfree(request);
}

static ssize_t led_current_show(struct device *dev,
				struct device_attribute *attr, char *buf)
{
	struct led_classdev *cdev = dev_get_drvdata(dev);
	struct ef67_led *led = container_of(cdev, struct ef67_led, cdev);

	return scnprintf(buf, PAGE_SIZE, "%u\n", led->led_current);
}

static ssize_t led_current_store(struct device *dev,
				 struct device_attribute *attr,
				 const char *buf, size_t count)
{
	struct led_classdev *cdev = dev_get_drvdata(dev);
	struct ef67_led *led = container_of(cdev, struct ef67_led, cdev);
	struct ef67_led_chip *chip = led->chip;
	unsigned long requested_current;
	unsigned long flags;
	bool stopping;
	int ret;

	if (kstrtoul(buf, 0, &requested_current) ||
	    requested_current > chip->max_current)
		return -EINVAL;
	if (led->channel == 2 || led->channel == 3 || led->channel >= 7)
		return -EOPNOTSUPP;
	mutex_lock(&chip->lock);
	spin_lock_irqsave(&chip->state_lock, flags);
	stopping = chip->stopping;
	spin_unlock_irqrestore(&chip->state_lock, flags);
	ret = stopping ? -ENODEV :
		ef67_led_write(chip, LP5523_CURRENT + led->channel,
			       requested_current);
	if (!ret)
		led->led_current = requested_current;
	mutex_unlock(&chip->lock);
	return ret ? ret : count;
}

static ssize_t max_current_show(struct device *dev,
				struct device_attribute *attr, char *buf)
{
	struct led_classdev *cdev = dev_get_drvdata(dev);
	struct ef67_led *led = container_of(cdev, struct ef67_led, cdev);

	return scnprintf(buf, PAGE_SIZE, "%u\n", led->chip->max_current);
}

static DEVICE_ATTR(led_current, S_IRUGO | S_IWUSR,
		   led_current_show, led_current_store);
static DEVICE_ATTR(max_current, S_IRUGO, max_current_show, NULL);

static struct attribute *ef67_led_attributes[] = {
	&dev_attr_led_current.attr,
	&dev_attr_max_current.attr,
	NULL,
};

static const struct attribute_group ef67_led_attribute_group = {
	.attrs = ef67_led_attributes,
};

static int ef67_led_init(struct ef67_led_chip *chip)
{
	static const u8 mux_programs[][EF67_PROGRAM_SIZE] = {
		{ 0x9c, 0x30, 0x9c, 0xb0, 0x9d, 0x80, 0xd8, 0 },
		{ 0x9c, 0x40, 0x9c, 0xc0, 0x9d, 0x80, 0xd8, 0 },
		{ 0x9c, 0x50, 0x9c, 0xd0, 0x9d, 0x80, 0xd8, 0 },
	};
	int ret, i;

	gpio_set_value_cansleep(chip->enable_gpio, 0);
	usleep_range(1000, 2000);
	gpio_set_value_cansleep(chip->enable_gpio, 1);
	usleep_range(1000, 2000);
	/* A reset can complete without an ACK. Detect the device afterwards. */
	ef67_led_write(chip, LP5523_RESET, 0xff);
	usleep_range(10000, 20000);
	ret = ef67_led_write(chip, LP5523_ENABLE, LP5523_CHIP_ENABLE);
	if (ret)
		return ret;
	usleep_range(1000, 2000);
	ret = i2c_smbus_read_byte_data(chip->client, LP5523_ENABLE);
	if (ret < 0)
		return ret;
	if (ret != LP5523_CHIP_ENABLE)
		return -ENODEV;
	ret = ef67_led_write(chip, LP5523_CONFIG, 0x7e);
	if (ret)
		return ret;
	ret = ef67_led_write(chip, LP5523_ENABLE_MSB, 0);
	if (ret)
		return ret;
	/* S stock enables D1/D2/D5/D6/D7; D9 is not an LED endpoint. */
	ret = ef67_led_write(chip, LP5523_ENABLE_LSB, 0x73);
	if (ret)
		return ret;
	for (i = 0; i < 3; i++) {
		ret = ef67_led_write(chip, LP5523_PROGRAM_START + i, i * 0x10);
		if (ret)
			return ret;
		ret = ef67_led_load_program(chip, i, mux_programs[i]);
		if (ret)
			return ret;
	}
	ret = ef67_led_run_engines(chip, 0x2a);
	if (ret)
		return ret;
	usleep_range(3000, 6000);
	ret = i2c_smbus_read_byte_data(chip->client, LP5523_STATUS);
	if (ret < 0)
		return ret;
	if ((ret & 7) != 7)
		return -ENODEV;
	ret = ef67_led_stop_engines(chip);
	if (ret)
		return ret;
	for (i = 0; i < EF67_LED_CHANNELS; i++) {
		ret = ef67_led_write(chip, LP5523_PWM + i, 0);
		if (ret)
			return ret;
		ret = ef67_led_write(chip, LP5523_CURRENT + i,
				      chip->leds[i].led_current);
		if (ret)
			return ret;
	}
	return 0;
}

static void ef67_led_unregister(struct ef67_led_chip *chip, unsigned int count)
{
	unsigned long flags;

	spin_lock_irqsave(&chip->state_lock, flags);
	chip->stopping = true;
	spin_unlock_irqrestore(&chip->state_lock, flags);
	while (count--) {
		sysfs_remove_group(&chip->leds[count].cdev.dev->kobj,
				   &ef67_led_attribute_group);
		led_classdev_unregister(&chip->leds[count].cdev);
	}
	/* Queued commands can still arm fade-off while they drain. */
	flush_workqueue(chip->queue);
	cancel_delayed_work_sync(&chip->rgb_fade_off_work);
	destroy_workqueue(chip->queue);
	gpio_set_value_cansleep(chip->enable_gpio, 0);
}

static int ef67_led_probe(struct i2c_client *client,
			  const struct i2c_device_id *id)
{
	struct device *dev = &client->dev;
	struct ef67_led_chip *chip;
	const u32 *boot_data;
	unsigned int boot_data_size = 0;
	u32 clock, channels, led_current, max_current, touchkeys[2];
	int ret, i;

	if (!i2c_check_functionality(client->adapter,
		I2C_FUNC_SMBUS_BYTE_DATA | I2C_FUNC_SMBUS_WRITE_I2C_BLOCK))
		return -EOPNOTSUPP;
	if (of_property_read_u32(dev->of_node, "lp5523,clock_mode", &clock) ||
	    of_property_read_u32(dev->of_node, "lp5523,max_chan", &channels) ||
	    of_property_read_u32(dev->of_node, "lp5523,led_current", &led_current) ||
	    of_property_read_u32(dev->of_node, "lp5523,max_current", &max_current) ||
	    of_property_read_u32_array(dev->of_node,
			"lp5523,trg_touchkey-backlight", touchkeys, 2))
		return -EINVAL;
	if (clock != 1 || channels != EF67_LED_CHANNELS || !led_current ||
	    led_current > max_current || max_current > 255 ||
	    touchkeys[0] != 4 || touchkeys[1] != 5)
		return -EINVAL;

	/* Read the existing S/L boot data; do not allocate or overwrite it. */
	boot_data = smem_get_entry(SMEM_ID_VENDOR1, &boot_data_size, 0,
				   SMEM_ANY_HOST_FLAG);
	if (IS_ERR(boot_data))
		return PTR_ERR(boot_data);
	if (!boot_data)
		return -ENODATA;
	if (boot_data_size != 40 && boot_data_size != 48)
		return -EINVAL;

	chip = devm_kzalloc(dev, sizeof(*chip), GFP_KERNEL);
	if (!chip)
		return -ENOMEM;
	chip->client = client;
	chip->max_current = max_current;
	chip->enable_gpio = of_get_named_gpio(dev->of_node, "lp5523,led_en_gpio", 0);
	if (chip->enable_gpio < 0)
		return chip->enable_gpio;
	ret = devm_gpio_request_one(dev, chip->enable_gpio,
				   GPIOF_OUT_INIT_LOW, "ef67-lp5523-enable");
	if (ret)
		return ret;
	mutex_init(&chip->lock);
	spin_lock_init(&chip->state_lock);
	INIT_DELAYED_WORK(&chip->rgb_fade_off_work, ef67_led_fade_off_work);
	chip->queue = alloc_ordered_workqueue("ef67-lp5523", WQ_MEM_RECLAIM);
	if (!chip->queue)
		return -ENOMEM;
	i2c_set_clientdata(client, chip);
	for (i = 0; i < EF67_LED_CHANNELS; i++)
		chip->leds[i].led_current = led_current;
	ret = ef67_led_init(chip);
	if (ret)
		goto fail;
	/* Both S and L stock test power_on_mode at byte offset 16 for zero. */
	if (!boot_data[4]) {
		ret = ef67_led_start_offline_charging(chip);
		if (ret)
			goto fail;
	}
	for (i = 0; i < EF67_LED_CHANNELS; i++) {
		struct ef67_led *led = &chip->leds[i];

		led->chip = chip;
		led->channel = i;
		snprintf(led->name, sizeof(led->name), "lp5523:channel%d", i);
		led->cdev.name = led->name;
		led->cdev.brightness_set = ef67_led_set_brightness;
		ret = led_classdev_register(dev, &led->cdev);
		if (ret)
			goto unregister;
		ret = sysfs_create_group(&led->cdev.dev->kobj,
					 &ef67_led_attribute_group);
		if (ret) {
			led_classdev_unregister(&led->cdev);
			goto unregister;
		}
	}
	return 0;

unregister:
	ef67_led_unregister(chip, i);
	return ret;
fail:
	destroy_workqueue(chip->queue);
	gpio_set_value_cansleep(chip->enable_gpio, 0);
	return ret;
}

static int ef67_led_remove(struct i2c_client *client)
{
	struct ef67_led_chip *chip = i2c_get_clientdata(client);

	ef67_led_unregister(chip, EF67_LED_CHANNELS);
	return 0;
}

static void ef67_led_shutdown(struct i2c_client *client)
{
	struct ef67_led_chip *chip = i2c_get_clientdata(client);
	unsigned long flags;

	spin_lock_irqsave(&chip->state_lock, flags);
	chip->stopping = true;
	spin_unlock_irqrestore(&chip->state_lock, flags);
	flush_workqueue(chip->queue);
	cancel_delayed_work_sync(&chip->rgb_fade_off_work);
	mutex_lock(&chip->lock);
	gpio_set_value_cansleep(chip->enable_gpio, 0);
	mutex_unlock(&chip->lock);
}

static const struct of_device_id ef67_led_match[] = {
	{ .compatible = "ti,lp5523_led_driver" },
	{ }
};
MODULE_DEVICE_TABLE(of, ef67_led_match);

static const struct i2c_device_id ef67_led_ids[] = {
	{ "lp5523_led_driver", 0 },
	{ }
};
MODULE_DEVICE_TABLE(i2c, ef67_led_ids);

static struct i2c_driver ef67_led_driver = {
	.driver = {
		.name = "ef67-lp5523",
		.of_match_table = ef67_led_match,
	},
	.probe = ef67_led_probe,
	.remove = ef67_led_remove,
	.shutdown = ef67_led_shutdown,
	.id_table = ef67_led_ids,
};
module_i2c_driver(ef67_led_driver);

MODULE_DESCRIPTION("Pantech EF67 LP5523 RGB and touchkey LEDs");
MODULE_LICENSE("GPL v2");
