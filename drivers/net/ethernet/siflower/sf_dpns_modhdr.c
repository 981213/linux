// SPDX-License-Identifier: GPL-2.0
/*
 * SF21 DPNS egress modify-header hardware.
 *
 * Each of the 64 routed-interface rows has a same-numbered 384-bit
 * modify-header row.  The interface row selects whether the contents are
 * interpreted as a PPPoE header or an IPv6 tunnel header.  For PPPoE the
 * hardware consumes the version/type, code, session ID and PPP protocol from
 * the first 64 bits, and supplies the payload length while transmitting.
 * Configuration therefore writes the modify row first and only then makes
 * the selecting interface row valid; teardown reverses that order.
 *
 * IPv6 tunnel headers use the rest of the same 384-bit row.  Their typed
 * representation will be added together with the supported 4-in-6 flowtable
 * path; rejecting other tunnel actions avoids interpreting a PPPoE row as a
 * partially initialized tunnel header.
 */

#include <linux/errno.h>
#include <linux/ppp_defs.h>

#include "dpns.h"
#include "sf_dpns_modhdr.h"
#include "sf_dpns_table.h"

#define DPNS_MODHDR_WORDS	12

int dpns_modhdr_write(struct dpns_priv *priv, u8 index,
		      const struct dpns_modhdr_cfg *cfg)
{
	u32 row[DPNS_MODHDR_WORDS] = {};

	if (!cfg || cfg->type == DPNS_MODHDR_NONE)
		return dpns_modhdr_clear(priv, index);

	switch (cfg->type) {
	case DPNS_MODHDR_PPPOE:
		if (!cfg->pppoe.sid ||
		    (cfg->pppoe.proto != PPP_IP &&
		     cfg->pppoe.proto != PPP_IPV6))
			return -EINVAL;

		/* Mirrors the vendor's little-endian 64-bit table image:
		 * 0x1100 (version/type and code), SID, generated length, PPP proto.
		 */
		row[0] = cfg->pppoe.proto;
		row[1] = 0x11000000 | cfg->pppoe.sid;
		break;
	default:
		return -EOPNOTSUPP;
	}

	return dpns_table_write(priv, DPNS_TABLE_MODIFY_HEADER, index, row,
				sizeof(row));
}

int dpns_modhdr_clear(struct dpns_priv *priv, u8 index)
{
	u32 row[DPNS_MODHDR_WORDS] = {};

	return dpns_table_write(priv, DPNS_TABLE_MODIFY_HEADER, index, row,
				sizeof(row));
}
