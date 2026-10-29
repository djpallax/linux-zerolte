// SPDX-License-Identifier: GPL-2.0-only
/*
 * Zerolte PCIe1 bring-up. Register sequence from Samsung Exynos7420
 * pci-exynos.c/pci-exynos7420_cal.c, Copyright2013-2014 Samsung Electronics.
 * Only the WLAN root complex, no modem controller or automatic retry.
 */
#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/interrupt.h>
#include <linux/iopoll.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/pci.h>
#include <linux/platform_device.h>
#include "pcie-designware.h"
struct s6_pcie {
	raw_spinlock_t dbi_lock;
	struct dw_pcie pci;
	void __iomem *phy, *block, *pmu, *cmu, *pcs;
	bool phy_started;
	struct gpio_desc *enable, *perst;
	struct clk_bulk_data clocks[12];
};

static struct s6_pcie *s6_ep(struct dw_pcie *pci)
{
	return dev_get_drvdata(pci->dev);
}

static bool s6_link(struct dw_pcie *pci)
{
	u32 s = readl(pci->elbi_base + 0x74) & 0x1f;

	return s >= 0xd && s <= 0x15;
}

static int s6_start(struct dw_pcie *pci)
{
	writel(1, pci->elbi_base + 0x2c);
	return 0;
}

static void s6_stop(struct dw_pcie *pci)
{
	writel(0, pci->elbi_base + 0x2c);
}

static irqreturn_t s6_irq(int irq, void *arg)
{
	struct s6_pcie *d = arg;
	void __iomem *b = d->pci.elbi_base;
	u32 level = readl(b + 4), pulse = readl(b), special = readl(b + 8);

	if (!(level | pulse | special))
		return IRQ_NONE;
	if (level & BIT(1))
		dw_handle_msi_irq(&d->pci.pp);
	writel(pulse, b);
	writel(level, b + 4);
	writel(special, b + 8);
	return IRQ_HANDLED;
}

