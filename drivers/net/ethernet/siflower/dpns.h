#ifndef __SF_DPNS_H__
#define __SF_DPNS_H__
#include <asm/mmio.h>
#include <linux/clk.h>
#include <linux/device.h>


#define SE_CLR_RAM_CTRL			0x180004
#define SE_CLR_RAM_CTRL_MASK		GENMASK(20, 0)
#define SE_CONFIG0			0x180008
#define SE_CONFIG0_IPSPL_ZERO_LIMIT	BIT(19)
#define SE_CONFIG0_IPORT_VALID		BIT(8)
#define SE_TB_OP			0x18003c
#define SE_TB_OP_BUSY			BIT(31)
#define SE_TB_OP_WR			BIT(24)
#define SE_TB_OP_REQ_ID			GENMASK(21, 16)
#define SE_TB_OP_REQ_ADDR		GENMASK(15, 0)
#define SE_TB_WRDATA0			0x180040

#define SE_TB_IPORT_ID			1

#define NPU_MIB_BASE			0x380000
#define NPU_MIB(x)			(NPU_MIB_BASE + (x) * 4)
#define NPU_MIB_PKT_RCV_PORT(x)		(NPU_MIB_BASE + 0x2000 + (x) * 4)
#define NPU_MIB_NCI_RD_DATA2		(NPU_MIB_BASE + 0x301c)
#define NPU_MIB_NCI_RD_DATA3		(NPU_MIB_BASE + 0x3020)

struct dpns_priv {
	void __iomem *ioaddr;
	struct clk *clk;
	struct device *dev;
	struct dentry *debugfs;
};

static inline u32 dpns_r32(struct dpns_priv *priv, unsigned reg)
{
	return readl(priv->ioaddr + reg);
}

static inline void dpns_w32(struct dpns_priv *priv, unsigned reg, u32 val)
{
	writel(val, priv->ioaddr + reg);
}

static inline void dpns_rmw(struct dpns_priv *priv, unsigned reg, u32 clr,
			    u32 set)
{
	u32 val = dpns_r32(priv, reg);
	val &= ~clr;
	val |= set;
	dpns_w32(priv, reg, val);
}

int dpns_tmu_init(struct dpns_priv *priv);
void sf_dpns_debugfs_init(struct dpns_priv *priv);

#endif /* __SF_DPNS_H__ */