/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __SF_DPNS_PORT_H__
#define __SF_DPNS_PORT_H__

#include <linux/types.h>

struct dpns_port;
struct net_device;

struct dpns_port *sf_dpns_port_register(struct net_device *ndev, u8 id);
void sf_dpns_port_unregister(struct dpns_port *port);
bool sf_dpns_port_offload_fwd_mark(const struct dpns_port *port);

#endif /* __SF_DPNS_PORT_H__ */
