// SPDX-License-Identifier: GPL-2.0-only
/*
 * S2MPB02 LDO17 (TSP AVDD), Samsung7420 table: 0x2e,600mV+25mV*n.
 * Enable-only pilot: require inherited selector108=3.3V; never change it,
 * never touch another output, interrupts or NVM.
 */
#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/regmap.h>
#include <linux/regulator/driver.h>
static bool s6_avdd_readable(struct device *dev, unsigned int reg)
{
	return reg == 0 || reg == 0x2e;
}

static bool s6_avdd_writeable(struct device *dev, unsigned int reg)
{
	return reg == 0x2e;
}

static const struct regmap_config s6_avdd_map = {
	.reg_bits = 8,
	.val_bits = 8,
	.max_register = 0x2e,
	.readable_reg = s6_avdd_readable,
	.writeable_reg = s6_avdd_writeable
};

static const struct regulator_ops s6_avdd_ops = {
	.enable = regulator_enable_regmap,
	.is_enabled = regulator_is_enabled_regmap
};

static const struct regulator_desc s6_avdd_desc = { .name = "s6-tsp-avdd",
						    .of_match = "LDO17",
						    .regulators_node =
							    "regulators",
						    .type = REGULATOR_VOLTAGE,
						    .owner = THIS_MODULE,
						    .ops = &s6_avdd_ops,
						    .n_voltages = 1,
						    .fixed_uV = 3300000,
						    .enable_reg = 0x2e,
						    .enable_mask = 0x80,
						    .enable_time = 180 };
static int s6_avdd_probe(struct i2c_client *client)
{
	struct regmap *map;
	struct regulator_config cfg = {};
	struct regulator_dev *rdev;
	unsigned int id, value;
	int r;

	if (!of_machine_is_compatible("samsung,exynos7420-zerolte") ||
	    client->addr != 0x59)
		return -ENODEV;
	map = devm_regmap_init_i2c(client, &s6_avdd_map);
	if (IS_ERR(map))
		return PTR_ERR(map);
	r = regmap_read(map, 0, &id);
	if (r)
		return r;
	r = regmap_read(map, 0x2e, &value);
	if (r)
		return r;
	if (id == 0xff || (value & 0x7f) != 108)
		return dev_err_probe(
			&client->dev, -EINVAL,
			"Unexpected S2MPB02/LDO17 state; no write\n");
	dev_dbg(&client->dev,
		"S2MPB02 id=%02x LDO17=%02x, fixed3.3V, enable-bit-only\n", id,
		value);
	cfg.dev = &client->dev;
	cfg.regmap = map;
	rdev = devm_regulator_register(&client->dev, &s6_avdd_desc, &cfg);
	return PTR_ERR_OR_ZERO(rdev);
}

static const struct of_device_id s6_avdd_match[] = {
	{ .compatible = "samsung,zerolte-tsp-avdd" },
	{}
};

static struct i2c_driver s6_avdd_driver = {
	.driver = { .name = "s6-tsp-avdd", .of_match_table = s6_avdd_match },
	.probe = s6_avdd_probe
};

module_i2c_driver(s6_avdd_driver);
MODULE_LICENSE("GPL");
