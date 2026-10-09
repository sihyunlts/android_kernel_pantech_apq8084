// SPDX-License-Identifier: GPL-2.0-only
/* Pantech EF67 FPC1021 image capture interface. */
#include <linux/delay.h>
#include <linux/fs.h>
#include <linux/gpio.h>
#include <linux/hwmon-sysfs.h>
#include <linux/interrupt.h>
#include <linux/kref.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of_gpio.h>
#include <linux/poll.h>
#include <linux/regulator/consumer.h>
#include <linux/slab.h>
#include <linux/spi/spi.h>
#include <linux/uaccess.h>
#include <linux/workqueue.h>
#include <mach/gpio.h>

#define FPC_FRAME_SIZE (160 * 160)
#define FPC_FRAME_COUNT 5
#define FPC_SPI_HZ 8000000
#define FPC_CAPTURE _IO(0xfc, 0)
#define FPC_ABORT _IO(0xfc, 1)
#define FPC_NAVIGATION _IOW(0xfc, 4, unsigned int)
#define FPC_STOP _IO(0xfc, 5)
#define FPC_FIRST_TRY _IOW(0xfc, 6, unsigned int)

struct ef67_fpc {
	struct spi_device *spi;
	struct miscdevice misc;
	struct regulator *supplies[3];
	unsigned int supply_votes;
	struct kref ref;
	/* control serializes cancellation, configuration, PM and removal. */
	struct mutex control;
	struct mutex data_lock;
	struct work_struct capture_work;
	wait_queue_head_t irq_wait;
	wait_queue_head_t read_wait;
	atomic_t cancel;
	atomic_t irq_pending;
	u8 *images;
	u8 *spi_buffer;
	u8 adc[19];
	u8 capture_adc[19];
	unsigned int capture_mode;
	unsigned int active_capture_mode;
	unsigned int threshold;
	unsigned int frames_captured;
	unsigned int frames_stored;
	unsigned int capture_time;
	unsigned int finger_status;
	unsigned int capture_generation;
	size_t available;
	size_t read_offset;
	int power_gpio;
	int reset_gpio;
	int irq_gpio;
	int irq;
	bool opened;
	bool running;
	bool complete;
	bool suspended;
	bool removed;
	bool first_try;
};

static void ef67_fpc_free(struct kref *ref)
{
	struct ef67_fpc *fpc = container_of(ref, struct ef67_fpc, ref);

	kfree(fpc->spi_buffer);
	kfree(fpc->images);
	kfree(fpc);
}

/* All SPI buffers are heap backed: the QUP controller can use DMA. */
static int ef67_fpc_transfer(struct ef67_fpc *fpc, size_t length)
{
	struct spi_transfer transfer = {
		.tx_buf = fpc->spi_buffer,
		.rx_buf = fpc->spi_buffer,
		.len = length,
		.speed_hz = FPC_SPI_HZ,
	};
	struct spi_message message;

	spi_message_init(&message);
	spi_message_add_tail(&transfer, &message);
	return spi_sync(fpc->spi, &message);
}

static int ef67_fpc_register(struct ef67_fpc *fpc, u8 reg,
			     u64 *value, unsigned int size, bool write)
{
	unsigned int i;
	int ret;

	if (!size || size > sizeof(*value))
		return -EINVAL;
	memset(fpc->spi_buffer, 0, size + 1);
	fpc->spi_buffer[0] = reg;
	if (write)
		for (i = 0; i < size; i++)
			fpc->spi_buffer[size - i] = *value >> (i * 8);
	ret = ef67_fpc_transfer(fpc, size + 1);
	if (!ret && !write) {
		*value = 0;
		for (i = 1; i <= size; i++)
			*value = (*value << 8) | fpc->spi_buffer[i];
	}
	return ret;
}

static int ef67_fpc_write_register(struct ef67_fpc *fpc, u8 reg,
				   u64 value, unsigned int size)
{
	return ef67_fpc_register(fpc, reg, &value, size, true);
}

static int ef67_fpc_read_irq(struct ef67_fpc *fpc)
{
	u64 value;
	int ret = ef67_fpc_register(fpc, 0x1c, &value, 1, false);

	return ret ? ret : value;
}

static irqreturn_t ef67_fpc_irq(int irq, void *data)
{
	struct ef67_fpc *fpc = data;

	if (!gpio_get_value(fpc->irq_gpio))
		return IRQ_NONE;
	atomic_set(&fpc->irq_pending, 1);
	wake_up(&fpc->irq_wait);
	return IRQ_HANDLED;
}

