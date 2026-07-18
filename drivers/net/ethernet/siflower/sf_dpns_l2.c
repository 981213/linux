// SPDX-License-Identifier: GPL-2.0
/*
 * SF21 DPNS L2 lookup hardware.
 *
 * The block consists of a 2048-row MAC action RAM preceded by ten hash
 * subtables of decreasing widths.  Each hash row stores a MAC-RAM index.  To
 * install an address, software computes the vendor CRC16 for each subtable,
 * selects the first empty hash row, then writes the action row containing
 * MAC, VID, destination-port bitmap and forwarding actions.  Removal clears
 * both rows.  Linux bridge learning owns ageing; its FDB delete notifications
 * remove the corresponding hardware row.
 */

#include <linux/bitmap.h>
#include <linux/bitfield.h>
#include <linux/etherdevice.h>
#include <linux/if_vlan.h>
#include <linux/list.h>
#include <linux/slab.h>

#include "dpns.h"
#include "sf_dpns_l2.h"
#include "sf_dpns_se.h"
#include "sf_dpns_table.h"

#define DPNS_L2_MAC_ENTRIES	2048
#define DPNS_L2_HASH_LAYERS	10
#define DPNS_L2_PORTS		27

#define SE_L2_MPP_CFG2		0x24004
#define  SE_L2_BCAST_LOOKUP	BIT(9)
#define  SE_L2_8021X_FORCE_CPU	BIT(20)

enum dpns_l2_cml {
	DPNS_CML_DROP,
	DPNS_CML_TO_CPU,
	DPNS_CML_FORWARD,
	DPNS_CML_FORWARD_AND_CPU,
};

struct dpns_l2_entry {
	struct list_head list;
	u8 addr[ETH_ALEN];
	u16 vid;
	u16 mac_index;
	u16 hash_index;
	u8 hash_table;
	u32 port_mask;
	bool ageing;
};

struct dpns_l2 {
	struct dpns_priv *priv;
	DECLARE_BITMAP(mac_map, DPNS_L2_MAC_ENTRIES);
	struct list_head entries;
};

static const u8 dpns_l2_hash_width[DPNS_L2_HASH_LAYERS] = {
	10, 9, 9, 8, 8, 7, 7, 7, 6, 6,
};

static const u8 dpns_l2_hash_poly[DPNS_L2_HASH_LAYERS] = {
	0, 1, 0, 1, 0, 1, 0, 1, 0, 1,
};

static u16 dpns_l2_crc16(const u8 *data, size_t len, u8 poly_sel)
{
	static const u16 polynomial[] = {
		0x1021, 0x8005, 0xa097, 0x8bb7,
		0xc867, 0x3d65, 0x0589, 0x509d,
	};
	u16 crc = 0;
	unsigned int bit;

	while (len--) {
		crc ^= *data++ << 8;
		for (bit = 0; bit < 8; bit++)
			crc = crc & BIT(15) ?
				(crc << 1) ^ polynomial[poly_sel] : crc << 1;
	}

	return crc;
}

static struct dpns_l2_entry *dpns_l2_find(struct dpns_l2 *l2,
					  const u8 *addr, u16 vid)
{
	struct dpns_l2_entry *entry;

	list_for_each_entry(entry, &l2->entries, list)
		if (entry->vid == vid && ether_addr_equal(entry->addr, addr))
			return entry;

	return NULL;
}

static int dpns_l2_write_mac(struct dpns_l2 *l2,
			     const struct dpns_l2_entry *entry)
{
	u32 row[5] = {};
	u64 mac = ether_addr_to_u64(entry->addr);

	/* See vendor tbl_mac: port bitmap starts at bit 41, MAC at bit 68. */
	dpns_table_field_set(row, 21, 1, entry->ageing);
	dpns_table_field_set(row, 22, 2, DPNS_CML_FORWARD);
	dpns_table_field_set(row, 24, 2, DPNS_CML_FORWARD);
	dpns_table_field_set(row, 41, 27, entry->port_mask);
	dpns_table_field_set(row, 68, 48, mac);
	dpns_table_field_set(row, 116, 12, entry->vid);
	dpns_table_field_set(row, 129, 1, 1);

	return dpns_table_write(l2->priv, DPNS_TABLE_L2_MAC,
				entry->mac_index, row, sizeof(row));
}

