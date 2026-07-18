// SPDX-License-Identifier: GPL-2.0
/*
 * Linux switchdev control plane for the SF21 DPNS physical ports.
 *
 * Only the six XGMAC netdevs explicitly registered by the MAC driver are
 * eligible. Bridged ports use the DPNS VLAN and L2 blocks; atomic FDB events
 * are moved to an ordered workqueue. The hardware VID key has no bridge/FID
 * component, so only one bridge is offloaded at a time. VLAN netdevs and all
 * other virtual or foreign netdevs are not handled by this implementation.
 */

#include <linux/bitmap.h>
#include <linux/etherdevice.h>
#include <linux/if_bridge.h>
#include <linux/if_vlan.h>
#include <linux/netdevice.h>
#include <linux/rtnetlink.h>
#include <linux/slab.h>
#include <linux/workqueue.h>
#include <net/switchdev.h>

#include "dpns.h"
#include "sf_dpns_l2.h"
#include "sf_dpns_nat.h"
#include "sf_dpns_port.h"
#include "sf_dpns_vlan.h"

#define DPNS_BRIDGE_FLAGS (BR_LEARNING | BR_FLOOD | BR_MCAST_FLOOD | \
			   BR_BCAST_FLOOD)

struct dpns_bridge_vlan {
	bool valid;
	u16 vid;
	u32 members;
	u32 untagged;
};

struct dpns_port {
	struct dpns_switchdev *sw;
	struct net_device *ndev;
	struct net_device *bridge;
	u8 id;
	u8 stp_state;
	u16 pvid;
	unsigned long bridge_flags;
	bool offload_fwd_mark;
};

struct dpns_switchdev {
	struct dpns_priv *priv;
	/* Serializes the software model and multi-table hardware updates. */
	struct mutex lock;
	struct net_device *bridge;
	struct dpns_port *ports[DPNS_PHYS_PORTS];
	u32 bridge_mask;
	bool vlan_filtering;
	DECLARE_BITMAP(vlan_map, DPNS_VLAN_ENTRIES);
	struct dpns_bridge_vlan vlans[DPNS_VLAN_ENTRIES];
	struct notifier_block netdevice_nb;
	struct notifier_block switchdev_nb;
	struct notifier_block switchdev_blocking_nb;
	struct workqueue_struct *fdb_wq;
};

struct dpns_fdb_work {
	struct work_struct work;
	struct dpns_port *port;
	unsigned long event;
	u8 addr[ETH_ALEN];
	u16 vid;
	bool added_by_user;
	bool is_local;
};

static DEFINE_MUTEX(dpns_instance_lock);
static struct dpns_switchdev *dpns_instance;

static struct dpns_port *dpns_port_from_dev(struct dpns_switchdev *sw,
					    const struct net_device *ndev)
{
	unsigned int i;

	if (!sw)
		return NULL;
	for (i = 0; i < DPNS_PHYS_PORTS; i++)
		if (sw->ports[i] && sw->ports[i]->ndev == ndev)
			return sw->ports[i];

	return NULL;
}

static bool dpns_switchdev_dev_check(const struct net_device *ndev)
{
	return dpns_port_from_dev(READ_ONCE(dpns_instance), ndev);
}

static u8 dpns_stp_state(u8 state)
{
	switch (state) {
	case BR_STATE_LISTENING:
		return DPNS_STP_LISTEN;
	case BR_STATE_LEARNING:
		return DPNS_STP_LEARN;
	case BR_STATE_FORWARDING:
		return DPNS_STP_FORWARD;
	default:
		return DPNS_STP_BLOCK;
	}
}

static struct dpns_bridge_vlan *dpns_vlan_find(struct dpns_switchdev *sw,
					       u16 vid)
{
	unsigned int i;

	for_each_set_bit(i, sw->vlan_map, DPNS_VLAN_ENTRIES)
		if (sw->vlans[i].valid && sw->vlans[i].vid == vid)
			return &sw->vlans[i];

	return NULL;
}

