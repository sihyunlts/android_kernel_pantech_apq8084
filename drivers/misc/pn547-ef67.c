// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Pantech EF67 PN547 userspace interface.
 * Based on the Pantech PN547 driver, Copyright (C) 2010 Trusted Logic S.A.
 */

#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/fs.h>
#include <linux/gpio.h>
#include <linux/i2c.h>
#include <linux/interrupt.h>
#include <linux/jiffies.h>
#include <linux/kref.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of_gpio.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/wakelock.h>

#define PN547_SET_PWR	_IOW(0xe9, 1, unsigned int)
#define PN547_BUFFER_SIZE	512

struct ef67_pn547 {
	struct i2c_client *client;
	struct miscdevice misc;
	struct clk *clk;
	struct kref ref;
	struct mutex io_lock;
	struct mutex read_lock;
	spinlock_t irq_lock;
	wait_queue_head_t read_wait;
	struct wake_lock wake_lock;
	atomic_t irq_pending;
	atomic_t cancel_pending;
	int irq_gpio;
	int ven_gpio;
	int firm_gpio;
	int irq;
	unsigned int power_generation;
	bool irq_enabled;
	bool irq_wake_enabled;
	bool powered;
	bool removed;
};

static void ef67_pn547_free(struct kref *ref)
{
	struct ef67_pn547 *nfc = container_of(ref, struct ef67_pn547, ref);

	wake_lock_destroy(&nfc->wake_lock);
	mutex_destroy(&nfc->read_lock);
	mutex_destroy(&nfc->io_lock);
	kfree(nfc);
}

static void ef67_pn547_disable_irq(struct ef67_pn547 *nfc)
{
	unsigned long flags;

	spin_lock_irqsave(&nfc->irq_lock, flags);
	if (nfc->irq_enabled) {
		disable_irq_nosync(nfc->irq);
		nfc->irq_enabled = false;
	}
	spin_unlock_irqrestore(&nfc->irq_lock, flags);
}

/* io_lock excludes teardown while the IRQ is being armed. */
static void ef67_pn547_enable_irq(struct ef67_pn547 *nfc)
{
	unsigned long flags;

	atomic_set(&nfc->irq_pending, 0);
	spin_lock_irqsave(&nfc->irq_lock, flags);
	nfc->irq_enabled = true;
	spin_unlock_irqrestore(&nfc->irq_lock, flags);
	enable_irq(nfc->irq);
}

static irqreturn_t ef67_pn547_irq(int irq, void *data)
{
	struct ef67_pn547 *nfc = data;

	ef67_pn547_disable_irq(nfc);
	atomic_set(&nfc->irq_pending, 1);
	wake_up_interruptible(&nfc->read_wait);
	return IRQ_HANDLED;
}

static int ef67_pn547_open(struct inode *inode, struct file *file)
{
	struct ef67_pn547 *nfc = container_of(file->private_data,
						struct ef67_pn547, misc);
	int ret = 0;

	mutex_lock(&nfc->io_lock);
	if (nfc->removed) {
		ret = -ENODEV;
	} else {
		kref_get(&nfc->ref);
		file->private_data = nfc;
		nonseekable_open(inode, file);
	}
	mutex_unlock(&nfc->io_lock);
	return ret;
}

static int ef67_pn547_release(struct inode *inode, struct file *file)
{
	struct ef67_pn547 *nfc = file->private_data;

	kref_put(&nfc->ref, ef67_pn547_free);
	return 0;
}

