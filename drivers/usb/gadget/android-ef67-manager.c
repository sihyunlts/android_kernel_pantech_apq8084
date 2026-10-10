/*
 * EF67 stock usb_manager protocol, included by android.c.
 * Protocol follows Pantech's GPL USB manager and IM-A930S S0226114.
 * Normal CAF functions only; factory/CDROM compositions are not provided.
 * SPDX-License-Identifier: GPL-2.0
 */
#include <linux/miscdevice.h>
#include <linux/poll.h>
#include <linux/usb/ef67.h>

#define EF67_USB_COMMAND_SIZE 256

struct ef67_usb_composition {
	const char *name;
	const char *configuration;
	u32 type;
	u16 vendor, product;
	u8 device_class, subclass, protocol;
};

#include "android-ef67-compositions.h"

static const char * const ef67_usb_modes[] = {
	"pc_mode", "mtp_mode", "ums_mode", "charge_mode", "field_mode",
	"factory_mode", "ptp_mode",
};

static DEFINE_MUTEX(ef67_usb_request_lock);
static DEFINE_MUTEX(ef67_usb_control_lock);
static DEFINE_SPINLOCK(ef67_usb_event_lock);
static DECLARE_WAIT_QUEUE_HEAD(ef67_usb_events);
static DECLARE_WAIT_QUEUE_HEAD(ef67_usb_responses);
static struct android_dev *ef67_usb_dev;
static bool ef67_usb_available, ef67_usb_opened, ef67_usb_ready;
static bool ef67_usb_vbus_present, ef67_usb_reconnect;
static bool ef67_usb_mode_pending, ef67_usb_descriptor, ef67_usb_configured;
static bool ef67_usb_blocked, ef67_usb_block_pending;
static bool ef67_usb_factory_eligible, ef67_usb_transfer_seen;
static int ef67_usb_connection, ef67_usb_mode;
static int ef67_usb_uevent_state = -1;
static const struct ef67_usb_composition *ef67_usb_active_composition;
static unsigned int ef67_usb_request, ef67_usb_response;
static char ef67_usb_requested[EF67_USB_COMMAND_SIZE];
static char ef67_usb_current[EF67_USB_COMMAND_SIZE];
static char ef67_usb_read_message[EF67_USB_COMMAND_SIZE + 128];
static unsigned int ef67_usb_read_length;

static bool ef67_usb_managed(struct android_dev *dev)
{
	return dev == ACCESS_ONCE(ef67_usb_dev) &&
		ACCESS_ONCE(ef67_usb_available) && ACCESS_ONCE(ef67_usb_opened);
}

/* PRESENT is the cable signal, before a gadget can enumerate. */
void ef67_usb_vbus(bool present)
{
	unsigned long flags;

	spin_lock_irqsave(&ef67_usb_event_lock, flags);
	if (ef67_usb_vbus_present != present) {
		ef67_usb_vbus_present = present;
		ef67_usb_transfer_seen = false;
		ef67_usb_connection = present ? 1 : -1;
		if (!present) {
			ef67_usb_configured = false;
			ef67_usb_descriptor = false;
			ef67_usb_reconnect = false;
		}
	}
	spin_unlock_irqrestore(&ef67_usb_event_lock, flags);
	wake_up_interruptible(&ef67_usb_events);
}
EXPORT_SYMBOL_GPL(ef67_usb_vbus);

/* Stock android_disconnect also reports a host/controller disconnect. */
static void ef67_usb_disconnect(struct android_dev *dev)
{
	unsigned long flags;

	if (!ef67_usb_managed(dev) || !ACCESS_ONCE(ef67_usb_uevent_state))
		return;
	spin_lock_irqsave(&ef67_usb_event_lock, flags);
	ef67_usb_connection = -1;
	ef67_usb_transfer_seen = false;
	ef67_usb_configured = false;
	ef67_usb_descriptor = false;
	spin_unlock_irqrestore(&ef67_usb_event_lock, flags);
	wake_up_interruptible(&ef67_usb_events);
}

static void ef67_usb_bus_state(struct android_dev *dev, int state)
{
	unsigned long flags;

	if (!ef67_usb_managed(dev))
		return;
	spin_lock_irqsave(&ef67_usb_event_lock, flags);
	if (state == USB_DISCONNECTED) {
		ef67_usb_configured = false;
		ef67_usb_descriptor = false;
	} else if (ef67_usb_vbus_present && state == USB_CONFIGURED) {
		ef67_usb_configured = true;
	}
	spin_unlock_irqrestore(&ef67_usb_event_lock, flags);
	wake_up_interruptible(&ef67_usb_events);
}

