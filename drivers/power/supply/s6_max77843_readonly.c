// SPDX-License-Identifier: GPL-2.0-only
/*
 * SM-G925I bring-up: MAX77843 telemetry, no model/charger/IRQ writes.
 * Units follow Samsung max77843_fuelgauge/charger and ModelGauge documentation.
 */
#include <linux/i2c.h>
#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/jiffies.h>
#include <linux/mfd/max77693-common.h>
#include <linux/mfd/max77843-private.h>
#include <linux/module.h>
#include <linux/iio/consumer.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/power_supply.h>
#include <linux/regmap.h>
#include <linux/workqueue.h>

struct s6_power {
	struct iio_channel *battery_ntc, *charger_ntc;
	struct max77693_dev *max;
	struct regmap *fg;
	struct power_supply *battery, *usb;
	struct power_supply_desc battery_desc;
	struct delayed_work poll;
	bool external_temp, temp_enabled;
};

static bool s6_fg_readable(struct device *dev, unsigned int reg)
{
	switch (reg) {
	case 0x00:
	case 0x05:
	case 0x06:
	case 0x08:
	case 0x09:
	case 0x0a:
	case 0x0b:
	case 0x10:
	case 0x19:
	case 0x1d:
		return true;
	default:
		return false;
	}
}

static bool s6_fg_writeable(struct device *dev, unsigned int reg)
{
	return false;
}

static const struct regmap_config s6_fg_map = {
	.reg_bits = 8,
	.val_bits = 16,
	.max_register = 0xff,
	.val_format_endian = REGMAP_ENDIAN_LITTLE,
	.cache_type = REGCACHE_NONE,
	.readable_reg = s6_fg_readable,
	.writeable_reg = s6_fg_writeable,
};

static enum power_supply_property s6_battery_props[] = {
	POWER_SUPPLY_PROP_STATUS,
	POWER_SUPPLY_PROP_PRESENT,
	POWER_SUPPLY_PROP_TECHNOLOGY,
	POWER_SUPPLY_PROP_CAPACITY,
	POWER_SUPPLY_PROP_VOLTAGE_NOW,
	POWER_SUPPLY_PROP_VOLTAGE_AVG,
	POWER_SUPPLY_PROP_CURRENT_NOW,
	POWER_SUPPLY_PROP_CURRENT_AVG,
	POWER_SUPPLY_PROP_CHARGE_NOW,
	POWER_SUPPLY_PROP_CHARGE_FULL,
	/* Last: omitted if the temperature channel requires host updates. */
	POWER_SUPPLY_PROP_TEMP,
};

/*
 * OEM zerolte NTC tables; linear interpolation in tenths of a degree C.
 * Reject open/short/outside calibration; never clamp a sensor failure.
 */
struct s6_ntc_point {
	int adc, temp;
};

static const struct s6_ntc_point s6_battery_ntc[] = {
	{ 280, 900 },	{ 326, 850 },	{ 383, 800 },	{ 429, 750 },
	{ 489, 700 },	{ 559, 650 },	{ 631, 600 },	{ 727, 550 },
	{ 807, 500 },	{ 925, 450 },	{ 1015, 400 },	{ 1114, 350 },
	{ 1217, 300 },	{ 1341, 250 },	{ 1448, 200 },	{ 1545, 150 },
	{ 1625, 100 },	{ 1712, 50 },	{ 1778, 0 },	{ 1830, -50 },
	{ 1873, -100 }, { 1914, -150 }, { 1937, -200 },
};

static const struct s6_ntc_point s6_charger_ntc[] = {
	{ 499, 750 },  { 538, 700 },   { 598, 650 },   { 717, 600 },
	{ 835, 550 },  { 1026, 500 },  { 1124, 450 },  { 1240, 400 },
	{ 1478, 350 }, { 1568, 300 },  { 1834, 250 },  { 2023, 200 },
	{ 2189, 150 }, { 2429, 100 },  { 2631, 50 },   { 2851, 0 },
	{ 2948, -50 }, { 3053, -100 }, { 3239, -150 }, { 3325, -200 },
};

