/* SPDX-License-Identifier: GPL-2.0-only */
/* MAX17058 voltage and state-of-charge reporting for Pantech EF67. */

#include <linux/bug.h>
#include <linux/i2c.h>
#include <linux/jiffies.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/pm.h>
#include <linux/power_supply.h>
#include <linux/slab.h>
#include <linux/workqueue.h>

#define MAX17058_VCELL_REG	0x02
#define MAX17058_SOC_REG		0x04
#define MAX17058_POLL_MS		30000
#define EF67_SOC_SCALE		512
#define EF67_SOC_EMPTY_MILLIPERCENT	2140
#define EF67_SOC_FULL_MILLIPERCENT	94700

struct ef67_max17058 {
	struct i2c_client *client;
	struct power_supply bms;
	struct delayed_work poll_work;
	int last_voltage_uv;
	int last_capacity;
};

static int max17058_read_word(struct i2c_client *client, u8 reg)
{
	u8 data[2];
	struct i2c_msg msgs[] = {
		{
			.addr = client->addr,
			.len = 1,
			.buf = &reg,
		},
		{
			.addr = client->addr,
			.flags = I2C_M_RD,
			.len = sizeof(data),
			.buf = data,
		},
	};
	int rc;

	rc = i2c_transfer(client->adapter, msgs, ARRAY_SIZE(msgs));
	if (rc < 0)
		return rc;
	if (rc != ARRAY_SIZE(msgs))
		return -EIO;

	return (data[0] << 8) | data[1];
}

static int ef67_max17058_capacity(int raw_soc)
{
	/* EF67's installed battery model uses 1/512 percent SOC units. */
	int soc_millipercent = raw_soc * 1000 / EF67_SOC_SCALE;
	int capacity = (soc_millipercent - EF67_SOC_EMPTY_MILLIPERCENT) *
		100 / (EF67_SOC_FULL_MILLIPERCENT - EF67_SOC_EMPTY_MILLIPERCENT);

	return clamp(capacity, 0, 100);
}

static int ef67_max17058_voltage(int raw_voltage)
{
	return (raw_voltage >> 4) * 1250;
}

static enum power_supply_property ef67_max17058_properties[] = {
	POWER_SUPPLY_PROP_VOLTAGE_NOW,
	POWER_SUPPLY_PROP_CAPACITY,
};

static char *ef67_max17058_supplied_to[] = {
	"battery",
};

static int ef67_max17058_get_property(struct power_supply *psy,
				    enum power_supply_property prop,
				    union power_supply_propval *val)
{
	struct ef67_max17058 *chip = container_of(psy,
						struct ef67_max17058, bms);
	int raw;

	switch (prop) {
	case POWER_SUPPLY_PROP_VOLTAGE_NOW:
		raw = max17058_read_word(chip->client, MAX17058_VCELL_REG);
		if (raw < 0)
			return raw;
		val->intval = ef67_max17058_voltage(raw);
		return 0;
	case POWER_SUPPLY_PROP_CAPACITY:
		raw = max17058_read_word(chip->client, MAX17058_SOC_REG);
		if (raw < 0)
			return raw;
		val->intval = ef67_max17058_capacity(raw);
		return 0;
	default:
		return -EINVAL;
	}
}

static void ef67_max17058_poll(struct work_struct *work)
{
	struct ef67_max17058 *chip = container_of(to_delayed_work(work),
					struct ef67_max17058, poll_work);
	union power_supply_propval voltage, capacity;
	int rc;

	rc = ef67_max17058_get_property(&chip->bms,
				POWER_SUPPLY_PROP_VOLTAGE_NOW, &voltage);
	if (rc)
		goto reschedule;
	rc = ef67_max17058_get_property(&chip->bms,
				POWER_SUPPLY_PROP_CAPACITY, &capacity);
	if (rc)
		goto reschedule;

	if (voltage.intval != chip->last_voltage_uv ||
	    capacity.intval != chip->last_capacity) {
		chip->last_voltage_uv = voltage.intval;
		chip->last_capacity = capacity.intval;
		power_supply_changed(&chip->bms);
	}

reschedule:
	schedule_delayed_work(&chip->poll_work,
			     msecs_to_jiffies(MAX17058_POLL_MS));
}

static int ef67_max17058_probe(struct i2c_client *client,
			      const struct i2c_device_id *id)
{
	struct ef67_max17058 *chip;
	int raw, rc;

	if (!i2c_check_functionality(client->adapter, I2C_FUNC_I2C))
		return -EOPNOTSUPP;

	raw = max17058_read_word(client, MAX17058_SOC_REG);
	if (raw < 0)
		return raw;

	chip = devm_kzalloc(&client->dev, sizeof(*chip), GFP_KERNEL);
	if (!chip)
		return -ENOMEM;

	chip->client = client;
	chip->last_voltage_uv = -1;
	chip->last_capacity = -1;
	chip->bms.name = "bms";
	chip->bms.type = POWER_SUPPLY_TYPE_BMS;
	chip->bms.properties = ef67_max17058_properties;
	chip->bms.num_properties = ARRAY_SIZE(ef67_max17058_properties);
	chip->bms.supplied_to = ef67_max17058_supplied_to;
	chip->bms.num_supplicants = ARRAY_SIZE(ef67_max17058_supplied_to);
	chip->bms.get_property = ef67_max17058_get_property;
	i2c_set_clientdata(client, chip);
	INIT_DEFERRABLE_WORK(&chip->poll_work, ef67_max17058_poll);

	rc = power_supply_register(&client->dev, &chip->bms);
	if (rc)
		return rc;

	schedule_delayed_work(&chip->poll_work, 0);
	return 0;
}

static int ef67_max17058_remove(struct i2c_client *client)
{
	struct ef67_max17058 *chip = i2c_get_clientdata(client);

	cancel_delayed_work_sync(&chip->poll_work);
	power_supply_unregister(&chip->bms);
	return 0;
}

#ifdef CONFIG_PM_SLEEP
static int ef67_max17058_suspend(struct device *dev)
{
	struct ef67_max17058 *chip = i2c_get_clientdata(to_i2c_client(dev));

	cancel_delayed_work_sync(&chip->poll_work);
	return 0;
}

static int ef67_max17058_resume(struct device *dev)
{
	struct ef67_max17058 *chip = i2c_get_clientdata(to_i2c_client(dev));

	schedule_delayed_work(&chip->poll_work, 0);
	return 0;
}
#endif

static SIMPLE_DEV_PM_OPS(ef67_max17058_pm_ops,
			ef67_max17058_suspend, ef67_max17058_resume);

static const struct of_device_id ef67_max17058_match[] = {
	{ .compatible = "maxim,max17058-i2c" },
	{ },
};
MODULE_DEVICE_TABLE(of, ef67_max17058_match);

static const struct i2c_device_id ef67_max17058_id[] = {
	{ "max17058_i2c", 0 },
	{ },
};
MODULE_DEVICE_TABLE(i2c, ef67_max17058_id);

static struct i2c_driver ef67_max17058_driver = {
	.driver = {
		.name = "ef67-max17058",
		.owner = THIS_MODULE,
		.of_match_table = of_match_ptr(ef67_max17058_match),
		.pm = &ef67_max17058_pm_ops,
	},
	.probe = ef67_max17058_probe,
	.remove = ef67_max17058_remove,
	.id_table = ef67_max17058_id,
};
module_i2c_driver(ef67_max17058_driver);

MODULE_DESCRIPTION("Pantech EF67 MAX17058 fuel gauge");
MODULE_LICENSE("GPL v2");
