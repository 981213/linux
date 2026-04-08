// SPDX-License-Identifier: GPL-2.0

/*
 * Siflower SF19A2890 peripheral clock and reset manager driver.
 *
 * Each managed block has three registers:
 *   RESET    at base + 0x0  – per-peripheral reset bitmasks
 *   CLK_GATE at base + 0x4  – one bit per clock gate
 *   BOE      at base + 0xc  – shared Bus Output Enable; bits [1:0] must be set
 *                             for any APB peripheral in the block to respond
 */

#include <linux/clk-provider.h>
#include <linux/io.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/reset-controller.h>
#include <linux/slab.h>
#include <linux/spinlock.h>

#define REG_RESET	0x0
#define REG_CLK_GATE	0x4
#define REG_BOE		0xc
#define BOE_EN		GENMASK(1, 0)

struct sf19a2890_periphcrm {
	void __iomem *base;
	spinlock_t lock;
	struct reset_controller_dev rcdev;
	u32 *reset_masks;
	struct clk_hw_onecell_data clk_data;
};

struct sf19a2890_periphclk {
	struct sf19a2890_periphcrm *crm;
	struct clk_hw hw;
	u32 idx;
};

static inline struct sf19a2890_periphclk *hw_to_periphclk(struct clk_hw *hw)
{
	return container_of(hw, struct sf19a2890_periphclk, hw);
}

static int sf19a2890_periphclk_enable(struct clk_hw *hw)
{
	struct sf19a2890_periphclk *gate = hw_to_periphclk(hw);
	struct sf19a2890_periphcrm *crm = gate->crm;
	u32 reg;

	reg = readl(crm->base + REG_CLK_GATE);
	writel(reg | BIT(gate->idx), crm->base + REG_CLK_GATE);
	writel(BOE_EN, crm->base + REG_BOE);
	return 0;
}

static void sf19a2890_periphclk_disable(struct clk_hw *hw)
{
	struct sf19a2890_periphclk *gate = hw_to_periphclk(hw);
	struct sf19a2890_periphcrm *crm = gate->crm;
	u32 reg;

	reg = readl(crm->base + REG_CLK_GATE);
	reg &= ~BIT(gate->idx);
	writel(reg, crm->base + REG_CLK_GATE);
	if (reg == 0)
		writel(0, crm->base + REG_BOE);
}

static int sf19a2890_periphclk_is_enabled(struct clk_hw *hw)
{
	struct sf19a2890_periphclk *gate = hw_to_periphclk(hw);
	struct sf19a2890_periphcrm *crm = gate->crm;

	return !!(readl(crm->base + REG_CLK_GATE) & BIT(gate->idx));
}

static const struct clk_ops sf19a2890_periphclk_ops = {
	.enable = sf19a2890_periphclk_enable,
	.disable = sf19a2890_periphclk_disable,
	.is_enabled = sf19a2890_periphclk_is_enabled,
};

static inline struct sf19a2890_periphcrm *
rcdev_to_periphcrm(struct reset_controller_dev *rcdev)
{
	return container_of(rcdev, struct sf19a2890_periphcrm, rcdev);
}

static int sf19a2890_periphcrm_reset_update(struct reset_controller_dev *rcdev,
					    unsigned long id, bool assert)
{
	struct sf19a2890_periphcrm *crm = rcdev_to_periphcrm(rcdev);
	unsigned long flags;
	u32 reg;

	spin_lock_irqsave(&crm->lock, flags);
	reg = readl(crm->base + REG_RESET);
	if (assert)
		reg |= crm->reset_masks[id];
	else
		reg &= ~crm->reset_masks[id];
	writel(reg, crm->base + REG_RESET);
	spin_unlock_irqrestore(&crm->lock, flags);
	return 0;
}

static int sf19a2890_periphcrm_reset_assert(struct reset_controller_dev *rcdev,
					    unsigned long id)
{
	return sf19a2890_periphcrm_reset_update(rcdev, id, true);
}

static int
sf19a2890_periphcrm_reset_deassert(struct reset_controller_dev *rcdev,
				   unsigned long id)
{
	return sf19a2890_periphcrm_reset_update(rcdev, id, false);
}

static int sf19a2890_periphcrm_reset_status(struct reset_controller_dev *rcdev,
					    unsigned long id)
{
	struct sf19a2890_periphcrm *crm = rcdev_to_periphcrm(rcdev);

	return !!(readl(crm->base + REG_RESET) & crm->reset_masks[id]);
}

static const struct reset_control_ops sf19a2890_periphcrm_reset_ops = {
	.assert = sf19a2890_periphcrm_reset_assert,
	.deassert = sf19a2890_periphcrm_reset_deassert,
	.status = sf19a2890_periphcrm_reset_status,
};

/* ---------- platform driver ------------------------------------------- */

