/*
 * Pantech EF67 Atmel mXT641T touchscreen.
 *
 * Object protocol and T100 input follow the CAF atmel_maxtouch_ts driver
 * and the Pantech Atmel reference. Board sequencing, configurations and
 * key regions follow IM-A930S S0226114. No firmware or NVM writes.
 *
 * Copyright (C) 2010 Samsung Electronics Co.Ltd
 * Copyright (C) 2011-2012 Atmel Corporation
 * Copyright (C) 2012 Google, Inc.
 * Licensed under the GNU General Public License version 2.
 */
#include <linux/delay.h>
#include <linux/fb.h>
#include <linux/gpio.h>
#include <linux/i2c.h>
#include <linux/input/mt.h>
#include <linux/interrupt.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of_gpio.h>
#include <linux/regulator/consumer.h>
#include <linux/slab.h>
#include <asm/unaligned.h>

#include "atmel-mxt-ef67-config.h"

#define EF67_MXT_CONTACTS	10
#define EF67_MXT_SCREEN_X	1439
#define EF67_MXT_SCREEN_Y	2559
#define EF67_MXT_SENSOR_Y	2684
#define EF67_MXT_CONFIG_HEADER	11
#define EF67_MXT_MAX_MESSAGE	32
#define EF67_MXT_MESSAGE_T5	5
#define EF67_MXT_COMMAND_T6	6
#define EF67_MXT_POWER_T7		7
#define EF67_MXT_COUNT_T44	44
#define EF67_MXT_TOUCH_T100	100
#define EF67_MXT_RESET		0
#define EF67_MXT_CALIBRATE	2
#define EF67_MXT_T100_UNSUPPRESS	2

struct ef67_mxt_object {
	u8 type;
	__le16 address;
	u8 size_minus_one;
	u8 instances_minus_one;
	u8 report_ids;
} __packed;

enum ef67_mxt_mode {
	EF67_MXT_FINGER,
	EF67_MXT_GLOVE,
	EF67_MXT_STYLUS,
};

struct ef67_mxt {
	struct i2c_client *client;
	struct input_dev *input;
	struct mutex lock;
	struct regulator *avdd;
	struct ef67_mxt_object *objects;
	u8 object_count;
	u16 message_address;
	u16 command_address;
	u16 power_address;
	u16 count_address;
	u8 message_size;
	u8 touch_report_min;
	u8 touch_report_max;
	u8 command_report;
	u8 *messages;
	int reset_gpio;
	int power_gpio;
	int irq_gpio;
	enum ef67_mxt_mode mode;
	u8 contact_route[EF67_MXT_CONTACTS];
	u16 key_contacts[3];
	bool powered;
	bool configured;
	bool controller_fault;
	bool irq_enabled;
	bool system_suspended;
	bool display_blank;
	bool scanning;
	bool asleep;
	bool stopping;
#ifdef CONFIG_FB
	struct notifier_block fb_notifier;
#endif
};

/* S stock report_input uses vendor code 253 for the left capacitive key. */
static const unsigned int ef67_mxt_keys[] = { 253, KEY_HOMEPAGE, KEY_BACK };
static const char * const ef67_mxt_mode_names[] = {
	"finger", "glove", "stylus",
};

static int ef67_mxt_read(struct ef67_mxt *touch, u16 address,
			 unsigned int size, void *buffer)
{
	u8 reg[] = { address & 0xff, address >> 8 };
	struct i2c_msg messages[] = {
		{ .addr = touch->client->addr, .len = sizeof(reg), .buf = reg },
		{ .addr = touch->client->addr, .flags = I2C_M_RD,
		  .len = size, .buf = buffer },
	};
	int ret = i2c_transfer(touch->client->adapter, messages, 2);

	return ret == 2 ? 0 : (ret < 0 ? ret : -EIO);
}