static struct dpns_bridge_vlan *dpns_vlan_get(struct dpns_switchdev *sw, u16 vid)
{
	struct dpns_bridge_vlan *vlan = dpns_vlan_find(sw, vid);
	unsigned long index;

	if (vlan)
		return vlan;
	index = vid ? find_next_zero_bit(sw->vlan_map,
					 DPNS_VLAN_ENTRIES, 1) : 0;
	if (index >= DPNS_VLAN_ENTRIES || test_bit(index, sw->vlan_map))
		return ERR_PTR(-ENOSPC);

	vlan = &sw->vlans[index];
	vlan->valid = true;
	vlan->vid = vid;
	set_bit(index, sw->vlan_map);
	return vlan;
}

static void dpns_vlan_put_empty(struct dpns_switchdev *sw,
				struct dpns_bridge_vlan *vlan)
{
	unsigned int index = vlan - sw->vlans;

	if (vlan->members)
		return;
	memset(vlan, 0, sizeof(*vlan));
	clear_bit(index, sw->vlan_map);
}

/* Caller holds sw->lock. Rebuild all 64 rows to make mode changes atomic. */
static int dpns_switchdev_apply(struct dpns_switchdev *sw)
{
	u8 stp[DPNS_PHYS_PORTS] = {};
	unsigned int i;
	int ret;

	for (i = 0; i < DPNS_PHYS_PORTS; i++) {
		struct dpns_port *port = sw->ports[i];
		bool bridged = port && port->bridge == sw->bridge;
		bool learning = bridged &&
				(port->bridge_flags & BR_LEARNING);

		if (port)
			stp[i] = dpns_stp_state(port->stp_state);
		ret = dpns_vlan_port_config(sw->priv, i, bridged, learning,
					    sw->vlan_filtering,
					    sw->vlan_filtering && bridged ?
					    port->pvid : 0);
		if (ret)
			return ret;
		if (port)
			WRITE_ONCE(port->offload_fwd_mark,
				   bridged && stp[i] == DPNS_STP_FORWARD);
	}

	for (i = 0; i < DPNS_VLAN_ENTRIES; i++) {
		ret = dpns_vlan_entry_clear(sw->priv, i);
		if (ret)
			return ret;
	}
	if (!sw->bridge_mask)
		return 0;
	if (sw->vlan_filtering) {
		for_each_set_bit(i, sw->vlan_map, DPNS_VLAN_ENTRIES) {
			struct dpns_bridge_vlan *vlan = &sw->vlans[i];
			u32 members = vlan->members & sw->bridge_mask;

			if (!vlan->valid || !members)
				continue;
			ret = dpns_vlan_entry_write(sw->priv, i, vlan->vid,
						    members,
						    vlan->untagged & members,
						    stp);
			if (ret)
				return ret;
		}
		return 0;
	}
	return dpns_vlan_entry_write(sw->priv, 0, 0, sw->bridge_mask,
				     sw->bridge_mask, stp);
}

static int dpns_port_vlan_add(struct dpns_port *port,
			      const struct switchdev_obj_port_vlan *obj,
			      struct netlink_ext_ack *extack)
{
	struct dpns_switchdev *sw = port->sw;
	struct dpns_bridge_vlan *vlan;
	int ret;

	if (obj->vid >= VLAN_N_VID)
		return -EINVAL;
	if (obj->obj.orig_dev != port->ndev)
		return 0;

	mutex_lock(&sw->lock);
	vlan = dpns_vlan_get(sw, obj->vid);
	if (IS_ERR(vlan)) {
		NL_SET_ERR_MSG_MOD(extack, "DPNS supports at most 63 active VLANs");
		ret = PTR_ERR(vlan);
		goto out;
	}
	vlan->members |= BIT(port->id);
	if (obj->flags & BRIDGE_VLAN_INFO_UNTAGGED)
		vlan->untagged |= BIT(port->id);
	else
		vlan->untagged &= ~BIT(port->id);
	if (obj->flags & BRIDGE_VLAN_INFO_PVID)
		port->pvid = obj->vid;
	else if (obj->changed && port->pvid == obj->vid)
		port->pvid = 0;
	ret = dpns_switchdev_apply(sw);
out:
	mutex_unlock(&sw->lock);
	return ret;
}

