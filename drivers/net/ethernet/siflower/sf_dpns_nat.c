// SPDX-License-Identifier: GPL-2.0
/*
 * SF21 DPNS NAT and routed-flow engine.
 *
 * Packets are classified as SNAT-side or DNAT-side before one of two sets of
 * eight CRC16 hash subtables is searched.  A hash hit selects an internal
 * NAPT row.  IPv4 rows occupy either half of a 192-bit NAPT0/NAPT1 pair;
 * IPv6 rows occupy the combined 448-bit NAPT01 row.  The row describes the
 * private, remote (called "public" by the hardware), and translated/router
 * tuples plus egress L2 and interface-table indices.  Programming order is:
 * reserve NAPT/router-IP resources, acquire L2 and interface rows, write the
 * NAPT action, then publish its hash.  Removal clears the hash first.
 *
 * TCP and UDP use symmetric matching: all five tuple fields participate in
 * the hash.  Basic, full-cone, host-restricted and port-restricted NAT differ
 * only in the mode registers and in which tuple fields software zeroes before
 * hashing; they are intentionally not exposed yet.  IPv6 without translation
 * uses the hardware L2-forward mode, whose default mode 0 hashes destination
 * IP only and leaves L3 addresses and L4 ports unchanged.
 *
 * Direction selection can use configured private IPv4/IPv6 prefixes (up to
 * eight of each) or the ingress WAN-port table.  Linux has no generic LAN/WAN
 * netdev attribute, so this physical-port-only driver selects port mode and
 * derives the single WAN port from the flowtable route: SNAT-side egress and
 * DNAT-side ingress are WAN.  For an untranslated IPv6 flow, conntrack's
 * original direction is treated as LAN-to-WAN.  A later contradictory route
 * is rejected instead of silently changing the global hardware direction.
 *
 * NAT results contain L2 MAC-table and routed-interface indices rather than
 * literal Ethernet addresses.  Consequently every offloaded flow also holds
 * references in the L2 and interface modules.  This is why the vendor NAT
 * implementation manipulates L2 entries: the dependency is part of the
 * hardware action format, not bridge learning duplicated in the NAT layer.
 */

#include <linux/bitmap.h>
#include <linux/bitfield.h>
#include <linux/debugfs.h>
#include <linux/etherdevice.h>
#include <linux/if_ether.h>
#include <linux/if_vlan.h>
#include <linux/iopoll.h>
#include <linux/ip.h>
#include <linux/ipv6.h>
#include <linux/list.h>
#include <linux/mutex.h>
#include <linux/module.h>
#include <linux/seq_file.h>
#include <linux/slab.h>
#include <linux/tcp.h>
#include <linux/udp.h>
#include <net/flow_offload.h>
#include <net/pkt_cls.h>

#include "dpns.h"
#include "sf_dpns_intf.h"
#include "sf_dpns_l2.h"
#include "sf_dpns_nat.h"
#include "sf_dpns_port.h"
#include "sf_dpns_table.h"

#define DPNS_NAT_ROWS			4096
#define DPNS_NAT_HASH_LAYERS		8
#define DPNS_NAT_ROUTER_V4_ENTRIES	8
#define DPNS_NAT_ROUTER_V6_ENTRIES	8
#define DPNS_NAT_ROUTER_V6_BASE		8

#define NPU_NAT_MPP_CFG			0x00000
#define  NAT_MPP_CFG_BYPASS		BIT(24)
#define NPU_NAT_HOST_MODE		0x000bc
#define  NAT_HOST_V6_LF_MODE		BIT(29)
#define  NAT_HOST_V4_LF_MODE		BIT(28)
#define  NAT_HOST_PORT_DIRECTION	BIT(27)
#define  NAT_HOST_TCP_MODE		GENMASK(26, 24)
#define  NAT_HOST_UDP_MODE		GENMASK(21, 19)

#define NPU_L3_MPP_CFG			0x20000
#define  NPU_L3_CARE_NAT_RESULT		BIT(19)

#define SE_WAN_DATA0			0x1800c0
#define  SE_WAN_VALID			BIT(17)
#define  SE_WAN_VID			GENMASK(16, 5)
#define  SE_WAN_PORT			GENMASK(4, 0)

#define SE_NAT_CONFIG0			0x188004
#define  SE_NAT_DNAT_SUBTABLES		GENMASK(31, 24)
#define  SE_NAT_SNAT_SUBTABLES		GENMASK(23, 16)
#define  SE_NAT_DISABLE_DDR		BIT(6)
#define  SE_NAT_UDP_MODE		GENMASK(2, 0)
#define SE_NAT_CONFIG1			0x188008
#define  SE_NAT_V6_LF_ENABLE		BIT(31)
#define  SE_NAT_V4_LF_ENABLE		BIT(30)
#define  SE_NAT_V4_LF_MODE		GENMASK(29, 27)
#define  SE_NAT_TCP_MODE		GENMASK(26, 24)
#define  SE_NAT_HASH_POLY(x)		GENMASK((x) * 3 + 2, (x) * 3)
#define SE_NAT_CONFIG5			0x188018
#define  SE_NAT_V6_LF_MODE		GENMASK(14, 12)
#define SE_NAT_CONFIG6			0x18801c
#define SE_NAT_CLEAR			0x188024
#define SE_NAT_TABLE_OP			0x18803c
#define  SE_NAT_TABLE_BUSY		BIT(31)
#define  SE_NAT_TABLE_WRITE		BIT(24)
#define  SE_NAT_TABLE_ID		GENMASK(19, 16)
#define  SE_NAT_TABLE_INDEX		GENMASK(12, 0)
#define SE_NAT_WRITE_DATA(n)		(0x188040 + 4 * (n))
#define SE_NAT_READ_DATA(n)		(0x188080 + 4 * (n))
#define SE_NAT_VISIT(n)		(0x189000 + 4 * (n))