static const struct ef67_usb_composition *ef67_usb_composition(const char *name)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(ef67_usb_compositions); i++)
		if (!strcmp(name, ef67_usb_compositions[i].name))
			return &ef67_usb_compositions[i];
	return NULL;
}

static const struct ef67_usb_composition *
ef67_usb_requested_composition(char *name)
{
	const struct ef67_usb_composition *function;
	char *token;
	u32 type = 0;
	unsigned int i;

	while ((token = strsep(&name, ","))) {
		if (!strcmp(token, "ffs"))
			token = "adb";
		function = ef67_usb_composition(token);
		if (!function)
			return NULL;
		type |= function->type;
	}
	for (i = 0; i < ARRAY_SIZE(ef67_usb_compositions); i++)
		if (ef67_usb_compositions[i].type == type)
			return &ef67_usb_compositions[i];
	return NULL;
}

static int ef67_usb_functions_supported(const char *name)
{
	struct android_usb_function **functions, *function;
	char buffer[EF67_USB_COMMAND_SIZE], *remaining, *token;
	bool found;

	strlcpy(buffer, name, sizeof(buffer));
	remaining = buffer;
	while ((token = strsep(&remaining, ","))) {
		if (!strcmp(token, "adb"))
			token = "ffs";
		found = false;
		functions = ef67_usb_dev->functions;
		while ((function = *functions++))
			if (!strcmp(token, function->name)) {
				found = true;
				break;
			}
		if (!found)
			return -EOPNOTSUPP;
	}
	return 0;
}

static ssize_t ef67_usb_request_functions(struct android_dev *dev,
					const char *buffer, size_t count)
{
	char name[EF67_USB_COMMAND_SIZE];
	char *trimmed;
	const struct ef67_usb_composition *composition;
	unsigned long flags;
	unsigned int request;
	long waited;
	int ret;

	if (!count || count >= sizeof(name) || memchr(buffer, 0, count))
		return -EINVAL;
	memcpy(name, buffer, count);
	name[count] = 0;
	trimmed = strim(name);
	memmove(name, trimmed, strlen(trimmed) + 1);
	if (!strcmp(name, "none")) {
		ret = mutex_lock_interruptible(&ef67_usb_request_lock);
		if (ret)
			return ret;
		mutex_lock(&ef67_usb_control_lock);
		ret = ef67_usb_available ?
			enable_store(dev->dev, NULL, "0", 1) : -ENODEV;
		mutex_unlock(&ef67_usb_control_lock);
		mutex_unlock(&ef67_usb_request_lock);
		return ret < 0 ? ret : count;
	}
	composition = ef67_usb_requested_composition(name);
	if (!composition)
		return -EOPNOTSUPP;
	strlcpy(name, composition->name, sizeof(name));
	ret = mutex_lock_interruptible(&ef67_usb_request_lock);
	if (ret)
		return ret;
	spin_lock_irqsave(&ef67_usb_event_lock, flags);
	if (!ef67_usb_available || !ef67_usb_opened || !ef67_usb_ready) {
		spin_unlock_irqrestore(&ef67_usb_event_lock, flags);
		ret = -ENODEV;
		goto out;
	}
	/* Stock leaves an already active function set connected. */
	if (!strcmp(name, ef67_usb_current) && ACCESS_ONCE(dev->enabled)) {
		spin_unlock_irqrestore(&ef67_usb_event_lock, flags);
		ret = 0;
		goto out;
	}
	request = ++ef67_usb_request;
	strlcpy(ef67_usb_requested, name, sizeof(ef67_usb_requested));
	ef67_usb_reconnect = !strcmp(name, ef67_usb_current);
	spin_unlock_irqrestore(&ef67_usb_event_lock, flags);
	wake_up_interruptible(&ef67_usb_events);
	waited = wait_event_interruptible_timeout(ef67_usb_responses,
		ACCESS_ONCE(ef67_usb_response) == request ||
		!ACCESS_ONCE(ef67_usb_opened) || !ACCESS_ONCE(ef67_usb_available),
		msecs_to_jiffies(30000));
	if (waited < 0)
		ret = waited;
	else if (!waited)
		ret = -ETIMEDOUT;
	else if (!ACCESS_ONCE(ef67_usb_opened) || !ACCESS_ONCE(ef67_usb_available))
		ret = -ENODEV;
	else
		ret = 0;
out:
	mutex_unlock(&ef67_usb_request_lock);
	return ret ? ret : count;
}