static int ef67_mxt_write(struct ef67_mxt *touch, u16 address,
			  unsigned int size, const void *buffer)
{
	u8 packet[257];
	int ret;

	if (size > sizeof(packet) - 2)
		return -EINVAL;
	packet[0] = address & 0xff;
	packet[1] = address >> 8;
	memcpy(packet + 2, buffer, size);
	ret = i2c_master_send(touch->client, packet, size + 2);
	return ret == size + 2 ? 0 : (ret < 0 ? ret : -EIO);
}

static int ef67_mxt_command(struct ef67_mxt *touch, u8 offset, u8 value)
{
	u8 pending;
	int ret, retry;

	ret = ef67_mxt_write(touch, touch->command_address + offset, 1, &value);
	if (ret)
		return ret;
	for (retry = 0; retry < 100; retry++) {
		msleep(20);
		ret = ef67_mxt_read(touch, touch->command_address + offset,
				    1, &pending);
		if (ret || !pending)
			return ret;
	}
	return -ETIMEDOUT;
}

static unsigned int ef67_mxt_object_size(const struct ef67_mxt_object *object)
{
	return object->size_minus_one + 1;
}

static struct ef67_mxt_object *ef67_mxt_object(struct ef67_mxt *touch, u8 type)
{
	unsigned int i;

	for (i = 0; i < touch->object_count; i++)
		if (touch->objects[i].type == type)
			return &touch->objects[i];
	return NULL;
}

static u32 ef67_mxt_crc(const u8 *buffer, unsigned int size)
{
	u32 crc = 0;
	unsigned int i;

	for (i = 0; i < size; i += 2) {
		u16 word = buffer[i];

		if (i + 1 < size)
			word |= buffer[i + 1] << 8;
		crc = (crc << 1) ^ word;
		if (crc & BIT(24))
			crc ^= 0x80001b;
	}
	return crc & 0xffffff;
}

static int ef67_mxt_read_objects(struct ef67_mxt *touch)
{
	struct device *dev = &touch->client->dev;
	struct ef67_mxt_object *object;
	u8 info[7], *block;
	unsigned int size, i, j, report_id = 1, address, object_size, instances;
	u32 crc;
	int ret;

	ret = ef67_mxt_read(touch, 0, sizeof(info), info);
	if (ret)
		return ret;
	/* Configurations are for this firmware revision, not EF71 firmware. */
	if (memcmp(info, ef67_mxt_finger_config, sizeof(info))) {
		dev_err(dev, "Unsupported mXT641T firmware or object count\n");
		return -ENODEV;
	}
	touch->object_count = info[6];
	size = sizeof(info) + touch->object_count * sizeof(*object) + 3;
	block = devm_kzalloc(dev, size, GFP_KERNEL);
	if (!block)
		return -ENOMEM;
	ret = ef67_mxt_read(touch, 0, size, block);
	if (ret)
		return ret;
	crc = block[size - 3] | (block[size - 2] << 8) |
		(block[size - 1] << 16);
	if (!crc || crc != ef67_mxt_crc(block, size - 3))
		return -EBADMSG;
	touch->objects = (void *)(block + sizeof(info));
	for (i = 0; i < touch->object_count; i++) {
		object = &touch->objects[i];
		for (j = 0; j < i; j++)
			if (touch->objects[j].type == object->type)
				return -EINVAL;
		address = le16_to_cpu(object->address);
		object_size = ef67_mxt_object_size(object);
		instances = object->instances_minus_one + 1;
		if (address < size || address + object_size * instances > 65536 ||
		    report_id + object->report_ids * instances > 255)
			return -EINVAL;
		switch (object->type) {
		case EF67_MXT_MESSAGE_T5:
			touch->message_address = address;
			touch->message_size = object_size - 1;
			break;
		case EF67_MXT_COMMAND_T6:
			if (object_size < 6 || instances != 1 || object->report_ids != 1)
				return -EINVAL;
			touch->command_address = address;
			touch->command_report = report_id;
			break;
		case EF67_MXT_POWER_T7:
			touch->power_address = address;
			break;
		case EF67_MXT_COUNT_T44:
			if (object_size != 1 || instances != 1)
				return -EINVAL;
			touch->count_address = address;
			break;
		case EF67_MXT_TOUCH_T100:
			/* Report capacity includes two status IDs and can exceed NUMTCH. */
			if (instances != 1 || object->report_ids < EF67_MXT_CONTACTS + 2)
				return -EINVAL;
			touch->touch_report_min = report_id;
			touch->touch_report_max = report_id + object->report_ids - 1;
			break;
		}
		report_id += object->report_ids * instances;
	}
	if (!touch->message_address || !touch->command_address ||
	    !touch->power_address || !touch->touch_report_min ||
	    !touch->count_address || touch->message_size < 7 ||
	    touch->message_size > EF67_MXT_MAX_MESSAGE)
		return -EINVAL;
	touch->messages = devm_kzalloc(dev, 255 * touch->message_size, GFP_KERNEL);
	return touch->messages ? 0 : -ENOMEM;
}

