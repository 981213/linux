/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __SF_DPNS_MODHDR_H__
#define __SF_DPNS_MODHDR_H__

#include <linux/types.h>

struct dpns_priv;

enum dpns_modhdr_type {
	DPNS_MODHDR_NONE,
	DPNS_MODHDR_PPPOE,
};

struct dpns_modhdr_cfg {
	enum dpns_modhdr_type type;
	union {
		struct {
			u16 sid;
			u16 proto;
		} pppoe;
	};
};

int dpns_modhdr_write(struct dpns_priv *priv, u8 index,
		      const struct dpns_modhdr_cfg *cfg);
int dpns_modhdr_clear(struct dpns_priv *priv, u8 index);

#endif /* __SF_DPNS_MODHDR_H__ */