enum dpns_nat_table_id {
	DPNS_NAT_DNAT_HASH0,
	DPNS_NAT_DNAT_HASH1,
	DPNS_NAT_SNAT_HASH0,
	DPNS_NAT_SNAT_HASH1,
	DPNS_NAT_NAPT0,
	DPNS_NAT_NAPT1,
	DPNS_NAT_NAPT01,
	DPNS_NAT_SPEED_LIMIT,
	DPNS_NAT_ROUTER_IP0,
	DPNS_NAT_ROUTER_IP1,
};

enum dpns_nat_mode {
	DPNS_NAT_MODE_BASIC,
	DPNS_NAT_MODE_SYMMETRIC,
	DPNS_NAT_MODE_FULL_CONE,
	DPNS_NAT_MODE_HOST_RESTRICTED,
	DPNS_NAT_MODE_PORT_RESTRICTED,
};

struct dpns_nat_hash_tuple {
	u8 l4_type;
	__be16 sport;
	union {
		struct in6_addr sip6;
		struct {
			__be32 sip4_pad[3];
			__be32 sip4;
		};
	};
	__be16 dport;
	union {
		struct in6_addr dip6;
		struct {
			__be32 dip4_pad[3];
			__be32 dip4;
		};
	};
} __packed;

struct dpns_nat_data {
	union {
		struct {
			u32 private_ip;
			u32 public_ip;
			u32 router_ip;
		};
		struct {
			u32 private_ip6[4];
			u32 public_ip6[4];
			u32 router_ip6[4];
		};
	};
	u16 private_port;
	u16 public_port;
	u16 router_port;
	u16 mac_index;
	u16 vid;
	u8 intf_index;
	u8 ingress_port;
	u8 output_port;
	u8 l4_type;
	bool v6;
	bool dnat;
	bool l2_forward;
	u8 src_mac[ETH_ALEN];
	u8 dst_mac[ETH_ALEN];
};

struct dpns_nat_router4 {
	u32 addr;
	u16 refs;
};

struct dpns_nat_router6 {
	u32 addr[4];
	u16 refs;
};

struct dpns_nat_entry {
	struct list_head list;
	unsigned long cookie;
	u16 row;
	u16 hash_index;
	u16 nat_id;
	u8 hash_table;
	u8 router_index;
	u8 intf_index;
	u8 output_port;
	u16 vid;
	bool second_slot;
	bool v6;
	bool dnat;
	bool l2_forward;
	u8 dst_mac[ETH_ALEN];
	u64 lastused;
};

struct dpns_nat {
	struct dpns_priv *priv;
	struct mutex lock;
	struct list_head entries;
	struct list_head block_cb_list;
	DECLARE_BITMAP(napt0_map, DPNS_NAT_ROWS);
	DECLARE_BITMAP(napt1_map, DPNS_NAT_ROWS);
	struct dpns_nat_router4 router4[DPNS_NAT_ROUTER_V4_ENTRIES];
	struct dpns_nat_router6 router6[DPNS_NAT_ROUTER_V6_ENTRIES];
	int wan_port;
};

static const u16 dpns_nat_hash_mask[DPNS_NAT_HASH_LAYERS] = {
	0xfff, 0x7ff, 0x7ff, 0x3ff, 0x3ff, 0x3ff, 0x1ff, 0x1ff,
};

static const u16 dpns_nat_hash_offset[DPNS_NAT_HASH_LAYERS] = {
	0, 0x1000, 0x1800, 0, 0x400, 0x800, 0xc00, 0xe00,
};

static const u16 dpns_nat_crc_poly[DPNS_NAT_HASH_LAYERS] = {
	0x1021, 0x8005, 0xa097, 0x8bb7,
	0xc867, 0x3d65, 0x0589, 0x509d,
};

static u16 dpns_nat_crc16(const u8 *data, size_t len, u8 poly)
{
	u16 crc = 0;
	unsigned int bit;

	while (len--) {
		crc ^= *data++ << 8;
		for (bit = 0; bit < 8; bit++)
			crc = crc & BIT(15) ?
				(crc << 1) ^ dpns_nat_crc_poly[poly] : crc << 1;
	}
	return crc;
}

static int dpns_nat_table_access(struct dpns_nat *nat, bool write, u8 table,
				 u16 index, u32 *data, unsigned int words)
{
	struct dpns_priv *priv = nat->priv;
	u32 val;
	unsigned int i;
	int ret;

	if (!words || words > 16)
		return -EINVAL;
	if (write)
		for (i = 0; i < words; i++)
			dpns_w32(priv, SE_NAT_WRITE_DATA(i), data[i]);
	dpns_w32(priv, SE_NAT_TABLE_OP,
		 FIELD_PREP(SE_NAT_TABLE_ID, table) |
		 FIELD_PREP(SE_NAT_TABLE_INDEX, index) |
		 (write ? SE_NAT_TABLE_WRITE : 0));
	ret = readl_poll_timeout(priv->ioaddr + SE_NAT_TABLE_OP, val,
				 !(val & SE_NAT_TABLE_BUSY), 1, 1000);
	if (!ret && !write)
		for (i = 0; i < words; i++)
			data[i] = dpns_r32(priv, SE_NAT_READ_DATA(i));
	if (ret)
		dev_err_ratelimited(priv->dev,
				    "NAT table %u row %u timed out\n",
				    table, index);
	return ret;
}

