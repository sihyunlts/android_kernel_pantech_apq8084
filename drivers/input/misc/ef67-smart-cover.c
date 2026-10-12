/* EF67 smart-cover input and S0226114 control interface. */
#include <linux/delay.h>
#include <linux/gpio.h>
#include <linux/input.h>
#include <linux/interrupt.h>
#include <linux/kref.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of_gpio.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/workqueue.h>
#include <mach/gpio.h>

struct ef67_cover {
	struct input_dev *input;
	struct delayed_work debounce;
	spinlock_t lock;
	int gpio;
	int irq;
	u32 debounce_ms;
	bool invert;
	bool enabled;
	bool candidate_valid;
	int candidate;
};

struct ef67_cover_control {
	struct miscdevice misc;
	struct kref ref;
	struct mutex lock;
	struct ef67_cover *cover;
};

static void ef67_cover_report(struct ef67_cover *cover, int closed)
{
	input_report_switch(cover->input, SW_LID, closed);
	input_sync(cover->input);
}

static void ef67_cover_debounce(struct work_struct *work)
{
	struct ef67_cover *cover = container_of(to_delayed_work(work),
					struct ef67_cover, debounce);
	unsigned long flags;
	int state;
	bool stable;

	spin_lock_irqsave(&cover->lock, flags);
	state = !!gpio_get_value(cover->gpio) ^ cover->invert;
	stable = cover->candidate_valid && state == cover->candidate;
	cover->candidate = state;
	cover->candidate_valid = !stable;
	if (!stable)
		mod_delayed_work(system_wq, &cover->debounce,
				 msecs_to_jiffies(cover->debounce_ms));
	spin_unlock_irqrestore(&cover->lock, flags);
	if (stable)
		ef67_cover_report(cover, state);
}

static irqreturn_t ef67_cover_irq(int irq, void *data)
{
	struct ef67_cover *cover = data;
	unsigned long flags;

	spin_lock_irqsave(&cover->lock, flags);
	cover->candidate_valid = false;
	mod_delayed_work(system_wq, &cover->debounce,
			 msecs_to_jiffies(cover->debounce_ms));
	spin_unlock_irqrestore(&cover->lock, flags);
	return IRQ_HANDLED;
}

static void ef67_cover_control_release(struct kref *ref)
{
	kfree(container_of(ref, struct ef67_cover_control, ref));
}

static int ef67_cover_open(struct inode *inode, struct file *file)
{
	struct miscdevice *misc = file->private_data;
	struct ef67_cover_control *control =
		container_of(misc, struct ef67_cover_control, misc);

	kref_get(&control->ref);
	file->private_data = control;
	return nonseekable_open(inode, file);
}

static int ef67_cover_close(struct inode *inode, struct file *file)
{
	struct ef67_cover_control *control = file->private_data;

	kref_put(&control->ref, ef67_cover_control_release);
	return 0;
}

static long ef67_cover_ioctl(struct file *file, unsigned int command,
			    unsigned long argument)
{
	struct ef67_cover_control *control = file->private_data;
	struct ef67_cover *cover;
	int ret = 0;

	mutex_lock(&control->lock);
	cover = control->cover;
	if (!cover) {
		ret = -ENODEV;
		goto out;
	}
	switch (command) {
	case 0:
		/* Stock changes only sub_smart_cover1_detection, absent on EF67S. */
		break;
	case 10:
		if (!cover->enabled) {
			ret = enable_irq_wake(cover->irq);
			if (ret)
				break;
			cover->enabled = true;
			enable_irq(cover->irq);
		}
		break;
	case 11:
		if (cover->enabled) {
			disable_irq_wake(cover->irq);
			disable_irq(cover->irq);
			cover->enabled = false;
		}
		cancel_delayed_work_sync(&cover->debounce);
		msleep(20);
		ef67_cover_report(cover, 0);
		break;
	default:
		ret = -ENOTTY;
	}
out:
	mutex_unlock(&control->lock);
	return ret;
}

static const struct file_operations ef67_cover_fops = {
	.owner = THIS_MODULE,
	.open = ef67_cover_open,
	.release = ef67_cover_close,
	.unlocked_ioctl = ef67_cover_ioctl,
	.llseek = no_llseek,
};