static int ef67_fpc_command(struct ef67_fpc *fpc, u8 command, u8 mask)
{
	long waited;
	int ret;

	if (atomic_read(&fpc->cancel))
		return -ECANCELED;
	/* Drain the preceding IRQ before clearing its software indication. */
	synchronize_irq(fpc->irq);
	atomic_set(&fpc->irq_pending, 0);
	fpc->spi_buffer[0] = command;
	ret = ef67_fpc_transfer(fpc, 1);
	if (ret || !mask)
		return ret;
	waited = wait_event_timeout(fpc->irq_wait,
		atomic_read(&fpc->cancel) || atomic_read(&fpc->irq_pending) ||
		gpio_get_value(fpc->irq_gpio), msecs_to_jiffies(1000));
	if (atomic_read(&fpc->cancel))
		return -ECANCELED;
	if (!waited)
		return -ETIMEDOUT;
	ret = ef67_fpc_read_irq(fpc);
	if (ret < 0)
		return ret;
	return (ret & mask) && !(ret & 4) ? 0 : -EIO;
}

static int ef67_fpc_reset(struct ef67_fpc *fpc)
{
	int ret;

	gpio_set_value(fpc->reset_gpio, 0);
	msleep(25);
	gpio_set_value(fpc->reset_gpio, 1);
	usleep_range(31250, 32000);
	if (!gpio_get_value(fpc->irq_gpio))
		return -EIO;
	disable_irq(fpc->irq);
	atomic_set(&fpc->irq_pending, 0);
	enable_irq(fpc->irq);
	ret = ef67_fpc_read_irq(fpc);
	if (ret < 0)
		return ret;
	if (ret != 0xff)
		return -EIO;
	ret = ef67_fpc_read_irq(fpc);
	return ret < 0 ? ret : (ret || gpio_get_value(fpc->irq_gpio) ? -EIO : 0);
}

static int ef67_fpc_verify_id(struct ef67_fpc *fpc)
{
	u64 value;
	int ret = ef67_fpc_register(fpc, 0xfc, &value, 2, false);

	if (ret)
		return ret;
	return value == 0x021a || value == 0x021b ? 0 : -ENODEV;
}

static int ef67_fpc_setup(struct ef67_fpc *fpc)
{
	int ret;

	ret = ef67_fpc_write_register(fpc, 0x90, 9, 1);
	if (!ret)
		ret = ef67_fpc_write_register(fpc, 0x68, 0x0808080814141414ULL, 8);
	if (!ret)
		ret = ef67_fpc_write_register(fpc, 0x8c, 2, 1);
	if (!ret)
		ret = ef67_fpc_write_register(fpc, 0xa0,
				(fpc->capture_adc[1] << 8) | fpc->capture_adc[0], 2);
	if (!ret)
		ret = ef67_fpc_write_register(fpc, 0xa8,
				(fpc->capture_adc[2] << 8) | fpc->capture_adc[3], 2);
	if (!ret)
		ret = ef67_fpc_write_register(fpc, 0xd8, 0x50, 1);
	if (!ret)
		ret = ef67_fpc_write_register(fpc, 0xdc, 0xff, 2);
	return ret;
}

static int ef67_fpc_finger_count(struct ef67_fpc *fpc)
{
	u64 value;
	int ret;

	ret = ef67_fpc_write_register(fpc, 0xa0,
				(fpc->capture_adc[1] << 8) | fpc->capture_adc[0], 2);
	if (!ret) {
		ret = ef67_fpc_read_irq(fpc);
		if (ret >= 0)
			ret = ef67_fpc_command(fpc, 0x20, 0x80);
	}
	if (!ret)
		ret = ef67_fpc_register(fpc, 0xd4, &value, 2, false);
	return ret ? ret : hweight16(value & 0xfff);
}

static int ef67_fpc_wait_finger(struct ef67_fpc *fpc)
{
	int ret;

	ret = ef67_fpc_read_irq(fpc);
	if (ret < 0)
		return ret;
	ret = ef67_fpc_command(fpc, 0x24, 0);
	if (ret)
		return ret;
	wait_event(fpc->irq_wait,
		atomic_read(&fpc->cancel) || atomic_read(&fpc->irq_pending) ||
		gpio_get_value(fpc->irq_gpio));
	if (atomic_read(&fpc->cancel))
		return -ECANCELED;
	ret = ef67_fpc_read_irq(fpc);
	if (ret < 0)
		return ret;
	if (!(ret & 0x81) || (ret & 4))
		return -EIO;
	for (;;) {
		if (atomic_read(&fpc->cancel))
			return -ECANCELED;
		ret = ef67_fpc_finger_count(fpc);
		if (ret < 0 || ret > 6)
			return ret < 0 ? ret : 0;
		msleep(20);
	}
}

