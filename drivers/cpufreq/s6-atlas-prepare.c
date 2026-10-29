// SPDX-License-Identifier: GPL-2.0-only
/* Prepare the dormant ATLAS cluster before the first PSCI CPU_ON. */
#include <linux/clk.h>
#include <linux/cpu.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/regulator/consumer.h>
struct s6_atlas {
	struct clk *clk;
	struct regulator *supply;
	void __iomem *base;
	bool ready;
};

static ssize_t ready_show(struct device *dev, struct device_attribute *a,
			  char *buf)
{
	struct s6_atlas *d = dev_get_drvdata(dev);
	int cpu;
	bool active = false;

	for (cpu = 1; cpu <= 4; cpu++)
		if (cpu_online(cpu))
			active = true;
	if (!active)
		return sysfs_emit(buf, "ready=%u cluster_offline=1\n",
				  d->ready);
	return sysfs_emit(
		buf,
		"ready=%u rate=%lu microvolts=%d pll=%08x mux=%08x stat=%08x div0=%08x div1=%08x\n",
		d->ready, clk_get_rate(d->clk),
		regulator_get_voltage(d->supply), readl(d->base + 0x100),
		readl(d->base + 0x208), readl(d->base + 0x408),
		readl(d->base + 0x600), readl(d->base + 0x604));
}

static DEVICE_ATTR_RO(ready);
static int s6_atlas_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct s6_atlas *d;
	int ret;

	if (!of_machine_is_compatible("samsung,exynos7420-zerolte"))
		return -ENODEV;

	d = devm_kzalloc(dev, sizeof(*d), GFP_KERNEL);
	if (!d)
		return -ENOMEM;
	d->clk = devm_clk_get(dev, NULL);
	if (IS_ERR(d->clk))
		return dev_err_probe(dev, PTR_ERR(d->clk), "ATLAS clock\n");
	d->supply = devm_regulator_get(dev, "cpu");
	if (IS_ERR(d->supply))
		return dev_err_probe(dev, PTR_ERR(d->supply), "ATLAS supply\n");
	d->base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(d->base))
		return PTR_ERR(d->base);
	/*
	 * Early clamp already ran before SMP; this driver only verifies and
	 * adds a supply range constraint after the real regulator becomes available.
	 */
	if (clk_get_rate(d->clk) != 500000000)
		return -EIO;
	ret = regulator_set_voltage(d->supply, 1000000, 1050000);
	if (ret)
		return ret;
	if (regulator_get_voltage(d->supply) < 1000000)
		return -EIO;
	d->ready = true;
	platform_set_drvdata(pdev, d);
	ret = device_create_file(dev, &dev_attr_ready);
	if (ret)
		return ret;
	dev_dbg(dev,
		"dormant cluster ready at500MHz/1.00V; no CPUs automatically enabled\n");
	return 0;
}

static const struct of_device_id s6_atlas_match[] = {
	{ .compatible = "samsung,zerolte-atlas-prepare" },
	{}
};

static struct platform_driver s6_atlas_driver = {
	.probe = s6_atlas_probe,
	.driver = { .name = "s6-atlas-prepare",
		    .of_match_table = s6_atlas_match }
};

module_platform_driver(s6_atlas_driver);
MODULE_LICENSE("GPL");