static bool ef67_usb_event_pending(void)
{
	return !ACCESS_ONCE(ef67_usb_available) ||
		ACCESS_ONCE(ef67_usb_read_length) ||
		ACCESS_ONCE(ef67_usb_connection) ||
		ACCESS_ONCE(ef67_usb_requested[0]) ||
		ACCESS_ONCE(ef67_usb_reconnect) || ACCESS_ONCE(ef67_usb_mode_pending) ||
		ACCESS_ONCE(ef67_usb_block_pending) ||
		ACCESS_ONCE(ef67_usb_descriptor) || ACCESS_ONCE(ef67_usb_configured);
}

static int ef67_usb_open(struct inode *inode, struct file *file)
{
	unsigned long flags;
	int ret = 0;

	spin_lock_irqsave(&ef67_usb_event_lock, flags);
	if (!ef67_usb_available)
		ret = -ENODEV;
	else if (ef67_usb_opened)
		ret = -EBUSY;
	else {
		ef67_usb_opened = true;
		ef67_usb_ready = false;
		ef67_usb_connection = ef67_usb_vbus_present ? 1 : -1;
	}
	spin_unlock_irqrestore(&ef67_usb_event_lock, flags);
	return ret;
}

static int ef67_usb_release(struct inode *inode, struct file *file)
{
	unsigned long flags;

	mutex_lock(&ef67_usb_control_lock);
	spin_lock_irqsave(&ef67_usb_event_lock, flags);
	ef67_usb_opened = false;
	ef67_usb_ready = false;
	ef67_usb_requested[0] = 0;
	ef67_usb_read_length = 0;
	ef67_usb_current[0] = 0;
	ef67_usb_transfer_seen = false;
	ACCESS_ONCE(ef67_usb_active_composition) = NULL;
	ef67_usb_reconnect = false;
	ef67_usb_configured = false;
	ef67_usb_descriptor = false;
	spin_unlock_irqrestore(&ef67_usb_event_lock, flags);
	mutex_unlock(&ef67_usb_control_lock);
	wake_up_interruptible(&ef67_usb_responses);
	return 0;
}

static unsigned int ef67_usb_poll(struct file *file, poll_table *wait)
{
	unsigned long flags;
	bool acknowledge = false;

	poll_wait(file, &ef67_usb_events, wait);
	if (!ACCESS_ONCE(ef67_usb_available))
		return POLLHUP;
	spin_lock_irqsave(&ef67_usb_event_lock, flags);
	if (ef67_usb_response != ef67_usb_request) {
		/* Stock poll acknowledges receipt before userspace changes properties.
		 * Waiting for composition completion would deadlock Android init.
		 */
		ef67_usb_response = ef67_usb_request;
		acknowledge = true;
	}
	spin_unlock_irqrestore(&ef67_usb_event_lock, flags);
	if (acknowledge)
		wake_up_interruptible(&ef67_usb_responses);
	return ef67_usb_event_pending() ? POLLIN | POLLRDNORM : 0;
}