static void ef67_fpc_capture(struct work_struct *work)
{
	struct ef67_fpc *fpc = container_of(work, struct ef67_fpc, capture_work);
	unsigned long started = jiffies;
	unsigned int frame;
	int ret = ef67_fpc_setup(fpc);

	if (!ret && fpc->active_capture_mode == 1) {
		/* Subsequent attempts wait for release before accepting a new touch. */
		if (!ACCESS_ONCE(fpc->first_try)) {
			for (;;) {
				if (atomic_read(&fpc->cancel)) {
					ret = -ECANCELED;
					break;
				}
				ret = ef67_fpc_finger_count(fpc);
				if (ret < 0 || ret <= fpc->threshold)
					break;
				msleep(20);
			}
			if (ret >= 0)
				ret = 0;
		}
		if (!ret)
			ret = ef67_fpc_wait_finger(fpc);
		if (!ret) {
			mutex_lock(&fpc->data_lock);
			fpc->finger_status = 1;
			mutex_unlock(&fpc->data_lock);
		}
	}
	if (!ret)
		msleep(30);
	started = jiffies;
	for (frame = 0; !ret && frame < FPC_FRAME_COUNT; frame++) {
		ret = ef67_fpc_write_register(fpc, 0xa0,
			(fpc->capture_adc[9 + frame] << 8) |
			fpc->capture_adc[4 + frame], 2);
		if (!ret)
			ret = ef67_fpc_write_register(fpc, 0xa8,
				(fpc->capture_adc[14 + frame] << 8) |
				fpc->capture_adc[3], 2);
		if (!ret)
			ret = ef67_fpc_command(fpc, 0xc0, 0x20);
		if (ret)
			break;
		memset(fpc->spi_buffer, 0, FPC_FRAME_SIZE + 2);
		fpc->spi_buffer[0] = 0xc4;
		ret = ef67_fpc_transfer(fpc, FPC_FRAME_SIZE + 2);
		if (!ret) {
			int irq;

			/* Register reads reuse spi_buffer, including its first pixels. */
			memcpy(fpc->images + frame * FPC_FRAME_SIZE,
			       fpc->spi_buffer + 2, FPC_FRAME_SIZE);
			irq = ef67_fpc_read_irq(fpc);
			if (irq < 0 || (irq & 4))
				ret = irq < 0 ? irq : -EIO;
		}
		if (ret)
			break;
		mutex_lock(&fpc->data_lock);
		fpc->frames_captured++;
		mutex_unlock(&fpc->data_lock);
		if (fpc->active_capture_mode == 1) {
			ret = ef67_fpc_finger_count(fpc);
			if (ret < 0)
				break;
			if (ret <= fpc->threshold) {
				mutex_lock(&fpc->data_lock);
				fpc->finger_status = 2;
				mutex_unlock(&fpc->data_lock);
				ret = 0;
				break;
			}
			ret = 0;
		}
		if (atomic_read(&fpc->cancel)) {
			ret = -ECANCELED;
			break;
		}
		mutex_lock(&fpc->data_lock);
		fpc->frames_stored++;
		fpc->available += FPC_FRAME_SIZE;
		mutex_unlock(&fpc->data_lock);
		wake_up_interruptible(&fpc->read_wait);
	}
	mutex_lock(&fpc->data_lock);
	fpc->capture_time = jiffies_to_msecs(jiffies - started);
	fpc->running = false;
	fpc->complete = true;
	mutex_unlock(&fpc->data_lock);
	if (ret && ret != -ECANCELED)
		dev_err(&fpc->spi->dev, "capture failed: %d\n", ret);
	wake_up_interruptible(&fpc->read_wait);
}

/* Called with control held, never while holding data_lock. */
static void ef67_fpc_cancel(struct ef67_fpc *fpc)
{
	atomic_set(&fpc->cancel, 1);
	wake_up(&fpc->irq_wait);
	cancel_work_sync(&fpc->capture_work);
	mutex_lock(&fpc->data_lock);
	fpc->capture_generation++;
	fpc->available = 0;
	fpc->read_offset = 0;
	fpc->running = false;
	fpc->complete = true;
	mutex_unlock(&fpc->data_lock);
	wake_up_interruptible(&fpc->read_wait);
}

static int ef67_fpc_power_off(struct ef67_fpc *fpc)
{
	static const unsigned int order[] = { 0, 2, 1 };
	unsigned int i;
	int error = 0, ret;

	gpio_set_value(fpc->reset_gpio, 0);
	for (i = 0; i < ARRAY_SIZE(order); i++) {
		unsigned int supply = order[i];

		if (i == 2)
			gpio_set_value(fpc->power_gpio, 0);
		if (!(fpc->supply_votes & BIT(supply)))
			continue;
		ret = regulator_disable(fpc->supplies[supply]);
		if (!ret)
			fpc->supply_votes &= ~BIT(supply);
		else {
			dev_err(&fpc->spi->dev, "supply %u disable failed: %d\n",
				supply, ret);
			if (!error)
				error = ret;
		}
	}
	return error;
}

