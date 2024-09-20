// SPDX-License-Identifier: GPL-2.0

#include <linux/bitfield.h>
#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/gpio.h>
#include <linux/kernel.h>
#include <linux/mfd/syscon.h>
#include <linux/module.h>
#include <linux/of_gpio.h>
#include <linux/of_device.h>
#include <linux/of_address.h>
#include <linux/pci.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/resource.h>
#include <linux/signal.h>
#include <linux/types.h>
#include <linux/reset.h>
#include <linux/pm_domain.h>
#include <linux/pm_runtime.h>

#include "pcie-designware.h"

#define SF_PCIE_MAX_TIMEOUT	10000

#define AHB_SYSM_OFFSET		0x10000

#define AHB_SYSM_REG(n)		(AHB_SYSM_OFFSET + 0x4 * n)
#define CFG_PIPE_RSTN		1

#define ELBI_REG(n)			(0x4 * n)
#define APP_LTSSM_ENABLE	23

#define TOPSYS_RSTN_CFG		0xC0
#define SERDES_CSR_SW_RST_N	24

#define TOPSYS_LVDS_CFG		0xE8

#define TOPSYS_LVDS_CFG1	0x120

#define BIAS_EN			20
#define RXEN			3
#define TXEN			2

#define to_sf_pcie(x)	dev_get_drvdata((x)->dev)

static bool phy_init_done = false;
static DEFINE_MUTEX(pcie_phy_lock);
int volt[3] = { 33, 0, 28 };

enum pcie_device_type {
	PCIE_EP = 0,
	PCIE_RC = 4,
	PCIE_UNKNOW,
};

enum pcie_lane_mode {
	PCIE0_LANE0_PCIE1_LANE1,
	PCIE0_LANE0_LANE1,
	PCIE1_LANE0_PCIE0_LANE1,
	PCIE1_LANE0_LANE1,
};

struct sf_pcie {
	struct dw_pcie			pci;
	void __iomem			*elbi;
	struct clk			*csr_clk;
	struct clk			*ref_clk;
	struct clk			*phy_clk;
	struct regmap			*topsys;
	struct regmap			*ahbsys;
	u32				ctrl_id;
	u32				reset_ms;
	struct gpio_desc		*reset_gpio;
	bool				link_state;
	enum pcie_device_type		device_type;
	enum pcie_lane_mode		lane_mode;
};



void sf_pcie_phy0_cr_write(struct sf_pcie *sf_pcie, u32 data, u32 addr)
{
	u32 val, check = 0, count = 0;
	val = ((0xffff & addr) << 16) | (0xffff & data);
	regmap_write(sf_pcie->ahbsys, AHB_SYSM_REG(0xB), val);
	usleep_range(1, 1);
	while(count<0x10){
		regmap_read(sf_pcie->ahbsys, AHB_SYSM_REG(0xC), &check);
		check &= 0x1;
		if(check)
			break;
		count++;
		usleep_range(1, 1);
	}
}

void sf_pcie_phy1_cr_write(struct sf_pcie *sf_pcie, u32 data, u32 addr)
{
	u32 val, check = 0,count=0;
	val = ((0xffff & addr) << 16) | (0xffff & data);
	regmap_write(sf_pcie->ahbsys, AHB_SYSM_REG(0xF), val);
	usleep_range(1, 1);
	while(count<0x10){
		regmap_read(sf_pcie->ahbsys, AHB_SYSM_REG(0x10), &check);
		check &= 0x1;
		if(check)
			break;
		count++;
		usleep_range(1, 1);
	}
}

static void sf_pcie_enable_dbi_ro_wr_en(struct sf_pcie *sf_pcie)
{
	u32 val;
	val = readl(sf_pcie->pci.dbi_base + 0x8bc);
	val = val | 0x1 ;
	msleep(10);
	writel(val, sf_pcie->pci.dbi_base + 0x8bc);
	val = readl(sf_pcie->pci.dbi_base + 0x8bc);
	msleep(20);
}

static void sf_pcie_enable_part_lanes_rxei_exit(struct sf_pcie *sf_pcie)
{
	u32 val;
	val = readl(sf_pcie->pci.dbi_base + 0x708);
	val = val | 0x1 << 22;
	writel(val, sf_pcie->pci.dbi_base + 0x708);
	val = readl(sf_pcie->pci.dbi_base + 0x708);
	msleep(20);
}