static ssize_t ef67_pn547_read(struct file *file, char __user *buf,
			     size_t count, loff_t *offset)
{
	struct ef67_pn547 *nfc = file->private_data;
	char data[PN547_BUFFER_SIZE];
	unsigned int generation;
	int ret;

	count = min_t(size_t, count, sizeof(data));
	if (!count)
		return 0;
	ret = mutex_lock_interruptible(&nfc->read_lock);
	if (ret)
		return ret;
	mutex_lock(&nfc->io_lock);
	if (nfc->removed) {
		ret = -ENODEV;
		goto unlock;
	}
	if (atomic_xchg(&nfc->cancel_pending, 0)) {
		/* Pantech's arg=3 ABI returns -1 for the cancelled read. */
		ret = -EPERM;
		goto unlock;
	}
	if (!nfc->powered) {
		ret = -EHOSTDOWN;
		goto unlock;
	}
	if (!gpio_get_value(nfc->irq_gpio)) {
		if (file->f_flags & O_NONBLOCK) {
			ret = -EAGAIN;
			goto unlock;
		}
		generation = nfc->power_generation;
		ef67_pn547_enable_irq(nfc);
		mutex_unlock(&nfc->io_lock);
		ret = wait_event_interruptible(nfc->read_wait,
			atomic_read(&nfc->irq_pending) ||
			atomic_read(&nfc->cancel_pending) ||
			ACCESS_ONCE(nfc->removed) ||
			ACCESS_ONCE(nfc->power_generation) != generation);
		mutex_lock(&nfc->io_lock);
		if (nfc->removed) {
			ret = -ENODEV;
			goto unlock;
		}
		ef67_pn547_disable_irq(nfc);
		if (atomic_xchg(&nfc->cancel_pending, 0))
			ret = -EPERM;
		else if (nfc->power_generation != generation)
			ret = -EHOSTDOWN;
		if (ret)
			goto unlock;
	}
	ret = i2c_master_recv(nfc->client, data, count);
	wake_lock_timeout(&nfc->wake_lock, msecs_to_jiffies(100));
	if (ret >= 0 && ret > count)
		ret = -EIO;
unlock:
	mutex_unlock(&nfc->io_lock);
	mutex_unlock(&nfc->read_lock);
	if (ret > 0 && copy_to_user(buf, data, ret))
		return -EFAULT;
	return ret;
}

static ssize_t ef67_pn547_write(struct file *file, const char __user *buf,
			      size_t count, loff_t *offset)
{
	struct ef67_pn547 *nfc = file->private_data;
	char data[PN547_BUFFER_SIZE];
	int ret;

	count = min_t(size_t, count, sizeof(data));
	if (!count)
		return 0;
	if (copy_from_user(data, buf, count))
		return -EFAULT;
	mutex_lock(&nfc->io_lock);
	if (nfc->removed)
		ret = -ENODEV;
	else if (!nfc->powered)
		ret = -EHOSTDOWN;
	else {
		ret = i2c_master_send(nfc->client, data, count);
		if (ret != count)
			ret = -EIO;
	}
	mutex_unlock(&nfc->io_lock);
	return ret;
}

/* Called with io_lock held; delays and pin order match EF67 stock. */
static void ef67_pn547_power(struct ef67_pn547 *nfc, unsigned long arg)
{
	nfc->power_generation++;
	switch (arg) {
	case 0:
		gpio_set_value(nfc->firm_gpio, 0);
		gpio_set_value(nfc->ven_gpio, 0);
		msleep(60);
		break;
	case 1:
		gpio_set_value(nfc->firm_gpio, 0);
		gpio_set_value(nfc->ven_gpio, 1);
		msleep(20);
		break;
	case 2:
		gpio_set_value(nfc->ven_gpio, 1);
		gpio_set_value(nfc->firm_gpio, 1);
		msleep(20);
		gpio_set_value(nfc->ven_gpio, 0);
		msleep(60);
		gpio_set_value(nfc->ven_gpio, 1);
		msleep(20);
		break;
	case 255:
		gpio_set_value(nfc->ven_gpio, 0);
		gpio_set_value(nfc->firm_gpio, 0);
		msleep(60);
		gpio_set_value(nfc->ven_gpio, 1);
		msleep(20);
		gpio_set_value(nfc->ven_gpio, 0);
		msleep(20);
		break;
	}
	nfc->powered = arg == 1 || arg == 2;
	wake_up_interruptible(&nfc->read_wait);
}

