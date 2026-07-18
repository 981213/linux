// SPDX-License-Identifier: GPL-2.0
/*
 * SF21 DPNS Search Engine indirect table access.
 *
 * The Search Engine (SE) has a single indirect window shared by its ingress
 * VLAN, egress VLAN, L2 and interface RAMs.  A transaction first fills the
 * write window (for writes), submits a table ID and row address, waits for the
 * BUSY bit to clear, and then consumes the read window (for reads).  The mutex
 * below keeps multi-word rows from different hardware modules from mixing.
 */

#include <linux/bitfield.h>
#include <linux/iopoll.h>

#include "sf_dpns_table.h"

#define SE_TABLE_OP			0x18003c
#define  SE_TABLE_OP_BUSY		BIT(31)
#define  SE_TABLE_OP_WRITE		BIT(24)
#define  SE_TABLE_OP_ID		GENMASK(20, 16)
#define  SE_TABLE_OP_INDEX		GENMASK(15, 0)
#define SE_TABLE_WRDATA(n)		(0x180040 + 4 * (n))
#define SE_TABLE_RDDATA(n)		(0x180080 + 4 * (n))
#define SE_TABLE_WORDS			16

static int dpns_table_access(struct dpns_priv *priv, bool write, u8 table,
			     u16 index, u32 *data, size_t size)
{
	u32 op, val;
	unsigned int words, i;
	int ret;

	if (!size || size % sizeof(u32))
		return -EINVAL;

	words = size / sizeof(u32);
	if (words > SE_TABLE_WORDS || table > FIELD_MAX(SE_TABLE_OP_ID))
		return -EINVAL;

	op = FIELD_PREP(SE_TABLE_OP_ID, table) |
	     FIELD_PREP(SE_TABLE_OP_INDEX, index);
	if (write)
		op |= SE_TABLE_OP_WRITE;

	mutex_lock(&priv->table_lock);
	if (write)
		for (i = 0; i < words; i++)
			dpns_w32(priv, SE_TABLE_WRDATA(i), data[i]);

	dpns_w32(priv, SE_TABLE_OP, op);
	ret = readl_poll_timeout(priv->ioaddr + SE_TABLE_OP, val,
				 !(val & SE_TABLE_OP_BUSY), 1, 1000);
	if (!ret && !write)
		for (i = 0; i < words; i++)
			data[i] = dpns_r32(priv, SE_TABLE_RDDATA(i));
	mutex_unlock(&priv->table_lock);

	if (ret)
		dev_err_ratelimited(priv->dev,
				    "SE table %u index %u operation timed out\n",
				    table, index);

	return ret;
}

int dpns_table_read(struct dpns_priv *priv, u8 table, u16 index,
		    u32 *data, size_t size)
{
	return dpns_table_access(priv, false, table, index, data, size);
}

int dpns_table_write(struct dpns_priv *priv, u8 table, u16 index,
		     const u32 *data, size_t size)
{
	return dpns_table_access(priv, true, table, index, (u32 *)data, size);
}