static ssize_t ef67_usb_read(struct file *file, char __user *buffer,
			   size_t count, loff_t *position)
{
	const char *state, *block, *tail;
	unsigned long flags;
	ssize_t ret;

	mutex_lock(&ef67_usb_control_lock);
	spin_lock_irqsave(&ef67_usb_event_lock, flags);
	if (!ef67_usb_available) {
		ret = -ENODEV;
		goto unlock;
	}
	if (!ef67_usb_event_pending()) {
		ret = 0;
		goto unlock;
	}
	if (!ef67_usb_read_length) {
		/* Stock uses one field for either UI mode or cable state. */
		state = ef67_usb_mode_pending ? ef67_usb_modes[ef67_usb_mode] :
			ef67_usb_connection > 0 ? "usb_connected" :
			ef67_usb_connection < 0 ? "usb_disconnected" : "none";
		block = ef67_usb_block_pending ?
			(ef67_usb_blocked ? "usb_disable" : "usb_enable") : "none";
		tail = ef67_usb_configured ? "enumerated" :
			ef67_usb_descriptor ? "get_desc" : "none";
		ef67_usb_read_length = scnprintf(ef67_usb_read_message,
			sizeof(ef67_usb_read_message), "kor|%s|%s|%s|%s|%s",
			state, ef67_usb_requested[0] ? ef67_usb_requested : "none",
			ef67_usb_reconnect ? "usb_reconnect" : "none", block, tail);
		/* Move this snapshot into a retryable read; new events stay queued. */
		if (!ef67_usb_mode_pending)
			ef67_usb_connection = 0;
		ef67_usb_mode_pending = false;
		ef67_usb_requested[0] = 0;
		ef67_usb_reconnect = false;
		ef67_usb_block_pending = false;
		if (!ef67_usb_configured)
			ef67_usb_descriptor = false;
		ef67_usb_configured = false;
	}
	spin_unlock_irqrestore(&ef67_usb_event_lock, flags);
	if (count < ef67_usb_read_length)
		ret = -EMSGSIZE;
	else if (copy_to_user(buffer, ef67_usb_read_message, ef67_usb_read_length))
		ret = -EFAULT;
	else {
		ret = ef67_usb_read_length;
		ACCESS_ONCE(ef67_usb_read_length) = 0;
	}
	goto out;
unlock:
	spin_unlock_irqrestore(&ef67_usb_event_lock, flags);
out:
	mutex_unlock(&ef67_usb_control_lock);
	return ret;
}

static int ef67_usb_set_composition(const char *name)
{
	const struct ef67_usb_composition *composition;
	struct android_configuration *conf;
	char stock_functions[EF67_USB_COMMAND_SIZE];
	char caf_functions[EF67_USB_COMMAND_SIZE];
	char *remaining, *token;
	unsigned int length = 0;
	unsigned long flags;
	ssize_t ret;
	bool was_enabled;

	composition = ef67_usb_composition(name);
	if (!composition)
		return -EOPNOTSUPP;
	if (!strcmp(name, ef67_usb_current))
		return 0;
	ret = ef67_usb_functions_supported(name);
	if (ret)
		return ret;
	was_enabled = ef67_usb_dev->enabled;
	ret = enable_store(ef67_usb_dev->dev, NULL, "0", 1);
	if (ret < 0)
		return ret;
	spin_lock_irqsave(&ef67_usb_event_lock, flags);
	ef67_usb_current[0] = 0;
	ef67_usb_transfer_seen = false;
	ACCESS_ONCE(ef67_usb_active_composition) = NULL;
	spin_unlock_irqrestore(&ef67_usb_event_lock, flags);
	/* CAF implements stock ADB through FunctionFS, independent of aliases. */
	strlcpy(stock_functions, name, sizeof(stock_functions));
	remaining = stock_functions;
	while ((token = strsep(&remaining, ",")))
		length += scnprintf(caf_functions + length,
			sizeof(caf_functions) - length, "%s%s",
			length ? "," : "", !strcmp(token, "adb") ? "ffs" : token);
	ret = functions_store_direct(ef67_usb_dev->dev, NULL,
				     caf_functions, strlen(caf_functions));
	if (ret < 0)
		return ret;
	mutex_lock(&ef67_usb_dev->mutex);
	ACCESS_ONCE(ef67_usb_active_composition) = composition;
	device_desc.idVendor = cpu_to_le16(composition->vendor);
	device_desc.idProduct = cpu_to_le16(composition->product);
	device_desc.bcdDevice = cpu_to_le16(0x290 +
		(composition->type & 0x8 ? 1 : composition->type & 0x800 ? 2 : 0));
	strings_dev[STRING_CONFIG_IDX].s = composition->configuration;
	list_for_each_entry(conf, &ef67_usb_dev->configs, list_item)
		conf->usb_config.iConfiguration = strings_dev[STRING_CONFIG_IDX].id;
	device_desc.bDeviceClass = composition->device_class;
	device_desc.bDeviceSubClass = composition->subclass;
	device_desc.bDeviceProtocol = composition->protocol;
	mutex_unlock(&ef67_usb_dev->mutex);
	spin_lock_irqsave(&ef67_usb_event_lock, flags);
	strlcpy(ef67_usb_current, name, sizeof(ef67_usb_current));
	spin_unlock_irqrestore(&ef67_usb_event_lock, flags);
	/* Preserve prior authorization; initial connection needs gadget_on. */
	if (was_enabled) {
		ret = enable_store(ef67_usb_dev->dev, NULL, "1", 1);
		if (ret < 0 || !ef67_usb_dev->enabled)
			return ret < 0 ? ret : -EIO;
	}
	return 0;
}