static int ef67_cover_probe(struct platform_device *pdev)
{
	struct device_node *node;
	struct ef67_cover_control *control;
	struct ef67_cover *cover;
	enum of_gpio_flags flags;
	int ret;

	cover = devm_kzalloc(&pdev->dev, sizeof(*cover), GFP_KERNEL);
	if (!cover)
		return -ENOMEM;
	node = of_get_child_by_name(pdev->dev.of_node, "cover");
	if (!node)
		return -ENODEV;
	cover->gpio = of_get_named_gpio_flags(node, "gpios", 0, &flags);
	ret = of_property_read_u32(node, "debounce-interval", &cover->debounce_ms);
	of_node_put(node);
	if (cover->gpio < 0)
		return cover->gpio;
	if (ret || !cover->debounce_ms)
		return ret ? ret : -EINVAL;
	cover->invert = !!(flags & OF_GPIO_ACTIVE_LOW);
	spin_lock_init(&cover->lock);
	INIT_DELAYED_WORK(&cover->debounce, ef67_cover_debounce);
	ret = devm_gpio_request(&pdev->dev, cover->gpio, "smart_cover");
	if (ret)
		return ret;
	ret = gpio_tlmm_config(GPIO_CFG(cover->gpio, 0, GPIO_CFG_INPUT,
			GPIO_CFG_PULL_UP, GPIO_CFG_2MA), GPIO_CFG_ENABLE);
	if (ret)
		return ret;
	ret = gpio_direction_input(cover->gpio);
	if (ret)
		return ret;
	cover->irq = gpio_to_irq(cover->gpio);
	if (cover->irq < 0)
		return cover->irq;
	cover->input = input_allocate_device();
	if (!cover->input)
		return -ENOMEM;
	cover->input->name = "smart_cover";
	cover->input->dev.parent = &pdev->dev;
	cover->input->id.bustype = BUS_HOST;
	input_set_capability(cover->input, EV_SW, SW_LID);
	ret = input_register_device(cover->input);
	if (ret)
		goto free_input;
	ret = device_init_wakeup(&pdev->dev, true);
	if (ret)
		goto unregister_input;
	ret = request_irq(cover->irq, ef67_cover_irq,
		IRQF_TRIGGER_RISING | IRQF_TRIGGER_FALLING | IRQF_NO_SUSPEND,
		"smart_cover", cover);
	if (ret)
		goto disable_wakeup;
	ret = enable_irq_wake(cover->irq);
	if (ret)
		goto free_irq;
	cover->enabled = true;
	ef67_cover_report(cover, !!gpio_get_value(cover->gpio) ^ cover->invert);
	control = kzalloc(sizeof(*control), GFP_KERNEL);
	if (!control) {
		ret = -ENOMEM;
		goto disable_irq_wake;
	}
	kref_init(&control->ref);
	mutex_init(&control->lock);
	control->cover = cover;
	control->misc.minor = MISC_DYNAMIC_MINOR;
	control->misc.name = "gpio_switch_dev";
	control->misc.fops = &ef67_cover_fops;
	control->misc.parent = &pdev->dev;
	control->misc.mode = 0600;
	ret = misc_register(&control->misc);
	if (ret) {
		kref_put(&control->ref, ef67_cover_control_release);
		goto disable_irq_wake;
	}
	platform_set_drvdata(pdev, control);
	return 0;

disable_irq_wake:
	disable_irq_wake(cover->irq);
free_irq:
	free_irq(cover->irq, cover);
	cancel_delayed_work_sync(&cover->debounce);
disable_wakeup:
	device_init_wakeup(&pdev->dev, false);
unregister_input:
	input_unregister_device(cover->input);
	return ret;
free_input:
	input_free_device(cover->input);
	return ret;
}

static int ef67_cover_remove(struct platform_device *pdev)
{
	struct ef67_cover_control *control = platform_get_drvdata(pdev);
	struct ef67_cover *cover;

	misc_deregister(&control->misc);
	mutex_lock(&control->lock);
	cover = control->cover;
	control->cover = NULL;
	if (cover->enabled)
		disable_irq_wake(cover->irq);
	free_irq(cover->irq, cover);
	cancel_delayed_work_sync(&cover->debounce);
	device_init_wakeup(&pdev->dev, false);
	input_unregister_device(cover->input);
	mutex_unlock(&control->lock);
	kref_put(&control->ref, ef67_cover_control_release);
	return 0;
}

static const struct of_device_id ef67_cover_match[] = {
	{ .compatible = "pantech,ef67-smart-cover" },
	{ }
};
MODULE_DEVICE_TABLE(of, ef67_cover_match);

static int ef67_cover_resume(struct device *dev)
{
	struct ef67_cover_control *control = dev_get_drvdata(dev);
	struct ef67_cover *cover;

	mutex_lock(&control->lock);
	cover = control->cover;
	if (cover && cover->enabled)
		ef67_cover_report(cover,
			 !!gpio_get_value(cover->gpio) ^ cover->invert);
	mutex_unlock(&control->lock);
	return 0;
}

static SIMPLE_DEV_PM_OPS(ef67_cover_pm, NULL, ef67_cover_resume);

static struct platform_driver ef67_cover_driver = {
	.probe = ef67_cover_probe,
	.remove = ef67_cover_remove,
	.driver = {
		.name = "ef67-smart-cover",
		.owner = THIS_MODULE,
		.of_match_table = ef67_cover_match,
		.pm = &ef67_cover_pm,
	},
};
module_platform_driver(ef67_cover_driver);
MODULE_DESCRIPTION("Pantech EF67 smart-cover control");
MODULE_LICENSE("GPL");
