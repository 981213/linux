// SPDX-License-Identifier: GPL-2.0
/*
 * SF21 DPNS external NAT lookup (ENAPT) hardware.
 *
 * Internal NAPT rows are selected through eight small on-chip hash tables.
 * On a miss the engine makes a direct read from coherent DDR, using a
 * CRC16-derived row in a 2 MiB SNAT or DNAT array.  A
 * 64-byte row holds either two IPv4 actions or one IPv6 action.  The action
 * carries the complete translated tuple, L2/interface indices, output port
 * and a 16-bit visit-counter ID, so it does not consume router-IP RAM.
 *
 * Initialization allocates both coherent arrays below 4 GiB, programs their
 * base registers, selects one subtable for both address families and then
 * enables DDR lookup.  Insertion fills an invalid action, orders all coherent
 * writes with dma_wmb(), and publishes the valid bit last.  Deletion clears
 * valid first before recycling the row and visit ID.  This ordering prevents
 * the lookup engine from observing a partially initialized action.
 */

#include <linux/bitmap.h>
#include <linux/bitfield.h>
#include <linux/dma-mapping.h>
#include <linux/slab.h>

#include "dpns.h"
#include "sf_dpns_nat_ext.h"
#include "sf_dpns_table.h"

#define DPNS_NAT_EXT_BYTES		SZ_2M
#define DPNS_NAT_EXT_ROW_BYTES		64
#define DPNS_NAT_EXT_ROWS		(DPNS_NAT_EXT_BYTES / \
					 DPNS_NAT_EXT_ROW_BYTES)
/* 1/2/4/8 subtables differ only in the two access-count register fields. */
#define DPNS_NAT_EXT_ACCESS_LOG2	0
#define DPNS_NAT_EXT_SUBTABLES		BIT(DPNS_NAT_EXT_ACCESS_LOG2)
#define DPNS_NAT_EXT_SUBTABLE_ROWS	(DPNS_NAT_EXT_ROWS / \
					 DPNS_NAT_EXT_SUBTABLES)
#define DPNS_NAT_EXT_ID_BASE		8192
#define DPNS_NAT_EXT_IDS		(32768 - DPNS_NAT_EXT_ID_BASE)

#define DPNS_NAT_EXT_V4_SLOT0		BIT(0)
#define DPNS_NAT_EXT_V4_SLOT1		BIT(1)
#define DPNS_NAT_EXT_V6_ROW		BIT(2)

#define SE_NAT_CONFIG0			0x188004
#define  SE_NAT_EXT_V6_ACCESSES		GENMASK(14, 13)
#define  SE_NAT_EXT_V4_ACCESSES		GENMASK(12, 11)
#define  SE_NAT_EXT_TABLE_SIZE		GENMASK(10, 9)
#define  SE_NAT_EXT_DISABLE		BIT(6)
#define SE_NAT_DNAT_BASE		0x188010
#define SE_NAT_SNAT_BASE		0x188014

enum dpns_nat_ext_dir {
	DPNS_NAT_EXT_SNAT,
	DPNS_NAT_EXT_DNAT,
};

struct dpns_nat_ext {
	struct dpns_priv *priv;
	void *table[2];
	dma_addr_t dma[2];
	u8 *row_state[2];
	unsigned long *id_map;
};

static u16 dpns_nat_ext_crc16(const u8 *data, size_t len, bool poly1)
{
	u16 polynomial = poly1 ? 0x8005 : 0x1021;
	u16 crc = 0;
	unsigned int bit;

	while (len--) {
		crc ^= *data++ << 8;
		for (bit = 0; bit < 8; bit++)
			crc = crc & BIT(15) ?
				(crc << 1) ^ polynomial : crc << 1;
	}
	return crc;
}

static int dpns_nat_ext_find_row(struct dpns_nat_ext *ext,
				 const void *tuple, size_t tuple_len,
				 bool dnat, bool v6, u16 *index,
				 bool *second_slot)
{
	u8 *state = ext->row_state[dnat ? DPNS_NAT_EXT_DNAT :
					 DPNS_NAT_EXT_SNAT];
	unsigned int layer;

	for (layer = 0; layer < DPNS_NAT_EXT_SUBTABLES; layer++) {
		u16 crc = dpns_nat_ext_crc16(tuple, tuple_len, !(layer & 1));
		u16 row = layer * DPNS_NAT_EXT_SUBTABLE_ROWS +
			  (crc & (DPNS_NAT_EXT_SUBTABLE_ROWS - 1));

		if (v6) {
			if (!state[row]) {
				*index = row;
				*second_slot = false;
				return 0;
			}
			continue;
		}
		if (state[row] & DPNS_NAT_EXT_V6_ROW)
			continue;
		if (!(state[row] & DPNS_NAT_EXT_V4_SLOT0)) {
			*index = row;
			*second_slot = false;
			return 0;
		}
		if (!(state[row] & DPNS_NAT_EXT_V4_SLOT1)) {
			*index = row;
			*second_slot = true;
			return 0;
		}
	}
	return -ENOSPC;
}

