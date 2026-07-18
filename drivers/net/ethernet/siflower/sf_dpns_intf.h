/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __SF_DPNS_INTF_H__
#define __SF_DPNS_INTF_H__

#include <linux/types.h>

struct dpns_priv;

int dpns_intf_init(struct dpns_priv *priv);
void dpns_intf_fini(struct dpns_priv *priv);
int dpns_intf_get(struct dpns_priv *priv, const u8 *src, u16 vid,
		  bool wan, u8 *index);
void dpns_intf_put(struct dpns_priv *priv, u8 index);

#endif /* __SF_DPNS_INTF_H__ */