static int dpns_nat_router_get4(struct dpns_nat *nat, u32 addr, u8 *index)
{
	int free = -1;
	u32 data = addr;
	u8 i;
	int ret;

	for (i = 0; i < ARRAY_SIZE(nat->router4); i++) {
		if (!nat->router4[i].refs) {
			if (free < 0)
				free = i;
			continue;
		}
		if (nat->router4[i].addr == addr) {
			nat->router4[i].refs++;
			*index = i;
			return 0;
		}
	}
	if (free < 0)
		return -ENOSPC;
	ret = dpns_nat_table_access(nat, true, DPNS_NAT_ROUTER_IP0, free,
				    &data, 1);
	if (ret)
		return ret;
	ret = dpns_nat_table_access(nat, true, DPNS_NAT_ROUTER_IP1, free,
				    &data, 1);
	if (ret)
		return ret;
	nat->router4[free].addr = addr;
	nat->router4[free].refs = 1;
	*index = free;
	return 0;
}

static int dpns_nat_router_get6(struct dpns_nat *nat, const u32 *addr,
				u8 *index)
{
	int free = -1;
	u8 i;
	int ret;

	for (i = 0; i < ARRAY_SIZE(nat->router6); i++) {
		if (!nat->router6[i].refs) {
			if (free < 0)
				free = i;
			continue;
		}
		if (!memcmp(nat->router6[i].addr, addr, sizeof(nat->router6[i].addr))) {
			nat->router6[i].refs++;
			*index = i + DPNS_NAT_ROUTER_V6_BASE;
			return 0;
		}
	}
	if (free < 0)
		return -ENOSPC;
	ret = dpns_nat_table_access(nat, true, DPNS_NAT_ROUTER_IP1,
				    free + DPNS_NAT_ROUTER_V6_BASE,
				    (u32 *)addr, 4);
	if (ret)
		return ret;
	memcpy(nat->router6[free].addr, addr, sizeof(nat->router6[free].addr));
	nat->router6[free].refs = 1;
	*index = free + DPNS_NAT_ROUTER_V6_BASE;
	return 0;
}

static void dpns_nat_router_put(struct dpns_nat *nat, bool v6, u8 index)
{
	u32 zero[4] = {};

	if (!v6) {
		if (index >= ARRAY_SIZE(nat->router4) ||
		    !nat->router4[index].refs || --nat->router4[index].refs)
			return;
		dpns_nat_table_access(nat, true, DPNS_NAT_ROUTER_IP0,
				      index, zero, 1);
		dpns_nat_table_access(nat, true, DPNS_NAT_ROUTER_IP1,
				      index, zero, 1);
		memset(&nat->router4[index], 0, sizeof(nat->router4[index]));
		return;
	}
	if (index < DPNS_NAT_ROUTER_V6_BASE)
		return;
	index -= DPNS_NAT_ROUTER_V6_BASE;
	if (index >= ARRAY_SIZE(nat->router6) ||
	    !nat->router6[index].refs || --nat->router6[index].refs)
		return;
	dpns_nat_table_access(nat, true, DPNS_NAT_ROUTER_IP1,
			      index + DPNS_NAT_ROUTER_V6_BASE, zero, 4);
	memset(&nat->router6[index], 0, sizeof(nat->router6[index]));
}

static int dpns_nat_alloc_row(struct dpns_nat *nat, bool v6,
			      struct dpns_nat_entry *entry)
{
	unsigned long row;

	if (v6) {
		for (row = 1; row < DPNS_NAT_ROWS; row++)
			if (!test_bit(row, nat->napt0_map) &&
			    !test_bit(row, nat->napt1_map))
				break;
		if (row == DPNS_NAT_ROWS)
			return -ENOSPC;
		set_bit(row, nat->napt0_map);
		set_bit(row, nat->napt1_map);
		entry->row = row;
		entry->nat_id = row << 1;
		entry->v6 = true;
		return 0;
	}

	for (row = 1; row < DPNS_NAT_ROWS; row++) {
		if (!test_bit(row, nat->napt0_map)) {
			set_bit(row, nat->napt0_map);
			entry->row = row;
			entry->nat_id = row << 1;
			return 0;
		}
		if (!test_bit(row, nat->napt1_map)) {
			set_bit(row, nat->napt1_map);
			entry->row = row;
			entry->nat_id = (row << 1) | 1;
			entry->second_slot = true;
			return 0;
		}
	}
	return -ENOSPC;
}

static void dpns_nat_free_row(struct dpns_nat *nat,
			      const struct dpns_nat_entry *entry)
{
	u32 zero[14] = {};

	if (entry->v6) {
		dpns_nat_table_access(nat, true, DPNS_NAT_NAPT01, entry->row,
				      zero, ARRAY_SIZE(zero));
		clear_bit(entry->row, nat->napt0_map);
		clear_bit(entry->row, nat->napt1_map);
	} else {
		dpns_nat_table_access(nat, true,
				      entry->second_slot ? DPNS_NAT_NAPT1 :
				      DPNS_NAT_NAPT0, entry->row, zero, 6);
		clear_bit(entry->row, entry->second_slot ? nat->napt1_map :
			  nat->napt0_map);
	}
}