static const u8 *ef67_mxt_config(enum ef67_mxt_mode mode)
{
	switch (mode) {
	case EF67_MXT_GLOVE:
		return ef67_mxt_glove_config;
	case EF67_MXT_STYLUS:
		return ef67_mxt_stylus_config;
	default:
		return ef67_mxt_finger_config;
	}
}

/* Validate every record before changing the controller's volatile settings. */
static int ef67_mxt_apply_config(struct ef67_mxt *touch,
				 enum ef67_mxt_mode mode)
{
	const u8 *config = ef67_mxt_config(mode);
	const unsigned int length = sizeof(ef67_mxt_finger_config);
	struct ef67_mxt_object *object;
	unsigned int offset, size, instance;
	u16 address;
	int ret;

	for (offset = EF67_MXT_CONFIG_HEADER; offset < length; offset += 3 + size) {
		if (length - offset < 3)
			return -EINVAL;
		object = ef67_mxt_object(touch, config[offset]);
		instance = config[offset + 1];
		size = config[offset + 2];
		if (!object || !size || size > length - offset - 3 ||
		    size != ef67_mxt_object_size(object) ||
		    instance > object->instances_minus_one)
			return -EINVAL;
	}
	touch->configured = false;
	touch->asleep = false;
	for (offset = EF67_MXT_CONFIG_HEADER; offset < length; offset += 3 + size) {
		object = ef67_mxt_object(touch, config[offset]);
		instance = config[offset + 1];
		size = config[offset + 2];
		address = le16_to_cpu(object->address) + size * instance;
		ret = ef67_mxt_write(touch, address, size, config + offset + 3);
		if (ret)
			return ret;
	}
	ret = ef67_mxt_command(touch, EF67_MXT_CALIBRATE, 1);
	if (ret)
		return ret;
	touch->mode = mode;
	touch->configured = true;
	touch->controller_fault = false;
	return 0;
}

static int ef67_mxt_power_on(struct ef67_mxt *touch)
{
	int ret = regulator_enable(touch->avdd);

	if (ret)
		return ret;
	touch->powered = true;
	gpio_set_value_cansleep(touch->power_gpio, 1);
	msleep(10);
	gpio_set_value_cansleep(touch->reset_gpio, 0);
	msleep(100);
	gpio_set_value_cansleep(touch->reset_gpio, 1);
	msleep(110);
	return 0;
}

static void ef67_mxt_power_off(struct ef67_mxt *touch)
{
	int ret;

	if (!touch->powered)
		return;
	gpio_set_value_cansleep(touch->reset_gpio, 0);
	gpio_set_value_cansleep(touch->power_gpio, 0);
	msleep(10);
	ret = regulator_disable(touch->avdd);
	if (ret)
		dev_err(&touch->client->dev, "AVDD disable failed: %d\n", ret);
	else
		touch->powered = false;
	touch->configured = false;
}