/* EF67S stock table at 0xc12ab50c: 40 bytes alone, 64 with ADB. */
static const struct {
	u32 type;
	struct mtp_ext_config_desc_header header;
	struct mtp_ext_config_desc_function functions[2];
} ef67_usb_ms_descriptors[] = {
	{ 0x8, { __constant_cpu_to_le32(40), 0x100,
		__constant_cpu_to_le16(4), 1, { 0 } },
		{ { 0, 1, { 'M', 'T', 'P' } } } },
	{ 0x800, { __constant_cpu_to_le32(40), 0x100,
		__constant_cpu_to_le16(4), 1, { 0 } },
		{ { 0, 1, { 'P', 'T', 'P' } } } },
	{ 0xa, { __constant_cpu_to_le32(64), 0x100,
		__constant_cpu_to_le16(4), 2, { 0 } },
		{ { 0, 1, { 'M', 'T', 'P' } }, { 1, 1 } } },
	{ 0x802, { __constant_cpu_to_le32(64), 0x100,
		__constant_cpu_to_le16(4), 2, { 0 } },
		{ { 0, 1, { 'P', 'T', 'P' } }, { 1, 1 } } },
};

static int ef67_usb_ctrlrequest(struct android_dev *dev,
		struct usb_composite_dev *cdev, const struct usb_ctrlrequest *ctrl)
{
	const struct ef67_usb_composition *composition;
	unsigned long flags;
	u16 index = le16_to_cpu(ctrl->wIndex);
	u16 length = le16_to_cpu(ctrl->wLength);
	unsigned int i;
	int value, ret;

	if (!ef67_usb_managed(dev))
		return -EOPNOTSUPP;
	if ((ctrl->bRequestType & USB_TYPE_MASK) == USB_TYPE_STANDARD &&
	    ctrl->bRequest == USB_REQ_GET_DESCRIPTOR) {
		spin_lock_irqsave(&ef67_usb_event_lock, flags);
		if (ef67_usb_vbus_present && !ef67_usb_transfer_seen) {
			ef67_usb_transfer_seen = true;
			ef67_usb_descriptor = true;
		}
		spin_unlock_irqrestore(&ef67_usb_event_lock, flags);
		wake_up_interruptible(&ef67_usb_events);
	}
	if ((ctrl->bRequestType & USB_TYPE_MASK) != USB_TYPE_VENDOR ||
	    !(ctrl->bRequestType & USB_DIR_IN) || ctrl->bRequest != 1 ||
	    (index != 4 && index != 5))
		return -EOPNOTSUPP;
	composition = ACCESS_ONCE(ef67_usb_active_composition);
	if (!composition)
		return -EOPNOTSUPP;
	for (i = 0; i < ARRAY_SIZE(ef67_usb_ms_descriptors); i++) {
		if (ef67_usb_ms_descriptors[i].type != composition->type)
			continue;
		value = min_t(unsigned int, length,
			le32_to_cpu(ef67_usb_ms_descriptors[i].header.dwLength));
		memcpy(cdev->req->buf, &ef67_usb_ms_descriptors[i].header, value);
		cdev->req->zero = value < length;
		cdev->req->length = value;
		ret = usb_ep_queue(cdev->gadget->ep0, cdev->req, GFP_ATOMIC);
		if (ret < 0)
			pr_err("android_usb: EF67 OS response queue failed (%d)\n", ret);
		return value;
	}
	return -EOPNOTSUPP;
}