static int dpns_nat_write_row(struct dpns_nat *nat,
			      const struct dpns_nat_entry *entry,
			      const struct dpns_nat_data *data)
{
	u32 row[14] = {};

	if (!data->v6) {
		dpns_table_field_set(row, 0, 32, data->public_ip);
		dpns_table_field_set(row, 32, 16, data->public_port);
		dpns_table_field_set(row, 48, 32, data->private_ip);
		dpns_table_field_set(row, 80, 16, data->private_port);
		dpns_table_field_set(row, 96, 4, entry->router_index);
		dpns_table_field_set(row, 100, 16, data->router_port);
		dpns_table_field_set(row, 116, 1, data->l4_type);
		dpns_table_field_set(row, 117, 1, 1);
		if (data->dnat) {
			dpns_table_field_set(row, 124, 6, data->intf_index);
			dpns_table_field_set(row, 130, 11, data->mac_index);
			dpns_table_field_set(row, 180, 5, data->output_port);
		} else {
			dpns_table_field_set(row, 118, 6, data->intf_index);
			dpns_table_field_set(row, 141, 11, data->mac_index);
			dpns_table_field_set(row, 175, 5, data->output_port);
		}
		return dpns_nat_table_access(nat, true,
				entry->second_slot ? DPNS_NAT_NAPT1 :
				DPNS_NAT_NAPT0, entry->row, row, 6);
	}

	/* dpns_table_field_set accepts 64 bits, so fill IPv6 as four words. */
	memcpy(row, data->public_ip6, sizeof(data->public_ip6));
	dpns_table_field_set(row, 128, 16, data->public_port);
	memcpy((u8 *)row + 18, data->private_ip6, sizeof(data->private_ip6));
	dpns_table_field_set(row, 272, 16, data->private_port);
	dpns_table_field_set(row, 288, 4, entry->router_index);
	dpns_table_field_set(row, 292, 16, data->router_port);
	dpns_table_field_set(row, 308, 1, data->l4_type);
	dpns_table_field_set(row, 309, 1, 1);
	if (data->dnat) {
		dpns_table_field_set(row, 316, 6, data->intf_index);
		dpns_table_field_set(row, 322, 11, data->mac_index);
		dpns_table_field_set(row, 372, 5, data->output_port);
	} else {
		dpns_table_field_set(row, 310, 6, data->intf_index);
		dpns_table_field_set(row, 333, 11, data->mac_index);
		dpns_table_field_set(row, 367, 5, data->output_port);
	}
	dpns_table_field_set(row, 377, 1, 1);
	return dpns_nat_table_access(nat, true, DPNS_NAT_NAPT01,
				    entry->row, row, ARRAY_SIZE(row));
}

static int dpns_nat_insert_hash(struct dpns_nat *nat,
				struct dpns_nat_entry *entry,
				const struct dpns_nat_hash_tuple *tuple,
				bool dnat)
{
	u32 value;
	u16 crc, index;
	u8 table;
	int ret;
	u8 i;

	for (i = 0; i < DPNS_NAT_HASH_LAYERS; i++) {
		bool hash1 = i >= 3;

		table = dnat ?
			(hash1 ? DPNS_NAT_DNAT_HASH1 : DPNS_NAT_DNAT_HASH0) :
			(hash1 ? DPNS_NAT_SNAT_HASH1 : DPNS_NAT_SNAT_HASH0);
		crc = dpns_nat_crc16((const u8 *)tuple, sizeof(*tuple), i);
		index = (crc & dpns_nat_hash_mask[i]) |
			dpns_nat_hash_offset[i];
		ret = dpns_nat_table_access(nat, false, table, index, &value, 1);
		if (ret)
			return ret;
		if (value)
			continue;
		value = entry->row;
		ret = dpns_nat_table_access(nat, true, table, index, &value, 1);
		if (ret)
			return ret;
		entry->hash_table = table;
		entry->hash_index = index;
		return 0;
	}
	return -ENOSPC;
}

static void dpns_nat_mangle_eth(const struct flow_action_entry *act,
				void *eth)
{
	void *dst = eth + act->mangle.offset;
	const void *src = &act->mangle.val;

	if (act->mangle.offset > 8)
		return;
	if (act->mangle.mask == 0xffff) {
		src += 2;
		dst += 2;
	}
	memcpy(dst, src, act->mangle.mask ? 2 : 4);
}

static void dpns_nat_write_wan_slot(struct dpns_nat *nat, u8 slot, u32 value)
{
	u8 reg = slot * 18 / 32;
	u8 shift = slot * 18 % 32;
	u64 pair;

	pair = dpns_r32(nat->priv, SE_WAN_DATA0 + reg * 4);
	pair |= (u64)dpns_r32(nat->priv, SE_WAN_DATA0 + (reg + 1) * 4) << 32;
	pair &= ~(0x3ffffULL << shift);
	pair |= (u64)value << shift;
	dpns_w32(nat->priv, SE_WAN_DATA0 + reg * 4, lower_32_bits(pair));
	dpns_w32(nat->priv, SE_WAN_DATA0 + (reg + 1) * 4,
		 upper_32_bits(pair));
}

