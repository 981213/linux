/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __SF_DPNS_NAT_H__
#define __SF_DPNS_NAT_H__

#include <linux/netdevice.h>

struct dpns_port;
struct dpns_priv;

int dpns_nat_init(struct dpns_priv *priv);
void dpns_nat_fini(struct dpns_priv *priv);
int dpns_nat_setup_tc(struct dpns_priv *priv, struct dpns_port *port,
		      enum tc_setup_type type, void *type_data);

#endif /* __SF_DPNS_NAT_H__ */
