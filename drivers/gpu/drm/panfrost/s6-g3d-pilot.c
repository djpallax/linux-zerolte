// SPDX-License-Identifier: GPL-2.0-only
/*
 * Zerolte GPU pilot: measured Samsung 266MHz/693750uV operating point.
 * BUCK6 and LDO10 share GPB0-4. Keep their inherited external-control mode
 * and DAC value intact; a real regulator GPIO holds both rails on for the pilot.
 * Only the G3D clock's sleepable prepare operation may turn on PD_G3D.
 * No suspend, runtime domain cycling or GPU DVFS is implemented here.
 */
#include <linux/clk.h>
#include <linux/clk-provider.h>
#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/i2c.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/mfd/samsung/core.h>
#include <linux/mfd/samsung/s2mps15.h>
#include <linux/module.h>
#include <linux/math64.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/regulator/driver.h>
#include <linux/regulator/of_regulator.h>

#define G3D_RATE 266000000UL
#define G3D_UV 693750
#define G3D_SYNC_REG 0x52 /* Samsung S2MPS15_REG_LDO_RSVD3 */
struct s6_g3d {
	struct device *dev;
	void __iomem *cmu, *pmu, *topc, *ccore;
	struct regmap *pmic;
	struct gpio_desc *enable;
	struct clk_hw hw;
	struct regulator_desc supply_desc;
	struct mutex lock;
	bool ready;
	int last_error, ocp_steps;
};

#define to_g3d(h) container_of(h, struct s6_g3d, hw)
static u32 s6_update(void __iomem *p, u32 mask, u32 value)
{
	u32 v = readl(p);

	writel((v & ~mask) | (value & mask), p);
	return v;
}

static int s6_voltage(struct s6_g3d *d)
{
	unsigned int v;
	int ret = regmap_read(d->pmic, S2MPS15_REG_B6CTRL2, &v);

	if (ret)
		return ret;
	if (v < 0x20 || v > 0xc0)
		return -ERANGE;
	return 500000 + ((int)v - 0x20 - d->ocp_steps) * 6250;
}

static unsigned long s6_actual_rate(struct s6_g3d *d, unsigned long parent)
{
	u32 v, p, m, s, mux;
	/* CMU_G3D is inaccessible while the domain is off. */
	if ((readl(d->pmu + 0x4104) & 0xf) != 0xf)
		return 0;
	if ((readl(d->pmu + 0x6100) & BIT(0)) &&
	    !(readl(d->pmu + 0x6104) & BIT(0)))
		return 0;
	v = readl(d->cmu + 0x100);
	mux = readl(d->cmu + 0x200);
	if ((v & (BIT(31) | BIT(29))) != (BIT(31) | BIT(29)) ||
	    !(mux & BIT(0)) || (mux & BIT(8)) ||
	    !(readl(d->cmu + 0x800) & BIT(0)))
		return 0;
	p = (v >> 8) & 0x3f;
	m = (v >> 16) & 0x3ff;
	s = v & 7;
	return p ? div_u64((u64)parent * m, p << s) : 0;
}

static unsigned long s6_rate(struct clk_hw *hw, unsigned long parent)
{
	struct s6_g3d *d = to_g3d(hw);

	return d->ready ? s6_actual_rate(d, parent) : 0;
}

static int s6_determine(struct clk_hw *hw, struct clk_rate_request *req)
{
	if (req->min_rate > G3D_RATE || req->max_rate < G3D_RATE)
		return -EINVAL;
	req->rate = G3D_RATE;
	req->best_parent_rate = 24000000;
	return 0;
}

static int s6_set_rate(struct clk_hw *hw, unsigned long rate,
		       unsigned long parent)
{
	/* A fixed pilot operating point, backed by actual PLL readback. */
	if (rate != G3D_RATE || parent != 24000000)
		return -EINVAL;
	return to_g3d(hw)->ready && s6_rate(hw, parent) == rate ? 0 : -EAGAIN;
}