static int s6_ntc_read(struct iio_channel *channel,
		       const struct s6_ntc_point *table, unsigned int size,
		       int *temp)
{
	int raw, ret;
	unsigned int i;

	ret = iio_read_channel_raw(channel, &raw);
	if (ret < 0)
		return ret;
	if (raw < table[0].adc || raw > table[size - 1].adc)
		return -ENODATA;
	for (i = 1; i < size; i++) {
		if (raw <= table[i].adc) {
			*temp = table[i - 1].temp +
				(raw - table[i - 1].adc) *
					(table[i].temp - table[i - 1].temp) /
					(table[i].adc - table[i - 1].adc);
			return 0;
		}
	}
	return -ENODATA;
}

static int s6_battery_get(struct power_supply *psy,
			  enum power_supply_property prop,
			  union power_supply_propval *val)
{
	struct s6_power *d = power_supply_get_drvdata(psy);
	unsigned int raw, reg, status;
	int ret;

	switch (prop) {
	case POWER_SUPPLY_PROP_TECHNOLOGY:
		val->intval = POWER_SUPPLY_TECHNOLOGY_LION;
		return 0;
	case POWER_SUPPLY_PROP_STATUS:
		ret = regmap_read(d->max->regmap_chg,
				  MAX77843_CHG_REG_CHG_DTLS_01, &raw);
		if (ret)
			return ret;
		switch (raw & 0xf) {
		case 0:
		case 1:
		case 2:
			val->intval = POWER_SUPPLY_STATUS_CHARGING;
			break;
		case 3:
		case 4:
			val->intval = POWER_SUPPLY_STATUS_FULL;
			break;
		case 5:
		case 6:
		case 7:
			val->intval = POWER_SUPPLY_STATUS_NOT_CHARGING;
			break;
		case 8:
		case 10:
		case 11:
			val->intval = POWER_SUPPLY_STATUS_DISCHARGING;
			break;
		default:
			val->intval = POWER_SUPPLY_STATUS_UNKNOWN;
			break;
		}
		return 0;
	case POWER_SUPPLY_PROP_PRESENT:
		reg = 0x00;
		break;
	case POWER_SUPPLY_PROP_CAPACITY:
		ret = regmap_read(d->fg, 0x00, &status);
		if (ret)
			return ret;
		if (status &
		    BIT(1)) /* POR: do not report an uninitialized SOC. */
			return -ENODATA;
		reg = 0x06;
		break;
	case POWER_SUPPLY_PROP_VOLTAGE_NOW:
		reg = 0x09;
		break;
	case POWER_SUPPLY_PROP_VOLTAGE_AVG:
		reg = 0x19;
		break;
	case POWER_SUPPLY_PROP_CURRENT_NOW:
		reg = 0x0a;
		break;
	case POWER_SUPPLY_PROP_CURRENT_AVG:
		reg = 0x0b;
		break;
	case POWER_SUPPLY_PROP_CHARGE_NOW:
		reg = 0x05;
		break;
	case POWER_SUPPLY_PROP_CHARGE_FULL:
		reg = 0x10;
		break;
	case POWER_SUPPLY_PROP_TEMP:
		return s6_ntc_read(d->battery_ntc, s6_battery_ntc,
				   ARRAY_SIZE(s6_battery_ntc), &val->intval);
	default:
		return -EINVAL;
	}
	ret = regmap_read(d->fg, reg, &raw);
	if (ret)
		return ret;
	switch (prop) {
	case POWER_SUPPLY_PROP_PRESENT:
		val->intval = !(raw & BIT(3));
		break;
	case POWER_SUPPLY_PROP_CAPACITY:
		if (raw > 110 * 256)
			return -ENODATA;
		val->intval = min_t(unsigned int, raw / 256, 100);
		break;
	case POWER_SUPPLY_PROP_VOLTAGE_NOW:
	case POWER_SUPPLY_PROP_VOLTAGE_AVG:
		val->intval = raw * 625 / 8; /* 78.125 uV/LSB. */
		if (val->intval < 2500000 || val->intval > 5000000)
			return -ENODATA;
		break;
	case POWER_SUPPLY_PROP_CURRENT_NOW:
	case POWER_SUPPLY_PROP_CURRENT_AVG:
		val->intval =
			(s16)raw * 625 / 4; /* 156.25 uA/LSB at 10 mOhm. */
		break;
	case POWER_SUPPLY_PROP_CHARGE_NOW:
	case POWER_SUPPLY_PROP_CHARGE_FULL:
		val->intval = raw * 500; /* uAh, 10 mOhm. */
		break;
	case POWER_SUPPLY_PROP_TEMP:
		val->intval = (s16)raw * 10 / 256; /* tenths C. */
		if (val->intval < -400 || val->intval > 1000)
			return -ENODATA;
		break;
	default:
		return -EINVAL;
	}
	return 0;
}