static ssize_t ef67_usb_write(struct file *file, const char __user *buffer,
			    size_t count, loff_t *position)
{
	char command[EF67_USB_COMMAND_SIZE];
	char *events[] = { NULL, NULL };
	unsigned int i;
	unsigned long flags;
	int event_state = -1;
	int ret = 0;

	if (!count || count >= sizeof(command))
		return -EINVAL;
	if (copy_from_user(command, buffer, count))
		return -EFAULT;
	if (memchr(command, 0, count))
		return -EINVAL;
	command[count] = 0;
	mutex_lock(&ef67_usb_control_lock);
	if (!ACCESS_ONCE(ef67_usb_available)) {
		ret = -ENODEV;
		goto out;
	}
	if (!strcmp(command, "manager_on")) {
		ACCESS_ONCE(ef67_usb_ready) = true;
	} else if (!strncmp(command, "carrier_mode", 12)) {
		if (count != 16 || (command[12] != '|' && command[12] != ':') ||
		    strcmp(command + 13, "kor"))
			ret = -EOPNOTSUPP;
	} else if (!strcmp(command, "gadget_on")) {
		if (!ef67_usb_current[0])
			ret = -EINVAL;
		else {
			ret = enable_store(ef67_usb_dev->dev, NULL, "1", 1);
			if (ret >= 0 && !ef67_usb_dev->enabled)
				ret = -EIO;
		}
	} else if (!strcmp(command, "usb_cable_detach")) {
		spin_lock_irqsave(&ef67_usb_event_lock, flags);
		ef67_usb_current[0] = 0;
		ef67_usb_transfer_seen = false;
		ACCESS_ONCE(ef67_usb_active_composition) = NULL;
		ef67_usb_configured = false;
		ef67_usb_descriptor = false;
		ef67_usb_reconnect = false;
		spin_unlock_irqrestore(&ef67_usb_event_lock, flags);
		ret = enable_store(ef67_usb_dev->dev, NULL, "0", 1);
	} else if (!strcmp(command, "gadget_off")) {
		ret = enable_store(ef67_usb_dev->dev, NULL, "0", 1);
	} else if (!strcmp(command, "connected")) {
		events[0] = "USB_STATE=CONNECTED";
		event_state = 1;
	} else if (!strcmp(command, "disconnected")) {
		events[0] = "USB_STATE=DISCONNECTED";
		event_state = 0;
	} else if (!strcmp(command, "configured")) {
		events[0] = "USB_STATE=CONFIGURED";
		event_state = 2;
	} else {
		for (i = 0; i < ARRAY_SIZE(ef67_usb_modes); i++)
			if (!strcmp(command, ef67_usb_modes[i]))
				break;
		if (i < ARRAY_SIZE(ef67_usb_modes)) {
			spin_lock_irqsave(&ef67_usb_event_lock, flags);
			ef67_usb_mode = i;
			spin_unlock_irqrestore(&ef67_usb_event_lock, flags);
		} else {
			ret = ef67_usb_set_composition(command);
		}
	}
	if (events[0] && event_state != ef67_usb_uevent_state &&
	    !(ef67_usb_uevent_state == 2 && event_state == 1)) {
		kobject_uevent_env(&ef67_usb_dev->dev->kobj, KOBJ_CHANGE, events);
		ef67_usb_uevent_state = event_state;
	}
out:
	mutex_unlock(&ef67_usb_control_lock);
	return ret < 0 ? ret : count;
}

static const struct file_operations ef67_usb_operations = {
	.owner = THIS_MODULE,
	.open = ef67_usb_open,
	.release = ef67_usb_release,
	.read = ef67_usb_read,
	.write = ef67_usb_write,
	.poll = ef67_usb_poll,
	.llseek = no_llseek,
};

static struct miscdevice ef67_usb_device = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = "usb_device_mode",
	.fops = &ef67_usb_operations,
	.mode = 0600,
};

static ssize_t usb_manager_show(struct device *dev,
			       struct device_attribute *attribute, char *buffer)
{
	return scnprintf(buffer, PAGE_SIZE, "%s\n",
			ef67_usb_modes[ACCESS_ONCE(ef67_usb_mode)]);
}

static ssize_t usb_manager_store(struct device *dev,
		struct device_attribute *attribute, const char *buffer, size_t count)
{
	unsigned int i;
	unsigned long flags;

	for (i = 0; i < ARRAY_SIZE(ef67_usb_modes); i++)
		if (sysfs_streq(buffer, ef67_usb_modes[i]))
			break;
	if (i == ARRAY_SIZE(ef67_usb_modes))
		return -EINVAL;
	spin_lock_irqsave(&ef67_usb_event_lock, flags);
	ef67_usb_mode = i;
	ef67_usb_mode_pending = true;
	spin_unlock_irqrestore(&ef67_usb_event_lock, flags);
	wake_up_interruptible(&ef67_usb_events);
	return count;
}

static ssize_t usb_mdm_mode_show(struct device *dev,
			       struct device_attribute *attribute, char *buffer)
{
	return scnprintf(buffer, PAGE_SIZE, "%u", ACCESS_ONCE(ef67_usb_blocked));
}