static void sf_pcie_enable_speed_change(struct sf_pcie *sf_pcie)
{
	u32 val;
	val = readl(sf_pcie->pci.dbi_base + 0x80c);
	val = val | 0x1 << 17;
	writel(val, sf_pcie->pci.dbi_base + 0x80c);
	val = readl(sf_pcie->pci.dbi_base + 0x80c);
	msleep(20);
}

static void sf_pcie_assert_pipe_reset(struct sf_pcie *sf_pcie)
{
	u32 mask;
	switch (sf_pcie->lane_mode) {
	case PCIE0_LANE0_PCIE1_LANE1:
		mask = (sf_pcie->ctrl_id ? 0x2 : 0x1) << CFG_PIPE_RSTN;
		break;
	case PCIE1_LANE0_PCIE0_LANE1:
		mask = (sf_pcie->ctrl_id ? 0x1 : 0x2) << CFG_PIPE_RSTN;
		break;
	case PCIE0_LANE0_LANE1:
	case PCIE1_LANE0_LANE1:
	default:
		mask = 0x3 << CFG_PIPE_RSTN;
		break;
	}
	regmap_clear_bits(sf_pcie->ahbsys, AHB_SYSM_REG(1), mask);
}

static void sf_pcie_deassert_pipe_reset(struct sf_pcie *sf_pcie)
{
	u32 mask;
	switch (sf_pcie->lane_mode) {
	case PCIE0_LANE0_PCIE1_LANE1:
		mask = (sf_pcie->ctrl_id ? 0x2 : 0x1) << CFG_PIPE_RSTN;
		break;
	case PCIE1_LANE0_PCIE0_LANE1:
		mask = (sf_pcie->ctrl_id ? 0x1 : 0x2) << CFG_PIPE_RSTN;
		break;
	case PCIE0_LANE0_LANE1:
	case PCIE1_LANE0_LANE1:
	default:
		mask = 0x3 << CFG_PIPE_RSTN;
		break;
	}
	regmap_set_bits(sf_pcie->ahbsys, AHB_SYSM_REG(1), mask);
}

static void sf_pcie_assert_core_reset(struct sf_pcie *sf_pcie)
{
	u32 mask;
	mask = sf_pcie->ctrl_id ? GENMASK(8,6) : GENMASK(5,3);
	regmap_clear_bits(sf_pcie->ahbsys, AHB_SYSM_REG(1), mask);
}

static void sf_pcie_deassert_core_reset(struct sf_pcie *sf_pcie)
{
	u32 mask;

	mask = sf_pcie->ctrl_id ? GENMASK(8,6) : GENMASK(5,3);
	regmap_set_bits(sf_pcie->ahbsys, AHB_SYSM_REG(1), mask);
}

static void sf_pcie_configure_device_type(struct sf_pcie *sf_pcie)
{
	if (sf_pcie->ctrl_id) {
		regmap_update_bits(sf_pcie->ahbsys, AHB_SYSM_REG(0), GENMASK(7,4), FIELD_PREP(GENMASK(7,4), sf_pcie->device_type));
	} else {
		regmap_update_bits(sf_pcie->ahbsys, AHB_SYSM_REG(0), GENMASK(3,0), FIELD_PREP(GENMASK(3,0), sf_pcie->device_type));
	}
}

static void sf_pcie_configure_lane_mode(struct sf_pcie *sf_pcie)
{
	regmap_update_bits(sf_pcie->ahbsys, AHB_SYSM_REG(0), GENMASK(9, 8),
			   FIELD_PREP(GENMASK(9, 8), sf_pcie->lane_mode));
}

static void sf_pcie_init_legacy_interrupt(struct sf_pcie *sf_pcie)
{
	/* disable all interrupts by default. they will be enabled later */
	writel_relaxed(0, sf_pcie->elbi + ELBI_REG(38));
	writel_relaxed(0, sf_pcie->elbi + ELBI_REG(39));
	writel_relaxed(0, sf_pcie->elbi + ELBI_REG(40));
	writel_relaxed(0, sf_pcie->elbi + ELBI_REG(41));
}