/* S stock get_touch_key_pos at 0xc0646c10, inclusive boundaries. */
static unsigned int ef67_mxt_key_route(unsigned int x, unsigned int y)
{
	if (y < 2585 || y > 2685)
		return 0;
	if (x >= 125 && x <= 355)
		return 2;
	if (x >= 520 && x <= 920)
		return 3;
	if (x >= 1085 && x <= 1315)
		return 4;
	return 0;
}

static void ef67_mxt_release_contact(struct ef67_mxt *touch, unsigned int id)
{
	unsigned int route = touch->contact_route[id];

	input_mt_slot(touch->input, id);
	input_mt_report_slot_state(touch->input, MT_TOOL_FINGER, false);
	if (route >= 2) {
		touch->key_contacts[route - 2] &= ~BIT(id);
		input_report_key(touch->input, ef67_mxt_keys[route - 2],
				 !!touch->key_contacts[route - 2]);
	}
	touch->contact_route[id] = 0;
}

static void ef67_mxt_sync(struct ef67_mxt *touch)
{
	input_mt_report_pointer_emulation(touch->input, false);
	input_sync(touch->input);
}

static void ef67_mxt_release_all(struct ef67_mxt *touch)
{
	unsigned int id;

	for (id = 0; id < EF67_MXT_CONTACTS; id++)
		ef67_mxt_release_contact(touch, id);
	ef67_mxt_sync(touch);
}

static void ef67_mxt_report_touch(struct ef67_mxt *touch, const u8 *message)
{
	unsigned int id = message[0] - touch->touch_report_min - 2;
	unsigned int type = message[1] & 0x70;
	unsigned int event = message[1] & 0x0f;
	unsigned int x = get_unaligned_le16(message + 2);
	unsigned int y = get_unaligned_le16(message + 4);
	unsigned int route, tool;

	if (id >= EF67_MXT_CONTACTS)
		return;
	if (!(message[1] & 0x80) || event == 3 || event == 5 ||
	    (type != 0x10 && type != 0x20 && type != 0x50) ||
	    x > EF67_MXT_SCREEN_X || y > EF67_MXT_SENSOR_Y) {
		ef67_mxt_release_contact(touch, id);
		return;
	}
	/* Stock accepts NONE, MOVE, DOWN and UNSUPPRESS for detected touches. */
	if (event != 0 && event != 1 && event != 4 &&
	    event != EF67_MXT_T100_UNSUPPRESS)
		return;
	route = touch->contact_route[id];
	if (!route) {
		route = y <= EF67_MXT_SCREEN_Y ? 1 : ef67_mxt_key_route(x, y);
		touch->contact_route[id] = route;
		if (route >= 2) {
			touch->key_contacts[route - 2] |= BIT(id);
			input_report_key(touch->input, ef67_mxt_keys[route - 2], 1);
		}
	}
	if (route != 1)
		return;
	tool = type == 0x20 ? MT_TOOL_PEN : MT_TOOL_FINGER;
	input_mt_slot(touch->input, id);
	input_mt_report_slot_state(touch->input, tool, true);
	input_report_abs(touch->input, ABS_MT_POSITION_X, x);
	input_report_abs(touch->input, ABS_MT_POSITION_Y,
			 min_t(unsigned int, y, EF67_MXT_SCREEN_Y));
	input_report_abs(touch->input, ABS_MT_TOUCH_MAJOR,
			 tool == MT_TOOL_PEN ? 1 : message[6]);
}