static int sf19a2890_periphcrm_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct device_node *node = dev->of_node;
	struct sf19a2890_periphclk *gates;
	struct sf19a2890_periphcrm *crm;
	struct clk_init_data init = {};
	u32 valid_gates, critical_gates;
	int num_clks, nr_resets;
	int i, idx, ret;
	u32 tmp, reg;

	num_clks = of_count_phandle_with_args(node, "clocks", "#clock-cells");
	if (num_clks < 1 || num_clks > 32)
		return -EINVAL;

	ret = of_property_read_u32(node, "siflower,valid-gates", &valid_gates);
	if (ret)
		valid_gates = BIT(num_clks) - 1;

	ret = of_property_read_u32(node, "siflower,critical-gates",
				   &critical_gates);
	if (ret)
		critical_gates = 0;

	nr_resets = of_property_count_u32_elems(node, "siflower,reset-masks");
	if (nr_resets < 1) {
		ret = of_property_read_u32(node, "siflower,num-resets", &tmp);
		if (ret || tmp < 1)
			return dev_err_probe(
				dev, -EINVAL,
				"missing siflower,reset-masks or siflower,num-resets\n");
		nr_resets = tmp;
	}

	if (nr_resets > 32)
		return dev_err_probe(dev, -EINVAL, "too many resets (%d)\n",
				     nr_resets);

	crm = devm_kzalloc(dev, struct_size(crm, clk_data.hws, num_clks),
			   GFP_KERNEL);
	if (!crm)
		return -ENOMEM;

	crm->clk_data.num = num_clks;

	gates = devm_kcalloc(dev, num_clks, sizeof(*gates), GFP_KERNEL);
	if (!gates)
		return -ENOMEM;

	crm->reset_masks =
		devm_kcalloc(dev, nr_resets, sizeof(u32), GFP_KERNEL);
	if (!crm->reset_masks)
		return -ENOMEM;

	crm->base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(crm->base))
		return PTR_ERR(crm->base);

	spin_lock_init(&crm->lock);

	/* Populate reset masks; fall back to one bit per reset index. */
	ret = of_property_read_u32_array(node, "siflower,reset-masks",
					 crm->reset_masks, nr_resets);
	if (ret)
		for (i = 0; i < nr_resets; i++)
			crm->reset_masks[i] = BIT(i);

	crm->rcdev.owner = THIS_MODULE;
	crm->rcdev.nr_resets = nr_resets;
	crm->rcdev.ops = &sf19a2890_periphcrm_reset_ops;
	crm->rcdev.of_node = node;

	ret = devm_reset_controller_register(dev, &crm->rcdev);
	if (ret)
		return ret;

	/*
	 * Clear any stale bits outside valid_gates so the BOE-disable
	 * check in periphclk_disable() works correctly.
	 */
	reg = readl(crm->base + REG_CLK_GATE) & valid_gates;
	writel(reg, crm->base + REG_CLK_GATE);

	for (i = 0, idx = 0; i < num_clks; i++, idx++) {
		const char *name, *parent;

		ret = of_property_read_string_index(node, "clock-output-names",
						    i, &name);
		if (ret)
			return dev_err_probe(
				dev, ret,
				"failed to read clock-output-names[%d]\n", i);

		parent = of_clk_get_parent_name(node, i);
		if (!parent)
			return dev_err_probe(
				dev, -EINVAL,
				"failed to get parent clock for gate %d\n", i);

		/* Advance idx to the next set bit in valid_gates. */
		while (!(valid_gates & BIT(idx))) {
			idx++;
			if (idx >= 32)
				return dev_err_probe(
					dev, -EINVAL,
					"not enough valid gates\n");
		}

		gates[i].crm = crm;
		gates[i].idx = idx;
		init.name = name;
		init.ops = &sf19a2890_periphclk_ops;
		init.parent_names = &parent;
		init.num_parents = 1;
		init.flags = (critical_gates & BIT(idx)) ? CLK_IS_CRITICAL : 0;
		gates[i].hw.init = &init;

		ret = devm_clk_hw_register(dev, &gates[i].hw);
		if (ret)
			return dev_err_probe(dev, ret,
					     "failed to register gate %d\n", i);

		crm->clk_data.hws[i] = &gates[i].hw;
	}

	return devm_of_clk_add_hw_provider(dev, of_clk_hw_onecell_get,
					   &crm->clk_data);
}

static const struct of_device_id sf19a2890_periphcrm_dt_ids[] = {
	{ .compatible = "siflower,sf19a2890-periph-crm" },
	{ /* sentinel */ },
};

static struct platform_driver sf19a2890_periphcrm_driver = {
	.probe	= sf19a2890_periphcrm_probe,
	.driver = {
		.name		= "sf19a2890-periph-crm",
		.of_match_table	= sf19a2890_periphcrm_dt_ids,
	},
};
builtin_platform_driver(sf19a2890_periphcrm_driver);