static enum power_supply_property s6_usb_props[] = {
	POWER_SUPPLY_PROP_ONLINE,
	POWER_SUPPLY_PROP_INPUT_CURRENT_LIMIT,
	POWER_SUPPLY_PROP_CONSTANT_CHARGE_CURRENT,
	POWER_SUPPLY_PROP_TEMP,
};

static int s6_usb_get(struct power_supply *psy, enum power_supply_property prop,
		      union power_supply_propval *val)
{
	struct s6_power *d = power_supply_get_drvdata(psy);
	unsigned int reg, raw;
	int ret;

	switch (prop) {
	case POWER_SUPPLY_PROP_TEMP:
		return s6_ntc_read(d->charger_ntc, s6_charger_ntc,
				   ARRAY_SIZE(s6_charger_ntc), &val->intval);
	case POWER_SUPPLY_PROP_ONLINE:
		reg = MAX77843_CHG_REG_CHG_INT_OK;
		break;
	case POWER_SUPPLY_PROP_INPUT_CURRENT_LIMIT:
		reg = MAX77843_CHG_REG_CHG_CNFG_09;
		break;
	case POWER_SUPPLY_PROP_CONSTANT_CHARGE_CURRENT:
		reg = MAX77843_CHG_REG_CHG_CNFG_02;
		break;
	default:
		return -EINVAL;
	}
	ret = regmap_read(d->max->regmap_chg, reg, &raw);
	if (ret)
		return ret;
	switch (prop) {
	case POWER_SUPPLY_PROP_ONLINE:
		val->intval = !!(raw & MAX77843_CHG_CHGIN_OK);
		break;
	case POWER_SUPPLY_PROP_INPUT_CURRENT_LIMIT:
		raw &= 0x7f;
		val->intval = raw <= 3	  ? 100000 :
			      raw >= 0x78 ? 4000000 :
					    raw * 100000 / 3;
		break;
	case POWER_SUPPLY_PROP_CONSTANT_CHARGE_CURRENT:
		raw &= 0x3f;
		val->intval = raw <= 2 ? 100000 : raw * 50000;
		break;
	default:
		return -EINVAL;
	}
	return 0;
}

static const struct power_supply_desc s6_usb_desc = {
	.name = "s6-usb",
	.type = POWER_SUPPLY_TYPE_USB,
	.properties = s6_usb_props,
	.num_properties = ARRAY_SIZE(s6_usb_props),
	.get_property = s6_usb_get,
};

static ssize_t temperature_source_show(struct device *dev,
				       struct device_attribute *attr, char *buf)
{
	return sysfs_emit(
		buf,
		"Exynos7420 ADC1 battery NTC, ADC3 charger NTC; OEM tables, fresh conversion per read\n");
}

static DEVICE_ATTR_RO(temperature_source);

static ssize_t raw_snapshot_show(struct device *dev,
				 struct device_attribute *attr, char *buf)
{
	struct s6_power *d = dev_get_drvdata(dev);
	const unsigned int fg[] = { 0x00, 0x05, 0x06, 0x08, 0x09,
				    0x0a, 0x0b, 0x10, 0x19, 0x1d };
	const unsigned int chg[] = { 0xb2, 0xb3, 0xb4, 0xb7, 0xb9, 0xbb, 0xc0 };
	unsigned int raw;
	int i, ret, n = 0;

	for (i = 0; i < ARRAY_SIZE(fg); i++) {
		ret = regmap_read(d->fg, fg[i], &raw);
		if (ret)
			return ret;
		n += sysfs_emit_at(buf, n, "FG %02x %04x\n", fg[i], raw);
	}
	for (i = 0; i < ARRAY_SIZE(chg); i++) {
		ret = regmap_read(d->max->regmap_chg, chg[i], &raw);
		if (ret)
			return ret;
		n += sysfs_emit_at(buf, n, "CHG %02x %02x\n", chg[i], raw);
	}
	return n;
}

static DEVICE_ATTR_RO(raw_snapshot);
static struct attribute *s6_power_attrs[] = {
	&dev_attr_temperature_source.attr,
	&dev_attr_raw_snapshot.attr,
	NULL,
};

static const struct attribute_group s6_power_group = { .attrs =
							       s6_power_attrs };

