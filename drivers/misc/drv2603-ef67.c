// SPDX-License-Identifier: GPL-2.0-only
/* Pantech EF67 DRV2603 haptic controller. */

#include <linux/gpio.h>
#include <linux/hrtimer.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/pwm.h>
#include <linux/qpnp/pwm.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/workqueue.h>

#include "../staging/android/timed_output.h"

#define EF67_VIB_PWM_CHANNEL	1
#define EF67_VIB_PERIOD_US	100
#define EF67_VIB_MAX_LEVEL	10
#define EF67_VIB_MAX_TIME_MS	30000

struct ef67_vibrator {
	struct device *dev;
	struct pwm_device *pwm;
	struct timed_output_dev timed;
	struct hrtimer timer;
	struct work_struct work;
	struct mutex lock;
	spinlock_t state_lock;
	unsigned int enable_gpio;
	unsigned int level;
	bool blocked;
};

static void ef67_vibrator_off(struct ef67_vibrator *vib)
{
	/* EN gates the motor even if a PMIC PWM write fails. */
	gpio_set_value(vib->enable_gpio, 0);
	pwm_disable(vib->pwm);
}

static void ef67_vibrator_work(struct work_struct *work)
{
	struct ef67_vibrator *vib = container_of(work, struct ef67_vibrator,
						work);
	unsigned long flags;
	unsigned int level;
	int ret;

	mutex_lock(&vib->lock);
	spin_lock_irqsave(&vib->state_lock, flags);
	level = vib->level;
	spin_unlock_irqrestore(&vib->state_lock, flags);

	if (vib->blocked || !level) {
		ef67_vibrator_off(vib);
		goto out;
	}

	/* Stock uses 50 + 5 * level microseconds on LPG channel 1. */
	ef67_vibrator_off(vib);
	ret = pwm_config_us(vib->pwm, 50 + 5 * level, EF67_VIB_PERIOD_US);
	if (!ret)
		ret = pwm_enable(vib->pwm);
	if (ret) {
		ef67_vibrator_off(vib);
		hrtimer_cancel(&vib->timer);
		spin_lock_irqsave(&vib->state_lock, flags);
		vib->level = 0;
		spin_unlock_irqrestore(&vib->state_lock, flags);
		dev_err(vib->dev, "cannot start vibration: %d\n", ret);
		goto out;
	}
	/* Do not turn EN on if the deadline passed during PWM setup. */
	spin_lock_irqsave(&vib->state_lock, flags);
	gpio_set_value(vib->enable_gpio, !!vib->level);
	spin_unlock_irqrestore(&vib->state_lock, flags);
out:
	mutex_unlock(&vib->lock);
}

static enum hrtimer_restart ef67_vibrator_expire(struct hrtimer *timer)
{
	struct ef67_vibrator *vib = container_of(timer, struct ef67_vibrator,
						timer);
	unsigned long flags;

	spin_lock_irqsave(&vib->state_lock, flags);
	vib->level = 0;
	/* The TLMM enable GPIO can stop the motor directly at the deadline. */
	gpio_set_value(vib->enable_gpio, 0);
	spin_unlock_irqrestore(&vib->state_lock, flags);
	schedule_work(&vib->work);
	return HRTIMER_NORESTART;
}

static void ef67_vibrator_enable(struct timed_output_dev *timed, int value)
{
	struct ef67_vibrator *vib = container_of(timed, struct ef67_vibrator,
						timed);
	unsigned int level = value > 0 ? (unsigned int)value >> 16 : 0;
	unsigned int duration_ms = (unsigned int)value & 0xffff;
	unsigned long flags;

	/* Pantech packs strength in the high word and milliseconds in the low. */
	level = min(level, (unsigned int)EF67_VIB_MAX_LEVEL);
	duration_ms = min(duration_ms, (unsigned int)EF67_VIB_MAX_TIME_MS);
	if (!duration_ms)
		level = 0;

	mutex_lock(&vib->lock);
	if (vib->blocked)
		goto out;

	hrtimer_cancel(&vib->timer);
	spin_lock_irqsave(&vib->state_lock, flags);
	vib->level = level;
	spin_unlock_irqrestore(&vib->state_lock, flags);
	if (level)
		hrtimer_start(&vib->timer,
				ktime_set(duration_ms / 1000,
					(duration_ms % 1000) * NSEC_PER_MSEC),
				HRTIMER_MODE_REL);
	else
		ef67_vibrator_off(vib);
	schedule_work(&vib->work);
out:
	mutex_unlock(&vib->lock);
}

