// SPDX-License-Identifier: GPL-2.0

#include <linux/clk-provider.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/of.h>
#include <asm/mach-ralink/ralink_regs.h>
#include <dt-bindings/clock/mt7621-clk.h>

struct mt7621_clk_gate {
	struct clk_hw hw;
	unsigned long rate;
	int bit;
};

#define to_mt7621_gate(_hw) container_of(_hw, struct mt7621_clk_gate, hw)

#define SYSC_REG_SYSTEM_CONFIG0         0x10
#define SYSC_REG_SYSTEM_CONFIG1         0x14
#define SYSC_REG_CLKCFG0		0x2c
#define SYSC_REG_CLKCFG1		0x30
#define SYSC_REG_CUR_CLK_STS		0x44

#define MEMC_REG_CPU_PLL		0x648
#define XTAL_MODE_SEL_MASK		0x7
#define XTAL_MODE_SEL_SHIFT		6

#define CPU_CLK_SEL_MASK		0x3
#define CPU_CLK_SEL_SHIFT		30

#define CUR_CPU_FDIV_MASK		0x1f
#define CUR_CPU_FDIV_SHIFT		8
#define CUR_CPU_FFRAC_MASK		0x1f
#define CUR_CPU_FFRAC_SHIFT		0

#define CPU_PLL_PREDIV_MASK		0x3
#define CPU_PLL_PREDIV_SHIFT		12
#define CPU_PLL_FBDIV_MASK		0x7f
#define CPU_PLL_FBDIV_SHIFT		4

static unsigned long mt7621_xtal_recalc_rate(struct clk_hw *hw,
					     unsigned long parent_rate)
{
	u32 val;

	val = rt_sysc_r32(SYSC_REG_SYSTEM_CONFIG0);
	val = (val >> XTAL_MODE_SEL_SHIFT) & XTAL_MODE_SEL_MASK;

	if (val <= 2)
		return 20000000;
	else if (val <= 5)
		return 40000000;
	return 25000000;
}

static unsigned long mt7621_cpu_recalc_rate(struct clk_hw *hw,
					    unsigned long xtal_clk)
{
	const static unsigned int prediv_tbl[] = { 0, 1, 2, 2 };
	u32 clkcfg, clk_sel, curclk, ffiv, ffrac;
	u32 pll, prediv, fbdiv;
	unsigned long cpu_clk;

	clkcfg = rt_sysc_r32(SYSC_REG_CLKCFG0);
	clk_sel = (clkcfg >> CPU_CLK_SEL_SHIFT) & CPU_CLK_SEL_MASK;

	curclk = rt_sysc_r32(SYSC_REG_CUR_CLK_STS);
	ffiv = (curclk >> CUR_CPU_FDIV_SHIFT) & CUR_CPU_FDIV_MASK;
	ffrac = (curclk >> CUR_CPU_FFRAC_SHIFT) & CUR_CPU_FFRAC_MASK;

	switch (clk_sel) {
	case 0:
		cpu_clk = 500000000;
		break;
	case 1:
		pll = rt_memc_r32(MEMC_REG_CPU_PLL);
		fbdiv = (pll >> CPU_PLL_FBDIV_SHIFT) & CPU_PLL_FBDIV_MASK;
		prediv = (pll >> CPU_PLL_PREDIV_SHIFT) & CPU_PLL_PREDIV_MASK;
		cpu_clk = ((fbdiv + 1) * xtal_clk) >> prediv_tbl[prediv];
		break;
	default:
		cpu_clk = xtal_clk;
	}

	return cpu_clk / ffiv * ffrac;
}

static unsigned long mt7621_bus_recalc_rate(struct clk_hw *hw,
					    unsigned long parent_rate)
{
	return parent_rate / 4;
}

#define CLK_BASE_NOPARENT(_name, _recalc) {				\
	.init = &(struct clk_init_data) {				\
		.name = _name,						\
		.ops = &(const struct clk_ops) {			\
			.recalc_rate = _recalc,				\
		},							\
	},								\
}

#define CLK_BASE(_name, _parent, _recalc) {				\
	.init = &(struct clk_init_data) {				\
		.name = _name,						\
		.ops = &(const struct clk_ops) {			\
			.recalc_rate = _recalc,				\
		},							\
		.parent_names = (const char *const[]) { _parent },	\
		.num_parents = 1,					\
	},								\
}

static struct clk_hw mt7621_clks_base[] = {
	CLK_BASE_NOPARENT("xtal", mt7621_xtal_recalc_rate),
	CLK_BASE("cpu", "xtal", mt7621_cpu_recalc_rate),
	CLK_BASE("bus", "cpu", mt7621_bus_recalc_rate),
};

static unsigned long mt7621_gate_recalc_rate(struct clk_hw *hw,
					     unsigned long parent_rate)
{
	struct mt7621_clk_gate *pd = to_mt7621_gate(hw);

	if (pd->rate)
		return pd->rate;
	return parent_rate;
}

static int mt7621_gate_is_enabled(struct clk_hw *hw)
{
	struct mt7621_clk_gate *pd = to_mt7621_gate(hw);
	u32 val = rt_sysc_r32(SYSC_REG_CLKCFG1);
	return !!(val & BIT(pd->bit));
}

static int mt7621_gate_enable(struct clk_hw *hw)
{
	struct mt7621_clk_gate *pd = to_mt7621_gate(hw);

	rt_sysc_m32(0, BIT(pd->bit), SYSC_REG_CLKCFG1);
	return 0;
}