/* The stock short test checks reset, ID, capture IRQ and IRQ clearing. */
static int ef67_fpc_selftest(struct ef67_fpc *fpc)
{
	int ret, reset_ret;

	if (fpc->suspended)
		return -EHOSTDOWN;
	if (ACCESS_ONCE(fpc->running))
		return -EBUSY;
	ret = ef67_fpc_reset(fpc);
	if (!ret)
		ret = ef67_fpc_verify_id(fpc);
	if (!ret) {
		atomic_set(&fpc->cancel, 0);
		ret = ef67_fpc_command(fpc, 0xc0, 0x20);
		if (!ret && gpio_get_value(fpc->irq_gpio))
			ret = -EIO;
	}
	reset_ret = ef67_fpc_reset(fpc);
	return !ret && !reset_ret;
}

static int ef67_fpc_power_on(struct ef67_fpc *fpc)
{
	unsigned int i;
	int ret;

	for (i = 0; i < ARRAY_SIZE(fpc->supplies); i++) {
		if (i == 2)
			gpio_set_value(fpc->power_gpio, 1);
		if (fpc->supply_votes & BIT(i))
			continue;
		ret = regulator_enable(fpc->supplies[i]);
		if (ret) {
			ef67_fpc_power_off(fpc);
			return ret;
		}
		fpc->supply_votes |= BIT(i);
	}
	msleep(25);
	return 0;
}

static int ef67_fpc_open(struct inode *inode, struct file *file)
{
	struct ef67_fpc *fpc = container_of(file->private_data, struct ef67_fpc, misc);
	int ret = 0;

	mutex_lock(&fpc->control);
	if (fpc->removed)
		ret = -ENODEV;
	else if (fpc->opened)
		ret = -EBUSY;
	else {
		fpc->opened = true;
		kref_get(&fpc->ref);
		file->private_data = fpc;
		nonseekable_open(inode, file);
	}
	mutex_unlock(&fpc->control);
	return ret;
}

static int ef67_fpc_release(struct inode *inode, struct file *file)
{
	struct ef67_fpc *fpc = file->private_data;

	mutex_lock(&fpc->control);
	ef67_fpc_cancel(fpc);
	fpc->opened = false;
	mutex_unlock(&fpc->control);
	kref_put(&fpc->ref, ef67_fpc_free);
	return 0;
}

static ssize_t ef67_fpc_read(struct file *file, char __user *buf,
			   size_t count, loff_t *offset)
{
	struct ef67_fpc *fpc = file->private_data;
	unsigned int generation;
	int ret;

	if (!count)
		return 0;
	ret = mutex_lock_interruptible(&fpc->data_lock);
	if (ret)
		return ret;
	generation = fpc->capture_generation;
	for (;;) {
		if (fpc->removed) {
			ret = -ENODEV;
			break;
		}
		if (fpc->capture_generation != generation) {
			/* End the old attempt without consuming the replacement capture. */
			ret = 0;
			break;
		}
		if (fpc->available) {
			count = min(count, fpc->available);
			if (copy_to_user(buf, fpc->images + fpc->read_offset, count)) {
				ret = -EFAULT;
				break;
			}
			fpc->read_offset += count;
			fpc->available -= count;
			ret = count;
			break;
		}
		if (fpc->complete) {
			/* The stock interface reports capture completion as EOF. */
			ret = 0;
			break;
		}
		mutex_unlock(&fpc->data_lock);
		if (file->f_flags & O_NONBLOCK)
			return -EAGAIN;
		ret = wait_event_interruptible(fpc->read_wait,
			ACCESS_ONCE(fpc->available) || ACCESS_ONCE(fpc->complete) ||
			ACCESS_ONCE(fpc->removed) ||
			ACCESS_ONCE(fpc->capture_generation) != generation);
		if (ret)
			return ret;
		ret = mutex_lock_interruptible(&fpc->data_lock);
		if (ret)
			return ret;
	}
	mutex_unlock(&fpc->data_lock);
	return ret;
}

static unsigned int ef67_fpc_poll(struct file *file, poll_table *wait)
{
	struct ef67_fpc *fpc = file->private_data;
	unsigned int mask = 0;

	poll_wait(file, &fpc->read_wait, wait);
	mutex_lock(&fpc->data_lock);
	if (fpc->removed)
		mask = POLLERR | POLLHUP;
	else if (fpc->available)
		mask |= POLLIN | POLLRDNORM;
	else if (fpc->complete)
		mask = POLLHUP;
	mutex_unlock(&fpc->data_lock);
	return mask;
}