static ssize_t usb_mdm_mode_store(struct device *dev,
		struct device_attribute *attribute, const char *buffer, size_t count)
{
	unsigned long flags;
	int value;
	int ret = kstrtoint(buffer, 10, &value);

	if (ret)
		return -EINVAL;
	mutex_lock(&ef67_usb_control_lock);
	spin_lock_irqsave(&ef67_usb_event_lock, flags);
	if (ef67_usb_blocked != !!value) {
		ef67_usb_blocked = !!value;
		ef67_usb_block_pending = ef67_usb_ready;
	}
	spin_unlock_irqrestore(&ef67_usb_event_lock, flags);
	mutex_unlock(&ef67_usb_control_lock);
	wake_up_interruptible(&ef67_usb_events);
	return count;
}

static ssize_t factory_mode_show(struct device *dev,
				struct device_attribute *attribute, char *buffer)
{
	return scnprintf(buffer, PAGE_SIZE, "%u",
			 ACCESS_ONCE(ef67_usb_factory_eligible));
}

static ssize_t factory_mode_store(struct device *dev,
		struct device_attribute *attribute, const char *buffer, size_t count)
{
	int value;
	int ret = kstrtoint(buffer, 10, &value);

	if (ret)
		return -EINVAL;
	/* Eligibility is not a factory-cable request and does not enable ADB. */
	ACCESS_ONCE(ef67_usb_factory_eligible) = !!value;
	return count;
}

static DEVICE_ATTR(factory_mode, 0644, factory_mode_show, factory_mode_store);
static DEVICE_ATTR(usb_manager, 0644, usb_manager_show, usb_manager_store);
static DEVICE_ATTR(usb_mdm_mode, 0644, usb_mdm_mode_show, usb_mdm_mode_store);

static struct attribute *ef67_usb_attributes[] = {
	&dev_attr_usb_manager.attr, &dev_attr_usb_mdm_mode.attr,
	&dev_attr_factory_mode.attr, NULL,
};

static const struct attribute_group ef67_usb_attribute_group = {
	.attrs = ef67_usb_attributes,
};

static int ef67_usb_manager_init(struct android_dev *dev)
{
	unsigned long flags;
	int ret;

	if (dev->pdata && dev->pdata->usb_core_id)
		return 0;
	/* An old open file must not become a handle to a replacement device. */
	if (ACCESS_ONCE(ef67_usb_opened))
		return -EBUSY;
	spin_lock_irqsave(&ef67_usb_event_lock, flags);
	ef67_usb_ready = false;
	ef67_usb_reconnect = false;
	ef67_usb_mode_pending = false;
	ef67_usb_descriptor = false;
	ef67_usb_configured = false;
	ef67_usb_blocked = false;
	ef67_usb_block_pending = false;
	ef67_usb_factory_eligible = false;
	ef67_usb_transfer_seen = false;
	ef67_usb_mode = 0;
	ef67_usb_uevent_state = -1;
	ef67_usb_request = ef67_usb_response = 0;
	ef67_usb_requested[0] = ef67_usb_current[0] = 0;
	ef67_usb_active_composition = NULL;
	ef67_usb_read_length = 0;
	spin_unlock_irqrestore(&ef67_usb_event_lock, flags);
	ef67_usb_dev = dev;
	ret = sysfs_create_group(&dev->dev->kobj, &ef67_usb_attribute_group);
	if (ret) {
		ef67_usb_dev = NULL;
		return ret;
	}
	ef67_usb_available = true;
	ret = misc_register(&ef67_usb_device);
	if (ret) {
		ef67_usb_available = false;
		sysfs_remove_group(&dev->dev->kobj, &ef67_usb_attribute_group);
		ef67_usb_dev = NULL;
	}
	return ret;
}

static void ef67_usb_manager_cleanup(struct android_dev *dev)
{
	if (dev != ef67_usb_dev)
		return;
	ACCESS_ONCE(ef67_usb_available) = false;
	wake_up_interruptible(&ef67_usb_events);
	wake_up_interruptible(&ef67_usb_responses);
	misc_deregister(&ef67_usb_device);
	sysfs_remove_group(&dev->dev->kobj, &ef67_usb_attribute_group);
	mutex_lock(&ef67_usb_request_lock);
	mutex_lock(&ef67_usb_control_lock);
	ef67_usb_dev = NULL;
	mutex_unlock(&ef67_usb_control_lock);
	mutex_unlock(&ef67_usb_request_lock);
}