static int dpns_port_vlan_del(struct dpns_port *port,
			      const struct switchdev_obj_port_vlan *obj)
{
	struct dpns_switchdev *sw = port->sw;
	struct dpns_bridge_vlan *vlan;
	int ret = 0;

	if (obj->obj.orig_dev != port->ndev)
		return 0;
	mutex_lock(&sw->lock);
	vlan = dpns_vlan_find(sw, obj->vid);
	if (!vlan)
		goto out;
	vlan->members &= ~BIT(port->id);
	vlan->untagged &= ~BIT(port->id);
	if (port->pvid == obj->vid)
		port->pvid = 0;
	dpns_vlan_put_empty(sw, vlan);
	ret = dpns_switchdev_apply(sw);
out:
	mutex_unlock(&sw->lock);
	return ret;
}

static int dpns_mdb_change(struct dpns_port *port,
			   const struct switchdev_obj_port_mdb *mdb, bool add)
{
	struct dpns_switchdev *sw = port->sw;
	u32 mask = mdb->obj.orig_dev == port->ndev ? BIT(port->id) :
		   BIT(DPNS_HOST_PORT);
	int ret;

	mutex_lock(&sw->lock);
	if (add)
		ret = dpns_l2_addr_add_ports(sw->priv, mdb->addr, mdb->vid,
					     mask);
	else
		ret = dpns_l2_addr_del_ports(sw->priv, mdb->addr, mdb->vid,
					     mask);
	mutex_unlock(&sw->lock);
	return ret;
}

static int dpns_switchdev_obj_add(struct net_device *ndev, const void *ctx,
				  const struct switchdev_obj *obj,
				  struct netlink_ext_ack *extack)
{
	struct dpns_port *port = dpns_port_from_dev(dpns_instance, ndev);

	if (!port || !port->bridge || (ctx && ctx != port))
		return -EOPNOTSUPP;
	switch (obj->id) {
	case SWITCHDEV_OBJ_ID_PORT_VLAN:
		return dpns_port_vlan_add(port, SWITCHDEV_OBJ_PORT_VLAN(obj),
					  extack);
	case SWITCHDEV_OBJ_ID_PORT_MDB:
	case SWITCHDEV_OBJ_ID_HOST_MDB:
		return dpns_mdb_change(port, SWITCHDEV_OBJ_PORT_MDB(obj), true);
	default:
		return -EOPNOTSUPP;
	}
}

static int dpns_switchdev_obj_del(struct net_device *ndev, const void *ctx,
				  const struct switchdev_obj *obj)
{
	struct dpns_port *port = dpns_port_from_dev(dpns_instance, ndev);

	if (!port || !port->bridge || (ctx && ctx != port))
		return -EOPNOTSUPP;
	switch (obj->id) {
	case SWITCHDEV_OBJ_ID_PORT_VLAN:
		return dpns_port_vlan_del(port, SWITCHDEV_OBJ_PORT_VLAN(obj));
	case SWITCHDEV_OBJ_ID_PORT_MDB:
	case SWITCHDEV_OBJ_ID_HOST_MDB:
		return dpns_mdb_change(port, SWITCHDEV_OBJ_PORT_MDB(obj), false);
	default:
		return -EOPNOTSUPP;
	}
}

static int dpns_switchdev_attr_set(struct net_device *ndev, const void *ctx,
				   const struct switchdev_attr *attr,
				   struct netlink_ext_ack *extack)
{
	struct dpns_port *port = dpns_port_from_dev(dpns_instance, ndev);
	struct dpns_switchdev *sw;
	int ret = 0;

	if (!port || !port->bridge || (ctx && ctx != port))
		return -EOPNOTSUPP;
	sw = port->sw;