static long ef67_pn547_ioctl(struct file *file, unsigned int cmd,
			    unsigned long arg)
{
	struct ef67_pn547 *nfc = file->private_data;
	int ret = 0;

	if (cmd != PN547_SET_PWR || (arg > 3 && arg != 255))
		return -EINVAL;
	mutex_lock(&nfc->io_lock);
	if (nfc->removed) {
		ret = -ENODEV;
	} else if (arg == 3) {
		atomic_set(&nfc->cancel_pending, 1);
		wake_up_interruptible(&nfc->read_wait);
	} else {
		ef67_pn547_power(nfc, arg);
	}
	mutex_unlock(&nfc->io_lock);
	return ret;
}

static const struct file_operations ef67_pn547_fops = {
	.owner = THIS_MODULE,
	.open = ef67_pn547_open,
	.release = ef67_pn547_release,
	.read = ef67_pn547_read,
	.write = ef67_pn547_write,
	.unlocked_ioctl = ef67_pn547_ioctl,
	.llseek = no_llseek,
};

static int ef67_pn547_probe(struct i2c_client *client,
			    const struct i2c_device_id *id)
{
	struct device *dev = &client->dev;
	struct ef67_pn547 *nfc;
	int ret;

	if (!dev->of_node)
		return -ENODEV;
	if (!i2c_check_functionality(client->adapter, I2C_FUNC_I2C))
		return -EOPNOTSUPP;
	nfc = kzalloc(sizeof(*nfc), GFP_KERNEL);
	if (!nfc)
		return -ENOMEM;
	nfc->client = client;
	nfc->irq_gpio = of_get_named_gpio(dev->of_node, "nxp,irq-gpio", 0);
	nfc->ven_gpio = of_get_named_gpio(dev->of_node, "nxp,ven-gpio", 0);
	nfc->firm_gpio = of_get_named_gpio(dev->of_node, "nxp,firm-gpio", 0);
	ret = nfc->irq_gpio;
	if (ret < 0)
		goto free;
	ret = nfc->ven_gpio;
	if (ret < 0)
		goto free;
	ret = nfc->firm_gpio;
	if (ret < 0)
		goto free;
	ret = devm_gpio_request_one(dev, nfc->irq_gpio, GPIOF_IN, "pn547-irq");
	if (ret)
		goto free;
	ret = devm_gpio_request_one(dev, nfc->ven_gpio, GPIOF_OUT_INIT_LOW,
				    "pn547-ven");
	if (ret)
		goto free;
	ret = devm_gpio_request_one(dev, nfc->firm_gpio, GPIOF_OUT_INIT_LOW,
				    "pn547-firm");
	if (ret)
		goto free;
	if (gpio_cansleep(nfc->irq_gpio) || gpio_cansleep(nfc->ven_gpio) ||
	    gpio_cansleep(nfc->firm_gpio)) {
		ret = -EINVAL;
		goto free;
	}
	nfc->irq = gpio_to_irq(nfc->irq_gpio);
	ret = nfc->irq;
	if (ret < 0)
		goto free;
	nfc->clk = clk_get_sys("pn547", "xo");
	if (IS_ERR(nfc->clk)) {
		ret = PTR_ERR(nfc->clk);
		goto free;
	}
	ret = clk_prepare_enable(nfc->clk);
	if (ret)
		goto put_clock;
	kref_init(&nfc->ref);
	mutex_init(&nfc->io_lock);
	mutex_init(&nfc->read_lock);
	spin_lock_init(&nfc->irq_lock);
	init_waitqueue_head(&nfc->read_wait);
	atomic_set(&nfc->irq_pending, 0);
	atomic_set(&nfc->cancel_pending, 0);
	wake_lock_init(&nfc->wake_lock, WAKE_LOCK_SUSPEND, "pn547");
	nfc->irq_enabled = true;
	/* Stock requests a high-level IRQ, overriding the DT trigger. */
	ret = request_irq(nfc->irq, ef67_pn547_irq, IRQF_TRIGGER_HIGH,
			  "pn547", nfc);
	if (ret)
		goto destroy;
	ef67_pn547_disable_irq(nfc);
	nfc->misc.minor = MISC_DYNAMIC_MINOR;
	nfc->misc.name = "pn547";
	nfc->misc.fops = &ef67_pn547_fops;
	nfc->misc.parent = dev;
	i2c_set_clientdata(client, nfc);
	ret = misc_register(&nfc->misc);
	if (ret)
		goto free_irq;
	return 0;

free_irq:
	i2c_set_clientdata(client, NULL);
	free_irq(nfc->irq, nfc);
destroy:
	wake_lock_destroy(&nfc->wake_lock);
	mutex_destroy(&nfc->read_lock);
	mutex_destroy(&nfc->io_lock);
	clk_disable_unprepare(nfc->clk);
put_clock:
	clk_put(nfc->clk);
free:
	kfree(nfc);
	return ret;
}