static int dpns_nat_set_wan_port(struct dpns_nat *nat, u8 port,
				 struct netlink_ext_ack *extack)
{
	if (nat->wan_port >= 0 && nat->wan_port != port) {
		NL_SET_ERR_MSG_MOD(extack,
			"flow route conflicts with the established DPNS WAN port");
		return -EOPNOTSUPP;
	}
	if (nat->wan_port < 0) {
		u32 value = FIELD_PREP(SE_WAN_PORT, port) |
			    FIELD_PREP(SE_WAN_VID, 0) | SE_WAN_VALID;

		nat->wan_port = port;
		/* Hardware keeps IPv4 and IPv6 WAN descriptors in slots 0 and 8. */
		dpns_nat_write_wan_slot(nat, 0, value);
		dpns_nat_write_wan_slot(nat, 8, value);
		dev_info(nat->priv->dev, "NAT WAN role assigned to physical port %u\n",
			 port);
	}
	return 0;
}

static void dpns_nat_ipv6_to_hw(u32 *dst, const struct in6_addr *src)
{
	unsigned int i;

	for (i = 0; i < 4; i++)
		dst[i] = ntohl(src->s6_addr32[3 - i]);
}

static int dpns_nat_parse_rule(struct dpns_nat *nat,
			       struct flow_cls_offload *f,
			       struct dpns_nat_data *data,
			       struct dpns_nat_hash_tuple *tuple)
{
	struct flow_rule *rule = flow_cls_offload_flow_rule(f);
	struct ethhdr eth = {};
	struct flow_action_entry *act;
	bool ip_mangled = false, port_mangled = false, redirected = false;
	bool source_xlate = false, dest_xlate = false;
	u8 wan_port;
	int i, ret;

	/* Netfilter expresses NAT with pedit actions in each tuple direction. */
	flow_action_for_each(i, act, &rule->action) {
		if (act->id == FLOW_ACTION_REDIRECT) {
			ret = sf_dpns_port_id_by_netdev(act->dev,
							&data->output_port);
			if (ret)
				return ret;
			redirected = true;
			continue;
		}
		if (act->id != FLOW_ACTION_MANGLE)
			continue;
		switch (act->mangle.htype) {
		case FLOW_ACT_MANGLE_HDR_TYPE_IP4:
			if (act->mangle.offset == offsetof(struct iphdr, saddr))
				source_xlate = true;
			else if (act->mangle.offset == offsetof(struct iphdr, daddr))
				dest_xlate = true;
			break;
		case FLOW_ACT_MANGLE_HDR_TYPE_TCP:
		case FLOW_ACT_MANGLE_HDR_TYPE_UDP:
			if (act->mangle.mask == ~htonl(0xffff0000))
				source_xlate = true;
			else if (act->mangle.mask == ~htonl(0x0000ffff))
				dest_xlate = true;
			break;
		default:
			break;
		}
	}
	if (source_xlate && dest_xlate) {
		NL_SET_ERR_MSG_MOD(f->common.extack,
			"simultaneous SNAT and DNAT is not supported");
		return -EOPNOTSUPP;
	}
	data->dnat = dest_xlate;
	data->l2_forward = !source_xlate && !dest_xlate;

	if (!flow_rule_match_key(rule, FLOW_DISSECTOR_KEY_META) ||
	    !flow_rule_match_key(rule, FLOW_DISSECTOR_KEY_CONTROL) ||
	    !flow_rule_match_key(rule, FLOW_DISSECTOR_KEY_BASIC) ||
	    !flow_rule_match_key(rule, FLOW_DISSECTOR_KEY_PORTS))
		return -EOPNOTSUPP;
	if (flow_rule_match_key(rule, FLOW_DISSECTOR_KEY_VLAN) ||
	    flow_rule_match_key(rule, FLOW_DISSECTOR_KEY_CVLAN)) {
		NL_SET_ERR_MSG_MOD(f->common.extack,
			"only untagged physical-port flows are supported");
		return -EOPNOTSUPP;
	}
	{
		struct flow_match_meta match;

		flow_rule_match_meta(rule, &match);
		ret = sf_dpns_port_id_by_ifindex(match.key->ingress_ifindex,
						   &data->ingress_port);
		if (ret)
			return ret;
		if (data->l2_forward && nat->wan_port >= 0)
			data->dnat = data->ingress_port == nat->wan_port;
	}
	{
		struct flow_match_control match;

		flow_rule_match_control(rule, &match);
		if (flow_rule_has_control_flags(match.mask->flags,
						f->common.extack))
			return -EOPNOTSUPP;
		switch (match.key->addr_type) {
		case FLOW_DISSECTOR_KEY_IPV4_ADDRS:
			if (data->l2_forward)
				return -EOPNOTSUPP;
			break;
		case FLOW_DISSECTOR_KEY_IPV6_ADDRS:
			if (!data->l2_forward) {
				NL_SET_ERR_MSG_MOD(f->common.extack,
					"IPv6 translation is not enabled; only forwarding is supported");
				return -EOPNOTSUPP;
			}
			data->v6 = true;
			break;
		default:
			return -EOPNOTSUPP;
		}
	}
	{
		struct flow_match_basic match;

		flow_rule_match_basic(rule, &match);
		switch (match.key->ip_proto) {
		case IPPROTO_TCP:
			data->l4_type = 0;
			tuple->l4_type = 0;
			break;
		case IPPROTO_UDP:
			data->l4_type = 1;
			tuple->l4_type = 1;
			break;
		default:
			return -EOPNOTSUPP;
		}
	}
	{
		struct flow_match_ports match;

		flow_rule_match_ports(rule, &match);
		tuple->sport = match.key->src;
		tuple->dport = match.key->dst;
		if (data->dnat) {
			data->public_port = ntohs(match.key->src);
			data->router_port = ntohs(match.key->dst);
		} else {
			data->private_port = ntohs(match.key->src);
			data->public_port = ntohs(match.key->dst);
		}
	}
	if (!data->v6) {
		struct flow_match_ipv4_addrs match;

		if (!flow_rule_match_key(rule, FLOW_DISSECTOR_KEY_IPV4_ADDRS))
			return -EOPNOTSUPP;
		flow_rule_match_ipv4_addrs(rule, &match);
		tuple->sip4 = match.key->src;
		tuple->dip4 = match.key->dst;
		if (data->dnat) {
			data->public_ip = ntohl(match.key->src);
			data->router_ip = ntohl(match.key->dst);
		} else {
			data->private_ip = ntohl(match.key->src);
			data->public_ip = ntohl(match.key->dst);
		}
	} else {
		struct flow_match_ipv6_addrs match;

		if (!flow_rule_match_key(rule, FLOW_DISSECTOR_KEY_IPV6_ADDRS))
			return -EOPNOTSUPP;
		flow_rule_match_ipv6_addrs(rule, &match);
		tuple->sip6 = match.key->src;
		tuple->dip6 = match.key->dst;
		if (data->dnat) {
			dpns_nat_ipv6_to_hw(data->public_ip6, &match.key->src);
			dpns_nat_ipv6_to_hw(data->router_ip6, &match.key->dst);
		} else {
			dpns_nat_ipv6_to_hw(data->private_ip6, &match.key->src);
			dpns_nat_ipv6_to_hw(data->public_ip6, &match.key->dst);
		}
	}