	switch (attr->id) {
	case SWITCHDEV_ATTR_ID_PORT_PRE_BRIDGE_FLAGS:
		if (attr->u.brport_flags.mask & ~DPNS_BRIDGE_FLAGS) {
			NL_SET_ERR_MSG_MOD(extack, "unsupported DPNS bridge port flag");
			return -EINVAL;
		}
		return 0;
	case SWITCHDEV_ATTR_ID_BRIDGE_VLAN_PROTOCOL:
		if (attr->u.vlan_protocol != htons(ETH_P_8021Q)) {
			NL_SET_ERR_MSG_MOD(extack, "DPNS supports only 802.1Q VLANs");
			return -EOPNOTSUPP;
		}
		return 0;
	case SWITCHDEV_ATTR_ID_BRIDGE_AGEING_TIME:
		/* Linux owns ageing and sends the resulting FDB deletes. */
		return 0;
	default:
		break;
	}

	mutex_lock(&sw->lock);
	switch (attr->id) {
	case SWITCHDEV_ATTR_ID_PORT_STP_STATE:
		port->stp_state = attr->u.stp_state;
		ret = dpns_switchdev_apply(sw);
		break;
	case SWITCHDEV_ATTR_ID_PORT_BRIDGE_FLAGS:
		port->bridge_flags &= ~attr->u.brport_flags.mask;
		port->bridge_flags |= attr->u.brport_flags.val &
				      attr->u.brport_flags.mask;
		ret = dpns_switchdev_apply(sw);
		break;
	case SWITCHDEV_ATTR_ID_BRIDGE_VLAN_FILTERING:
		if (sw->vlan_filtering != attr->u.vlan_filtering) {
			sw->vlan_filtering = attr->u.vlan_filtering;
			dpns_l2_flush_all(sw->priv);
		}
		ret = dpns_switchdev_apply(sw);
		break;
	default:
		ret = -EOPNOTSUPP;
		break;
	}
	mutex_unlock(&sw->lock);

	return ret;
}

static int dpns_switchdev_blocking_event(struct notifier_block *nb,
					 unsigned long event, void *ptr)
{
	struct net_device *ndev = switchdev_notifier_info_to_dev(ptr);
	int ret;

	switch (event) {
	case SWITCHDEV_PORT_OBJ_ADD:
		ret = switchdev_handle_port_obj_add(ndev, ptr,
						    dpns_switchdev_dev_check,
						    dpns_switchdev_obj_add);
		return notifier_from_errno(ret);
	case SWITCHDEV_PORT_OBJ_DEL:
		ret = switchdev_handle_port_obj_del(ndev, ptr,
						    dpns_switchdev_dev_check,
						    dpns_switchdev_obj_del);
		return notifier_from_errno(ret);
	case SWITCHDEV_PORT_ATTR_SET:
		ret = switchdev_handle_port_attr_set(ndev, ptr,
						     dpns_switchdev_dev_check,
						     dpns_switchdev_attr_set);
		return notifier_from_errno(ret);
	default:
		return NOTIFY_DONE;
	}
}

static void dpns_fdb_offloaded(struct dpns_fdb_work *fdb)
{
	struct switchdev_notifier_fdb_info info = {
		.addr = fdb->addr,
		.vid = fdb->vid,
		.offloaded = true,
	};

	call_switchdev_notifiers(SWITCHDEV_FDB_OFFLOADED, fdb->port->ndev,
				 &info.info, NULL);
}

static void dpns_fdb_work_fn(struct work_struct *work)
{
	struct dpns_fdb_work *fdb = container_of(work, struct dpns_fdb_work,
						 work);
	struct dpns_port *port = fdb->port;
	struct dpns_switchdev *sw = port->sw;
	u32 mask = fdb->is_local ? BIT(DPNS_HOST_PORT) : BIT(port->id);
	bool handled = false;
	int ret = 0;

	rtnl_lock();
	mutex_lock(&sw->lock);
	if (!port->bridge || port->bridge != sw->bridge)
		goto out_unlock;
	if (fdb->event == SWITCHDEV_FDB_ADD_TO_DEVICE) {
		ret = dpns_l2_addr_set(sw->priv, fdb->addr, fdb->vid, mask,
				       !fdb->added_by_user && !fdb->is_local);
		handled = true;
	} else {
		ret = dpns_l2_addr_del_ports(sw->priv, fdb->addr, fdb->vid,
					     mask);
		handled = true;
	}
out_unlock:
	mutex_unlock(&sw->lock);
	if (handled && !ret && fdb->event == SWITCHDEV_FDB_ADD_TO_DEVICE)
		dpns_fdb_offloaded(fdb);
	rtnl_unlock();
	if (ret)
		netdev_err(port->ndev, "failed to program FDB %pM VID %u: %d\n",
			   fdb->addr, fdb->vid, ret);
	dev_put(port->ndev);
	kfree(fdb);
}

