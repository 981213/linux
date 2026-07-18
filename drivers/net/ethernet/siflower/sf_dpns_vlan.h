/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __SF_DPNS_VLAN_H__
#define __SF_DPNS_VLAN_H__

#include <linux/types.h>

#define DPNS_PHYS_PORTS	6
#define DPNS_HOST_PORT		6
#define DPNS_VLAN_ENTRIES	64

struct dpns_priv;

enum dpns_vlan_stp_state {
	DPNS_STP_BLOCK,
	DPNS_STP_LISTEN,
	DPNS_STP_LEARN,
	DPNS_STP_FORWARD,
};

int dpns_vlan_port_config(struct dpns_priv *priv, unsigned int port,
			  bool bridge, bool learning, bool vlan_aware,
			  u16 pvid);
int dpns_vlan_entry_write(struct dpns_priv *priv, unsigned int index,
			  u16 vid, u32 members, u32 untagged,
			  const u8 stp_state[DPNS_PHYS_PORTS]);
int dpns_vlan_entry_clear(struct dpns_priv *priv, unsigned int index);

#endif /* __SF_DPNS_VLAN_H__ */
