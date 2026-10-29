// SPDX-License-Identifier: GPL-2.0-only
/*
 * Zerolte APOLLO clocks. Register layout and dividers from Samsung7420;
 * standard Samsung PLL + bounded7420 CPU clock transition callbacks.
 */
#include <linux/clk-provider.h>
#include <linux/io.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include "clk.h"
#include "clk-cpu.h"
PNAME(s6_pll_parents) = { "fin_pll", "s6_apollo_pll" };
PNAME(s6_alt_parents) = { "fin_pll", "s6_bus_apollo" };
static const struct samsung_pll_rate_table s6_rates[] __initconst = {
	PLL_35XX_RATE(24000000, 1500000000, 250, 4, 0),
	PLL_35XX_RATE(24000000, 1000000000, 125, 3, 0),
	PLL_35XX_RATE(24000000, 500000000, 125, 3, 1),
	PLL_35XX_RATE(24000000, 400000000, 200, 3, 2),
	PLL_35XX_RATE(24000000, 200000000, 200, 3, 3),
	{}
};

static const struct samsung_pll_clock s6_plls[] __initconst = { PLL(
	pll_1451x, 1, "s6_apollo_pll", "fin_pll", 0x0000, 0x0100, s6_rates) };
static const struct samsung_mux_clock s6_muxes[] __initconst = {
	MUX_F(2, "s6_apollo_pll_mux", s6_pll_parents, 0x200, 0, 1,
	      CLK_SET_RATE_PARENT | CLK_RECALC_NEW_RATES, 0),
	MUX(3, "s6_apollo_alt", s6_alt_parents, 0x204, 0, 1)
};

static const struct exynos_cpuclk_cfg_data s6_cfg[] __initconst = {
	{ 1500000, 0x05555200, 0x31 }, { 1000000, 0x05555200, 0x31 },
	{ 500000, 0x04444100, 0x21 },  { 400000, 0x03333100, 0x21 },
	{ 200000, 0x03333100, 0x11 },  {}
};

static const struct samsung_cpu_clock s6_cpus[] __initconst = { CPU_CLK(
	4, "s6_apolloclk", 2, 3, CLK_CPU_HAS_DIV1, 0, CPUCLK_LAYOUT_E7420,
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

static void __init s6_apollo_init(struct device_node *np)
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
	    rate < 200000000 || rate > 1500000000) {
		pr_err("unexpected APOLLO boot state; refusing CPU clock registration\n");
		return;
	}
	samsung_cmu_register_one(np, &s6_cmu);
	pr_info("APOLLO boot=%lluHz; supported 200/400/500/1000/1500 MHz\n",
		rate);
}

CLK_OF_DECLARE(s6_apollo, "samsung,zerolte-apollo-clock", s6_apollo_init);