static int ef67_vibrator_get_time(struct timed_output_dev *timed)
{
	struct ef67_vibrator *vib = container_of(timed, struct ef67_vibrator,
						timed);
	s64 remaining_us = 0;

	mutex_lock(&vib->lock);
	/* The stock Pantech interface reports microseconds, despite its name. */
	if (hrtimer_active(&vib->timer))
		remaining_us = ktime_to_us(hrtimer_get_remaining(&vib->timer));
	mutex_unlock(&vib->lock);
	return max_t(s64, remaining_us, 0);
}

static void ef67_vibrator_stop(struct ef67_vibrator *vib)
{
	unsigned long flags;

	mutex_lock(&vib->lock);
	vib->blocked = true;
	hrtimer_cancel(&vib->timer);
	spin_lock_irqsave(&vib->state_lock, flags);
	vib->level = 0;
	spin_unlock_irqrestore(&vib->state_lock, flags);
	ef67_vibrator_off(vib);
	mutex_unlock(&vib->lock);
	cancel_work_sync(&vib->work);
}

static int ef67_vibrator_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct device_node *enable_node;
	struct ef67_vibrator *vib;
	u32 gpio;
	int ret;

	enable_node = of_get_child_by_name(dev->of_node, "drv2603_en");
	if (!enable_node)
		return -EINVAL;
	/* Retain the stock DT's raw GPIO number in the enable child node. */
	ret = of_property_read_u32(enable_node, "gpios", &gpio);
	of_node_put(enable_node);
	if (ret)
		return ret;
	if (!gpio_is_valid(gpio))
		return -EINVAL;

	vib = devm_kzalloc(dev, sizeof(*vib), GFP_KERNEL);
	if (!vib)
		return -ENOMEM;
	vib->dev = dev;
	vib->enable_gpio = gpio;
	mutex_init(&vib->lock);
	spin_lock_init(&vib->state_lock);
	INIT_WORK(&vib->work, ef67_vibrator_work);
	hrtimer_init(&vib->timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
	vib->timer.function = ef67_vibrator_expire;

	ret = devm_gpio_request_one(dev, gpio, GPIOF_OUT_INIT_LOW, "vib-en");
	if (ret)
		return ret;
	if (gpio_cansleep(gpio))
		return -EINVAL;
	vib->pwm = pwm_request(EF67_VIB_PWM_CHANNEL, "vib-pwm");
	if (IS_ERR(vib->pwm))
		return PTR_ERR(vib->pwm);
	ef67_vibrator_off(vib);

	vib->timed.name = "vibrator";
	vib->timed.enable = ef67_vibrator_enable;
	vib->timed.get_time = ef67_vibrator_get_time;
	platform_set_drvdata(pdev, vib);
	ret = timed_output_dev_register(&vib->timed);
	if (ret) {
		ef67_vibrator_stop(vib);
		pwm_free(vib->pwm);
		return ret;
	}
	return 0;
}

static int ef67_vibrator_remove(struct platform_device *pdev)
{
	struct ef67_vibrator *vib = platform_get_drvdata(pdev);

	ef67_vibrator_stop(vib);
	timed_output_dev_unregister(&vib->timed);
	pwm_free(vib->pwm);
	return 0;
}

static void ef67_vibrator_shutdown(struct platform_device *pdev)
{
	ef67_vibrator_stop(platform_get_drvdata(pdev));
}

static int ef67_vibrator_suspend(struct device *dev)
{
	ef67_vibrator_stop(dev_get_drvdata(dev));
	return 0;
}

static int ef67_vibrator_resume(struct device *dev)
{
	struct ef67_vibrator *vib = dev_get_drvdata(dev);

	mutex_lock(&vib->lock);
	vib->blocked = false;
	mutex_unlock(&vib->lock);
	return 0;
}

static SIMPLE_DEV_PM_OPS(ef67_vibrator_pm, ef67_vibrator_suspend,
			ef67_vibrator_resume);

static const struct of_device_id ef67_vibrator_match[] = {
	{ .compatible = "drv2603" },
	{ }
};
MODULE_DEVICE_TABLE(of, ef67_vibrator_match);

static struct platform_driver ef67_vibrator_driver = {
	.probe = ef67_vibrator_probe,
	.remove = ef67_vibrator_remove,
	.shutdown = ef67_vibrator_shutdown,
	.driver = {
		.name = "drv2603-ef67",
		.owner = THIS_MODULE,
		.of_match_table = ef67_vibrator_match,
		.pm = &ef67_vibrator_pm,
	},
};
module_platform_driver(ef67_vibrator_driver);

MODULE_DESCRIPTION("Pantech EF67 DRV2603 vibrator");
MODULE_LICENSE("GPL v2");