static int s6_prepare(struct clk_hw *hw)
{
	struct s6_g3d *d = to_g3d(hw);
	u32 v;
	int ret = 0, i;
	const u32 regs[] = { 0x1460, 0x14a0, 0x14e0, 0x1520, 0x1560, 0x15a0 };

	mutex_lock(&d->lock);
	if (d->ready) {
		ret = s6_rate(hw, 24000000) == G3D_RATE ? 0 : -EIO;
		goto out;
	}
	if (s6_voltage(d) != G3D_UV ||
	    gpiod_get_value_cansleep(d->enable) != 1) {
		ret = -EIO;
		goto out;
	}
	dev_dbg(d->dev, "rails verified; begin PD_G3D power-on\n");
	/* Samsung top-to-local and CCORE asynchronous-bridge gates. */
	s6_update(d->topc + 0xa04, BIT(9), BIT(9));
	s6_update(d->ccore + 0x800, BIT(22) | BIT(23), BIT(22) | BIT(23));
	if ((readl(d->pmu + 0x4104) & 0xf) == 0) {
		for (i = 0; i < ARRAY_SIZE(regs); i++)
			s6_update(d->pmu + regs[i], i == 4 ? 3 : 1, 0);
		s6_update(d->pmu + 0x4108, 0xffff, 0x1101);
		s6_update(d->pmu + 0x4100, 0xf, 0xf);
		ret = readl_poll_timeout(d->pmu + 0x4104, v, (v & 0xf) == 0xf,
					 20, 100000);
		if (ret) {
			dev_err(d->dev,
				"domain timeout status=%08x; no CMU/GPU access\n",
				v);
			goto out;
		}
	} else if ((readl(d->pmu + 0x4104) & 0xf) != 0xf) {
		ret = -EIO;
		goto out;
	} else
		dev_dbg(d->dev,
			"keep inherited PD_G3D ON; no power-domain writes\n");
	dev_dbg(d->dev,
		"domain ON; CMU pll=%08x con1=%08x con2=%08x mux=%08x\n",
		readl(d->cmu + 0x100), readl(d->cmu + 0x104),
		readl(d->cmu + 0x108), readl(d->cmu + 0x200));
	if (s6_actual_rate(d, 24000000) == G3D_RATE) {
		d->ready = true;
		dev_dbg(d->dev,
			"inherited real266MHz PLL/gate verified; no PLL writes required\n");
		goto out;
	}
	/* Switch the GPU mux to the 24MHz reference before changing PLL1452x. */
	s6_update(d->cmu + 0x300, BIT(0) | BIT(8), BIT(0) | BIT(8));
	s6_update(d->cmu + 0x200, BIT(8), BIT(8));
	ret = readl_poll_timeout(d->cmu + 0x400, v, ((v >> 8) & 7) == 2, 10,
				 10000);
	if (ret)
		goto out;
	writel(6 * 270, d->cmu + 0x000);
	s6_update(d->cmu + 0x100,
		  GENMASK(25, 16) | GENMASK(13, 8) | GENMASK(2, 0) | BIT(31),
		  (133 << 16) | (6 << 8) | 1 | BIT(31));
	ret = readl_poll_timeout(d->cmu + 0x100, v, v & BIT(29), 10, 10000);
	if (ret) {
		dev_err(d->dev,
			"PLL lock timeout; GPU mux left on reference\n");
		goto out;
	}
	s6_update(d->cmu + 0x200, BIT(0), BIT(0));
	ret = readl_poll_timeout(d->cmu + 0x400, v, (v & 7) == 2, 10, 10000);
	if (ret)
		goto out;
	s6_update(d->cmu + 0x200, BIT(8), 0);
	ret = readl_poll_timeout(d->cmu + 0x400, v, ((v >> 8) & 7) == 1, 10,
				 10000);
	if (ret)
		goto out;
	/* HPM uses the OEM divide-by-two setting; GPU itself is not divided. */
	s6_update(d->cmu + 0x600, GENMASK(9, 8), 1 << 8);
	ret = readl_poll_timeout(d->cmu + 0x700, v, !(v & BIT(8)), 10, 10000);
	if (ret)
		goto out;
	s6_update(d->cmu + 0x800, BIT(0) | BIT(1), BIT(0) | BIT(1));
	d->ready = true;
	if (s6_rate(hw, 24000000) != G3D_RATE) {
		d->ready = false;
		ret = -EIO;
		goto out;
	}
	dev_dbg(d->dev,
		"real PLL/gate readback 266MHz, supply693750uV; Panfrost may probe\n");
out:
	d->last_error = ret;
	mutex_unlock(&d->lock);
	return ret;
}

static void s6_unprepare(struct clk_hw *hw)
{
	/* Hold the verified domain/rails on for this pilot. No untested power-off. */
}

