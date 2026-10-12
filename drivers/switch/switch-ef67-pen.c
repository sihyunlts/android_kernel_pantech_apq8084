/* EF67 pen insertion switch, matching the S0226114 Android interface. */
#include <linux/delay.h>
#include <linux/gpio.h>
#include <linux/interrupt.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/switch.h>
#include <linux/workqueue.h>
#include <mach/gpio.h>

struct ef67_pen_switch {
	struct device *dev;
	struct switch_dev switch_dev;
	struct delayed_work debounce_work;
	spinlock_t lock;
	u32 gpio;
	u32 invert;
	u32 debounce_ms;
	int irq;
	int candidate;
	bool candidate_valid;
};

static ssize_t ef67_pen_print_state(struct switch_dev *sdev, char *buf)
{
	return scnprintf(buf, PAGE_SIZE, "%s\n", sdev->state ? "OFF" : "ON");
}

static void ef67_pen_debounce(struct work_struct *work)
{
	struct ef67_pen_switch *pen = container_of(to_delayed_work(work),
				struct ef67_pen_switch, debounce_work);
	unsigned long flags;
	int state;
	bool stable;

	/* MSM GPIOs do not sleep; sample under the same lock as edge reset. */
	spin_lock_irqsave(&pen->lock, flags);
	state = !!gpio_get_value(pen->gpio) ^ pen->invert;
	stable = pen->candidate_valid && pen->candidate == state;
	pen->candidate = state;
	pen->candidate_valid = !stable;
	if (!stable)
		mod_delayed_work(system_wq, &pen->debounce_work,
				 msecs_to_jiffies(pen->debounce_ms));
	spin_unlock_irqrestore(&pen->lock, flags);

	/* Stock requires two consecutive equal samples before notification. */
	if (stable)
		switch_set_state(&pen->switch_dev, state);
}

static irqreturn_t ef67_pen_interrupt(int irq, void *data)
{
	struct ef67_pen_switch *pen = data;
	unsigned long flags;

	pm_wakeup_event(pen->dev, 500);
	spin_lock_irqsave(&pen->lock, flags);
	pen->candidate_valid = false;
	mod_delayed_work(system_wq, &pen->debounce_work,
			 msecs_to_jiffies(pen->debounce_ms));
	spin_unlock_irqrestore(&pen->lock, flags);
	return IRQ_HANDLED;
}

static int ef67_pen_probe(struct platform_device *pdev)
{
	struct device_node *node;
	struct ef67_pen_switch *pen;
	const char *label;
	u32 event_mode;
	int ret;

	node = of_get_child_by_name(pdev->dev.of_node, "touch_pen_detection");
	if (!node)
		return -ENODEV;
	pen = devm_kzalloc(&pdev->dev, sizeof(*pen), GFP_KERNEL);
	if (!pen) {
		ret = -ENOMEM;
		goto put_node;
	}
	ret = of_property_read_string(node, "label", &label);
	if (ret)
		goto put_node;
	if (strcmp(label, "touch_pen_detection")) {
		ret = -EINVAL;
		goto put_node;
	}
	/* Stock uses a plain GPIO number here, not a GPIO phandle. */
	ret = of_property_read_u32(node, "gpios", &pen->gpio);
	if (ret)
		goto put_node;
	ret = of_property_read_u32(node, "flag", &pen->invert);
	if (ret)
		goto put_node;
	ret = of_property_read_u32(node, "debounce_time", &pen->debounce_ms);
	if (ret)
		goto put_node;
	ret = of_property_read_u32(node, "event_mode", &event_mode);
	if (ret)
		goto put_node;
	if (!gpio_is_valid(pen->gpio) || pen->invert > 1 ||
	    !pen->debounce_ms || event_mode != 1) {
		ret = -EINVAL;
		goto put_node;
	}
	of_node_put(node);
	pen->dev = &pdev->dev;
	pen->switch_dev.name = "touch_pen_detection";
	pen->switch_dev.print_state = ef67_pen_print_state;
	spin_lock_init(&pen->lock);
	INIT_DELAYED_WORK(&pen->debounce_work, ef67_pen_debounce);
	ret = gpio_request(pen->gpio, pen->switch_dev.name);
	if (ret)
		return ret;
	ret = gpio_tlmm_config(GPIO_CFG(pen->gpio, 0, GPIO_CFG_INPUT,
				GPIO_CFG_PULL_UP, GPIO_CFG_2MA), GPIO_CFG_ENABLE);
	if (ret)
		goto free_gpio;
	ret = gpio_direction_input(pen->gpio);
	if (ret)
		goto free_gpio;
	pen->irq = gpio_to_irq(pen->gpio);
	if (pen->irq < 0) {
		ret = pen->irq;
		goto free_gpio;
	}
	ret = switch_dev_register(&pen->switch_dev);
	if (ret)
		goto free_gpio;
	ret = device_init_wakeup(&pdev->dev, true);
	if (ret)
		goto unregister_switch;
	platform_set_drvdata(pdev, pen);
	ret = request_irq(pen->irq, ef67_pen_interrupt,
		IRQF_TRIGGER_RISING | IRQF_TRIGGER_FALLING | IRQF_NO_SUSPEND,
		pen->switch_dev.name, pen);
	if (ret)
		goto disable_wakeup;
	ret = enable_irq_wake(pen->irq);
	if (ret)
		goto free_irq;
	switch_set_state(&pen->switch_dev,
			 !!gpio_get_value(pen->gpio) ^ pen->invert);
	return 0;

free_irq:
	free_irq(pen->irq, pen);
	cancel_delayed_work_sync(&pen->debounce_work);
disable_wakeup:
	device_init_wakeup(&pdev->dev, false);
unregister_switch:
	switch_dev_unregister(&pen->switch_dev);
free_gpio:
	gpio_free(pen->gpio);
	return ret;
put_node:
	of_node_put(node);
	return ret;
}

static int ef67_pen_remove(struct platform_device *pdev)
{
	struct ef67_pen_switch *pen = platform_get_drvdata(pdev);

	disable_irq_wake(pen->irq);
	free_irq(pen->irq, pen);
	cancel_delayed_work_sync(&pen->debounce_work);
	device_init_wakeup(&pdev->dev, false);
	switch_dev_unregister(&pen->switch_dev);
	gpio_free(pen->gpio);
	return 0;
}

static const struct of_device_id ef67_pen_match[] = {
	{ .compatible = "gpio_switch" },
	{ },
};
MODULE_DEVICE_TABLE(of, ef67_pen_match);

static struct platform_driver ef67_pen_driver = {
	.probe = ef67_pen_probe,
	.remove = ef67_pen_remove,
	.driver = {
		.name = "ef67-pen-switch",
		.owner = THIS_MODULE,
		.of_match_table = ef67_pen_match,
	},
};
module_platform_driver(ef67_pen_driver);
MODULE_DESCRIPTION("Pantech EF67 pen insertion switch");
MODULE_LICENSE("GPL");
