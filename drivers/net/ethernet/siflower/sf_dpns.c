#include <linux/bitfield.h>
#include <linux/clk.h>
#include <linux/iopoll.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/of_platform.h>
#include <linux/device.h>
#include <linux/debugfs.h>
#include "dpns.h"

static int dpns_populate_table(struct dpns_priv *priv)
{
	void __iomem *ioaddr = priv->ioaddr;
	int ret, i;
	u32 reg;

	writel(SE_CLR_RAM_CTRL_MASK, ioaddr + SE_CLR_RAM_CTRL);
	ret = readl_poll_timeout(ioaddr + SE_CLR_RAM_CTRL, reg, !reg, 0, 1000);
	if (ret)
		return ret;

	dpns_rmw(priv, SE_CONFIG0, SE_CONFIG0_IPSPL_ZERO_LIMIT,
		 SE_CONFIG0_IPORT_VALID);
	dpns_w32(priv, SE_TB_WRDATA0, 0xa0000);
	for (i = 0; i < 6; i++) {
		reg = SE_TB_OP_WR | FIELD_PREP(SE_TB_OP_REQ_ADDR, i) |
		      FIELD_PREP(SE_TB_OP_REQ_ID, SE_TB_IPORT_ID);
		dpns_w32(priv, SE_TB_OP, reg);
		ret = readl_poll_timeout(ioaddr + SE_TB_OP, reg,
					 !(reg & SE_TB_OP_BUSY), 0, 100);
		if (ret)
			return ret;
	}

	return 0;
}

static int dpns_probe(struct platform_device *pdev)
{
	struct dpns_priv *priv;
	int ret;

	priv = devm_kzalloc(&pdev->dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;
	priv->dev = &pdev->dev;
	priv->ioaddr = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(priv->ioaddr))
		return PTR_ERR(priv->ioaddr);

	priv->clk = devm_clk_get_enabled(priv->dev, NULL);
	if (IS_ERR(priv->clk))
		return PTR_ERR(priv->clk);
	
	ret = dpns_populate_table(priv);
	if (ret)
		return dev_err_probe(priv->dev, ret, "failed to populate NPU tables.\n");

	ret = dpns_tmu_init(priv);
	if (ret)
		return dev_err_probe(priv->dev, ret, "failed to initialize TMU.\n");

	sf_dpns_debugfs_init(priv);
	platform_set_drvdata(pdev, priv);
	return 0;
}

static int dpns_remove(struct platform_device *pdev) {
	struct dpns_priv *priv = platform_get_drvdata(pdev);
	debugfs_remove_recursive(priv->debugfs);
	return 0;
}

static const struct of_device_id dpns_match[] = {
	{ .compatible = "siflower,sf21-dpns" },
	{},
};
MODULE_DEVICE_TABLE(of, dpns_match);

static struct platform_driver dpns_driver = {
	.probe	= dpns_probe,
	.remove = dpns_remove,
	.driver	= {
		.name		= "sfdpns",
		.of_match_table	= dpns_match,
	},
};
module_platform_driver(dpns_driver);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Qingfang Deng <qingfang.deng@siflower.com.cn>");
MODULE_DESCRIPTION("NPU stub driver for SF21A6826/SF21H8898 SoC");