	flow_action_for_each(i, act, &rule->action) {
		switch (act->id) {
		case FLOW_ACTION_MANGLE:
			switch (act->mangle.htype) {
			case FLOW_ACT_MANGLE_HDR_TYPE_ETH:
				dpns_nat_mangle_eth(act, &eth);
				break;
			case FLOW_ACT_MANGLE_HDR_TYPE_IP4:
				if (data->v6 || data->l2_forward)
					return -EOPNOTSUPP;
				if ((!data->dnat && act->mangle.offset !=
				     offsetof(struct iphdr, saddr)) ||
				    (data->dnat && act->mangle.offset !=
				     offsetof(struct iphdr, daddr)))
					return -EOPNOTSUPP;
				if (data->dnat)
					data->private_ip = ntohl(act->mangle.val);
				else
					data->router_ip = ntohl(act->mangle.val);
				ip_mangled = true;
				break;
			case FLOW_ACT_MANGLE_HDR_TYPE_TCP:
			case FLOW_ACT_MANGLE_HDR_TYPE_UDP:
				if (data->l2_forward)
					return -EOPNOTSUPP;
				if (data->dnat)
					data->private_port = ntohl(act->mangle.val) & 0xffff;
				else
					data->router_port = ntohl(act->mangle.val) >> 16;
				port_mangled = true;
				break;
			default:
				return -EOPNOTSUPP;
			}
			break;
		case FLOW_ACTION_REDIRECT:
			break;
		case FLOW_ACTION_CSUM:
			break;
		default:
			return -EOPNOTSUPP;
		}
	}
	if (!redirected || !is_valid_ether_addr(eth.h_source) ||
	    !is_valid_ether_addr(eth.h_dest))
		return -EINVAL;
	ether_addr_copy(data->src_mac, eth.h_source);
	ether_addr_copy(data->dst_mac, eth.h_dest);

	if (data->l2_forward) {
		if (data->v6) {
			if (data->dnat) {
				memcpy(data->private_ip6, data->router_ip6,
				       sizeof(data->private_ip6));
				data->private_port = data->router_port;
			} else {
				memcpy(data->router_ip6, data->private_ip6,
				       sizeof(data->router_ip6));
				data->router_port = data->private_port;
			}
		}
		/* L2-forward mode 0 hashes destination IP only. */
		tuple->l4_type = 0;
		tuple->sport = 0;
		tuple->sip6 = in6addr_any;
		tuple->dport = 0;
	} else if (data->dnat) {
		if (!ip_mangled)
			data->private_ip = data->router_ip;
		if (!port_mangled)
			data->private_port = data->router_port;
	} else {
		if (!ip_mangled)
			data->router_ip = data->private_ip;
		if (!port_mangled)
			data->router_port = data->private_port;
	}

	wan_port = data->dnat ? data->ingress_port : data->output_port;
	ret = dpns_nat_set_wan_port(nat, wan_port, f->common.extack);
	if (ret)
		return ret;
	return 0;
}

static struct dpns_nat_entry *dpns_nat_find(struct dpns_nat *nat,
					    unsigned long cookie)
{
	struct dpns_nat_entry *entry;

	list_for_each_entry(entry, &nat->entries, list)
		if (entry->cookie == cookie)
			return entry;
	return NULL;
}

static void dpns_nat_entry_release(struct dpns_nat *nat,
				   struct dpns_nat_entry *entry)
{
	u32 zero = 0;

	dpns_nat_table_access(nat, true, entry->hash_table,
			      entry->hash_index, &zero, 1);
	dpns_nat_free_row(nat, entry);
	dpns_nat_router_put(nat, entry->v6, entry->router_index);
	dpns_intf_put(nat->priv, entry->intf_index);
	dpns_l2_nexthop_put(nat->priv, entry->dst_mac, entry->vid,
			      entry->output_port);
	list_del(&entry->list);
	kfree(entry);
}