static void ef67_pn547_stop(struct ef67_pn547 *nfc)
{
	mutex_lock(&nfc->io_lock);
	if (!nfc->removed) {
		nfc->removed = true;
		ef67_pn547_disable_irq(nfc);
		if (nfc->irq_wake_enabled) {
			disable_irq_wake(nfc->irq);
			nfc->irq_wake_enabled = false;
		}
		ef67_pn547_power(nfc, 0);
		clk_disable_unprepare(nfc->clk);
	}
	mutex_unlock(&nfc->io_lock);
}

static int ef67_pn547_remove(struct i2c_client *client)
{
	struct ef67_pn547 *nfc = i2c_get_clientdata(client);

	/* misc_open holds misc_mtx while taking io_lock; keep this outside it. */
	misc_deregister(&nfc->misc);
	ef67_pn547_stop(nfc);
	free_irq(nfc->irq, nfc);
	clk_put(nfc->clk);
	i2c_set_clientdata(client, NULL);
	kref_put(&nfc->ref, ef67_pn547_free);
	return 0;
}

static void ef67_pn547_shutdown(struct i2c_client *client)
{
	ef67_pn547_stop(i2c_get_clientdata(client));
}

#ifdef CONFIG_PM_SLEEP
static int ef67_pn547_suspend(struct device *dev)
{
	struct ef67_pn547 *nfc = dev_get_drvdata(dev);
	int ret = 0;

	mutex_lock(&nfc->io_lock);
	if (!nfc->removed && nfc->powered && !nfc->irq_wake_enabled) {
		ret = enable_irq_wake(nfc->irq);
		if (!ret)
			nfc->irq_wake_enabled = true;
	}
	mutex_unlock(&nfc->io_lock);
	return ret;
}

static int ef67_pn547_resume(struct device *dev)
{
	struct ef67_pn547 *nfc = dev_get_drvdata(dev);
	int ret = 0;

	mutex_lock(&nfc->io_lock);
	if (!nfc->removed && nfc->powered && gpio_get_value(nfc->irq_gpio))
		wake_lock_timeout(&nfc->wake_lock, msecs_to_jiffies(100));
	if (nfc->irq_wake_enabled) {
		ret = disable_irq_wake(nfc->irq);
		if (!ret)
			nfc->irq_wake_enabled = false;
	}
	mutex_unlock(&nfc->io_lock);
	return ret;
}
#endif

static SIMPLE_DEV_PM_OPS(ef67_pn547_pm_ops, ef67_pn547_suspend,
			ef67_pn547_resume);

static const struct of_device_id ef67_pn547_of_match[] = {
	{ .compatible = "nxp,pn547" },
	{ }
};
MODULE_DEVICE_TABLE(of, ef67_pn547_of_match);

static const struct i2c_device_id ef67_pn547_id[] = {
	{ "pn547", 0 },
	{ }
};
MODULE_DEVICE_TABLE(i2c, ef67_pn547_id);

static struct i2c_driver ef67_pn547_driver = {
	.driver = {
		.name = "pn547",
		.owner = THIS_MODULE,
		.of_match_table = ef67_pn547_of_match,
		.pm = &ef67_pn547_pm_ops,
	},
	.probe = ef67_pn547_probe,
	.remove = ef67_pn547_remove,
	.shutdown = ef67_pn547_shutdown,
	.id_table = ef67_pn547_id,
};
module_i2c_driver(ef67_pn547_driver);

MODULE_DESCRIPTION("Pantech EF67 PN547 NFC interface");
MODULE_LICENSE("GPL");
