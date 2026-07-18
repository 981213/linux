/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __SF_DPNS_TABLE_H__
#define __SF_DPNS_TABLE_H__

#include "dpns.h"

enum dpns_table_id {
	DPNS_TABLE_IVLAN_SPL		= 0,
	DPNS_TABLE_IVLAN_IPORT		= 1,
	DPNS_TABLE_IVLAN_XLT		= 2,
	DPNS_TABLE_IVLAN_PBV		= 3,
	DPNS_TABLE_IVLAN_LKP		= 4,
	DPNS_TABLE_L2_HASH0		= 5,
	DPNS_TABLE_L2_HASH1		= 6,
	DPNS_TABLE_L2_MAC		= 7,
	DPNS_TABLE_L2_MAC_SPL		= 8,
	DPNS_TABLE_INTF		= 9,
	DPNS_TABLE_EVLAN_OTPID		= 10,
	DPNS_TABLE_EVLAN_TPID		= 11,
	DPNS_TABLE_EVLAN_XLT		= 12,
	DPNS_TABLE_EVLAN_LKP		= 13,
	DPNS_TABLE_EVLAN_ACT		= 14,
	DPNS_TABLE_MODIFY_HEADER	= 16,
	DPNS_TABLE_L2_ISOLATION		= 17,
};

int dpns_table_read(struct dpns_priv *priv, u8 table, u16 index,
		    u32 *data, size_t size);
int dpns_table_write(struct dpns_priv *priv, u8 table, u16 index,
		     const u32 *data, size_t size);

/* SE table formats are densely packed, little-endian bit streams. */
static inline void dpns_table_field_set(u32 *data, unsigned int offset,
					unsigned int width, u64 value)
{
	while (width) {
		unsigned int shift = offset % 32;
		unsigned int chunk = min(width, 32U - shift);
		u32 mask = chunk == 32 ? U32_MAX : GENMASK(chunk - 1, 0);

		data[offset / 32] &= ~(mask << shift);
		data[offset / 32] |= (value & mask) << shift;
		value >>= chunk;
		offset += chunk;
		width -= chunk;
	}
}

#endif /* __SF_DPNS_TABLE_H__ */