static int dpns_nat_replace(struct dpns_nat *nat, struct flow_cls_offload *f)
{
	struct dpns_nat_hash_tuple tuple = {};
	struct dpns_nat_data data = {};
	struct dpns_nat_entry *entry;
	int ret;

	mutex_lock(&nat->lock);
	if (dpns_nat_find(nat, f->cookie)) {
		ret = -EEXIST;
		goto out_unlock;
	}
	ret = dpns_nat_parse_rule(nat, f, &data, &tuple);
	if (ret)
		goto out_unlock;

	entry = kzalloc_obj(*entry, GFP_KERNEL);
	if (!entry) {
		ret = -ENOMEM;
		goto out_unlock;
	}
	entry->cookie = f->cookie;
	ret = dpns_nat_alloc_row(nat, data.v6, entry);
	if (ret)
		goto err_free;
	if (data.v6)
		ret = dpns_nat_router_get6(nat, data.router_ip6,
					   &entry->router_index);
	else
		ret = dpns_nat_router_get4(nat, data.router_ip,
					   &entry->router_index);
	if (ret)
		goto err_row;
	ret = dpns_l2_nexthop_get(nat->priv, data.dst_mac, data.vid,
				  data.output_port, &data.mac_index);
	if (ret)
		goto err_router;
	ret = dpns_intf_get(nat->priv, data.src_mac, data.vid,
			    data.output_port == nat->wan_port,
			    &data.intf_index);
	if (ret)
		goto err_l2;
	ret = dpns_nat_write_row(nat, entry, &data);
	if (ret)
		goto err_intf;
	ret = dpns_nat_insert_hash(nat, entry, &tuple, data.dnat);
	if (ret)
		goto err_intf;

	entry->intf_index = data.intf_index;
	entry->output_port = data.output_port;
	entry->vid = data.vid;
	entry->dnat = data.dnat;
	entry->l2_forward = data.l2_forward;
	ether_addr_copy(entry->dst_mac, data.dst_mac);
	entry->lastused = get_jiffies_64();
	list_add_tail(&entry->list, &nat->entries);
	mutex_unlock(&nat->lock);
	return 0;

err_intf:
	dpns_intf_put(nat->priv, data.intf_index);
err_l2:
	dpns_l2_nexthop_put(nat->priv, data.dst_mac, data.vid,
			      data.output_port);
err_router:
	dpns_nat_router_put(nat, data.v6, entry->router_index);
err_row:
	dpns_nat_free_row(nat, entry);
err_free:
	kfree(entry);
out_unlock:
	mutex_unlock(&nat->lock);
	return ret;
}

static int dpns_nat_destroy(struct dpns_nat *nat, struct flow_cls_offload *f)
{
	struct dpns_nat_entry *entry;
	int ret = 0;

	mutex_lock(&nat->lock);
	entry = dpns_nat_find(nat, f->cookie);
	if (!entry)
		ret = -ENOENT;
	else
		dpns_nat_entry_release(nat, entry);
	mutex_unlock(&nat->lock);
	return ret;
}

static int dpns_nat_stats(struct dpns_nat *nat, struct flow_cls_offload *f)
{
	struct dpns_nat_entry *entry;
	u32 visit;
	int ret = 0;

	mutex_lock(&nat->lock);
	entry = dpns_nat_find(nat, f->cookie);
	if (!entry) {
		ret = -ENOENT;
		goto out;
	}
	visit = dpns_r32(nat->priv, SE_NAT_VISIT(entry->nat_id / 32));
	if (visit & BIT(entry->nat_id % 32))
		entry->lastused = get_jiffies_64();
	f->stats.lastused = entry->lastused;
out:
	mutex_unlock(&nat->lock);
	return ret;
}

static int dpns_nat_flow_block_cb(enum tc_setup_type type, void *type_data,
				  void *cb_priv)
{
	struct dpns_nat *nat = cb_priv;
	struct flow_cls_offload *f = type_data;

	if (type != TC_SETUP_CLSFLOWER)
		return -EOPNOTSUPP;
	switch (f->command) {
	case FLOW_CLS_REPLACE:
		return dpns_nat_replace(nat, f);
	case FLOW_CLS_DESTROY:
		return dpns_nat_destroy(nat, f);
	case FLOW_CLS_STATS:
		return dpns_nat_stats(nat, f);
	default:
		return -EOPNOTSUPP;
	}
}

static int dpns_nat_debugfs_show(struct seq_file *m, void *unused)
{
	struct dpns_nat *nat = m->private;
	struct dpns_nat_entry *entry;
	unsigned int count = 0;

	mutex_lock(&nat->lock);
	list_for_each_entry(entry, &nat->entries, list)
		count++;
	seq_printf(m, "wan_port: %d\nflows: %u\n", nat->wan_port, count);
	list_for_each_entry(entry, &nat->entries, list)
		seq_printf(m,
			   "cookie=%#lx nat_id=%u family=ipv%u mode=%s direction=%s output=%u mac=%pM\n",
			   entry->cookie, entry->nat_id, entry->v6 ? 6 : 4,
			   entry->l2_forward ? "forward" : "nat",
			   entry->dnat ? "dnat" : "snat", entry->output_port,
			   entry->dst_mac);
	mutex_unlock(&nat->lock);
	return 0;
}

DEFINE_SHOW_ATTRIBUTE(dpns_nat_debugfs);