static void dpns_nat_ext_build_v4(u32 *row,
				  const struct dpns_nat_ext_action *action,
				  u16 nat_id)
{
	dpns_table_field_set(row, 0, 32, action->public_ip[0]);
	dpns_table_field_set(row, 32, 16, action->public_port);
	dpns_table_field_set(row, 48, 32, action->private_ip[0]);
	dpns_table_field_set(row, 80, 16, action->private_port);
	dpns_table_field_set(row, 96, 32, action->router_ip[0]);
	dpns_table_field_set(row, 128, 16, action->router_port);
	dpns_table_field_set(row, 144, 1, action->l4_type);
	dpns_table_field_set(row, 145, 1, 0); /* published last */
	dpns_table_field_set(row, 146, 6, action->intf_index);
	if (action->dnat)
		dpns_table_field_set(row, 152, 11, action->mac_index);
	else
		dpns_table_field_set(row, 163, 11, action->mac_index);
	dpns_table_field_set(row, 197, 16, nat_id);
	dpns_table_field_set(row, 213, 5, action->output_port);
	dpns_table_field_set(row, 251, 5, 0x1f);
}

static void dpns_nat_ext_build_v6(u32 *row,
				  const struct dpns_nat_ext_action *action,
				  u16 nat_id)
{
	memcpy(row, action->public_ip, sizeof(action->public_ip));
	dpns_table_field_set(row, 128, 16, action->public_port);
	memcpy((u8 *)row + 18, action->private_ip, sizeof(action->private_ip));
	dpns_table_field_set(row, 272, 16, action->private_port);
	memcpy((u8 *)row + 36, action->router_ip, sizeof(action->router_ip));
	dpns_table_field_set(row, 416, 16, action->router_port);
	dpns_table_field_set(row, 432, 1, action->l4_type);
	dpns_table_field_set(row, 433, 1, 0); /* published last */
	dpns_table_field_set(row, 434, 6, action->intf_index);
	if (action->dnat)
		dpns_table_field_set(row, 440, 11, action->mac_index);
	else
		dpns_table_field_set(row, 451, 11, action->mac_index);
	dpns_table_field_set(row, 485, 16, nat_id);
	dpns_table_field_set(row, 501, 5, action->output_port);
	dpns_table_field_set(row, 506, 1, 1);
	dpns_table_field_set(row, 507, 5, 0x1f);
}

int dpns_nat_ext_add(struct dpns_priv *priv, const void *tuple,
		     size_t tuple_len, const struct dpns_nat_ext_action *action,
		     struct dpns_nat_ext_handle *handle)
{
	struct dpns_nat_ext *ext = priv->nat_ext;
	unsigned long id;
	bool second_slot;
	u16 index;
	u32 row[16] = {};
	u32 *dst;
	u8 dir, state;
	int ret;

	if (!ext || !tuple || !tuple_len || !action || !handle)
		return -EINVAL;
	id = find_first_zero_bit(ext->id_map, DPNS_NAT_EXT_IDS);
	if (id >= DPNS_NAT_EXT_IDS)
		return -ENOSPC;
	ret = dpns_nat_ext_find_row(ext, tuple, tuple_len, action->dnat,
				    action->v6, &index, &second_slot);
	if (ret)
		return ret;

	dir = action->dnat ? DPNS_NAT_EXT_DNAT : DPNS_NAT_EXT_SNAT;
	dst = ext->table[dir] + index * DPNS_NAT_EXT_ROW_BYTES;
	if (!action->v6)
		dst += second_slot ? 8 : 0;
	if (action->v6)
		dpns_nat_ext_build_v6(row, action, id + DPNS_NAT_EXT_ID_BASE);
	else
		dpns_nat_ext_build_v4(row, action, id + DPNS_NAT_EXT_ID_BASE);

	set_bit(id, ext->id_map);
	state = action->v6 ? DPNS_NAT_EXT_V6_ROW :
		(second_slot ? DPNS_NAT_EXT_V4_SLOT1 : DPNS_NAT_EXT_V4_SLOT0);
	ext->row_state[dir][index] |= state;
	memcpy(dst, row, action->v6 ? sizeof(row) : sizeof(row) / 2);
	dma_wmb();
	WRITE_ONCE(dst[action->v6 ? 13 : 4],
		   READ_ONCE(dst[action->v6 ? 13 : 4]) | BIT(17));
	dma_wmb();

	handle->nat_id = id + DPNS_NAT_EXT_ID_BASE;
	handle->index = index;
	handle->valid = true;
	handle->v6 = action->v6;
	handle->dnat = action->dnat;
	handle->second_slot = second_slot;
	return 0;
}