static const struct clk_ops s6_clock_ops = { .prepare = s6_prepare,
					     .unprepare = s6_unprepare,
					     .recalc_rate = s6_rate,
					     .determine_rate = s6_determine,
					     .set_rate = s6_set_rate };
static int s6_supply_voltage(struct regulator_dev *rdev)
{
	return s6_voltage(rdev_get_drvdata(rdev));
}

static const struct regulator_ops s6_supply_ops = {
	.list_voltage = regulator_list_voltage_linear,
	.get_voltage = s6_supply_voltage,
};

static ssize_t state_show(struct device *dev, struct device_attribute *attr,
			  char *buf)
{
	struct s6_g3d *d = dev_get_drvdata(dev);
	u32 pd = readl(d->pmu + 0x4104);

	if ((pd & 0xf) != 0xf)
		return sysfs_emit(
			buf,
			"ready=%u error=%d pd=%08x voltage=%d domain_off_no_cmu_access\n",
			d->ready, d->last_error, pd, s6_voltage(d));
	return sysfs_emit(
		buf,
		"ready=%u error=%d pd=%08x voltage=%d pll=%08x con1=%08x con2=%08x mux=%08x stat=%08x gate=%08x rate=%lu\n",
		d->ready, d->last_error, pd, s6_voltage(d),
		readl(d->cmu + 0x100), readl(d->cmu + 0x104),
		readl(d->cmu + 0x108), readl(d->cmu + 0x200),
		readl(d->cmu + 0x400), readl(d->cmu + 0x800),
		s6_rate(&d->hw, 24000000));
}

static DEVICE_ATTR_RO(state);
static void s6_put_client(void *client)
{
	put_device(&((struct i2c_client *)client)->dev);
}