static int dpns_switchdev_event(struct notifier_block *nb,
				unsigned long event, void *ptr)
{
	struct switchdev_notifier_fdb_info *info = ptr;
	struct net_device *ndev = switchdev_notifier_info_to_dev(ptr);
	struct dpns_switchdev *sw = container_of(nb, struct dpns_switchdev,
						 switchdev_nb);
	struct dpns_fdb_work *fdb;
	struct dpns_port *port;

	if (event != SWITCHDEV_FDB_ADD_TO_DEVICE &&
	    event != SWITCHDEV_FDB_DEL_TO_DEVICE)
		return NOTIFY_DONE;
	port = dpns_port_from_dev(sw, ndev);
	if (!port || !READ_ONCE(port->bridge))
		return NOTIFY_DONE;
	if (info->info.ctx && info->info.ctx != port)
		return NOTIFY_DONE;

	fdb = kzalloc_obj(*fdb, GFP_ATOMIC);
	if (!fdb)
		return notifier_from_errno(-ENOMEM);
	INIT_WORK(&fdb->work, dpns_fdb_work_fn);
	fdb->port = port;
	fdb->event = event;
	ether_addr_copy(fdb->addr, info->addr);
	fdb->vid = info->vid;
	fdb->added_by_user = info->added_by_user;
	fdb->is_local = info->is_local;
	dev_hold(ndev);
	queue_work(sw->fdb_wq, &fdb->work);

	return NOTIFY_DONE;
}

static int dpns_port_pre_bridge_join(struct dpns_port *port,
				     struct net_device *bridge,
				     struct netlink_ext_ack *extack)
{
	struct dpns_switchdev *sw = port->sw;
	int ret = 0;

	mutex_lock(&sw->lock);
	if (sw->bridge && sw->bridge != bridge) {
		NL_SET_ERR_MSG_MOD(extack,
				   "DPNS can offload only one bridge/FID at a time");
		ret = -EOPNOTSUPP;
	}
	mutex_unlock(&sw->lock);
	return ret;
}

static int dpns_port_bridge_join(struct dpns_port *port,
				 struct net_device *bridge,
				 struct netlink_ext_ack *extack)
{
	struct dpns_switchdev *sw = port->sw;
	int ret;

	mutex_lock(&sw->lock);
	if (!sw->bridge)
		sw->bridge = bridge;
	port->bridge = bridge;
	port->stp_state = br_port_get_stp_state(port->ndev);
	port->bridge_flags = 0;
	if (br_port_flag_is_set(port->ndev, BR_LEARNING))
		port->bridge_flags |= BR_LEARNING;
	if (br_port_flag_is_set(port->ndev, BR_FLOOD))
		port->bridge_flags |= BR_FLOOD;
	if (br_port_flag_is_set(port->ndev, BR_MCAST_FLOOD))
		port->bridge_flags |= BR_MCAST_FLOOD;
	if (br_port_flag_is_set(port->ndev, BR_BCAST_FLOOD))
		port->bridge_flags |= BR_BCAST_FLOOD;
	sw->vlan_filtering = br_vlan_enabled(bridge);
	sw->bridge_mask |= BIT(port->id);
	ret = dpns_switchdev_apply(sw);
	mutex_unlock(&sw->lock);
	if (ret)
		goto rollback;