static int ef67_mxt_drain(struct ef67_mxt *touch, bool report)
{
	u8 count, *message;
	unsigned int batch, i;
	int ret;

	for (batch = 0; batch < 16; batch++) {
		ret = ef67_mxt_read(touch, touch->count_address, 1, &count);
		if (ret || !count)
			return ret;
		ret = ef67_mxt_read(touch, touch->message_address,
				    count * touch->message_size, touch->messages);
		if (ret)
			return ret;
		for (i = 0; i < count; i++) {
			message = touch->messages + i * touch->message_size;
			if (message[0] == touch->command_report &&
			    ((message[1] & 0x08) ||
			     (touch->configured && (message[1] & 0x80)))) {
				/* A reset discards RAM settings; require a full reapply. */
				ACCESS_ONCE(touch->controller_fault) = true;
				return -EIO;
			}
			if (report && message[0] >= touch->touch_report_min + 2 &&
			    message[0] <= touch->touch_report_max)
				ef67_mxt_report_touch(touch, message);
		}
		if (report)
			ef67_mxt_sync(touch);
	}
	return -EOVERFLOW;
}

static irqreturn_t ef67_mxt_interrupt(int irq, void *context)
{
	struct ef67_mxt *touch = context;
	int ret = ef67_mxt_drain(touch, !ACCESS_ONCE(touch->controller_fault));

	if (ret) {
		/* A failed FIFO read can leave a falling-edge IRQ asserted. */
		ACCESS_ONCE(touch->controller_fault) = true;
		ef67_mxt_release_all(touch);
		dev_err_ratelimited(&touch->client->dev,
				    "Touch message read failed: %d\n", ret);
	}
	return IRQ_HANDLED;
}

/* S stock pan_touch_resume_work_func resets and waits 100ms before start. */
static int ef67_mxt_wake(struct ef67_mxt *touch)
{
	u8 reset = 1;
	int ret;

	/* Reset reloads NVM, so restore the selected RAM configuration. */
	touch->configured = false;
	touch->asleep = false;
	ret = ef67_mxt_write(touch, touch->command_address + EF67_MXT_RESET,
			     1, &reset);
	if (ret)
		return ret;
	msleep(100);
	ret = ef67_mxt_drain(touch, false);
	if (ret)
		return ret;
	return ef67_mxt_apply_config(touch, touch->mode);
}

/* Control callbacks hold lock; the IRQ thread never takes it. */
static void ef67_mxt_disable_irq(struct ef67_mxt *touch)
{
	if (touch->irq_enabled) {
		disable_irq(touch->client->irq);
		touch->irq_enabled = false;
	}
}

static int ef67_mxt_update_scanning(struct ef67_mxt *touch)
{
	bool scan = !touch->system_suspended && !touch->display_blank;
	u8 sleep[] = { 0, 0 };
	u8 run[] = { 255, 255 };
	int ret;

	/* Only a completed T7 sleep write proves the controller is asleep. */
	if (touch->stopping || (!scan && touch->asleep) ||
	    (scan && touch->scanning && !ACCESS_ONCE(touch->controller_fault)))
		return 0;
	ef67_mxt_disable_irq(touch);
	ef67_mxt_release_all(touch);
	touch->scanning = false;
	if (!scan) {
		ret = ef67_mxt_write(touch, touch->power_address, 2, sleep);
		if (!ret)
			touch->asleep = true;
		return ret;
	}
	if (touch->asleep || !touch->configured ||
	    ACCESS_ONCE(touch->controller_fault)) {
		ret = ef67_mxt_wake(touch);
	} else {
		ret = ef67_mxt_write(touch, touch->power_address, 2, run);
		if (!ret)
			ret = ef67_mxt_command(touch, EF67_MXT_CALIBRATE, 1);
	}
	if (ret)
		return ret;
	ret = ef67_mxt_drain(touch, false);
	if (ret)
		return ret;
	touch->scanning = true;
	touch->irq_enabled = true;
	enable_irq(touch->client->irq);
	return 0;
}

static ssize_t touch_mode_show(struct device *dev,
			      struct device_attribute *attribute, char *buffer)
{
	struct ef67_mxt *touch = i2c_get_clientdata(to_i2c_client(dev));
	ssize_t ret;

