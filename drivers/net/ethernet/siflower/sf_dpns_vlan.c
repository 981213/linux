// SPDX-License-Identifier: GPL-2.0
/*
 * SF21 DPNS ingress/egress VLAN hardware.
 *
 * Ingress first consults a per-port row.  Standalone physical ports use the
 * CPU action and therefore retain normal NIC behaviour.  A bridged port uses
 * the forwarding action, optional port-based PVID insertion, then a VID lookup
 * row containing membership and per-port spanning-tree state.  The matching
 * egress VID row supplies the destination and untagged bitmaps.  Global setup
 * enables both lookup banks, installs 802.1Q TPIDs and four tag-edit action
 * rows tailored to the physical L2 datapath.  An ingress matchall DROP
 * overrides the normal CPU/forward action in that physical port's row.  The
 * separate per-port speed-limit table meters byte-rate policers in 72-bit
 * credits over a fixed NPU-clock window.  Configuration proceeds port/PVID
 * first, ingress membership second, and egress membership last.
 */

#include <linux/bitfield.h>
#include <linux/clk.h>
#include <linux/if_ether.h>
#include <linux/if_vlan.h>
#include <linux/math64.h>

#include "dpns.h"
#include "sf_dpns_se.h"
#include "sf_dpns_table.h"
#include "sf_dpns_vlan.h"

#define SE_IVLAN_MPP_CFG		0x8000
#define  SE_IVLAN_PBV_ENABLE		BIT(18)
#define SE_IVLAN_LKP_CFG		0xc004
#define  SE_IVLAN_VID0_LOOKUP		BIT(11)
#define  SE_IVLAN_MISS_FORWARD		BIT(6)
#define  SE_IVLAN_MISS_CPU		BIT(5)
#define  SE_IVLAN_MCAST_MODE		GENMASK(2, 1)

#define SE_EVLAN_ACT_CFG3		0x1c00c
#define  SE_EVLAN_L2_OVID_ACTION	GENMASK(19, 17)
#define  SE_EVLAN_L2_IVID_ACTION	GENMASK(21, 20)
#define  SE_EVLAN_L3_OVID0_DELETE	BIT(25)
#define  SE_EVLAN_L3_IVID0_DELETE	BIT(26)

#define DPNS_IPSPL_COMPARE_BITS		24
#define DPNS_IPSPL_CLOCK_MULT		2
#define DPNS_IPSPL_CREDIT_BYTES		9
#define DPNS_IPSPL_CREDIT_MAX		GENMASK(24, 0)

enum dpns_iport_action {
	DPNS_IPORT_DROP,
	DPNS_IPORT_CPU,
	DPNS_IPORT_FORWARD,
};

enum dpns_iport_learning {
	DPNS_LEARNING_ACCEPT,
	DPNS_NO_LEARNING_ACCEPT,
	DPNS_LEARNING_TO_CPU,
	DPNS_NO_LEARNING_DROP,
};

enum dpns_vlan_action {
	DPNS_VLAN_NONE,
	DPNS_VLAN_REPLACE,
	DPNS_VLAN_ADD,
	DPNS_VLAN_DELETE,
};

enum dpns_evlan_action {
	DPNS_EVLAN_NONE,
	DPNS_EVLAN_REPLACE_INTF,
	DPNS_EVLAN_REPLACE_XLT,
	DPNS_EVLAN_DELETE,
};

static int dpns_vlan_write_iport(struct dpns_priv *priv, unsigned int port,
				 bool bridge, bool learning, bool drop)
{
	u32 row = 0;

	/* Fixed-key VLAN translation and physical port 6 as the CPU port. */
	dpns_table_field_set(&row, 4, 1, 1);
	dpns_table_field_set(&row, 10, 5, DPNS_HOST_PORT);
	dpns_table_field_set(&row, 15, 2, learning ?
			     DPNS_LEARNING_TO_CPU : DPNS_NO_LEARNING_ACCEPT);
	dpns_table_field_set(&row, 17, 2, drop ? DPNS_IPORT_DROP : bridge ?
			     DPNS_IPORT_FORWARD : DPNS_IPORT_CPU);
	dpns_table_field_set(&row, 19, 1, 1);

	return dpns_table_write(priv, DPNS_TABLE_IVLAN_IPORT, port,
				&row, sizeof(row));
}

