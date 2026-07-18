/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __SF_DPNS_L2_H__
#define __SF_DPNS_L2_H__

#include <linux/types.h>

struct dpns_priv;

int dpns_l2_addr_set(struct dpns_priv *priv, const unsigned char *addr,
		     u16 vid, u32 port_mask, bool ageing);
int dpns_l2_addr_add_ports(struct dpns_priv *priv, const unsigned char *addr,
			   u16 vid, u32 port_mask);
int dpns_l2_addr_del_ports(struct dpns_priv *priv, const unsigned char *addr,
			   u16 vid, u32 port_mask);
void dpns_l2_flush_port(struct dpns_priv *priv, unsigned int port);
void dpns_l2_flush_all(struct dpns_priv *priv);

#endif /* __SF_DPNS_L2_H__ */