	mutex_lock(&touch->lock);
	ret = touch->configured && !ACCESS_ONCE(touch->controller_fault) ?
		scnprintf(buffer, PAGE_SIZE, "%s\n",
			ef67_mxt_mode_names[touch->mode]) : -EIO;
	mutex_unlock(&touch->lock);
	return ret;
}

static ssize_t touch_mode_store(struct device *dev,
			       struct device_attribute *attribute,
			       const char *buffer, size_t count)
{
	struct ef67_mxt *touch = i2c_get_clientdata(to_i2c_client(dev));
	unsigned int mode;
	int ret = 0;

	for (mode = 0; mode < ARRAY_SIZE(ef67_mxt_mode_names); mode++)
		if (sysfs_streq(buffer, ef67_mxt_mode_names[mode]))
			break;
	if (mode == ARRAY_SIZE(ef67_mxt_mode_names))
		return -EINVAL;
	mutex_lock(&touch->lock);
	if (touch->stopping) {
		ret = -ENODEV;
		goto out;
	}
	if (touch->system_suspended || touch->display_blank) {
		ret = -EBUSY;
		goto out;
	}
	if (touch->configured && !ACCESS_ONCE(touch->controller_fault) &&
	    touch->mode == mode) {
		ret = ef67_mxt_update_scanning(touch);
		goto out;
	}
	ef67_mxt_disable_irq(touch);
	ef67_mxt_release_all(touch);
	touch->scanning = false;
	ret = ef67_mxt_apply_config(touch, mode);
	if (!ret)
		ret = ef67_mxt_update_scanning(touch);
out:
	mutex_unlock(&touch->lock);
	return ret ? ret : count;
}

static DEVICE_ATTR(touch_mode, S_IRUGO | S_IWUSR,
		   touch_mode_show, touch_mode_store);

static struct attribute *ef67_mxt_attributes[] = {
	&dev_attr_touch_mode.attr,
	NULL,
};

static const struct attribute_group ef67_mxt_attribute_group = {
	.attrs = ef67_mxt_attributes,
};

#ifdef CONFIG_FB
static int ef67_mxt_fb_event(struct notifier_block *notifier,
			     unsigned long event, void *context)
{
	struct ef67_mxt *touch = container_of(notifier, struct ef67_mxt, fb_notifier);
	struct fb_event *fb_event = context;
	int blank, ret;

	if (event != FB_EVENT_BLANK || !fb_event || !fb_event->data ||
	    !fb_event->info || fb_event->info->node != 0)
		return NOTIFY_DONE;
	blank = *(int *)fb_event->data;
	mutex_lock(&touch->lock);
	touch->display_blank = blank != FB_BLANK_UNBLANK;
	ret = ef67_mxt_update_scanning(touch);
	mutex_unlock(&touch->lock);
	if (ret)
		dev_err(&touch->client->dev, "Touch display transition failed: %d\n", ret);
	return NOTIFY_OK;
}
#endif

static int ef67_mxt_setup_power(struct ef67_mxt *touch)
{
	struct device *dev = &touch->client->dev;
	int ret;

	touch->reset_gpio = of_get_named_gpio(dev->of_node, "atmel,reset-gpio", 0);
	if (touch->reset_gpio < 0)
		return touch->reset_gpio;
	touch->power_gpio = of_get_named_gpio(dev->of_node, "atmel,iovdd-gpio", 0);
	if (touch->power_gpio < 0)
		return touch->power_gpio;
	touch->irq_gpio = of_get_named_gpio(dev->of_node, "atmel,irq-gpio", 0);
	if (touch->irq_gpio < 0)
		return touch->irq_gpio;
	touch->avdd = devm_regulator_get(dev, "avdd");
	if (IS_ERR(touch->avdd))
		return PTR_ERR(touch->avdd);
	ret = regulator_set_voltage(touch->avdd, 3300000, 3300000);
	if (ret)
		return ret;
	ret = devm_gpio_request_one(dev, touch->reset_gpio, GPIOF_OUT_INIT_LOW,
				   "ef67-touch-reset");
	if (ret)
		return ret;
	ret = devm_gpio_request_one(dev, touch->power_gpio, GPIOF_OUT_INIT_LOW,
				   "ef67-touch-iovdd");
	if (ret)
		return ret;
	ret = devm_gpio_request_one(dev, touch->irq_gpio, GPIOF_IN, "ef67-touch-irq");
	if (ret)
		return ret;
	return ef67_mxt_power_on(touch);
}

