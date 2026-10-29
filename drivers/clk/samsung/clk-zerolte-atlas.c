// SPDX-License-Identifier: GPL-2.0-only
/*
 * Zerolte ATLAS clocks. Register layout and dividers from Samsung7420;
 * standard Samsung PLL + bounded7420 CPU clock transition callbacks.
 */
#include <linux/clk-provider.h>
#include <linux/clk.h>
#include <linux/smp.h>
#include <linux/io.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include "clk.h"
#include "clk-cpu.h"
PNAME(s6_pll_parents) = { "fin_pll", "s6_atlas_pll" };
PNAME(s6_alt_parents) = { "fin_pll", "s6_bus_atlas" };
static const struct samsung_pll_rate_table s6_rates[] __initconst = {
	PLL_35XX_RATE(24000000, 1000000000, 250, 3, 1),
	PLL_35XX_RATE(24000000, 800000000, 200, 3, 1),
	PLL_35XX_RATE(24000000, 500000000, 250, 3, 2),
	PLL_35XX_RATE(24000000, 400000000, 200, 3, 2),
	PLL_35XX_RATE(24000000, 200000000, 200, 3, 3),
	{}
};

static const struct samsung_pll_clock s6_plls[] __initconst = { PLL(
	pll_1450x, 1, "s6_atlas_pll", "fin_pll", 0x0000, 0x0100, s6_rates) };
static const struct samsung_mux_clock s6_muxes[] __initconst = {
	MUX_F(2, "s6_atlas_pll_mux", s6_pll_parents, 0x200, 0, 1,
	      CLK_SET_RATE_PARENT | CLK_RECALC_NEW_RATES, 0),
	MUX(3, "s6_atlas_alt", s6_alt_parents, 0x204, 0, 1)
};

static const struct exynos_cpuclk_cfg_data s6_cfg[] __initconst = {
	{ 1000000, 0x18606100, 0x631 }, { 800000, 0x14505100, 0x531 },
	{ 500000, 0x0c303100, 0x321 },	{ 400000, 0x0c303100, 0x321 },
	{ 200000, 0x0c303100, 0x311 },	{}
};

static const struct samsung_cpu_clock s6_cpus[] __initconst = { CPU_CLK(
	4, "s6_atlasclk", 2, 3, CLK_CPU_HAS_DIV1, 0, CPUCLK_LAYOUT_E7420,
	s6_cfg) };
static const unsigned long s6_regs[] __initconst = { 0x100, 0x104, 0x200, 0x204,
						     0x208, 0x600, 0x604 };
static const struct samsung_cmu_info s6_cmu __initconst = {
	.pll_clks = s6_plls,
	.nr_pll_clks = ARRAY_SIZE(s6_plls),
	.mux_clks = s6_muxes,
	.nr_mux_clks = ARRAY_SIZE(s6_muxes),
	.cpu_clks = s6_cpus,
	.nr_cpu_clks = ARRAY_SIZE(s6_cpus),
	.nr_clk_ids = 5,
	.clk_regs = s6_regs,
	.nr_clk_regs = ARRAY_SIZE(s6_regs)
};

static void __init s6_atlas_init(struct device_node *np)
{
	void __iomem *b;
	u32 pll, div, mux, p, m, s;
	u64 rate;

	if (!of_machine_is_compatible("samsung,exynos7420-zerolte"))
		return;
	b = of_iomap(np, 0);
	if (!b)
		return;
	pll = readl(b + 0x100);
	div = readl(b + 0x600);
	mux = readl(b + 0x208);
	p = (pll >> 8) & 0x3f;
	m = (pll >> 16) & 0x3ff;
	s = pll & 7;
	rate = p ? 24000000ULL * m / (p << s) : 0;
	iounmap(b);
	if (!(pll & BIT(31)) || !(pll & BIT(29)) || (div & 0x77) || (mux & 1) ||
	    rate < 200000000 || rate > 2500000000) {
		pr_err("unexpected ATLAS boot state; refusing CPU clock registration\n");
		return;
	}
	samsung_cmu_register_one(np, &s6_cmu);
	pr_info("ATLAS boot=%lluHz; supported 200/400/500/800/1000 MHz\n",
		rate);
}

CLK_OF_DECLARE(s6_atlas, "samsung,zerolte-atlas-clock", s6_atlas_init);

/*
 * early_initcall precedes smp_init, while every provider from of_clk_init exists.
 * Samsung500MHz is below the inherited stock operating point; no voltage reduction.
 * If the clamp cannot be verified, retain the single-core rescue boot.
 */
static int __init s6_early_cluster_clamp(void)
{
	static const char *const names[] = { "samsung,zerolte-apollo-clock",
					     "samsung,zerolte-atlas-clock" };
	struct of_phandle_args spec = { .args_count = 1, .args = { 4 } };
	struct clk *clk;
	int i, r;

	if (!of_machine_is_compatible("samsung,exynos7420-zerolte"))
		return 0;
	for (i = 0; i < 2; i++) {
		spec.np = of_find_compatible_node(NULL, NULL, names[i]);
		if (!spec.np) {
			r = -ENODEV;
			goto fail;
		}
		clk = of_clk_get_from_provider(&spec);
		of_node_put(spec.np);
		if (IS_ERR(clk)) {
			r = PTR_ERR(clk);
			goto fail;
		}
		r = clk_set_rate(clk, 500000000);
		if (!r && clk_get_rate(clk) != 500000000)
			r = -EIO;
		clk_put(clk);
		if (r)
			goto fail;
	}
	pr_info("pre-SMP APOLLO/ATLAS500MHz verified; normal capability discovery on all CPUs\n");
	return 0;
fail:
	setup_max_cpus = 1;
	pr_err("pre-SMP clock clamp failed %d; forced single-core rescue\n", r);
	return r;
}
early_initcall(s6_early_cluster_clamp);