static long ef67_fpc_ioctl(struct file *file, unsigned int command,
			   unsigned long arg)
{
	struct ef67_fpc *fpc = file->private_data;
	int ret = 0;

	mutex_lock(&fpc->control);
	if (fpc->removed) {
		ret = -ENODEV;
		goto out;
	}
	if (fpc->suspended) {
		ret = -EHOSTDOWN;
		goto out;
	}
	switch (command) {
	case FPC_CAPTURE:
		ef67_fpc_cancel(fpc);
		ret = ef67_fpc_reset(fpc);
		if (ret)
			break;
		/* Apply one coherent setup per attempt; userspace may prepare the next. */
		memcpy(fpc->capture_adc, fpc->adc, sizeof(fpc->capture_adc));
		fpc->active_capture_mode = fpc->capture_mode;
		mutex_lock(&fpc->data_lock);
		fpc->frames_captured = 0;
		fpc->frames_stored = 0;
		fpc->finger_status = 0;
		fpc->complete = false;
		fpc->running = true;
		mutex_unlock(&fpc->data_lock);
		atomic_set(&fpc->cancel, 0);
		schedule_work(&fpc->capture_work);
		break;
	case FPC_ABORT:
	case FPC_STOP:
	case 5000:
		ef67_fpc_cancel(fpc);
		ret = ef67_fpc_reset(fpc);
		break;
	case FPC_FIRST_TRY:
		/* Stock userspace updates this flag while a capture is active. */
		ACCESS_ONCE(fpc->first_try) = !!arg;
		break;
	case FPC_NAVIGATION:
	case 5001:
	case 5002:
	case 5003:
		ret = -EOPNOTSUPP;
		break;
	case 5005:
		ret = ef67_fpc_selftest(fpc);
		break;
	default:
		ret = -ENOTTY;
	}
out:
	mutex_unlock(&fpc->control);
	return ret;
}

static const struct file_operations ef67_fpc_fops = {
	.owner = THIS_MODULE,
	.open = ef67_fpc_open,
	.release = ef67_fpc_release,
	.read = ef67_fpc_read,
	.poll = ef67_fpc_poll,
	.unlocked_ioctl = ef67_fpc_ioctl,
	.compat_ioctl = ef67_fpc_ioctl,
	.llseek = no_llseek,
};

static ssize_t ef67_fpc_setup_show(struct device *dev,
				 struct device_attribute *attr, char *buf)
{
	struct ef67_fpc *fpc = dev_get_drvdata(dev);
	int index = to_sensor_dev_attr(attr)->index;
	unsigned int value;

	mutex_lock(&fpc->control);
	value = index == 20 ? fpc->capture_mode : fpc->adc[index];
	mutex_unlock(&fpc->control);
	return scnprintf(buf, PAGE_SIZE, "%u\n", value);
}

static ssize_t ef67_fpc_setup_store(struct device *dev,
			struct device_attribute *attr, const char *buf, size_t count)
{
	struct ef67_fpc *fpc = dev_get_drvdata(dev);
	int index = to_sensor_dev_attr(attr)->index;
	unsigned int value;
	int ret = kstrtouint(buf, 0, &value);

	if (ret)
		return ret;
	if ((index == 20 && value != 1 && value != 2) ||
	    (index != 20 && value > 255))
		return -EINVAL;
	mutex_lock(&fpc->control);
	if (index == 20)
		fpc->capture_mode = value;
	else
		fpc->adc[index] = value;
	mutex_unlock(&fpc->control);
	return ret ? ret : count;
}

#define FPC_ATTR(name, index) \
	static SENSOR_DEVICE_ATTR(name, 0660, ef67_fpc_setup_show, \
				  ef67_fpc_setup_store, index)