static int dpns_l2_alloc_hash(struct dpns_l2 *l2, const u8 *addr,
			      u8 *table, u16 *index)
{
	u16 offset = 0;
	unsigned int layer;

	for (layer = 0; layer < DPNS_L2_HASH_LAYERS; layer++) {
		u8 id = layer < 3 ? DPNS_TABLE_L2_HASH0 : DPNS_TABLE_L2_HASH1;
		u16 crc, row_index;
		u32 row;
		int ret;

		if (layer == 3)
			offset = 0;

		crc = dpns_l2_crc16(addr, ETH_ALEN, dpns_l2_hash_poly[layer]);
		row_index = offset + (crc & (BIT(dpns_l2_hash_width[layer]) - 1));
		ret = dpns_table_read(l2->priv, id, row_index, &row, sizeof(row));
		if (ret)
			return ret;
		if (!row) {
			*table = id;
			*index = row_index;
			return 0;
		}

		offset += BIT(dpns_l2_hash_width[layer]);
	}

	return -ENOSPC;
}

static int __dpns_l2_addr_set(struct dpns_l2 *l2, const u8 *addr, u16 vid,
			      u32 port_mask, bool ageing)
{
	struct dpns_l2_entry *entry;
	unsigned long mac_index;
	u32 hash_row;
	int ret;

	if (!is_valid_ether_addr(addr) && !is_multicast_ether_addr(addr))
		return -EINVAL;
	if (vid >= VLAN_N_VID || port_mask & ~GENMASK(DPNS_L2_PORTS - 1, 0))
		return -EINVAL;

	entry = dpns_l2_find(l2, addr, vid);
	if (entry) {
		entry->port_mask = port_mask;
		entry->ageing = ageing;
		return dpns_l2_write_mac(l2, entry);
	}

	mac_index = find_next_zero_bit(l2->mac_map, DPNS_L2_MAC_ENTRIES, 1);
	if (mac_index >= DPNS_L2_MAC_ENTRIES)
		return -ENOSPC;

	entry = kzalloc_obj(*entry, GFP_KERNEL);
	if (!entry)
		return -ENOMEM;

	ret = dpns_l2_alloc_hash(l2, addr, &entry->hash_table,
				 &entry->hash_index);
	if (ret)
		goto err_free;

	ether_addr_copy(entry->addr, addr);
	entry->vid = vid;
	entry->mac_index = mac_index;
	entry->port_mask = port_mask;
	entry->ageing = ageing;
	hash_row = mac_index;

	ret = dpns_table_write(l2->priv, entry->hash_table,
			       entry->hash_index, &hash_row, sizeof(hash_row));
	if (ret)
		goto err_free;

	ret = dpns_l2_write_mac(l2, entry);
	if (ret) {
		hash_row = 0;
		dpns_table_write(l2->priv, entry->hash_table,
				 entry->hash_index, &hash_row, sizeof(hash_row));
		goto err_free;
	}

	set_bit(mac_index, l2->mac_map);
	list_add_tail(&entry->list, &l2->entries);
	return 0;

err_free:
	kfree(entry);
	return ret;
}

static int dpns_l2_entry_remove(struct dpns_l2 *l2,
				struct dpns_l2_entry *entry)
{
	u32 mac_row[5] = {};
	u32 hash_row = 0;
	int ret, ret2;

	ret = dpns_table_write(l2->priv, DPNS_TABLE_L2_MAC,
			       entry->mac_index, mac_row, sizeof(mac_row));
	ret2 = dpns_table_write(l2->priv, entry->hash_table,
				entry->hash_index, &hash_row, sizeof(hash_row));
	if (!ret)
		ret = ret2;