void dpns_nat_ext_del(struct dpns_priv *priv,
		      struct dpns_nat_ext_handle *handle)
{
	struct dpns_nat_ext *ext = priv->nat_ext;
	u8 dir, state;
	u32 *dst;

	if (!ext || !handle || !handle->valid ||
	    handle->nat_id < DPNS_NAT_EXT_ID_BASE)
		return;
	dir = handle->dnat ? DPNS_NAT_EXT_DNAT : DPNS_NAT_EXT_SNAT;
	dst = ext->table[dir] + handle->index * DPNS_NAT_EXT_ROW_BYTES;
	if (!handle->v6)
		dst += handle->second_slot ? 8 : 0;
	WRITE_ONCE(dst[handle->v6 ? 13 : 4],
		   READ_ONCE(dst[handle->v6 ? 13 : 4]) & ~BIT(17));
	dma_wmb();
	memset(dst, 0, handle->v6 ? DPNS_NAT_EXT_ROW_BYTES :
				 DPNS_NAT_EXT_ROW_BYTES / 2);
	dma_wmb();

	state = handle->v6 ? DPNS_NAT_EXT_V6_ROW :
		(handle->second_slot ? DPNS_NAT_EXT_V4_SLOT1 :
		 DPNS_NAT_EXT_V4_SLOT0);
	ext->row_state[dir][handle->index] &= ~state;
	clear_bit(handle->nat_id - DPNS_NAT_EXT_ID_BASE, ext->id_map);
	memset(handle, 0, sizeof(*handle));
}

int dpns_nat_ext_init(struct dpns_priv *priv)
{
	struct dpns_nat_ext *ext;
	unsigned int dir;
	int ret;

	ret = dma_set_mask_and_coherent(priv->dev, DMA_BIT_MASK(32));
	if (ret)
		return ret;
	ext = devm_kzalloc(priv->dev, sizeof(*ext), GFP_KERNEL);
	if (!ext)
		return -ENOMEM;
	ext->priv = priv;
	for (dir = 0; dir < ARRAY_SIZE(ext->table); dir++) {
		ext->table[dir] = dmam_alloc_coherent(priv->dev,
						      DPNS_NAT_EXT_BYTES,
						      &ext->dma[dir], GFP_KERNEL);
		if (!ext->table[dir])
			return -ENOMEM;
		ext->row_state[dir] = devm_kcalloc(priv->dev,
						   DPNS_NAT_EXT_ROWS,
						   sizeof(*ext->row_state[dir]),
						   GFP_KERNEL);
		if (!ext->row_state[dir])
			return -ENOMEM;
		memset(ext->table[dir], 0, DPNS_NAT_EXT_BYTES);
	}
	ext->id_map = devm_bitmap_zalloc(priv->dev, DPNS_NAT_EXT_IDS,
					 GFP_KERNEL);
	if (!ext->id_map)
		return -ENOMEM;
	priv->nat_ext = ext;

	dpns_w32(priv, SE_NAT_DNAT_BASE, lower_32_bits(ext->dma[DPNS_NAT_EXT_DNAT]));
	dpns_w32(priv, SE_NAT_SNAT_BASE, lower_32_bits(ext->dma[DPNS_NAT_EXT_SNAT]));
	dpns_rmw(priv, SE_NAT_CONFIG0,
		 SE_NAT_EXT_DISABLE | SE_NAT_EXT_TABLE_SIZE |
		 SE_NAT_EXT_V4_ACCESSES | SE_NAT_EXT_V6_ACCESSES,
		 FIELD_PREP(SE_NAT_EXT_TABLE_SIZE, 2) |
		 FIELD_PREP(SE_NAT_EXT_V4_ACCESSES,
			    DPNS_NAT_EXT_ACCESS_LOG2) |
		 FIELD_PREP(SE_NAT_EXT_V6_ACCESSES,
			    DPNS_NAT_EXT_ACCESS_LOG2));
	dev_info(priv->dev,
		 "external NAT lookup enabled: 2 MiB SNAT at %pad, 2 MiB DNAT at %pad, one subtable\n",
		 &ext->dma[DPNS_NAT_EXT_SNAT], &ext->dma[DPNS_NAT_EXT_DNAT]);
	return 0;
}

void dpns_nat_ext_fini(struct dpns_priv *priv)
{
	struct dpns_nat_ext *ext = priv->nat_ext;

	if (!ext)
		return;
	dpns_rmw(priv, SE_NAT_CONFIG0, 0, SE_NAT_EXT_DISABLE);
	dpns_w32(priv, SE_NAT_DNAT_BASE, 0);
	dpns_w32(priv, SE_NAT_SNAT_BASE, 0);
	priv->nat_ext = NULL;
}