FPC_ATTR(adc_gain, 0);
FPC_ATTR(adc_shift, 1);
FPC_ATTR(adc_first_pxl, 2);
FPC_ATTR(adc_last_pxl, 3);
FPC_ATTR(adc_table_gain_normal, 4);
FPC_ATTR(adc_table_gain_wet1, 5);
FPC_ATTR(adc_table_gain_wet2, 6);
FPC_ATTR(adc_table_gain_dry1, 7);
FPC_ATTR(adc_table_gain_dry2, 8);
FPC_ATTR(adc_table_shift_normal, 9);
FPC_ATTR(adc_table_shift_wet1, 10);
FPC_ATTR(adc_table_shift_wet2, 11);
FPC_ATTR(adc_table_shift_dry1, 12);
FPC_ATTR(adc_table_shift_dry2, 13);
FPC_ATTR(adc_table_first_pxl_normal, 14);
FPC_ATTR(adc_table_first_pxl_wet1, 15);
FPC_ATTR(adc_table_first_pxl_wet2, 16);
FPC_ATTR(adc_table_first_pxl_dry1, 17);
FPC_ATTR(adc_table_first_pxl_dry2, 18);
FPC_ATTR(capture_mode, 20);
#define FPC_ATTRIBUTE(name) &sensor_dev_attr_##name.dev_attr.attr
static struct attribute *ef67_fpc_setup_attrs[] = {
	FPC_ATTRIBUTE(adc_gain), FPC_ATTRIBUTE(adc_shift),
	FPC_ATTRIBUTE(adc_first_pxl), FPC_ATTRIBUTE(adc_last_pxl),
	FPC_ATTRIBUTE(adc_table_gain_normal), FPC_ATTRIBUTE(adc_table_gain_wet1),
	FPC_ATTRIBUTE(adc_table_gain_wet2), FPC_ATTRIBUTE(adc_table_gain_dry1),
	FPC_ATTRIBUTE(adc_table_gain_dry2), FPC_ATTRIBUTE(adc_table_shift_normal),
	FPC_ATTRIBUTE(adc_table_shift_wet1), FPC_ATTRIBUTE(adc_table_shift_wet2),
	FPC_ATTRIBUTE(adc_table_shift_dry1), FPC_ATTRIBUTE(adc_table_shift_dry2),
	FPC_ATTRIBUTE(adc_table_first_pxl_normal),
	FPC_ATTRIBUTE(adc_table_first_pxl_wet1), FPC_ATTRIBUTE(adc_table_first_pxl_wet2),
	FPC_ATTRIBUTE(adc_table_first_pxl_dry1), FPC_ATTRIBUTE(adc_table_first_pxl_dry2),
	FPC_ATTRIBUTE(capture_mode), NULL,
};
static const struct attribute_group ef67_fpc_setup_group = {
	.name = "setup",
	.attrs = ef67_fpc_setup_attrs,
};

static ssize_t ef67_fpc_diag_show(struct device *dev,
				struct device_attribute *attr, char *buf)
{
	struct ef67_fpc *fpc = dev_get_drvdata(dev);
	int index = to_sensor_dev_attr(attr)->index;
	int value;

	mutex_lock(&fpc->control);
	if (index == 0) {
		value = ef67_fpc_selftest(fpc);
	} else {
		mutex_lock(&fpc->data_lock);
		switch (index) {
		case 4: value = fpc->capture_time; break;
		case 8: value = fpc->frames_captured; break;
		case 12: value = fpc->frames_stored; break;
		case 16: value = fpc->finger_status; break;
		default: value = fpc->threshold; break;
		}
		mutex_unlock(&fpc->data_lock);
	}
	mutex_unlock(&fpc->control);
	return value < 0 ? value : scnprintf(buf, PAGE_SIZE, "%d\n", value);
}

static ssize_t ef67_fpc_threshold_store(struct device *dev,
			struct device_attribute *attr, const char *buf, size_t count)
{
	struct ef67_fpc *fpc = dev_get_drvdata(dev);
	unsigned int value;
	int ret = kstrtouint(buf, 0, &value);

	if (ret)
		return ret;
	if (value > 12)
		return -EINVAL;
	mutex_lock(&fpc->control);
	if (ACCESS_ONCE(fpc->running))
		ret = -EBUSY;
	else
		fpc->threshold = value;
	mutex_unlock(&fpc->control);
	return ret ? ret : count;
}
static SENSOR_DEVICE_ATTR(selftest, 0444, ef67_fpc_diag_show, NULL, 0);
static SENSOR_DEVICE_ATTR(capture_time, 0444, ef67_fpc_diag_show, NULL, 4);
static SENSOR_DEVICE_ATTR(frames_captured, 0444, ef67_fpc_diag_show, NULL, 8);
static SENSOR_DEVICE_ATTR(frames_stored, 0444, ef67_fpc_diag_show, NULL, 12);
static SENSOR_DEVICE_ATTR(finger_status, 0444, ef67_fpc_diag_show, NULL, 16);
static SENSOR_DEVICE_ATTR(finger_up_threshold, 0660, ef67_fpc_diag_show,
			  ef67_fpc_threshold_store, 20);
static struct attribute *ef67_fpc_diag_attrs[] = {
	FPC_ATTRIBUTE(selftest), FPC_ATTRIBUTE(capture_time),
	FPC_ATTRIBUTE(frames_captured), FPC_ATTRIBUTE(frames_stored),
	FPC_ATTRIBUTE(finger_status), FPC_ATTRIBUTE(finger_up_threshold), NULL,
};
static const struct attribute_group ef67_fpc_diag_group = {
	.name = "diag",
	.attrs = ef67_fpc_diag_attrs,
};

