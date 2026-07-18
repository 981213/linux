/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __SF_DPNS_NAT_EXT_H__
#define __SF_DPNS_NAT_EXT_H__

#include <linux/types.h>

struct dpns_priv;

struct dpns_nat_ext_action {
	u32 private_ip[4];
	u32 public_ip[4];
	u32 router_ip[4];
	u16 private_port;
	u16 public_port;
	u16 router_port;
	u16 mac_index;
	u8 intf_index;
	u8 output_port;
	u8 l4_type;
	bool v6;
	bool dnat;
};

struct dpns_nat_ext_handle {
	u16 nat_id;
	u16 index;
	bool valid;
	bool v6;
	bool dnat;
	bool second_slot;
};

int dpns_nat_ext_init(struct dpns_priv *priv);
void dpns_nat_ext_fini(struct dpns_priv *priv);
int dpns_nat_ext_add(struct dpns_priv *priv, const void *tuple,
		     size_t tuple_len, const struct dpns_nat_ext_action *action,
		     struct dpns_nat_ext_handle *handle);
void dpns_nat_ext_del(struct dpns_priv *priv,
		      struct dpns_nat_ext_handle *handle);

#endif /* __SF_DPNS_NAT_EXT_H__ */