void dpns_nat_debugfs_init(struct dpns_priv *priv)
{
	if (priv->nat && priv->debugfs)
		debugfs_create_file("nat", 0400, priv->debugfs, priv->nat,
				    &dpns_nat_debugfs_fops);
}

int dpns_nat_setup_tc(struct dpns_priv *priv, struct dpns_port *port,
		      enum tc_setup_type type, void *type_data)
{
	struct flow_block_offload *f = type_data;
	struct dpns_nat *nat = priv->nat;
	struct flow_block_cb *block_cb;

	if (!nat || !port || type != TC_SETUP_FT ||
	    f->binder_type != FLOW_BLOCK_BINDER_TYPE_CLSACT_INGRESS)
		return -EOPNOTSUPP;
	f->driver_block_list = &nat->block_cb_list;
	switch (f->command) {
	case FLOW_BLOCK_BIND:
		block_cb = flow_block_cb_lookup(f->block,
					     dpns_nat_flow_block_cb, nat);
		if (block_cb) {
			flow_block_cb_incref(block_cb);
			return 0;
		}
		block_cb = flow_block_cb_alloc(dpns_nat_flow_block_cb, nat, nat,
					       NULL);
		if (IS_ERR(block_cb))
			return PTR_ERR(block_cb);
		flow_block_cb_incref(block_cb);
		flow_block_cb_add(block_cb, f);
		list_add_tail(&block_cb->driver_list, &nat->block_cb_list);
		return 0;
	case FLOW_BLOCK_UNBIND:
		block_cb = flow_block_cb_lookup(f->block,
					     dpns_nat_flow_block_cb, nat);
		if (!block_cb)
			return -ENOENT;
		if (!flow_block_cb_decref(block_cb)) {
			flow_block_cb_remove(block_cb, f);
			list_del(&block_cb->driver_list);
		}
		return 0;
	default:
		return -EOPNOTSUPP;
	}
}

int dpns_nat_init(struct dpns_priv *priv)
{
	struct dpns_nat *nat;
	u32 mask = 0, val = 0;
	u64 clk;
	unsigned int i;

	nat = devm_kzalloc(priv->dev, sizeof(*nat), GFP_KERNEL);
	if (!nat)
		return -ENOMEM;
	nat->priv = priv;
	nat->wan_port = -1;
	mutex_init(&nat->lock);
	INIT_LIST_HEAD(&nat->entries);
	INIT_LIST_HEAD(&nat->block_cb_list);
	set_bit(0, nat->napt0_map);
	set_bit(0, nat->napt1_map);
	priv->nat = nat;

	dpns_w32(priv, SE_NAT_CLEAR, 0x7ff);
	dpns_rmw(priv, NPU_NAT_MPP_CFG, NAT_MPP_CFG_BYPASS, 0);
	dpns_rmw(priv, NPU_L3_MPP_CFG, 0, NPU_L3_CARE_NAT_RESULT);
	dpns_rmw(priv, NPU_NAT_HOST_MODE,
		 NAT_HOST_TCP_MODE | NAT_HOST_UDP_MODE |
		 NAT_HOST_V4_LF_MODE,
		 NAT_HOST_PORT_DIRECTION | NAT_HOST_V6_LF_MODE |
		 FIELD_PREP(NAT_HOST_TCP_MODE, DPNS_NAT_MODE_SYMMETRIC) |
		 FIELD_PREP(NAT_HOST_UDP_MODE, DPNS_NAT_MODE_SYMMETRIC));
	dpns_rmw(priv, SE_NAT_CONFIG0,
		 SE_NAT_DNAT_SUBTABLES | SE_NAT_SNAT_SUBTABLES |
		 SE_NAT_UDP_MODE,
		 SE_NAT_DISABLE_DDR |
		 FIELD_PREP(SE_NAT_UDP_MODE, DPNS_NAT_MODE_SYMMETRIC));
	for (i = 0; i < DPNS_NAT_HASH_LAYERS; i++) {
		mask |= 0x7 << (i * 3);
		val |= i << (i * 3);
	}
	dpns_rmw(priv, SE_NAT_CONFIG1,
		 SE_NAT_V4_LF_ENABLE | SE_NAT_V4_LF_MODE |
		 SE_NAT_TCP_MODE | mask,
		 SE_NAT_V6_LF_ENABLE |
		 FIELD_PREP(SE_NAT_TCP_MODE, DPNS_NAT_MODE_SYMMETRIC) | val);
	dpns_rmw(priv, SE_NAT_CONFIG5, SE_NAT_V6_LF_MODE, 0);
	clk = clk_get_rate(priv->clk);
	if (clk)
		dpns_w32(priv, SE_NAT_CONFIG6, min_t(u64, clk - 1, U32_MAX));

	dev_info(priv->dev,
		 "NAT engine enabled (symmetric TCP/UDP, IPv4 NAT, IPv6 forwarding)\n");
	return 0;
}

void dpns_nat_fini(struct dpns_priv *priv)
{
	struct dpns_nat *nat = priv->nat;
	struct dpns_nat_entry *entry, *tmp;

	if (!nat)
		return;
	mutex_lock(&nat->lock);
	list_for_each_entry_safe(entry, tmp, &nat->entries, list)
		dpns_nat_entry_release(nat, entry);
	dpns_nat_write_wan_slot(nat, 0, 0);
	dpns_nat_write_wan_slot(nat, 8, 0);
	dpns_w32(priv, SE_NAT_CLEAR, 0x7ff);
	mutex_unlock(&nat->lock);
	priv->nat = NULL;
}