static int ef67_fpc_probe(struct spi_device *spi)
{
	static const char * const supplies[] = { "vddio", "vdd", "vddaux" };
	static const int voltages[] = { 1800000, 3300000, 1800000 };
	static const u8 adc[] = {
		15, 5, 15, 14, 24, 24, 24, 20, 20,
		24, 26, 28, 15, 13, 15, 15, 15, 15, 15,
	};
	struct ef67_fpc *fpc;
	unsigned int i;
	int ret;

	fpc = kzalloc(sizeof(*fpc), GFP_KERNEL);
	if (!fpc)
		return -ENOMEM;
	kref_init(&fpc->ref);
	mutex_init(&fpc->control);
	mutex_init(&fpc->data_lock);
	init_waitqueue_head(&fpc->read_wait);
	init_waitqueue_head(&fpc->irq_wait);
	INIT_WORK(&fpc->capture_work, ef67_fpc_capture);
	fpc->spi = spi;
	fpc->capture_mode = 1;
	fpc->threshold = 0;
	memcpy(fpc->adc, adc, sizeof(adc));
	fpc->images = kzalloc(FPC_FRAME_SIZE * FPC_FRAME_COUNT, GFP_KERNEL);
	fpc->spi_buffer = kzalloc(FPC_FRAME_SIZE + 2, GFP_KERNEL);
	if (!fpc->images || !fpc->spi_buffer) {
		ret = -ENOMEM;
		goto free;
	}
	fpc->power_gpio = of_get_named_gpio(spi->dev.of_node, "fpc,power-gpio", 0);
	fpc->reset_gpio = of_get_named_gpio(spi->dev.of_node, "fpc,reset-gpio", 0);
	fpc->irq_gpio = of_get_named_gpio(spi->dev.of_node, "idex,irq-gpio", 0);
	if (fpc->power_gpio < 0 || fpc->reset_gpio < 0 || fpc->irq_gpio < 0) {
		ret = min(fpc->power_gpio, min(fpc->reset_gpio, fpc->irq_gpio));
		goto free;
	}
	ret = devm_gpio_request_one(&spi->dev, fpc->power_gpio,
				    GPIOF_OUT_INIT_LOW, "fpc-power");
	if (ret)
		goto free;
	ret = gpio_tlmm_config(GPIO_CFG(fpc->power_gpio, 0, GPIO_CFG_OUTPUT,
				GPIO_CFG_PULL_UP, GPIO_CFG_2MA), GPIO_CFG_ENABLE);
	if (ret)
		goto free;
	ret = devm_gpio_request_one(&spi->dev, fpc->reset_gpio,
				    GPIOF_OUT_INIT_LOW, "fpc-reset");
	if (ret)
		goto free;
	ret = devm_gpio_request_one(&spi->dev, fpc->irq_gpio, GPIOF_IN, "fpc-irq");
	if (ret)
		goto free;
	for (i = 0; i < ARRAY_SIZE(supplies); i++) {
		fpc->supplies[i] = devm_regulator_get(&spi->dev, supplies[i]);
		if (IS_ERR(fpc->supplies[i])) {
			ret = PTR_ERR(fpc->supplies[i]);
			goto free;
		}
		ret = regulator_set_voltage(fpc->supplies[i], voltages[i], voltages[i]);
		if (ret)
			goto free;
	}
	spi->mode = SPI_MODE_0;
	spi->bits_per_word = 8;
	spi->max_speed_hz = FPC_SPI_HZ;
	ret = spi_setup(spi);
	if (ret)
		goto free;
	fpc->irq = gpio_to_irq(fpc->irq_gpio);
	if (fpc->irq < 0) {
		ret = fpc->irq;
		goto free;
	}
	ret = request_irq(fpc->irq, ef67_fpc_irq, IRQF_TRIGGER_RISING,
			  "fpc_btp", fpc);
	if (ret)
		goto free;
	ret = ef67_fpc_power_on(fpc);
	if (!ret)
		ret = ef67_fpc_reset(fpc);
	if (!ret)
		ret = ef67_fpc_verify_id(fpc);
	if (ret)
		goto power_off;
	spi_set_drvdata(spi, fpc);
	ret = sysfs_create_group(&spi->dev.kobj, &ef67_fpc_setup_group);
	if (ret)
		goto clear_data;
	ret = sysfs_create_group(&spi->dev.kobj, &ef67_fpc_diag_group);
	if (ret)
		goto remove_setup;
	fpc->misc.minor = MISC_DYNAMIC_MINOR;
	fpc->misc.name = "fpc_btp";
	fpc->misc.fops = &ef67_fpc_fops;
	fpc->misc.parent = &spi->dev;
	ret = misc_register(&fpc->misc);
	if (ret) {
		sysfs_remove_group(&spi->dev.kobj, &ef67_fpc_diag_group);
		goto remove_setup;
	}
	dev_info(&spi->dev, "FPC1021 image capture ready\n");
	return 0;
remove_setup:
	sysfs_remove_group(&spi->dev.kobj, &ef67_fpc_setup_group);
clear_data:
	spi_set_drvdata(spi, NULL);
power_off:
	ef67_fpc_power_off(fpc);
	free_irq(fpc->irq, fpc);
free:
	kref_put(&fpc->ref, ef67_fpc_free);
	return ret;
}

