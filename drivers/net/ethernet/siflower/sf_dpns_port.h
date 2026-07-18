/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __SF_DPNS_PORT_H__
#define __SF_DPNS_PORT_H__

#include <linux/types.h>
#include <linux/netdevice.h>

struct dpns_port;
struct net_device;

struct dpns_port *sf_dpns_port_register(struct net_device *ndev, u8 id);
void sf_dpns_port_unregister(struct dpns_port *port);
bool sf_dpns_port_offload_fwd_mark(const struct dpns_port *port);
int sf_dpns_port_id_by_netdev(const struct net_device *ndev, u8 *id);
int sf_dpns_port_id_by_ifindex(int ifindex, u8 *id);
int sf_dpns_port_setup_tc(struct dpns_port *port, enum tc_setup_type type,
			  void *type_data);

#endif /* __SF_DPNS_PORT_H__ */