static void sf_pcie_ltssm_enable(struct sf_pcie *sf_pcie)
{
	u32 val;
	val = readl(sf_pcie->elbi + ELBI_REG(0));
	val |= 0x1 << APP_LTSSM_ENABLE;
	writel(val, sf_pcie->elbi + ELBI_REG(0));
}

static int sf_pcie_establish_link(struct sf_pcie *sf_pcie)
{
	struct dw_pcie *pci = &sf_pcie->pci;

	if (dw_pcie_link_up(pci))
		return 0;

	sf_pcie_ltssm_enable(sf_pcie);

	return dw_pcie_wait_for_link(pci);
}

static int sf_pcie_clk_enable(struct sf_pcie *sf_pcie)
{
	int ret;
	ret = clk_prepare_enable(sf_pcie->csr_clk);
	if (ret)
		return ret;

	ret = clk_prepare_enable(sf_pcie->ref_clk);
	if (ret)
		return ret;

	ret = clk_prepare_enable(sf_pcie->phy_clk);
	if (ret)
		return ret;
	return 0;
}

static void sf_pcie_clk_disable(struct sf_pcie *sf_pcie)
{
	clk_disable_unprepare(sf_pcie->csr_clk);
	clk_disable_unprepare(sf_pcie->ref_clk);
	clk_disable_unprepare(sf_pcie->phy_clk);
}

static int sf_pcie_lvds_enable(struct sf_pcie *sf_pcie)
{
	int ret = 0;
	ret = regmap_set_bits(sf_pcie->topsys, TOPSYS_LVDS_CFG,
			      BIT(TXEN) | BIT(BIAS_EN));
	if (ret) {
		return ret;
	}

	ret = regmap_set_bits(sf_pcie->topsys, TOPSYS_LVDS_CFG1,
			      BIT(TXEN) | BIT(BIAS_EN));
	if (ret) {
		return ret;
	}
	return 0;
}

static int sf_pcie_lvds_disable(struct sf_pcie *sf_pcie)
{
	int ret = 0;
	ret = regmap_clear_bits(sf_pcie->topsys, TOPSYS_LVDS_CFG,
				BIT(TXEN) | BIT(BIAS_EN));
	if (ret) {
		return ret;
	}
	ret = regmap_clear_bits(sf_pcie->topsys, TOPSYS_LVDS_CFG1,
				BIT(TXEN) | BIT(BIAS_EN));
	if (ret) {
		return ret;
	}
	return 0;
}

static void sf_pcie_assert_phy_reset(struct sf_pcie *sf_pcie)
{
	regmap_clear_bits(sf_pcie->ahbsys, AHB_SYSM_REG(1), 0x1);
}

static void sf_pcie_deassert_phy_reset(struct sf_pcie *sf_pcie)
{
	regmap_set_bits(sf_pcie->ahbsys, AHB_SYSM_REG(1), 0x1);
}

static int sf_pcie_phy_init(struct sf_pcie *sf_pcie)
{
	if (phy_init_done) {
		dev_info(sf_pcie->pci.dev, "Phy already init. So return.\n");
		return 0;
	}
	/*
	 * Configure pcie lane mode
	 * */
	sf_pcie_configure_lane_mode(sf_pcie);

	/*
	 * deassert phy reset
	 * */
	sf_pcie_deassert_phy_reset(sf_pcie);

	return 0;
}

/*
 * The bus interconnect subtracts address offset from the request
 * before sending it to PCIE slave port. Since DT puts config space
 * at the beginning, we can obtain the address offset from there and
 * subtract it.
 */
static u64 sf_pcie_cpu_addr_fixup(struct dw_pcie *pci, u64 cpu_addr)
{
	struct dw_pcie_rp *pp = &pci->pp;

	return cpu_addr - pp->cfg0_base;
}

static int sf_pcie_init(struct sf_pcie *sf_pcie)
{
	int ret;

	ret = sf_pcie_lvds_enable(sf_pcie);
	if (ret) {
		dev_err(sf_pcie->pci.dev, "lvds enable failed.\n");
		return ret;
	}

	ret = sf_pcie_clk_enable(sf_pcie);
	if (ret) {
		dev_err(sf_pcie->pci.dev, "clk enbale failed.\n");
		return ret;
	}

	/*
	 * config device type
	 * */
	sf_pcie_configure_device_type(sf_pcie);

	/*
	 * init phy
	 * */
	mutex_lock(&pcie_phy_lock);
	ret = sf_pcie_phy_init(sf_pcie);
	mutex_unlock(&pcie_phy_lock);
	if (ret)
		return ret;

	return 0;
}