static int dpns_vlan_write_pvid(struct dpns_priv *priv, unsigned int port,
				u16 pvid)
{
	u32 row[2] = {};

	if (pvid) {
		/* Add an outer tag to untagged frames; replace priority VID 0. */
		dpns_table_field_set(row, 6, 2, DPNS_VLAN_ADD);
		dpns_table_field_set(row, 22, 2, DPNS_VLAN_REPLACE);
		dpns_table_field_set(row, 38, 12, pvid);
		dpns_table_field_set(row, 50, 12, pvid);
		dpns_table_field_set(row, 63, 1, 1);
	}

	return dpns_table_write(priv, DPNS_TABLE_IVLAN_PBV, port,
				row, sizeof(row));
}

static int dpns_vlan_write_egress_xlt(struct dpns_priv *priv,
				      unsigned int port, bool bridge,
				      bool vlan_aware)
{
	u32 row[3] = {};

	if (bridge) {
		/* Match any old VID on this output port. Action row 1 removes
		 * tags for the VLAN-unaware domain; row 2 preserves/replaces the
		 * selected VID for VLAN-aware forwarding. The egress VID lookup's
		 * untagged bitmap overrides this with the delete action as needed.
		 */
		dpns_table_field_set(row, 0, 1, 1);
		dpns_table_field_set(row, 1, 1, 1);
		dpns_table_field_set(row, 2, 1, 1);
		dpns_table_field_set(row, 4, 6, vlan_aware ? 2 : 1);
		dpns_table_field_set(row, 64, 5, port);
	}

	return dpns_table_write(priv, DPNS_TABLE_EVLAN_XLT, port,
				row, sizeof(row));
}

int dpns_vlan_port_config(struct dpns_priv *priv, unsigned int port,
			  bool bridge, bool learning, bool ingress_drop,
			  bool vlan_aware,
			  u16 pvid)
{
	int ret;

	if (port >= DPNS_PHYS_PORTS || pvid >= VLAN_N_VID)
		return -EINVAL;

	ret = dpns_vlan_write_pvid(priv, port, bridge ? pvid : 0);
	if (ret)
		return ret;
	ret = dpns_vlan_write_egress_xlt(priv, port, bridge, vlan_aware);
	if (ret)
		return ret;

	return dpns_vlan_write_iport(priv, port, bridge, learning,
				     ingress_drop);
}

int dpns_vlan_port_policer_set(struct dpns_priv *priv, unsigned int port,
			       u64 rate_bytes_ps)
{
	u64 clk = clk_get_rate(priv->clk);
	u64 divisor, credit;
	u32 row[2] = {};

	if (port >= DPNS_PHYS_PORTS || !clk)
		return -EINVAL;
	if (rate_bytes_ps) {
		/* The byte-mode policer adds one 72-bit datapath word per
		 * credit every 2^24 Search Engine clocks. The SE runs at twice
		 * the exported DPNS bus clock. Zero credit disables policing.
		 */
		divisor = clk * DPNS_IPSPL_CLOCK_MULT *
			  DPNS_IPSPL_CREDIT_BYTES;
		credit = mul_u64_add_u64_div_u64(rate_bytes_ps,
						 BIT_ULL(DPNS_IPSPL_COMPARE_BITS),
						 divisor / 2, divisor);
		if (!credit || credit > DPNS_IPSPL_CREDIT_MAX)
			return -ERANGE;
		row[0] = credit;
	}

	return dpns_table_write(priv, DPNS_TABLE_IVLAN_SPL, port,
				row, sizeof(row));
}