static int ef67_mxt_register_input(struct ef67_mxt *touch)
{
	struct input_dev *input;
	unsigned int i;
	int ret;

	input = input_allocate_device();
	if (!input)
		return -ENOMEM;
	touch->input = input;
	input->name = "atmel_mxt_641t";
	input->id.bustype = BUS_I2C;
	input->dev.parent = &touch->client->dev;
	input_set_capability(input, EV_KEY, BTN_TOUCH);
	for (i = 0; i < ARRAY_SIZE(ef67_mxt_keys); i++)
		input_set_capability(input, EV_KEY, ef67_mxt_keys[i]);
	input_set_abs_params(input, ABS_X, 0, EF67_MXT_SCREEN_X, 0, 0);
	input_set_abs_params(input, ABS_Y, 0, EF67_MXT_SCREEN_Y, 0, 0);
	input_set_abs_params(input, ABS_MT_POSITION_X, 0, EF67_MXT_SCREEN_X, 0, 0);
	input_set_abs_params(input, ABS_MT_POSITION_Y, 0, EF67_MXT_SCREEN_Y, 0, 0);
	input_set_abs_params(input, ABS_MT_TOOL_TYPE, 0, MT_TOOL_MAX, 0, 0);
	input_set_abs_params(input, ABS_MT_TOUCH_MAJOR, 0, 255, 0, 0);
	ret = input_mt_init_slots(input, EF67_MXT_CONTACTS, INPUT_MT_DIRECT);
	if (!ret)
		ret = input_register_device(input);
	if (ret) {
		input_free_device(input);
		touch->input = NULL;
	}
	return ret;
}

static int ef67_mxt_probe(struct i2c_client *client,
			  const struct i2c_device_id *id)
{
	struct ef67_mxt *touch;
	int ret;

	if (!i2c_check_functionality(client->adapter, I2C_FUNC_I2C))
		return -EOPNOTSUPP;
	if (client->irq <= 0)
		return -EINVAL;
	touch = devm_kzalloc(&client->dev, sizeof(*touch), GFP_KERNEL);
	if (!touch)
		return -ENOMEM;
	touch->client = client;
	mutex_init(&touch->lock);
	i2c_set_clientdata(client, touch);
	ret = ef67_mxt_setup_power(touch);
	if (ret)
		return ret;
	ret = ef67_mxt_read_objects(touch);
	if (ret)
		goto power_off;
	/* Consume the expected power-on reset before installing RAM settings. */
	ret = ef67_mxt_drain(touch, false);
	if (ret)
		goto power_off;
	ret = ef67_mxt_apply_config(touch, EF67_MXT_FINGER);
	if (ret)
		goto power_off;
	ret = ef67_mxt_drain(touch, false);
	if (ret)
		goto power_off;
	ret = ef67_mxt_register_input(touch);
	if (ret)
		goto power_off;
	ret = request_threaded_irq(client->irq, NULL, ef67_mxt_interrupt,
				   IRQF_TRIGGER_FALLING | IRQF_ONESHOT,
				   "ef67-mxt641t", touch);
	if (ret)
		goto free_input;
	touch->irq_enabled = true;
	touch->scanning = true;
#ifdef CONFIG_FB
	touch->fb_notifier.notifier_call = ef67_mxt_fb_event;
	ret = fb_register_client(&touch->fb_notifier);
	if (ret)
		goto free_irq;
#endif
	ret = sysfs_create_group(&client->dev.kobj, &ef67_mxt_attribute_group);
	if (ret)
		goto unregister_fb;
	dev_info(&client->dev, "EF67 mXT641T finger/glove/stylus input ready\n");
	return 0;

unregister_fb:
#ifdef CONFIG_FB
	fb_unregister_client(&touch->fb_notifier);
free_irq:
#endif
	ef67_mxt_disable_irq(touch);
	free_irq(client->irq, touch);
free_input:
	input_unregister_device(touch->input);
power_off:
	ef67_mxt_power_off(touch);
	return ret;
}