	ret = switchdev_bridge_port_offload(port->ndev, port->ndev, port,
					    &sw->switchdev_nb,
					    &sw->switchdev_blocking_nb,
					    false, extack);
	if (!ret) {
		netdev_info(port->ndev, "physical port offloaded to bridge %s\n",
			    bridge->name);
		return 0;
	}

rollback:
	mutex_lock(&sw->lock);
	port->bridge = NULL;
	port->offload_fwd_mark = false;
	sw->bridge_mask &= ~BIT(port->id);
	if (!sw->bridge_mask)
		sw->bridge = NULL;
	dpns_switchdev_apply(sw);
	mutex_unlock(&sw->lock);
	return ret;
}

static void dpns_port_bridge_leave(struct dpns_port *port)
{
	struct dpns_switchdev *sw = port->sw;

	switchdev_bridge_port_unoffload(port->ndev, port,
					&sw->switchdev_nb,
					&sw->switchdev_blocking_nb);
	mutex_lock(&sw->lock);
	dpns_l2_flush_port(sw->priv, port->id);
	port->bridge = NULL;
	port->pvid = 0;
	port->offload_fwd_mark = false;
	sw->bridge_mask &= ~BIT(port->id);
	if (!sw->bridge_mask) {
		sw->bridge = NULL;
		sw->vlan_filtering = false;
		dpns_l2_flush_all(sw->priv);
	}
	dpns_switchdev_apply(sw);
	mutex_unlock(&sw->lock);
	netdev_info(port->ndev, "physical port left DPNS bridge offload\n");
}

static int dpns_netdevice_event(struct notifier_block *nb,
				unsigned long event, void *ptr)
{
	struct netdev_notifier_changeupper_info *info = ptr;
	struct net_device *ndev = netdev_notifier_info_to_dev(ptr);
	struct dpns_switchdev *sw = container_of(nb, struct dpns_switchdev,
						 netdevice_nb);
	struct dpns_port *port = dpns_port_from_dev(sw, ndev);
	int ret = 0;

	if (!port)
		return NOTIFY_DONE;
	if (event != NETDEV_PRECHANGEUPPER && event != NETDEV_CHANGEUPPER)
		return NOTIFY_DONE;
	if (!netif_is_bridge_master(info->upper_dev))
		return NOTIFY_DONE;

	if (event == NETDEV_PRECHANGEUPPER && info->linking)
		ret = dpns_port_pre_bridge_join(port, info->upper_dev,
						info->info.extack);
	else if (event == NETDEV_CHANGEUPPER && info->linking)
		ret = dpns_port_bridge_join(port, info->upper_dev,
					    info->info.extack);
	else if (event == NETDEV_CHANGEUPPER)
		dpns_port_bridge_leave(port);

	return notifier_from_errno(ret);
}

struct dpns_port *sf_dpns_port_register(struct net_device *ndev, u8 id)
{
	struct dpns_switchdev *sw;
	struct dpns_port *port;

	if (!ndev || id >= DPNS_PHYS_PORTS)
		return ERR_PTR(-EINVAL);
	mutex_lock(&dpns_instance_lock);
	sw = dpns_instance;
	if (!sw) {
		mutex_unlock(&dpns_instance_lock);
		return ERR_PTR(-EPROBE_DEFER);
	}
	if (sw->ports[id]) {
		mutex_unlock(&dpns_instance_lock);
		return ERR_PTR(-EBUSY);
	}
	port = kzalloc(sizeof(*port), GFP_KERNEL);
	if (!port) {
		mutex_unlock(&dpns_instance_lock);
		return ERR_PTR(-ENOMEM);
	}
	port->sw = sw;
	port->ndev = ndev;
	port->id = id;
	port->stp_state = BR_STATE_DISABLED;
	sw->ports[id] = port;
	mutex_unlock(&dpns_instance_lock);

	return port;
}
EXPORT_SYMBOL_GPL(sf_dpns_port_register);

void sf_dpns_port_unregister(struct dpns_port *port)
{
	struct dpns_switchdev *sw;

	if (!port)
		return;
	sw = port->sw;
	flush_workqueue(sw->fdb_wq);
	mutex_lock(&dpns_instance_lock);
	if (sw->ports[port->id] == port)
		sw->ports[port->id] = NULL;
	mutex_unlock(&dpns_instance_lock);
	kfree(port);
}
EXPORT_SYMBOL_GPL(sf_dpns_port_unregister);