static void sf_pcie_deinit(struct sf_pcie *sf_pcie)
{
	sf_pcie_assert_phy_reset(sf_pcie);
	sf_pcie_assert_pipe_reset(sf_pcie);
	sf_pcie_assert_core_reset(sf_pcie);
	sf_pcie_lvds_disable(sf_pcie);
	sf_pcie_clk_disable(sf_pcie);
}

static int sf_pcie_host_init(struct dw_pcie_rp *pp)
{
	struct dw_pcie *pci = to_dw_pcie_from_pp(pp);
	struct sf_pcie *sf_pcie = to_sf_pcie(pci);
	int ret;

	ret = sf_pcie_init(sf_pcie);
	if (ret)
		return ret;

	sf_pcie_deassert_pipe_reset(sf_pcie);
	sf_pcie_deassert_core_reset(sf_pcie);

	sf_pcie_enable_dbi_ro_wr_en(sf_pcie);
	sf_pcie_enable_part_lanes_rxei_exit(sf_pcie);

	/*
	 * reset ep device by reset_gpio
	 * Note: The direction speed change maybe occur when we
	 * hold the reset gpio. So we should set the field
	 * DIRECTION_SPEED_CHANGE of GEN2_CTRL_OFF register after
	 * release reset gpio.
	 * */
	if (sf_pcie->reset_gpio) {
		gpiod_set_value_cansleep(sf_pcie->reset_gpio, 1);
		msleep(sf_pcie->reset_ms);
		gpiod_set_value_cansleep(sf_pcie->reset_gpio, 0);
		msleep(10);
	}

	sf_pcie_init_legacy_interrupt(sf_pcie);

	/*
	 * before link up with GEN1, we should config the field
	 * DIRECTION_SPEED_CHANGE of GEN2_CTRL_OFF register to insure
	 * the LTSSM to initiate a speed change to Gen2 or Gen3 after
	 * the link is initialized at Gen1 speed.
	 * */
	sf_pcie_enable_speed_change(sf_pcie);

	ret = sf_pcie_establish_link(sf_pcie);
	if (ret)
		return ret;

	return 0;
}

static const struct dw_pcie_host_ops sf_pcie_host_ops = {
	.host_init = sf_pcie_host_init,
};

static int sf_add_pcie_port(struct sf_pcie *sf_pcie,
			    struct platform_device *pdev)
{
	struct dw_pcie *pci = &sf_pcie->pci;
	struct dw_pcie_rp *pp = &pci->pp;
	struct device *dev = &pdev->dev;
	int ret;

	pp->ops = &sf_pcie_host_ops;

	ret = dw_pcie_host_init(pp);
	if (ret)
		dev_err_probe(dev, ret, "failed to initialize host\n");

	return ret;
}

static int sf_pcie_link_up(struct dw_pcie *pci)
{
	struct sf_pcie *pcie = to_sf_pcie(pci);
	u32 rdlh_link_up, smlh_link_up;

	rdlh_link_up = (readl(pcie->elbi + ELBI_REG(25)) >> 20) & 0x1;
	smlh_link_up = (readl(pcie->elbi + ELBI_REG(30)) >> 7) & 0x1;

	if (rdlh_link_up && smlh_link_up) {
		pcie->link_state = true;
		return 1;
	}

	/*
	 * nerver link up
	 * */
	pcie->link_state = false;
	return 0;
}

static const struct dw_pcie_ops dw_pcie_ops = {
	.cpu_addr_fixup = sf_pcie_cpu_addr_fixup,
	.link_up = sf_pcie_link_up,
};