static int ef67_mxt_remove(struct i2c_client *client)
{
	struct ef67_mxt *touch = i2c_get_clientdata(client);

	sysfs_remove_group(&client->dev.kobj, &ef67_mxt_attribute_group);
#ifdef CONFIG_FB
	fb_unregister_client(&touch->fb_notifier);
#endif
	mutex_lock(&touch->lock);
	touch->stopping = true;
	ef67_mxt_disable_irq(touch);
	ef67_mxt_release_all(touch);
	ef67_mxt_power_off(touch);
	mutex_unlock(&touch->lock);
	free_irq(client->irq, touch);
	input_unregister_device(touch->input);
	return 0;
}

static void ef67_mxt_shutdown(struct i2c_client *client)
{
	struct ef67_mxt *touch = i2c_get_clientdata(client);

	mutex_lock(&touch->lock);
	touch->stopping = true;
	ef67_mxt_disable_irq(touch);
	ef67_mxt_release_all(touch);
	ef67_mxt_power_off(touch);
	mutex_unlock(&touch->lock);
}

#ifdef CONFIG_PM_SLEEP
static int ef67_mxt_suspend(struct device *dev)
{
	struct ef67_mxt *touch = i2c_get_clientdata(to_i2c_client(dev));
	int ret, restore_ret;

	mutex_lock(&touch->lock);
	touch->system_suspended = true;
	ret = ef67_mxt_update_scanning(touch);
	if (ret) {
		touch->system_suspended = false;
		/* The PM core will not resume a device whose suspend failed. */
		restore_ret = ef67_mxt_update_scanning(touch);
		if (restore_ret)
			dev_err(dev, "Touch suspend rollback failed: %d\n", restore_ret);
	}
	mutex_unlock(&touch->lock);
	return ret;
}

static int ef67_mxt_resume(struct device *dev)
{
	struct ef67_mxt *touch = i2c_get_clientdata(to_i2c_client(dev));
	int ret;

	mutex_lock(&touch->lock);
	touch->system_suspended = false;
	ret = ef67_mxt_update_scanning(touch);
	mutex_unlock(&touch->lock);
	return ret;
}
#endif

static SIMPLE_DEV_PM_OPS(ef67_mxt_pm, ef67_mxt_suspend, ef67_mxt_resume);

static const struct of_device_id ef67_mxt_match[] = {
	{ .compatible = "atmel,atmel_mxt_641t" },
	{ }
};
MODULE_DEVICE_TABLE(of, ef67_mxt_match);

static const struct i2c_device_id ef67_mxt_ids[] = {
	{ "atmel_mxt_641t", 0 },
	{ }
};
MODULE_DEVICE_TABLE(i2c, ef67_mxt_ids);

static struct i2c_driver ef67_mxt_driver = {
	.driver = {
		.name = "ef67-mxt641t",
		.of_match_table = ef67_mxt_match,
		.pm = &ef67_mxt_pm,
	},
	.probe = ef67_mxt_probe,
	.remove = ef67_mxt_remove,
	.shutdown = ef67_mxt_shutdown,
	.id_table = ef67_mxt_ids,
};
module_i2c_driver(ef67_mxt_driver);

MODULE_DESCRIPTION("Pantech EF67 Atmel mXT641T touch and capacitive keys");
MODULE_LICENSE("GPL v2");