static int ef67_fpc_suspend(struct device *dev)
{
	struct ef67_fpc *fpc = dev_get_drvdata(dev);
	int ret;

	mutex_lock(&fpc->control);
	if (fpc->suspended) {
		mutex_unlock(&fpc->control);
		return 0;
	}
	fpc->suspended = true;
	ef67_fpc_cancel(fpc);
	disable_irq(fpc->irq);
	ret = ef67_fpc_power_off(fpc);
	if (ret) {
		/* A failed PM callback must restore the device for an aborted suspend. */
		int restore = ef67_fpc_power_on(fpc);

		if (!restore) {
			enable_irq(fpc->irq);
			restore = ef67_fpc_reset(fpc);
			if (!restore)
				restore = ef67_fpc_verify_id(fpc);
			if (restore)
				disable_irq(fpc->irq);
		}
		if (!restore)
			fpc->suspended = false;
		else
			ef67_fpc_power_off(fpc);
	}
	mutex_unlock(&fpc->control);
	return ret;
}

static int ef67_fpc_resume(struct device *dev)
{
	struct ef67_fpc *fpc = dev_get_drvdata(dev);
	int ret;

	mutex_lock(&fpc->control);
	if (!fpc->suspended) {
		mutex_unlock(&fpc->control);
		return 0;
	}
	ret = ef67_fpc_power_on(fpc);
	if (!ret) {
		enable_irq(fpc->irq);
		ret = ef67_fpc_reset(fpc);
		if (!ret)
			ret = ef67_fpc_verify_id(fpc);
		if (ret)
			disable_irq(fpc->irq);
	}
	if (ret)
		ef67_fpc_power_off(fpc);
	else
		fpc->suspended = false;
	mutex_unlock(&fpc->control);
	return ret;
}

static int ef67_fpc_remove(struct spi_device *spi)
{
	struct ef67_fpc *fpc = spi_get_drvdata(spi);

	misc_deregister(&fpc->misc);
	sysfs_remove_group(&spi->dev.kobj, &ef67_fpc_diag_group);
	sysfs_remove_group(&spi->dev.kobj, &ef67_fpc_setup_group);
	mutex_lock(&fpc->control);
	mutex_lock(&fpc->data_lock);
	fpc->removed = true;
	mutex_unlock(&fpc->data_lock);
	ef67_fpc_cancel(fpc);
	ef67_fpc_power_off(fpc);
	free_irq(fpc->irq, fpc);
	mutex_unlock(&fpc->control);
	spi_set_drvdata(spi, NULL);
	kref_put(&fpc->ref, ef67_fpc_free);
	return 0;
}

static void ef67_fpc_shutdown(struct spi_device *spi)
{
	struct ef67_fpc *fpc = spi_get_drvdata(spi);

	mutex_lock(&fpc->control);
	fpc->suspended = true;
	ef67_fpc_cancel(fpc);
	ef67_fpc_power_off(fpc);
	mutex_unlock(&fpc->control);
}

static const struct dev_pm_ops ef67_fpc_pm = {
	.suspend = ef67_fpc_suspend,
	.resume = ef67_fpc_resume,
};
static const struct of_device_id ef67_fpc_match[] = {
	{ .compatible = "idex,ix_btp" },
	{ },
};
MODULE_DEVICE_TABLE(of, ef67_fpc_match);
static struct spi_driver ef67_fpc_driver = {
	.driver = {
		.name = "fpc_btp",
		.owner = THIS_MODULE,
		.of_match_table = ef67_fpc_match,
		.pm = &ef67_fpc_pm,
	},
	.probe = ef67_fpc_probe,
	.remove = ef67_fpc_remove,
	.shutdown = ef67_fpc_shutdown,
};
module_spi_driver(ef67_fpc_driver);
MODULE_DESCRIPTION("Pantech EF67 FPC1021 image capture");
MODULE_LICENSE("GPL");
