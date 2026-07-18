// SPDX-License-Identifier: GPL-2.0
/*
 * SF21 DPNS routed-interface hardware.
 *
 * A NAT result does not carry a replacement source MAC directly.  Its
 * six-bit interface index selects this 64-row table, whose row contains the
 * egress source MAC, outer VID and WAN attribute.  Optional PPPoE/tunnel bits
 * select a companion modify-header row; physical untagged routing keeps those
 * bits clear.  Configuration therefore consists of allocating/deduplicating
 * a row, writing it before the NAT result becomes visible, and releasing it
 * after the last flow which references it is removed.
 */

#include <linux/etherdevice.h>
#include <linux/if_vlan.h>
#include <linux/mutex.h>
#include <linux/slab.h>

#include "dpns.h"
#include "sf_dpns_intf.h"
#include "sf_dpns_modhdr.h"
#include "sf_dpns_se.h"
#include "sf_dpns_table.h"

#define DPNS_INTF_ENTRIES	64

struct dpns_intf_entry {
	struct dpns_intf_cfg cfg;
	u16 refs;
};

struct dpns_intf {
	struct dpns_priv *priv;
	struct mutex lock;
	struct dpns_intf_entry entries[DPNS_INTF_ENTRIES];
};

static bool dpns_intf_cfg_equal(const struct dpns_intf_cfg *a,
				const struct dpns_intf_cfg *b)
{
	if (a->vid != b->vid || a->wan != b->wan ||
	    a->modhdr.type != b->modhdr.type ||
	    !ether_addr_equal(a->src, b->src))
		return false;

	switch (a->modhdr.type) {
	case DPNS_MODHDR_NONE:
		return true;
	case DPNS_MODHDR_PPPOE:
		return a->modhdr.pppoe.sid == b->modhdr.pppoe.sid &&
		       a->modhdr.pppoe.proto == b->modhdr.pppoe.proto;
	default:
		return false;
	}
}

static int dpns_intf_write(struct dpns_intf *intf, u8 index,
			   const struct dpns_intf_entry *entry)
{
	u32 row[4] = {};

	if (entry && entry->refs) {
		dpns_table_field_set(row, 0, 12, entry->cfg.vid);
		dpns_table_field_set(row, 12, 48,
				     ether_addr_to_u64(entry->cfg.src));
		dpns_table_field_set(row, 61, 1,
				     entry->cfg.modhdr.type == DPNS_MODHDR_PPPOE);
		dpns_table_field_set(row, 62, 1, entry->cfg.wan);
		dpns_table_field_set(row, 63, 1, 1);
	}

	return dpns_table_write(intf->priv, DPNS_TABLE_INTF, index, row,
				sizeof(row));
}

int dpns_intf_get(struct dpns_priv *priv, const struct dpns_intf_cfg *cfg,
		  u8 *index)
{
	struct dpns_intf *intf = priv->intf;
	int free = -1;
	int ret = 0;
	u8 i;

	if (!cfg || !is_valid_ether_addr(cfg->src) ||
	    cfg->vid >= VLAN_N_VID || !index)
		return -EINVAL;

	mutex_lock(&intf->lock);
	for (i = 0; i < DPNS_INTF_ENTRIES; i++) {
		struct dpns_intf_entry *entry = &intf->entries[i];

		if (!entry->refs) {
			if (free < 0)
				free = i;
			continue;
		}
		if (!dpns_intf_cfg_equal(&entry->cfg, cfg))
			continue;
		if (entry->refs == U16_MAX) {
			ret = -EOVERFLOW;
			goto out;
		}
		entry->refs++;
		*index = i;
		goto out;
	}

	if (free < 0) {
		ret = -ENOSPC;
		goto out;
	}
	intf->entries[free].cfg = *cfg;
	intf->entries[free].refs = 1;
	ret = dpns_modhdr_write(priv, free, &cfg->modhdr);
	if (ret)
		goto err_clear;
	ret = dpns_intf_write(intf, free, &intf->entries[free]);
	if (ret) {
		dpns_modhdr_clear(priv, free);
		goto err_clear;
	}
	*index = free;
	goto out;

err_clear:
	memset(&intf->entries[free], 0, sizeof(intf->entries[free]));
out:
	mutex_unlock(&intf->lock);
	return ret;
}

void dpns_intf_put(struct dpns_priv *priv, u8 index)
{
	struct dpns_intf *intf = priv->intf;
	struct dpns_intf_entry *entry;

	if (!intf || index >= DPNS_INTF_ENTRIES)
		return;
	mutex_lock(&intf->lock);
	entry = &intf->entries[index];
	if (entry->refs && !--entry->refs) {
		dpns_intf_write(intf, index, NULL);
		dpns_modhdr_clear(priv, index);
		memset(entry, 0, sizeof(*entry));
	}
	mutex_unlock(&intf->lock);
}

int dpns_intf_init(struct dpns_priv *priv)
{
	struct dpns_intf *intf;

	intf = devm_kzalloc(priv->dev, sizeof(*intf), GFP_KERNEL);
	if (!intf)
		return -ENOMEM;
	intf->priv = priv;
	mutex_init(&intf->lock);
	priv->intf = intf;

	dpns_rmw(priv, SE_CONFIG2, 0, SE_INTF_TABLE_VALID);
	return 0;
}

void dpns_intf_fini(struct dpns_priv *priv)
{
	struct dpns_intf *intf = priv->intf;
	u8 i;

	if (!intf)
		return;
	for (i = 0; i < DPNS_INTF_ENTRIES; i++)
		if (intf->entries[i].refs) {
			dpns_intf_write(intf, i, NULL);
			dpns_modhdr_clear(priv, i);
		}
	priv->intf = NULL;
}
