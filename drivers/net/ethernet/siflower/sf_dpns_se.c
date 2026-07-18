// SPDX-License-Identifier: GPL-2.0
/*
 * SF21 DPNS Search Engine reset and common policy.
 *
 * The SE owns the ingress VLAN, egress VLAN, L2 hash/action, isolation and
 * TCAM memories.  Initialization clears all internal RAM and TCAM banks,
 * waits for both asynchronous clear engines to finish, then changes malformed
 * ARP handling from drop to CPU trap.  Individual hardware modules configure
 * their own tables only after this common reset has completed.
 */

#include <linux/bitfield.h>
#include <linux/iopoll.h>

#include "dpns.h"
#include "sf_dpns_se.h"

int dpns_se_init(struct dpns_priv *priv)
{
	int ret;
	u32 reg;

	dpns_w32(priv, SE_CLR_RAM_CTRL, SE_CLR_RAM_ALL);
	dpns_w32(priv, SE_TCAM_CLR, SE_TCAM_CLR_ALL);

	ret = readl_poll_timeout(priv->ioaddr + SE_CLR_RAM_CTRL, reg, !reg, 0,
				 1000);
	if (ret)
		return ret;
	ret = readl_poll_timeout(priv->ioaddr + SE_TCAM_CLR, reg, !reg, 0,
				 1000);
	if (ret)
		return ret;

	/* Upload ARP packets which NPU considers invalid to host. */
	dpns_rmw(priv, PKT_ERR_STG_CFG2, ARP_REQ_ERR_OP | ARP_REPLY_ERR_OP,
		 ARP_REQ_ERR_UP | ARP_REPLY_ERR_UP);

	return 0;
}
