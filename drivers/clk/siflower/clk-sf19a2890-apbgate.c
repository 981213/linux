// SPDX-License-Identifier: GPL-2.0
#include <linux/of_clk.h>
#include <linux/of.h>
#include <linux/spinlock.h>
#include <linux/clk-provider.h>
#include <linux/of_address.h>
#include <linux/slab.h>

#define REG_GATE 0x0
/*
 * A shared 'Bus Output Enable' signal for all APB peripherals. The peripheral
 * only responds to bus requests if its dedicated clock is enabled and this
 * shared BOE is set.
 */
#define REG_BOE 0x8
#define BOE_EN GENMASK(1, 0)

struct sf19a2890_apbgate {
	void __iomem *base;
	struct clk_hw hw;
	u32 idx;
};

struct sf19a2890_apbgate_priv {
	struct sf19a2890_apbgate *gates;
	struct clk_hw_onecell_data clk_data;
};

static inline struct sf19a2890_apbgate *hw_to_apbgate(struct clk_hw *hw)
{
	return container_of(hw, struct sf19a2890_apbgate, hw);
}

static int sf19a2890_apbgate_enable(struct clk_hw *hw)
{
	struct sf19a2890_apbgate *priv = hw_to_apbgate(hw);
	u32 reg = readl(priv->base + REG_GATE);
	writel(reg | BIT(priv->idx), priv->base + REG_GATE);
	writel(BOE_EN, priv->base + REG_BOE);
	return 0;
}

static void sf19a2890_apbgate_disable(struct clk_hw *hw)
{
	struct sf19a2890_apbgate *priv = hw_to_apbgate(hw);
	u32 reg = readl(priv->base + REG_GATE);
	reg &= ~BIT(priv->idx);
	writel(reg, priv->base + REG_GATE);
	if (reg == 0)
		writel(0, priv->base + REG_BOE);
}

static int sf19a2890_apbgate_is_enabled(struct clk_hw *hw)
{
	struct sf19a2890_apbgate *priv = hw_to_apbgate(hw);
	return !!(readl(priv->base + REG_GATE) & BIT(priv->idx));
}

static const struct clk_ops sf19a28_apbgate_ops = {
	.enable = sf19a2890_apbgate_enable,
	.disable = sf19a2890_apbgate_disable,
	.is_enabled = sf19a2890_apbgate_is_enabled,
};

static void __init sf19a2890_apbgate_init(struct device_node *node)
{
	struct clk_init_data init = {};
	struct sf19a2890_apbgate_priv *priv;
	void __iomem *base;
	const char *name, *parent;
	int num_clks;
	int i, ret;
	u32 reg;

	num_clks = of_count_phandle_with_args(node, "clocks", "#clock-cells");
	if (num_clks < 1)
		return;

	priv = kzalloc(struct_size(priv, clk_data.hws, num_clks), GFP_KERNEL);
	if (!priv) {
		pr_err("failed to allocate private structure.");
		return;
	}
	priv->clk_data.num = num_clks;

	priv->gates = kzalloc(num_clks * sizeof(struct sf19a2890_apbgate),
			      GFP_KERNEL);
	if (!priv->gates) {
		pr_err("failed to allocate private structure for gates.");
		goto err1;
	}

	base = of_iomap(node, 0);
	if (!base) {
		pr_err("failed to map resources.\n");
		goto err2;
	}

	/* clear unused higher bits for BOE check in disable call. */
	reg = readl(base + REG_GATE);
	reg &= BIT(num_clks) - 1;
	writel(reg, base + REG_GATE);

	for (i = 0; i < num_clks; i++) {
		ret = of_property_read_string_index(node, "clock-output-names",
						    i, &name);
		if (ret != 0) {
			pr_err("failed to read output name for the %dth gate.\n",
			       i);
			goto err3;
		}
		parent = of_clk_get_parent_name(node, i);
		if (!parent) {
			pr_err("failed to get parent clock for the %dth gate.\n",
			       i);
			goto err3;
		}

		priv->gates[i].base = base;
		priv->gates[i].idx = i;
		init.name = name;
		init.ops = &sf19a28_apbgate_ops;
		init.parent_names = &parent;
		init.num_parents = 1;
		priv->gates[i].hw.init = &init;

		ret = clk_hw_register(NULL, &priv->gates[i].hw);
		if (ret) {
			pr_err("failed to register the %dth gate: %d.\n", i,
			       ret);
			goto err3;
		}
		priv->clk_data.hws[i] = &priv->gates[i].hw;
	}

	ret = of_clk_add_hw_provider(node, of_clk_hw_onecell_get,
				     &priv->clk_data);
	if (ret) {
		pr_err("failed to add hw provider.\n");
		goto err3;
	}
	return;
err3:
	for (i--; i >= 0; i--)
		clk_hw_unregister_gate(priv->clk_data.hws[i]);
err2:
	kfree(priv->gates);
err1:
	kfree(priv);
}

CLK_OF_DECLARE(sf19a2890_apbgate, "siflower,sf19a2890-apbgate",
	       sf19a2890_apbgate_init);