static int s6_host_init(struct dw_pcie_rp *pp)
{
	struct dw_pcie *pci = to_dw_pcie_from_pp(pp);
	struct s6_pcie *d = s6_ep(pci);
	void __iomem *b = pci->elbi_base;
	int i, r;
	static const u32 cmn[] = { 1,	 0xf,  0xa6, 0x71, 0x90, 0x62, 0x20,
				   0,	 0,    0xa7, 0xa,  0x37, 0x20, 8,
				   0xef, 0xfc, 0x96, 0x14, 0,	 0x10, 0x68,
				   1,	 0,    0,    4,	   0x20 };
	static const u32 trsv[] = { 0x31, 0xf4, 0xf4, 0x80, 0x25, 0x40, 0xc0,
				    3,	  0x35, 0x55, 0x4c, 0xc3, 0x10, 0x54,
				    0x70, 0xc5, 0,    0x2f, 0x44, 0xa4, 0,
				    0x3b, 0x34, 0xa7, 0x64, 0,	  0x1f, 0x83,
				    0x1b, 1,	0xe0, 0,    0,	  2,	0,
				    0,	  0,	0,    0,    0x1d, 0 };
	d->perst = pci->pe_rst;
	if (!d->perst)
		return -EINVAL;
	if (clk_get_rate(d->clocks[8].clk) != 24000000)
		return dev_err_probe(
			pci->dev, -EINVAL,
			"WLAN PHY requires documented24MHz reference\n");
	r = clk_bulk_prepare_enable(10, d->clocks);
	if (r)
		return r;
	gpiod_set_value_cansleep(d->enable, 1);
	msleep(150);
	/* Match Samsung: release endpoint PERST before setting root-complex PHY. */
	gpiod_set_value_cansleep(d->perst, 0);
	usleep_range(18000, 20000);
	writel(1, d->pmu);
	writel(readl(d->phy + 0x15 * 4) & ~(0xf << 3), d->phy + 0x15 * 4);
	writel(0, d->phy + 0x4e * 4);
	writel(0, d->phy + 0x4f * 4);
	writel(1, b + 0xf4);
	writel(1, b + 0xf0);
	writel(readl(d->cmu) | 1, d->cmu);
	writel(readl(d->block) & ~(3 << 4), d->block);
	writel((readl(d->block) & ~(3 << 2)) | (1 << 2), d->block);
	writel(1, b + 0x130);
	udelay(10);
	writel(0, b + 0x130);
	udelay(10);
	writel(1, b + 0x134);
	udelay(10);
	writel(0, b + 0x134);
	udelay(10);
	for (i = 0; i < ARRAY_SIZE(cmn); i++)
		writel(cmn[i], d->phy + i * 4);
	for (i = 0; i < ARRAY_SIZE(trsv); i++)
		writel(trsv[i], d->phy + (0x30 + i) * 4);
	writel(2, d->pcs);
	writel(readl(d->block) & ~BIT(7), d->block);
	writel(readl(d->block) | BIT(7), d->block);
	udelay(20);
	writel(readl(d->block) & ~BIT(7), d->block);
	r = clk_bulk_prepare_enable(2, &d->clocks[10]);
	if (r)
		return r;
	writel(readl(b + 0x18) & ~BIT(1), b + 0x18);
	writel(1, b + 0x38);
	writel(readl(b), b);
	writel(readl(b + 4), b + 4);
	writel(readl(b + 8), b + 8);
	r = devm_request_irq(pci->dev, pp->irq, s6_irq, IRQF_SHARED,
			     "s6-wlan-pcie", d);
	if (r)
		return r;
	writel(0x55, b + 0xc);
	writel(BIT(1), b + 0x10);
	writel(0, b + 0x14);
	d->phy_started = true;
	dev_dbg(pci->dev,
		"Samsung7420 PHY sequence complete; bounded link training follows\n");
	return 0;
}
/*
 * Samsung7420 DBI access requires temporarily leaving automatic L1 mode.
 * Serialize the sideband request with all configuration/MSI register access.
 */
static u32 s6_read_dbi(struct dw_pcie *pci, void __iomem *base, u32 reg,
		       size_t size)
{
	struct s6_pcie *d = s6_ep(pci);
	unsigned long flags;
	u32 value = ~0U;

	raw_spin_lock_irqsave(&d->dbi_lock, flags);
	writel(1, pci->elbi_base + 0x40);
	writel(0, pci->elbi_base + 0xf4);
	dw_pcie_read(base + reg, size, &value);
	writel(1, pci->elbi_base + 0xf4);
	writel(0, pci->elbi_base + 0x40);
	raw_spin_unlock_irqrestore(&d->dbi_lock, flags);
	return value;
}

static void s6_write_dbi(struct dw_pcie *pci, void __iomem *base, u32 reg,
			 size_t size, u32 value)
{
	struct s6_pcie *d = s6_ep(pci);
	unsigned long flags;

	raw_spin_lock_irqsave(&d->dbi_lock, flags);
	writel(1, pci->elbi_base + 0x40);
	writel(0, pci->elbi_base + 0xf4);
	dw_pcie_write(base + reg, size, value);
	writel(1, pci->elbi_base + 0xf4);
	writel(0, pci->elbi_base + 0x40);
	raw_spin_unlock_irqrestore(&d->dbi_lock, flags);
}

static const struct dw_pcie_ops s6_ops = { .read_dbi = s6_read_dbi,
					   .write_dbi = s6_write_dbi,
					   .link_up = s6_link,
					   .start_link = s6_start,
					   .stop_link = s6_stop };
static const struct dw_pcie_host_ops s6_host_ops = { .init = s6_host_init };
static void __iomem *s6_map(struct platform_device *pdev, const char *name)
{
	struct resource *res =
		platform_get_resource_byname(pdev, IORESOURCE_MEM, name);
	if (!res)
		return ERR_PTR(-EINVAL);
	/* Tiny SYSREG/PMU/CMU views coexist with their system providers. */
	return devm_ioremap(&pdev->dev, res->start, resource_size(res)) ?:
		       ERR_PTR(-ENOMEM);
}