static int dpns_vlan_write_ingress(struct dpns_priv *priv, unsigned int index,
				   u16 vid, u32 members,
				   const u8 stp_state[DPNS_PHYS_PORTS])
{
	u32 row[3] = {};
	unsigned int port;

	dpns_table_field_set(row, 0, 1, 1); /* valid */
	dpns_table_field_set(row, 2, 2, 1); /* flood unknown multicast */
	dpns_table_field_set(row, 4, 1, 1); /* IPv6 multicast lookup */
	dpns_table_field_set(row, 5, 1, 1); /* IPv4 multicast lookup */
	dpns_table_field_set(row, 6, 1, 1); /* DA miss to CPU */
	dpns_table_field_set(row, 8, 1, 1); /* non-unicast to CPU */
	for (port = 0; port < DPNS_PHYS_PORTS; port++)
		/* Physical port state slots ascend from bit 10 on SF21H8898. */
		dpns_table_field_set(row, 10 + port * 2, 2, stp_state[port]);
	dpns_table_field_set(row, 30, 27, members);
	dpns_table_field_set(row, 57, 12, vid);

	return dpns_table_write(priv, DPNS_TABLE_IVLAN_LKP, index,
				row, sizeof(row));
}

static int dpns_vlan_write_egress(struct dpns_priv *priv, unsigned int index,
				  u16 vid, u32 members, u32 untagged,
				  const u8 stp_state[DPNS_PHYS_PORTS])
{
	u32 row[3] = {};
	unsigned int port;

	dpns_table_field_set(row, 0, 1, 1); /* valid */
	for (port = 0; port < DPNS_PHYS_PORTS; port++)
		/* The egress format stores port 9 first and port 0 last. */
		dpns_table_field_set(row, 22 - port * 2, 2, stp_state[port]);
	dpns_table_field_set(row, 24, 27, untagged);
	dpns_table_field_set(row, 51, 27, members);
	dpns_table_field_set(row, 78, 12, vid);

	return dpns_table_write(priv, DPNS_TABLE_EVLAN_LKP, index,
				row, sizeof(row));
}

int dpns_vlan_entry_write(struct dpns_priv *priv, unsigned int index,
			  u16 vid, u32 members, u32 untagged,
			  const u8 stp_state[DPNS_PHYS_PORTS])
{
	int ret;

	if (index >= DPNS_VLAN_ENTRIES || vid >= VLAN_N_VID ||
	    members & ~GENMASK(26, 0) || untagged & ~members)
		return -EINVAL;

	ret = dpns_vlan_write_ingress(priv, index, vid, members, stp_state);
	if (ret)
		return ret;

	return dpns_vlan_write_egress(priv, index, vid, members, untagged,
				       stp_state);
}

int dpns_vlan_entry_clear(struct dpns_priv *priv, unsigned int index)
{
	u32 row[3] = {};
	int ret;

	if (index >= DPNS_VLAN_ENTRIES)
		return -EINVAL;

	ret = dpns_table_write(priv, DPNS_TABLE_IVLAN_LKP, index,
			       row, sizeof(row));
	if (ret)
		return ret;

	return dpns_table_write(priv, DPNS_TABLE_EVLAN_LKP, index,
				row, sizeof(row));
}

static int dpns_vlan_write_tpid(struct dpns_priv *priv, unsigned int port)
{
	u32 row = (ETH_P_8021Q << 16) | ETH_P_8021Q;
	int ret;

	ret = dpns_table_write(priv, DPNS_TABLE_EVLAN_TPID, port,
			       &row, sizeof(row));
	if (ret)
		return ret;

	row = ETH_P_8021Q;
	return dpns_table_write(priv, DPNS_TABLE_EVLAN_OTPID, port,
				&row, sizeof(row));
}

static int dpns_vlan_write_action(struct dpns_priv *priv, unsigned int index,
				  u8 single, u8 priority_single,
				  u8 double_tag, u8 priority_double,
				  u8 fallback)
{
	u32 row = 0;

	/* Four two-bit fields per tag class; only outer-tag actions are used. */
	dpns_table_field_set(&row, 6, 2, single);
	dpns_table_field_set(&row, 4, 2, priority_single);
	dpns_table_field_set(&row, 14, 2, double_tag);
	dpns_table_field_set(&row, 12, 2, priority_double);
	dpns_table_field_set(&row, 26, 2, fallback);

	return dpns_table_write(priv, DPNS_TABLE_EVLAN_ACT, index,
				&row, sizeof(row));
}