static int sf_pcie_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct device_node *node = dev->of_node;
	struct sf_pcie *sf_pcie;
	struct resource *res;
	int ret;

	sf_pcie = devm_kzalloc(dev, sizeof(*sf_pcie), GFP_KERNEL);
	if (!sf_pcie)
		return -ENOMEM;

	sf_pcie->pci.dev = dev;
	sf_pcie->pci.ops = &dw_pcie_ops;

	platform_set_drvdata(pdev, sf_pcie);

	sf_pcie->csr_clk = devm_clk_get(&pdev->dev, "csr");
	if (IS_ERR(sf_pcie->csr_clk))
		return PTR_ERR(sf_pcie->csr_clk);

	sf_pcie->ref_clk = devm_clk_get(&pdev->dev, "ref");
	if (IS_ERR(sf_pcie->ref_clk))
		return PTR_ERR(sf_pcie->ref_clk);

	sf_pcie->phy_clk = devm_clk_get(&pdev->dev, "phy");
	if (IS_ERR(sf_pcie->phy_clk))
		return PTR_ERR(sf_pcie->phy_clk);

	sf_pcie->reset_gpio = devm_gpiod_get_optional(dev, "reset",
						      GPIOD_OUT_HIGH);
	if (IS_ERR(sf_pcie->reset_gpio)) {
		return dev_err_probe(dev, PTR_ERR(sf_pcie->reset_gpio),
				     "unable to get reset gpio\n");
	}

	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "dbi");
	sf_pcie->pci.dbi_base = devm_pci_remap_cfg_resource(dev, res);
	if (IS_ERR(sf_pcie->pci.dbi_base)) {
		return PTR_ERR(sf_pcie->pci.dbi_base);
	}

	sf_pcie->topsys = syscon_regmap_lookup_by_phandle(pdev->dev.of_node,
						       "topsys");
	if (IS_ERR(sf_pcie->topsys))
		return PTR_ERR(sf_pcie->topsys);

	sf_pcie->ahbsys = syscon_regmap_lookup_by_phandle(pdev->dev.of_node,
						       "ahbsys");
	if (IS_ERR(sf_pcie->ahbsys))
		return PTR_ERR(sf_pcie->ahbsys);

	sf_pcie->elbi = devm_platform_ioremap_resource_byname(pdev, "elbi");
	if (IS_ERR(sf_pcie->elbi)) {
		return PTR_ERR(sf_pcie->elbi);
	}

	sf_pcie->pci.atu_base = devm_platform_ioremap_resource_byname(pdev, "atu");
	if (IS_ERR(sf_pcie->pci.atu_base)) {
		return PTR_ERR(sf_pcie->pci.atu_base);
	}

	ret = of_property_read_u32(node, "ctrl-id", &sf_pcie->ctrl_id);
	if (ret) {
		/* default use pcie0 */
		sf_pcie->ctrl_id = 0;
	}

	ret = of_property_read_u32(node, "reset-ms", &sf_pcie->reset_ms);
	if (ret) {
		/* default halt reset: 100ms */
		sf_pcie->reset_ms = 100;
	}

	ret = of_property_read_u32(node, "lane-mode", &sf_pcie->lane_mode);
	if (ret) {
		/* default use PCIE0_LANE0_PCIE1_LANE1 */
		dev_err(dev, "Use default PCIE0_LANE0_PCIE1_LANE1 lane mode.\n");
		sf_pcie->lane_mode = PCIE0_LANE0_PCIE1_LANE1;
	}
	sf_pcie->device_type = PCIE_RC;
	sf_pcie->link_state = false;

	ret = sf_add_pcie_port(sf_pcie, pdev);
	return ret;

}

static int sf_pcie_remove(struct platform_device *pdev)
{
	struct sf_pcie *pcie = platform_get_drvdata(pdev);
	dev_err(&pdev->dev, "pcie controller driver was remove.");
	if (!pcie->link_state)
		return 0;

	dw_pcie_host_deinit(&pcie->pci.pp);
	sf_pcie_deinit(pcie);
	return 0;

}

static const struct of_device_id sf_pcie_of_match[] = {
	{ .compatible = "siflower,sf21a6826-pcie", },
	{},
};

static struct platform_driver sf_pcie_driver = {
	.driver = {
		.name	= "sf-pcie",
		.of_match_table = sf_pcie_of_match,
	},
	.probe    = sf_pcie_probe,
	.remove	  = sf_pcie_remove,
};

module_platform_driver(sf_pcie_driver);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Kaijun Wang <kaijun.wang@siflower.com.cn>");
MODULE_DESCRIPTION("PCIe Controller driver for SF21H8898 SoC");
