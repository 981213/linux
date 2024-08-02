// SPDX-License-Identifier: GPL-2.0
#include "linux/of_clk.h"
#include <linux/of.h>
#include <linux/spinlock.h>
#include <linux/clk-provider.h>
#include <linux/of_address.h>
#include <linux/slab.h>

struct sf19a2890_apbgate_priv {
	spinlock_t lock;
	struct clk_hw_onecell_data clk_data;
};

static void __init sf19a2890_apbgate_init(struct device_node *node)
{
	struct sf19a2890_apbgate_priv *priv;
	void __iomem *base;
	const char *name, *parent;
	struct clk_hw *hw;
	int num_clks;
	int i, ret;

	num_clks = of_count_phandle_with_args(node, "clocks", "#clock-cells");
	if (num_clks < 1)
		return;

	priv = kzalloc(struct_size(priv, clk_data.hws, num_clks), GFP_KERNEL);
	if (!priv) {
		pr_err("failed to allocate private structure.");
		return;
	}
	priv->clk_data.num = num_clks;

	base = of_iomap(node, 0);
	if (!base) {
		pr_err("failed to map resources.\n");
		return;
	}

	spin_lock_init(&priv->lock);
	for (i = 0; i < num_clks; i++) {
		ret = of_property_read_string_index(node, "clock-output-names",
						    i, &name);
		if (ret != 0) {
			pr_err("failed to read output name for the %dth gate.\n",
			       i);
			return;
		}
		parent = of_clk_get_parent_name(node, i);
		if (!parent) {
			pr_err("failed to get parent clock for the %dth gate.\n",
			       i);
			return;
		}

		hw = clk_hw_register_gate(NULL, name, parent, CLK_IS_CRITICAL, base, i, 0,
					  &priv->lock);
		if (IS_ERR(hw)) {
			pr_err("failed to register the %dth gate: %ld.\n", i,
			       PTR_ERR(hw));
			return;
		}
		priv->clk_data.hws[i] = hw;
	}

	ret = of_clk_add_hw_provider(node, of_clk_hw_onecell_get,
				     &priv->clk_data);
	if (ret)
		pr_err("failed to add hw provider.\n");
}

CLK_OF_DECLARE(sf19a2890_apbgate, "siflower,sf19a2890-apbgate",
	       sf19a2890_apbgate_init);