int dpns_vlan_init(struct dpns_priv *priv)
{
	unsigned int port;
	int ret;

	dpns_rmw(priv, SE_CONFIG0,
		 SE_IVLKP_CFG_DISABLE | SE_IVXLT_CFG_DISABLE |
		 SE_IPSPL_DIS_STEP | SE_IPSPL_CMPT_LEN |
		 SE_IPSPL_ZERO_LIMIT | SE_IPSPL_CNT_MODE |
		 SE_IVLKP_CFG_ENTR_MINUS1 | SE_IVXLT_CFG_ENTR_MINUS1 |
		 SE_IPSPL_MODE,
		 SE_PORTBV_TABLE_VALID | SE_IPORT_TABLE_VALID |
		 SE_L2_VID_ZERO_MODE |
		 FIELD_PREP(SE_IPSPL_CMPT_LEN, DPNS_IPSPL_COMPARE_BITS) |
		 FIELD_PREP(SE_IVLKP_CFG_ENTR_MINUS1, 63) |
		 FIELD_PREP(SE_IVXLT_CFG_ENTR_MINUS1, 63));
	dpns_rmw(priv, SE_CONFIG2,
		 SE_EVLKP_CFG_DIS_TB | SE_EVXLT_CFG_DIS |
		 SE_EVLKP_CFG_ENTR_MINUS1 | SE_EVXLT_CFG_ENTR_MINUS1,
		 SE_EVACT_TABLE_VALID | SE_OTPID_TABLE_VALID |
		 SE_PTPID_TABLE_VALID |
		 FIELD_PREP(SE_EVLKP_CFG_ENTR_MINUS1, 63) |
		 FIELD_PREP(SE_EVXLT_CFG_ENTR_MINUS1, 63));

	dpns_rmw(priv, SE_IVLAN_MPP_CFG,
		 0, SE_IVLAN_PBV_ENABLE);
	dpns_rmw(priv, SE_IVLAN_LKP_CFG,
		 SE_IVLAN_VID0_LOOKUP | SE_IVLAN_MCAST_MODE,
		 FIELD_PREP(SE_IVLAN_MCAST_MODE, 1) | SE_IVLAN_MISS_CPU);
	dpns_rmw(priv, SE_EVLAN_ACT_CFG3, 0,
		 FIELD_PREP(SE_EVLAN_L2_OVID_ACTION, 5) |
		 FIELD_PREP(SE_EVLAN_L2_IVID_ACTION, 2) |
		 SE_EVLAN_L3_OVID0_DELETE | SE_EVLAN_L3_IVID0_DELETE);

	for (port = 0; port <= DPNS_HOST_PORT; port++) {
		ret = dpns_vlan_write_tpid(priv, port);
		if (ret)
			return ret;
	}

	for (port = 0; port < DPNS_PHYS_PORTS; port++) {
		ret = dpns_vlan_port_config(priv, port, false, false, false,
					    false, 0);
		if (ret)
			return ret;
	}

	/* The egress lookup selects row 56 for ports in its untagged bitmap.
	 * The vendor's full datapath replaces with an L3 interface VID here;
	 * a physical L2 bridge has no such interface metadata, so delete the
	 * outer tag directly for every outer-tag packet shape.
	 */
	ret = dpns_vlan_write_action(priv, 56, DPNS_EVLAN_DELETE,
				     DPNS_EVLAN_DELETE, DPNS_EVLAN_DELETE,
				     DPNS_EVLAN_DELETE, DPNS_EVLAN_DELETE);
	if (ret)
		return ret;
	ret = dpns_vlan_write_action(priv, 2, DPNS_EVLAN_REPLACE_INTF,
				     DPNS_EVLAN_REPLACE_XLT,
				     DPNS_EVLAN_NONE, DPNS_EVLAN_NONE,
				     DPNS_EVLAN_NONE);
	if (ret)
		return ret;
	ret = dpns_vlan_write_action(priv, 1, DPNS_EVLAN_DELETE,
				     DPNS_EVLAN_DELETE, DPNS_EVLAN_DELETE,
				     DPNS_EVLAN_DELETE, DPNS_EVLAN_DELETE);
	if (ret)
		return ret;

	return dpns_vlan_write_action(priv, 0, DPNS_EVLAN_REPLACE_XLT,
				       DPNS_EVLAN_REPLACE_XLT,
				       DPNS_EVLAN_NONE, DPNS_EVLAN_NONE,
				       DPNS_EVLAN_NONE);
}