bool sf_dpns_port_offload_fwd_mark(const struct dpns_port *port)
{
	return port && READ_ONCE(port->offload_fwd_mark);
}
EXPORT_SYMBOL_GPL(sf_dpns_port_offload_fwd_mark);

int sf_dpns_port_id_by_netdev(const struct net_device *ndev, u8 *id)
{
	struct dpns_port *port;

	port = dpns_port_from_dev(READ_ONCE(dpns_instance), ndev);
	if (!port)
		return -EOPNOTSUPP;
	*id = port->id;
	return 0;
}

int sf_dpns_port_id_by_ifindex(int ifindex, u8 *id)
{
	struct dpns_switchdev *sw = READ_ONCE(dpns_instance);
	unsigned int i;

	if (!sw)
		return -ENODEV;
	for (i = 0; i < DPNS_PHYS_PORTS; i++)
		if (sw->ports[i] && sw->ports[i]->ndev->ifindex == ifindex) {
			*id = i;
			return 0;
		}
	return -EOPNOTSUPP;
}

int sf_dpns_port_setup_tc(struct dpns_port *port, enum tc_setup_type type,
			  void *type_data)
{
	if (!port)
		return -EOPNOTSUPP;
	return dpns_nat_setup_tc(port->sw->priv, port, type, type_data);
}
EXPORT_SYMBOL_GPL(sf_dpns_port_setup_tc);

int dpns_switchdev_init(struct dpns_priv *priv)
{
	struct dpns_switchdev *sw;
	int ret;

	sw = devm_kzalloc(priv->dev, sizeof(*sw), GFP_KERNEL);
	if (!sw)
		return -ENOMEM;
	sw->priv = priv;
	mutex_init(&sw->lock);
	sw->fdb_wq = alloc_ordered_workqueue("sf_dpns_fdb", WQ_MEM_RECLAIM);
	if (!sw->fdb_wq)
		return -ENOMEM;
	sw->netdevice_nb.notifier_call = dpns_netdevice_event;
	sw->switchdev_nb.notifier_call = dpns_switchdev_event;
	sw->switchdev_blocking_nb.notifier_call = dpns_switchdev_blocking_event;

	ret = register_switchdev_notifier(&sw->switchdev_nb);
	if (ret)
		goto err_wq;
	ret = register_switchdev_blocking_notifier(&sw->switchdev_blocking_nb);
	if (ret)
		goto err_atomic;
	ret = register_netdevice_notifier(&sw->netdevice_nb);
	if (ret)
		goto err_blocking;

	mutex_lock(&dpns_instance_lock);
	if (dpns_instance) {
		ret = -EBUSY;
	} else {
		dpns_instance = sw;
		priv->switchdev = sw;
	}
	mutex_unlock(&dpns_instance_lock);
	if (ret)
		goto err_netdev;
	return 0;

err_netdev:
	unregister_netdevice_notifier(&sw->netdevice_nb);
err_blocking:
	unregister_switchdev_blocking_notifier(&sw->switchdev_blocking_nb);
err_atomic:
	unregister_switchdev_notifier(&sw->switchdev_nb);
err_wq:
	destroy_workqueue(sw->fdb_wq);
	return ret;
}

void dpns_switchdev_fini(struct dpns_priv *priv)
{
	struct dpns_switchdev *sw = priv->switchdev;

	if (!sw)
		return;
	mutex_lock(&dpns_instance_lock);
	if (dpns_instance == sw)
		dpns_instance = NULL;
	mutex_unlock(&dpns_instance_lock);
	unregister_netdevice_notifier(&sw->netdevice_nb);
	unregister_switchdev_blocking_notifier(&sw->switchdev_blocking_nb);
	unregister_switchdev_notifier(&sw->switchdev_nb);
	flush_workqueue(sw->fdb_wq);
	destroy_workqueue(sw->fdb_wq);
	priv->switchdev = NULL;
}