static void mt7621_gate_disable(struct clk_hw *hw)
{
	struct mt7621_clk_gate *pd = to_mt7621_gate(hw);

	rt_sysc_m32(BIT(pd->bit), 0, SYSC_REG_CLKCFG1);
}

static const struct clk_ops mt7621_gate_ops = {
	.enable = mt7621_gate_enable,
	.disable = mt7621_gate_disable,
	.is_enabled = mt7621_gate_is_enabled,
	.recalc_rate = mt7621_gate_recalc_rate,
};

#define CLK_GATE_PARENT(_name, _parent, _bit) {				\
	.bit = _bit,							\
	.hw.init = &(struct clk_init_data) {				\
		.name = _name,						\
		.ops = &mt7621_gate_ops,				\
		.parent_names = (const char *const[]) { _parent },	\
		.num_parents = 1,					\
	},								\
}

#define CLK_GATE_FIXED(_name, _mhz, _bit) {				\
	.bit = _bit,							\
	.rate = _mhz * 1000000,						\
	.hw.init = &(struct clk_init_data) {				\
		.name = _name,						\
		.ops = &mt7621_gate_ops,				\
		.parent_names = (const char *const[]) { "xtal" },	\
		.num_parents = 1,					\
	},								\
}

static struct mt7621_clk_gate mt7621_clks_gate[] = {
	CLK_GATE_FIXED("hsdma", 50, 5),
	CLK_GATE_FIXED("fe", 250, 6),
	CLK_GATE_FIXED("spidftx", 270, 7),
	CLK_GATE_FIXED("timer", 50, 8),
	CLK_GATE_FIXED("pcm", 270, 11),
	CLK_GATE_FIXED("pio", 50, 13),
	CLK_GATE_PARENT("gdma", "bus", 14),
	CLK_GATE_FIXED("nand", 125, 15),
	CLK_GATE_FIXED("i2c", 50, 16),
	CLK_GATE_FIXED("i2s", 270, 17),
	CLK_GATE_PARENT("spi", "bus", 18),
	CLK_GATE_FIXED("uart1", 50, 19),
	CLK_GATE_FIXED("uart2", 50, 20),
	CLK_GATE_FIXED("uart3", 50, 21),
	CLK_GATE_FIXED("eth", 50, 23),
	CLK_GATE_FIXED("pcie0", 125, 24),
	CLK_GATE_FIXED("pcie1", 125, 25),
	CLK_GATE_FIXED("pcie2", 125, 26),
	CLK_GATE_FIXED("crypto", 250, 29),
	CLK_GATE_FIXED("sdxc", 50, 30),
};

static struct clk_hw_onecell_data mt7621_clk_onecell_data = {
	.hws = {
		[MT7621_CLK_XTAL] = &mt7621_clks_base[0],
		[MT7621_CLK_CPU] = &mt7621_clks_base[1],
		[MT7621_CLK_BUS] = &mt7621_clks_base[2],
		[MT7621_CLK_HSDMA] = &mt7621_clks_gate[0].hw,
		[MT7621_CLK_FE] = &mt7621_clks_gate[1].hw,
		[MT7621_CLK_SPDIFTX] = &mt7621_clks_gate[2].hw,
		[MT7621_CLK_TIMER] = &mt7621_clks_gate[3].hw,
		[MT7621_CLK_PCM] = &mt7621_clks_gate[4].hw,
		[MT7621_CLK_PIO] = &mt7621_clks_gate[5].hw,
		[MT7621_CLK_GDMA] = &mt7621_clks_gate[6].hw,
		[MT7621_CLK_NAND] = &mt7621_clks_gate[7].hw,
		[MT7621_CLK_I2C] = &mt7621_clks_gate[8].hw,
		[MT7621_CLK_I2S] = &mt7621_clks_gate[9].hw,
		[MT7621_CLK_SPI] = &mt7621_clks_gate[10].hw,
		[MT7621_CLK_UART1] = &mt7621_clks_gate[11].hw,
		[MT7621_CLK_UART2] = &mt7621_clks_gate[12].hw,
		[MT7621_CLK_UART3] = &mt7621_clks_gate[13].hw,
		[MT7621_CLK_ETH] = &mt7621_clks_gate[14].hw,
		[MT7621_CLK_PCIE0] = &mt7621_clks_gate[15].hw,
		[MT7621_CLK_PCIE1] = &mt7621_clks_gate[16].hw,
		[MT7621_CLK_PCIE2] = &mt7621_clks_gate[17].hw,
		[MT7621_CLK_CRYPTO] = &mt7621_clks_gate[18].hw,
		[MT7621_CLK_SDXC] = &mt7621_clks_gate[19].hw,
	},
	.num = MT7621_CLK_MAX,
};

static void __init mt7621_clocks_init(struct device_node *np)
{
	int ret;
	int i;

	for (i = 0; i < ARRAY_SIZE(mt7621_clks_base); i++) {
		ret = of_clk_hw_register(np, &mt7621_clks_base[i]);
		if (ret)
			panic("failed to register critical clock.\n");
	}

	for (i = 0; i < ARRAY_SIZE(mt7621_clks_gate); i++) {
		ret = of_clk_hw_register(np, &mt7621_clks_gate[i].hw);
		if (ret)
			panic("failed to register peripheral clock.\n");
	}

	of_clk_add_hw_provider(np, of_clk_hw_onecell_get,
			       &mt7621_clk_onecell_data);
}
CLK_OF_DECLARE(mt7621_clk, "mediatek,mt7621-clk", mt7621_clocks_init);