static int s6_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct s6_pcie *d;
	struct dw_pcie *pci;
	int i, r, irq;
	static const char *const names[] = { "bus",	"ahb",	    "master",
					     "slave",	"dbi-gate", "pcs-gate",
					     "async",	"link",	    "ref24",
					     "refgate", "tx",	    "rx" };
	if (!of_machine_is_compatible("samsung,exynos7420-zerolte"))
		return -ENODEV;
	d = devm_kzalloc(dev, sizeof(*d), GFP_KERNEL);
	if (!d)
		return -ENOMEM;
	pci = &d->pci;
	pci->dev = dev;
	pci->ops = &s6_ops;
	dw_pcie_cap_set(pci, REQ_RES);
	platform_set_drvdata(pdev, d);
	raw_spin_lock_init(&d->dbi_lock);
	pci->elbi_base = s6_map(pdev, "elbi");
	if (IS_ERR(pci->elbi_base))
		return PTR_ERR(pci->elbi_base);
	pci->dbi_base = s6_map(pdev, "dbi");
	if (IS_ERR(pci->dbi_base))
		return PTR_ERR(pci->dbi_base);
	d->phy = s6_map(pdev, "phy");
	if (IS_ERR(d->phy))
		return PTR_ERR(d->phy);
	d->block = s6_map(pdev, "block");
	if (IS_ERR(d->block))
		return PTR_ERR(d->block);
	d->pmu = s6_map(pdev, "pmu");
	if (IS_ERR(d->pmu))
		return PTR_ERR(d->pmu);
	d->cmu = s6_map(pdev, "cmu");
	if (IS_ERR(d->cmu))
		return PTR_ERR(d->cmu);
	d->pcs = s6_map(pdev, "pcs");
	if (IS_ERR(d->pcs))
		return PTR_ERR(d->pcs);
	for (i = 0; i < 12; i++)
		d->clocks[i].id = names[i];
	r = devm_clk_bulk_get(dev, 12, d->clocks);
	if (r)
		return dev_err_probe(dev, r, "WLAN clocks\n");
	d->enable = devm_gpiod_get(dev, "enable", GPIOD_OUT_LOW);
	if (IS_ERR(d->enable))
		return dev_err_probe(dev, PTR_ERR(d->enable), "WLAN enable\n");

	irq = platform_get_irq(pdev, 0);
	if (irq < 0)
		return irq;
	pci->pp.irq = irq;
	pci->pp.msi_irq[0] = -ENODEV;
	pci->pp.num_vectors = 32;
	pci->pp.ops = &s6_host_ops;
	r = dw_pcie_host_init(&pci->pp);
	if (r) {
		if (d->phy_started)
			dev_err(dev,
				"failed link state=%08x pulse=%08x level=%08x special=%08x block=%08x pmu=%08x\n",
				readl(pci->elbi_base + 0x74),
				readl(pci->elbi_base),
				readl(pci->elbi_base + 4),
				readl(pci->elbi_base + 8), readl(d->block),
				readl(d->pmu));
		if (d->perst)
			gpiod_set_value_cansleep(d->perst, 1);
		gpiod_set_value_cansleep(d->enable, 0);
		return dev_err_probe(
			dev, r, "WLAN root-complex link failed; no retry\n");
	}
	dev_dbg(dev, "PCIe1 root complex registered\n");
	return 0;
}

static const struct of_device_id s6_match[] = {
	{ .compatible = "samsung,zerolte-pcie-wlan" },
	{}
};

static struct platform_driver s6_driver = {
	.probe = s6_probe,
	.driver = { .name = "s6-wlan-pcie", .of_match_table = s6_match }
};

module_platform_driver(s6_driver);
MODULE_LICENSE("GPL");