	clear_bit(entry->mac_index, l2->mac_map);
	list_del(&entry->list);
	kfree(entry);
	return ret;
}

int dpns_l2_addr_set(struct dpns_priv *priv, const u8 *addr, u16 vid,
		     u32 port_mask, bool ageing)
{
	return __dpns_l2_addr_set(priv->l2, addr, vid, port_mask, ageing);
}

int dpns_l2_addr_add_ports(struct dpns_priv *priv, const u8 *addr, u16 vid,
			   u32 port_mask)
{
	struct dpns_l2_entry *entry = dpns_l2_find(priv->l2, addr, vid);

	if (entry)
		port_mask |= entry->port_mask;
	return __dpns_l2_addr_set(priv->l2, addr, vid, port_mask, false);
}

int dpns_l2_addr_del_ports(struct dpns_priv *priv, const u8 *addr, u16 vid,
			   u32 port_mask)
{
	struct dpns_l2 *l2 = priv->l2;
	struct dpns_l2_entry *entry = dpns_l2_find(l2, addr, vid);

	if (!entry)
		return 0;

	entry->port_mask &= ~port_mask;
	if (entry->port_mask)
		return dpns_l2_write_mac(l2, entry);

	return dpns_l2_entry_remove(l2, entry);
}

void dpns_l2_flush_port(struct dpns_priv *priv, unsigned int port)
{
	struct dpns_l2 *l2 = priv->l2;
	struct dpns_l2_entry *entry, *tmp;

	list_for_each_entry_safe(entry, tmp, &l2->entries, list) {
		if (!(entry->port_mask & BIT(port)))
			continue;
		entry->port_mask &= ~BIT(port);
		if (entry->port_mask)
			dpns_l2_write_mac(l2, entry);
		else
			dpns_l2_entry_remove(l2, entry);
	}
}

void dpns_l2_flush_all(struct dpns_priv *priv)
{
	struct dpns_l2 *l2 = priv->l2;
	struct dpns_l2_entry *entry, *tmp;

	list_for_each_entry_safe(entry, tmp, &l2->entries, list)
		dpns_l2_entry_remove(l2, entry);
}

int dpns_l2_init(struct dpns_priv *priv)
{
	struct dpns_l2 *l2;
	u32 isolation[2] = {};
	unsigned int i;
	int ret;

	l2 = devm_kzalloc(priv->dev, sizeof(*l2), GFP_KERNEL);
	if (!l2)
		return -ENOMEM;
	l2->priv = priv;
	INIT_LIST_HEAD(&l2->entries);
	set_bit(0, l2->mac_map); /* Hardware reserves row zero. */
	priv->l2 = l2;

	dpns_rmw(priv, SE_CONFIG2, SE_L2_AGE_CLR_AFTER_RD |
		 SE_L2_SEG_NUM_MINUS1 | SE_MACSPL_MODE | SE_MAC_TABLE_VALID,
		 SE_MACSPL_MODE | SE_MAC_TABLE_VALID |
		 FIELD_PREP(SE_L2_SEG_NUM_MINUS1, 9));
	dpns_w32(priv, SE_CONFIG1, 0x48208208);
	dpns_rmw(priv, SE_L2_MPP_CFG2, 0,
		 SE_L2_BCAST_LOOKUP | SE_L2_8021X_FORCE_CPU);

	/* A set isolation bit permits the corresponding destination port. */
	dpns_table_field_set(isolation, 0, 27, GENMASK(26, 0));
	dpns_table_field_set(isolation, 27, 27, GENMASK(26, 0));
	for (i = 0; i < 64; i++) {
		ret = dpns_table_write(priv, DPNS_TABLE_L2_ISOLATION, i,
				       isolation, sizeof(isolation));
		if (ret)
			return ret;
	}

	return 0;
}

void dpns_l2_fini(struct dpns_priv *priv)
{
	if (priv->l2)
		dpns_l2_flush_all(priv);
}