static int s6_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct s6_g3d *d;
	struct device_node *np;
	struct i2c_client *client;
	struct sec_pmic_dev *pmic;
	struct regulator_config cfg = {};
	struct regulator_dev *rdev;
	struct clk *ref;
	unsigned int ctrl, sync, ldo;
	int ret;
	const struct clk_parent_data parent = { .fw_name = "ref" };
	struct clk_init_data init = { .name = "s6_g3dclk",
				      .ops = &s6_clock_ops,
				      .flags = CLK_GET_RATE_NOCACHE |
					       CLK_IGNORE_UNUSED,
				      .parent_data = &parent,
				      .num_parents = 1 };
	if (!of_machine_is_compatible("samsung,exynos7420-zerolte"))
		return -ENODEV;
	d = devm_kzalloc(dev, sizeof(*d), GFP_KERNEL);
	if (!d)
		return -ENOMEM;
	d->dev = dev;
	mutex_init(&d->lock);
	ref = devm_clk_get(dev, "ref");
	if (IS_ERR(ref))
		return dev_err_probe(dev, PTR_ERR(ref), "reference clock\n");
	if (clk_get_rate(ref) != 24000000)
		return dev_err_probe(dev, -EINVAL,
				     "24MHz reference required\n");
	np = of_parse_phandle(dev->of_node, "samsung,pmic", 0);
	if (!np)
		return -EINVAL;
	client = of_find_i2c_device_by_node(np);
	of_node_put(np);
	if (!client)
		return -EPROBE_DEFER;
	ret = devm_add_action_or_reset(dev, s6_put_client, client);
	if (ret)
		return ret;
	pmic = i2c_get_clientdata(client);
	if (!pmic || !pmic->regmap_pmic)
		return -EPROBE_DEFER;
	d->pmic = pmic->regmap_pmic;
	/*
	 * OEM OCP calibration metadata is outside the generic regmap (max0x52).
	 * Read this single non-interrupt byte through the existing kernel I2C client;
	 * never force a userspace claim or change the OTP/calibration configuration.
	 */
	ret = i2c_smbus_read_byte_data(client, 0x59);
	if (ret < 0)
		return ret;
	d->ocp_steps = (ret & 0x20) ? 0 : 5;
	dev_dbg(dev, "PMIC calibration59=%02x OCP correction=%d DAC steps\n",
		ret, d->ocp_steps);
	/*
	 * No PMIC writes. Only these non-interrupt registers are read.
	 * LDO10=0x54 and sync=0x4f were verified through the PMIC regmap.
	 */
	ret = regmap_read(d->pmic, S2MPS15_REG_B6CTRL1, &ctrl);
	if (ret)
		return ret;
	ret = regmap_read(d->pmic, G3D_SYNC_REG, &sync);
	if (ret)
		return ret;
	ret = regmap_read(d->pmic, S2MPS15_REG_L10CTRL, &ldo);
	if (ret)
		return ret;
	if ((ctrl & 0xc0) || !(sync & 0x40) || ldo != 0x54 ||
	    s6_voltage(d) != G3D_UV)
		return dev_err_probe(
			dev, -EINVAL,
			"unexpected BUCK6/LDO10 mode or ASV voltage: ctrl=%02x sync=%02x ldo=%02x uV=%d\n",
			ctrl, sync, ldo, s6_voltage(d));
	d->cmu = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(d->cmu))
		return PTR_ERR(d->cmu);
	d->pmu = devm_ioremap(dev, 0x105c0000, 0x6200);
	d->topc = devm_ioremap(dev, 0x10570000, 0x1000);
	d->ccore = devm_ioremap(dev, 0x105b0000, 0x1000);
	if (!d->pmu || !d->topc || !d->ccore)
		return -ENOMEM;
	ctrl = readl(d->pmu + 0x4104) & 0xf;
	if (ctrl != 0 && ctrl != 0xf)
		return dev_err_probe(
			dev, -EBUSY,
			"partial inherited G3D domain state; no GPIO changes\n");
	dev_dbg(dev, "initial PD_G3D=%x; verified GPIO high handoff\n", ctrl);
	/*
	 * Samsung requests DVS OFF before runtime domain resume. The DVS
	 * status must acknowledge normal voltage before any CMU/GPU access.
	 */
	if (readl(d->pmu + 0x6100) & BIT(0)) {
		u32 status;

		s6_update(d->pmu + 0x6100, BIT(1), 0);
		ret = readl_poll_timeout(d->pmu + 0x6104, status,
					 status & BIT(0), 100, 10000);
		if (ret)
			return dev_err_probe(
				dev, ret,
				"DVS OFF not acknowledged; no GPIO or GPU access\n");
	}
	/*
	 * Samsung direction_output preloads DAT before changing CON to output.
	 * OUT_HIGH preserves an already-enabled rail; never pulse it low.
	 */
	d->enable = gpiod_get(dev, "enable", GPIOD_OUT_HIGH);
	if (IS_ERR(d->enable))
		return dev_err_probe(dev, PTR_ERR(d->enable),
				     "BUCK6/LDO10 enable GPIO\n");
	d->supply_desc = (struct regulator_desc){ .name = "s6-g3d-buck6",
						  .owner = THIS_MODULE,
						  .type = REGULATOR_VOLTAGE,
						  .ops = &s6_supply_ops,
						  .n_voltages = 1,
						  .min_uV = G3D_UV,
						  .enable_time = 1000 };
	cfg.dev = dev;
	cfg.of_node = dev->of_node;
	cfg.driver_data = d;
	cfg.regmap = d->pmic;
	cfg.ena_gpiod = d->enable;
	cfg.init_data =
		of_get_regulator_init_data(dev, dev->of_node, &d->supply_desc);
	rdev = devm_regulator_register(dev, &d->supply_desc, &cfg);
	if (IS_ERR(rdev))
		return dev_err_probe(dev, PTR_ERR(rdev),
				     "physical G3D supply\n");
	d->hw.init = &init;
	ret = devm_clk_hw_register(dev, &d->hw);
	if (ret)
		return ret;
	ret = devm_of_clk_add_hw_provider(dev, of_clk_hw_simple_get, &d->hw);
	if (ret)
		return ret;
	platform_set_drvdata(pdev, d);
	ret = device_create_file(dev, &dev_attr_state);
	if (ret)
		return ret;
	dev_dbg(dev,
		"verified external BUCK6/LDO10 at693750uV; clock provider ready; inherited power preserved\n");
	return 0;
}

static const struct of_device_id s6_matches[] = {
	{ .compatible = "samsung,zerolte-g3d-pilot" },
	{}
};

MODULE_DEVICE_TABLE(of, s6_matches);
static struct platform_driver s6_driver = {
	.probe = s6_probe,
	.driver = { .name = "s6-g3d-pilot", .of_match_table = s6_matches }
};

module_platform_driver(s6_driver);
MODULE_LICENSE("GPL");