static void s6_power_poll(struct work_struct *work)
{
	struct s6_power *d =
		container_of(to_delayed_work(work), struct s6_power, poll);
	power_supply_changed(d->battery);
	power_supply_changed(d->usb);
	schedule_delayed_work(&d->poll, 15 * HZ);
}

static void s6_power_cancel(void *data)
{
	struct s6_power *d = data;

	cancel_delayed_work_sync(&d->poll);
}

static int s6_power_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct s6_power *d;
	struct i2c_client *client;
	struct power_supply_config config = {};
	unsigned int raw, rsense;
	int ret;

	if (!of_machine_is_compatible("samsung,exynos7420-zerolte"))
		return -ENODEV;
	d = devm_kzalloc(dev, sizeof(*d), GFP_KERNEL);
	if (!d)
		return -ENOMEM;
	d->battery_ntc = devm_iio_channel_get(dev, "battery-temp");
	if (IS_ERR(d->battery_ntc))
		return dev_err_probe(dev, PTR_ERR(d->battery_ntc),
				     "battery NTC ADC\n");
	d->charger_ntc = devm_iio_channel_get(dev, "charger-temp");
	if (IS_ERR(d->charger_ntc))
		return dev_err_probe(dev, PTR_ERR(d->charger_ntc),
				     "charger NTC ADC\n");
	d->max = dev_get_drvdata(dev->parent);
	if (!of_property_read_bool(d->max->dev->of_node,
				   "samsung,zerolte-power-readonly"))
		return -ENODEV;
	ret = of_property_read_u32(d->max->dev->of_node,
				   "maxim,rsense-micro-ohms", &rsense);
	if (ret || rsense != 10000)
		return -EINVAL;
	client = devm_i2c_new_dummy_device(dev, d->max->i2c->adapter,
					   I2C_ADDR_FG);
	if (IS_ERR(client))
		return dev_err_probe(dev, PTR_ERR(client), "FG client\n");
	d->fg = devm_regmap_init_i2c(client, &s6_fg_map);
	if (IS_ERR(d->fg))
		return dev_err_probe(dev, PTR_ERR(d->fg), "FG regmap\n");
	ret = regmap_read(d->fg, 0x1d, &raw);
	if (ret)
		return dev_err_probe(dev, ret, "FG CONFIG read\n");
	d->external_temp = !!(raw & BIT(8));
	d->temp_enabled = !!(raw & BIT(9));
	dev_dbg(dev, "read-only CONFIG=%04x Tex=%d Ten=%d Rsense=%u\n", raw,
		d->external_temp, d->temp_enabled, rsense);
	platform_set_drvdata(pdev, d);
	config.drv_data = d;
	d->battery_desc = (struct power_supply_desc){
		.name = "s6-battery",
		.type = POWER_SUPPLY_TYPE_BATTERY,
		.properties = s6_battery_props,
		.num_properties = ARRAY_SIZE(s6_battery_props),
		.get_property = s6_battery_get,
		.no_thermal = true,
	};

	d->battery = devm_power_supply_register(dev, &d->battery_desc, &config);
	if (IS_ERR(d->battery))
		return dev_err_probe(dev, PTR_ERR(d->battery),
				     "battery supply\n");
	d->usb = devm_power_supply_register(dev, &s6_usb_desc, &config);
	if (IS_ERR(d->usb))
		return dev_err_probe(dev, PTR_ERR(d->usb), "USB supply\n");
	ret = devm_device_add_group(dev, &s6_power_group);
	if (ret)
		return ret;
	INIT_DELAYED_WORK(&d->poll, s6_power_poll);
	ret = devm_add_action_or_reset(dev, s6_power_cancel, d);
	if (ret)
		return ret;
	schedule_delayed_work(&d->poll, 15 * HZ);
	dev_dbg(dev,
		"battery/USB telemetry ready; no FG/charger/IRQ register writes\n");
	return 0;
}

static const struct platform_device_id s6_power_ids[] = {
	{ "max77843-fuelgauge", 0 },
	{},
};

MODULE_DEVICE_TABLE(platform, s6_power_ids);
static struct platform_driver s6_power_driver = {
	.driver = { .name = "s6-max77843-readonly" },
	.probe = s6_power_probe,
	.id_table = s6_power_ids,
};

module_platform_driver(s6_power_driver);
MODULE_DESCRIPTION("SM-G925I MAX77843 read-only telemetry");
MODULE_LICENSE("GPL");
