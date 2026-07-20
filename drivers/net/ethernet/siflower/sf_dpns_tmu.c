// SPDX-License-Identifier: GPL-2.0
/*
 * SF21 DPNS Traffic Management Unit.
 *
 * Each of the ten TMU ports has eight packet queues, two schedulers and six
 * token-bucket shapers. Scheduler 0 collects queues 0..3; scheduler 1 merges
 * that output with queues 4..7 and feeds the physical port. A shaper's
 * location field selects a queue or scheduler output, with location 0 being
 * the final scheduler-1 output. The dequeue stage counts 24 bytes of Ethernet
 * preamble, FCS and inter-packet gap in addition to the frame length.
 *
 * Initialization resets queue thresholds, builds the two-level strict-
 * priority scheduler and leaves all shapers disabled. To offload a root TBF,
 * software disables shaper 0, programs its fixed-point byte credit rate and
 * burst ceiling, attaches it to location 0, then enables it. Teardown disables
 * the shaper first so partially updated rate parameters can never take effect.
 * Root PRIO and strict-only ETS restore the fixed scheduler connections and
 * select strict priority after validating that Linux's band map is the exact
 * reverse of the hardware's Q0-lowest to Q7-highest queue numbering. TBFs on
 * their four highest bands use the native per-queue shapers for Q4..Q7.
 */

#include <linux/of_device.h>
#include <linux/errno.h>
#include <linux/init.h>
#include <linux/math64.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/platform_device.h>
#include <linux/debugfs.h>
#include <linux/io.h>
#include <net/pkt_cls.h>

#include "dpns.h"
#include "sf_dpns_tmu.h"
#include "sf_dpns_vlan.h"

#define DPNS_TMU_ROOT_SHAPER 0
#define DPNS_TMU_PORT_OUTPUT 0
#define DPNS_TMU_FIRST_SHAPED_QUEUE 4
#define DPNS_TMU_QUEUE_SHAPER(queue) ((queue) - 2)
#define DPNS_TMU_MIN_CREDIT_DEFAULT 0x0003ff00

struct dpns_tmu_port {
	u32 tbf_handle;
	u32 prio_handle;
	u32 ets_handle;
	u32 queue_tbf_handle[QUE_MAX_NUM_PER_PORT];
};

struct dpns_tmu {
	struct dpns_priv *priv;
	/* Serializes qdisc state and each shaper's multi-register update. */
	struct mutex lock;
	struct dpns_tmu_port ports[TMU_MAX_PORT_CNT];
};

static u32 tmu_rm32(struct dpns_priv *priv, u32 reg, u32 mask, u32 shift)
{
	u32 t;

	t = dpns_r32(priv, reg);
	t &= mask;
	t >>= shift;

	return t;
}

static void tmu_rmw32(struct dpns_priv *priv, u32 reg, u32 mask, u32 shift,
		      u32 val)
{
	u32 t;

	val <<= shift;
	val &= mask;
	t = dpns_r32(priv, reg);
	t &= ~mask;
	t |= val;
	dpns_w32(priv, reg, t);
}

static int is_valid_port_idx(struct dpns_priv *priv, u32 port)
{
	if (port >= TMU_MAX_PORT_CNT)
		return 0;

	return 1;
}

static int is_valid_queue_idx(u32 q)
{
	if (q >= QUE_MAX_NUM_PER_PORT)
		return 0;

	return 1;
}

static int is_valid_sched_idx(struct dpns_priv *priv, u32 sched)
{
	if (sched >= QUE_SCH_NUM_PER_PORT)
		return 0;

	return 1;
}

static int is_valid_shaper_idx(struct dpns_priv *priv, u32 shaper)
{
	if (shaper >= QUE_SHAPER_NUM_PER_PORT)
		return 0;

	return 1;
}

static int tmu_port_writel(struct dpns_priv *priv, u32 port, u32 reg, u32 val)
{
	if (!is_valid_port_idx(priv, port))
		return -EINVAL;

	dpns_w32(priv, TMU_PORT_BASE(port) + reg, val);
	return 0;
}

static int tmu_port_rm32(struct dpns_priv *priv, u32 port, u32 reg, u32 mask,
			 u32 shift, u32 *val)
{
	if (!is_valid_port_idx(priv, port))
		return -EINVAL;

	*val = tmu_rm32(priv, TMU_PORT_BASE(port) + reg, mask, shift);
	return 0;
}

static int tmu_port_rmw32(struct dpns_priv *priv, u32 port, u32 reg, u32 mask,
			  u32 shift, u32 val)
{
	if (!is_valid_port_idx(priv, port))
		return -EINVAL;

	tmu_rmw32(priv, TMU_PORT_BASE(port) + reg, mask, shift, val);

	return 0;
}

static int tmu_queue_writel(struct dpns_priv *priv, u32 port, u32 queue,
			    u32 reg, u32 val)
{
	if (!is_valid_queue_idx(queue))
		return -EINVAL;

	return tmu_port_writel(priv, port, TMU_QUEUE_BASE(queue) + reg, val);
}

static int tmu_sched_writel(struct dpns_priv *priv, u32 port, u32 sched,
			    u32 reg, u32 val)
{
	if (!is_valid_sched_idx(priv, sched))
		return -EINVAL;

	return tmu_port_writel(priv, port, TMU_SCHED_BASE(sched) + reg, val);
}

static int tmu_shaper_writel(struct dpns_priv *priv, u32 port, u32 shaper,
			     u32 reg, u32 val)
{
	if (!is_valid_shaper_idx(priv, shaper))
		return -EINVAL;

	return tmu_port_writel(priv, port, TMU_SHAPER_BASE(shaper) + reg, val);
}

static int tmu_shaper_rmw32(struct dpns_priv *priv, u32 port, u32 shaper,
			    u32 reg, u32 mask, u32 shift, u32 val)
{
	if (!is_valid_shaper_idx(priv, shaper))
		return -EINVAL;

	return tmu_port_rmw32(priv, port, TMU_SHAPER_BASE(shaper) + reg, mask,
			      shift, val);
}

static int tdq_ctrl_is_configurable(struct dpns_priv *priv, u32 port)
{
	u32 val = 0;
	int err;

	err = tmu_port_rm32(priv, port, TMU_TDQ_CTRL, TMU_TDQ_ALLOW_CFG,
			    TMU_TDQ_ALLOW_CFG_SHIFT, &val);
	if (err)
		return 0;

	return val;
}

static void tmu_port_queue_cfg(struct dpns_priv *priv, u32 port)
{
	int comp;

	for (comp = 0; comp < QUE_MAX_NUM_PER_PORT; comp++) {
		tmu_queue_writel(priv, port, comp, TMU_PORT_QUEUE_CFG0,
				 0x00011f00);

		tmu_queue_writel(priv, port, comp, TMU_PORT_QUEUE_CFG1,
				 0x00000000);
		tmu_queue_writel(priv, port, comp, TMU_PORT_QUEUE_CFG2,
				 0x00000000);
		tmu_queue_writel(priv, port, comp, TMU_PORT_QUEUE_STS0,
				 0x00000000);
		tmu_queue_writel(priv, port, comp, TMU_PORT_QUEUE_STS1,
				 0x00000000);
		tmu_queue_writel(priv, port, comp, TMU_PORT_QUEUE_STS2,
				 0x00000000);
		tmu_queue_writel(priv, port, comp, TMU_PORT_QUEUE_CFG3,
				 0x000005ee);
	}
}

static void tmu_port_sched_cfg(struct dpns_priv *priv, u32 port)
{
	int comp;

	for (comp = 0; comp < QUE_SCH_NUM_PER_PORT; comp++) {
		tmu_sched_writel(priv, port, comp, TMU_SCH_CTRL, 0x00000000);
		tmu_sched_writel(priv, port, comp, TMU_SCH_Q0_WEIGHT,
				 0x00000000);
		tmu_sched_writel(priv, port, comp, TMU_SCH_Q1_WEIGHT,
				 0x00000000);
		tmu_sched_writel(priv, port, comp, TMU_SCH_Q2_WEIGHT,
				 0x00000000);
		tmu_sched_writel(priv, port, comp, TMU_SCH_Q3_WEIGHT,
				 0x00000000);
		tmu_sched_writel(priv, port, comp, TMU_SCH_Q4_WEIGHT,
				 0x00000000);
		tmu_sched_writel(priv, port, comp, TMU_SCH_Q5_WEIGHT,
				 0x00000000);
		tmu_sched_writel(priv, port, comp, TMU_SCH_Q6_WEIGHT,
				 0x00000000);
		tmu_sched_writel(priv, port, comp, TMU_SCH_Q7_WEIGHT,
				 0x00000000);

		switch (comp) {
		case 0:
			tmu_sched_writel(priv, port, comp, TMU_SCH_QUEUE_ALLOC0,
					 0x03020100);
			tmu_sched_writel(priv, port, comp, TMU_SCH_QUEUE_ALLOC1,
					 0x08080808);
			break;

		case 1:
			tmu_sched_writel(priv, port, comp, TMU_SCH_QUEUE_ALLOC0,
					 0x06050400);
			tmu_sched_writel(priv, port, comp, TMU_SCH_QUEUE_ALLOC1,
					 0x08080807);
			break;

		default:
			break;
		}

		tmu_sched_writel(priv, port, comp, TMU_SCH_BIT_RATE,
				 0x00000000);

		if (comp == 0)
			tmu_sched_writel(priv, port, comp, TMU_SCH0_POS,
					 0x00000000);
	}
}

static void tmu_port_shaper_cfg(struct dpns_priv *priv, u32 port)
{
	int comp;

	for (comp = 0; comp < QUE_SHAPER_NUM_PER_PORT; comp++) {
		tmu_shaper_writel(priv, port, comp, TMU_SHP_CTRL, 0x00000000);
		tmu_shaper_writel(priv, port, comp, TMU_SHP_WEIGHT, 0x00000000);
		tmu_shaper_writel(priv, port, comp, TMU_SHP_CTRL2, 0x00000000);
		tmu_shaper_writel(priv, port, comp, TMU_SHP_MIN_CREDIT,
				  0x0003ff00);
		tmu_shaper_writel(priv, port, comp, TMU_SHP_MAX_CREDIT,
				  0x00000400);
		tmu_shaper_rmw32(priv, port, comp, TMU_SHP_CTRL2, TMU_SHP_POS,
				 TMU_SHP_POS_SHIFT, comp);
	}
}

static void _tmu_reset(struct dpns_priv *priv, u32 port)
{
	tmu_port_queue_cfg(priv, port);
	tmu_port_sched_cfg(priv, port);
	tmu_port_shaper_cfg(priv, port);

	/* Include the preamble, FCS and inter-packet gap in shaper rates. */
	tmu_port_writel(priv, port, TMU_TDQ_IFG, 0x00000018);

	if (tdq_ctrl_is_configurable(priv, port))
		tmu_port_writel(priv, port, TMU_TDQ_CTRL, 0x0000002f);
}

static int tmu_reset(struct dpns_priv *priv)
{
	int port;

	dpns_w32(priv, TMU_CTRL, 0x00000006);
	dpns_w32(priv, TMU_LLM_FIFO_CTRL0, 0x07fe07ff);
	dpns_w32(priv, TMU_LLM_FIFO_CTRL1, 0x00280024);

	for (port = 0; port < TMU_MAX_PORT_CNT; port++)
		_tmu_reset(priv, port);

	return 0;
}

static void dpns_tmu_shaper_disable(struct dpns_tmu *tmu, u8 port, u8 shaper)
{
	struct dpns_priv *priv = tmu->priv;

	tmu_shaper_writel(priv, port, shaper, TMU_SHP_CTRL, 0);
	tmu_shaper_writel(priv, port, shaper, TMU_SHP_WEIGHT, 0);
}

static void dpns_tmu_tbf_disable(struct dpns_tmu *tmu, u8 port)
{
	dpns_tmu_shaper_disable(tmu, port, DPNS_TMU_ROOT_SHAPER);
	tmu->ports[port].tbf_handle = 0;
}

static void dpns_tmu_queue_tbf_disable(struct dpns_tmu *tmu, u8 port,
				       u8 queue)
{
	dpns_tmu_shaper_disable(tmu, port, DPNS_TMU_QUEUE_SHAPER(queue));
	tmu->ports[port].queue_tbf_handle[queue] = 0;
}

static void dpns_tmu_queue_tbfs_disable(struct dpns_tmu *tmu, u8 port)
{
	u8 queue;

	for (queue = DPNS_TMU_FIRST_SHAPED_QUEUE;
	     queue < QUE_MAX_NUM_PER_PORT; queue++)
		dpns_tmu_queue_tbf_disable(tmu, port, queue);
}

static int dpns_tmu_rate_cfg(struct dpns_tmu *tmu, u64 rate, u32 *ctrl,
			     u32 *weight)
{
	u64 clk = clk_get_rate(tmu->priv->clk);
	u64 scaled;
	int div;

	if (!clk || !rate)
		return -EINVAL;

	/* The 20-bit weight is an unsigned 8.12 byte value. Pick the
	 * largest divider that fits to retain the greatest precision.
	 */
	for (div = TMU_SHP_CLKDIV_MAX; div >= 0; div--) {
		scaled = DIV_ROUND_CLOSEST_ULL(rate << (div + 13), clk);
		if (scaled && scaled <= GENMASK(19, 0)) {
			*ctrl = FIELD_PREP(TMU_SHP_CLK_DIV, div) | TMU_SHP_EN;
			*weight = scaled;
			return 0;
		}
	}

	return -ERANGE;
}

static int dpns_tmu_shaper_replace(struct dpns_tmu *tmu, u8 port, u8 shaper,
				   u8 position,
				   struct tc_tbf_qopt_offload *qopt)
{
	const struct tc_tbf_qopt_offload_replace_params *params =
		&qopt->replace_params;
	u32 ctrl, ctrl2, max_credit, weight;
	int ret;

	if (params->rate.linklayer == TC_LINKLAYER_ATM ||
	    params->rate.overhead || params->rate.mpu)
		return -EOPNOTSUPP;
	if (!params->max_size ||
	    params->max_size > FIELD_MAX(TMU_SHP_MAX_CREDIT_MASK))
		return -ERANGE;

	ret = dpns_tmu_rate_cfg(tmu, params->rate.rate_bytes_ps, &ctrl,
				&weight);
	if (ret)
		return ret;

	max_credit = FIELD_PREP(TMU_SHP_MAX_CREDIT_MASK, params->max_size);
	ctrl2 = FIELD_PREP(TMU_SHP_POS, position) |
		FIELD_PREP(TMU_SHP_BIT_RATE, TMU_SHP_SCHED_PKT_LEN) |
		FIELD_PREP(TMU_SHP_MODE, TMU_SHP_MODE_KEEP_CREDIT);

	/* Disable first: CTRL is also the commit point for a new rate. */
	tmu_shaper_writel(tmu->priv, port, shaper, TMU_SHP_CTRL, 0);
	tmu_shaper_writel(tmu->priv, port, shaper, TMU_SHP_WEIGHT, weight);
	tmu_shaper_writel(tmu->priv, port, shaper, TMU_SHP_MAX_CREDIT,
			  max_credit);
	tmu_shaper_writel(tmu->priv, port, shaper, TMU_SHP_MIN_CREDIT,
			  DPNS_TMU_MIN_CREDIT_DEFAULT);
	tmu_shaper_writel(tmu->priv, port, shaper, TMU_SHP_CTRL2, ctrl2);
	tmu_shaper_writel(tmu->priv, port, shaper, TMU_SHP_CTRL, ctrl);

	return 0;
}

static int dpns_tmu_tbf_queue(struct dpns_tmu *tmu, u8 port, u32 parent,
			      u8 *queue)
{
	u32 sched_handle = tmu->ports[port].prio_handle ?:
			   tmu->ports[port].ets_handle;
	u32 band;

	if (!sched_handle || TC_H_MAJ(parent) != TC_H_MAJ(sched_handle))
		return -EOPNOTSUPP;

	band = TC_H_MIN(parent);
	if (!band || band > QUE_MAX_NUM_PER_PORT -
				    DPNS_TMU_FIRST_SHAPED_QUEUE)
		return -EOPNOTSUPP;

	*queue = QUE_MAX_NUM_PER_PORT - band;
	return 0;
}

static int dpns_tmu_tbf_replace(struct dpns_tmu *tmu, u8 port,
				struct tc_tbf_qopt_offload *qopt)
{
	u8 queue, shaper, position;
	int ret;

	if (qopt->parent == TC_H_ROOT) {
		shaper = DPNS_TMU_ROOT_SHAPER;
		position = DPNS_TMU_PORT_OUTPUT;
		ret = dpns_tmu_shaper_replace(tmu, port, shaper, position, qopt);
		if (ret)
			return ret;

		dpns_tmu_queue_tbfs_disable(tmu, port);
		tmu->ports[port].tbf_handle = qopt->handle;
		tmu->ports[port].prio_handle = 0;
		tmu->ports[port].ets_handle = 0;
		return 0;
	}

	ret = dpns_tmu_tbf_queue(tmu, port, qopt->parent, &queue);
	if (ret)
		return ret;

	shaper = DPNS_TMU_QUEUE_SHAPER(queue);
	ret = dpns_tmu_shaper_replace(tmu, port, shaper, shaper, qopt);
	if (!ret)
		tmu->ports[port].queue_tbf_handle[queue] = qopt->handle;

	return ret;
}

static bool dpns_tmu_tbf_active(struct dpns_tmu *tmu, u8 port, u32 handle)
{
	u8 queue;

	if (tmu->ports[port].tbf_handle == handle)
		return true;
	for (queue = DPNS_TMU_FIRST_SHAPED_QUEUE;
	     queue < QUE_MAX_NUM_PER_PORT; queue++)
		if (tmu->ports[port].queue_tbf_handle[queue] == handle)
			return true;

	return false;
}

static void dpns_tmu_tbf_destroy(struct dpns_tmu *tmu, u8 port, u32 handle)
{
	u8 queue;

	if (tmu->ports[port].tbf_handle == handle) {
		dpns_tmu_tbf_disable(tmu, port);
		return;
	}

	for (queue = DPNS_TMU_FIRST_SHAPED_QUEUE;
	     queue < QUE_MAX_NUM_PER_PORT; queue++)
		if (tmu->ports[port].queue_tbf_handle[queue] == handle) {
			dpns_tmu_queue_tbf_disable(tmu, port, queue);
			return;
		}
}

static bool dpns_tmu_prio_map_supported(const u8 *priomap)
{
	int prio;

	/* Linux band 0 and TMU queue 7 are both the highest priority.  The
	 * hardware classifier maps an 802.1p priority directly to the queue
	 * with the same number, hence the reversed band numbering.
	 */
	for (prio = 0; prio <= TC_PRIO_MAX; prio++)
		if (priomap[prio] != QUE_MAX_NUM_PER_PORT - 1 -
					     (prio & (QUE_MAX_NUM_PER_PORT - 1)))
			return false;

	return true;
}

static bool dpns_tmu_graft_supported(struct dpns_tmu *tmu, u8 port, u8 band,
				     u32 child_handle)
{
	u8 queue;

	if (!child_handle)
		return true;
	if (band >= QUE_MAX_NUM_PER_PORT - DPNS_TMU_FIRST_SHAPED_QUEUE)
		return false;

	queue = QUE_MAX_NUM_PER_PORT - 1 - band;
	return tmu->ports[port].queue_tbf_handle[queue] == child_handle;
}

static int dpns_tmu_prio_replace(struct dpns_tmu *tmu, u8 port,
				 struct tc_prio_qopt_offload *qopt)
{
	const struct tc_prio_qopt_offload_params *params =
		&qopt->replace_params;

	if (qopt->parent != TC_H_ROOT ||
	    params->bands != QUE_MAX_NUM_PER_PORT ||
	    !dpns_tmu_prio_map_supported(params->priomap)) {
		dpns_tmu_queue_tbfs_disable(tmu, port);
		tmu->ports[port].prio_handle = 0;
		return -EOPNOTSUPP;
	}

	/* Restore the fixed two-level topology as well as selecting PQ.  This
	 * makes replacement independent of any earlier debug register writes.
	 */
	if (tmu->ports[port].tbf_handle)
		dpns_tmu_tbf_disable(tmu, port);
	tmu_port_sched_cfg(tmu->priv, port);
	tmu->ports[port].prio_handle = qopt->handle;
	tmu->ports[port].ets_handle = 0;

	return 0;
}

static int dpns_tmu_setup_prio(struct dpns_tmu *tmu, u8 port,
			       struct tc_prio_qopt_offload *qopt)
{
	switch (qopt->command) {
	case TC_PRIO_REPLACE:
		return dpns_tmu_prio_replace(tmu, port, qopt);
	case TC_PRIO_DESTROY:
		if (tmu->ports[port].prio_handle == qopt->handle) {
			dpns_tmu_queue_tbfs_disable(tmu, port);
			tmu_port_sched_cfg(tmu->priv, port);
			tmu->ports[port].prio_handle = 0;
		}
		return 0;
	case TC_PRIO_STATS:
		return tmu->ports[port].prio_handle == qopt->handle ? 0 :
			-EOPNOTSUPP;
	case TC_PRIO_GRAFT:
		if (dpns_tmu_graft_supported(tmu, port,
					     qopt->graft_params.band,
					     qopt->graft_params.child_handle))
			return 0;
		dpns_tmu_queue_tbfs_disable(tmu, port);
		tmu->ports[port].prio_handle = 0;
		return -EOPNOTSUPP;
	default:
		return -EOPNOTSUPP;
	}
}

static int dpns_tmu_ets_replace(struct dpns_tmu *tmu, u8 port,
				struct tc_ets_qopt_offload *qopt)
{
	const struct tc_ets_qopt_offload_replace_params *params =
		&qopt->replace_params;
	int band;

	if (qopt->parent != TC_H_ROOT ||
	    params->bands != QUE_MAX_NUM_PER_PORT ||
	    !dpns_tmu_prio_map_supported(params->priomap))
		goto unsupported;

	for (band = 0; band < params->bands; band++)
		if (params->quanta[band])
			goto unsupported;

	if (tmu->ports[port].tbf_handle)
		dpns_tmu_tbf_disable(tmu, port);
	tmu_port_sched_cfg(tmu->priv, port);
	tmu->ports[port].ets_handle = qopt->handle;
	tmu->ports[port].prio_handle = 0;

	return 0;

unsupported:
	dpns_tmu_queue_tbfs_disable(tmu, port);
	tmu->ports[port].ets_handle = 0;
	return -EOPNOTSUPP;
}

static int dpns_tmu_setup_ets(struct dpns_tmu *tmu, u8 port,
			      struct tc_ets_qopt_offload *qopt)
{
	switch (qopt->command) {
	case TC_ETS_REPLACE:
		return dpns_tmu_ets_replace(tmu, port, qopt);
	case TC_ETS_DESTROY:
		if (tmu->ports[port].ets_handle == qopt->handle) {
			dpns_tmu_queue_tbfs_disable(tmu, port);
			tmu_port_sched_cfg(tmu->priv, port);
			tmu->ports[port].ets_handle = 0;
		}
		return 0;
	case TC_ETS_STATS:
		return tmu->ports[port].ets_handle == qopt->handle ? 0 :
			-EOPNOTSUPP;
	case TC_ETS_GRAFT:
		if (dpns_tmu_graft_supported(tmu, port,
					     qopt->graft_params.band,
					     qopt->graft_params.child_handle))
			return 0;
		dpns_tmu_queue_tbfs_disable(tmu, port);
		tmu->ports[port].ets_handle = 0;
		return -EOPNOTSUPP;
	default:
		return -EOPNOTSUPP;
	}
}

int dpns_tmu_setup_tc(struct dpns_priv *priv, u8 port, enum tc_setup_type type,
		      void *type_data)
{
	struct dpns_tmu *tmu = priv->tmu;
	int ret;

	if (!tmu || port >= DPNS_PHYS_PORTS)
		return -EOPNOTSUPP;

	mutex_lock(&tmu->lock);
	switch (type) {
	case TC_SETUP_QDISC_TBF: {
		struct tc_tbf_qopt_offload *qopt = type_data;

		switch (qopt->command) {
		case TC_TBF_REPLACE:
			ret = dpns_tmu_tbf_replace(tmu, port, qopt);
			break;
		case TC_TBF_DESTROY:
			dpns_tmu_tbf_destroy(tmu, port, qopt->handle);
			ret = 0;
			break;
		case TC_TBF_STATS:
			ret = dpns_tmu_tbf_active(tmu, port, qopt->handle) ? 0 :
				-EOPNOTSUPP;
			break;
		default:
			ret = -EOPNOTSUPP;
			break;
		}
		break;
	}
	case TC_SETUP_QDISC_PRIO:
		ret = dpns_tmu_setup_prio(tmu, port, type_data);
		break;
	case TC_SETUP_QDISC_ETS:
		ret = dpns_tmu_setup_ets(tmu, port, type_data);
		break;
	default:
		ret = -EOPNOTSUPP;
		break;
	}
	mutex_unlock(&tmu->lock);

	return ret;
}

int dpns_tmu_init(struct dpns_priv *priv)
{
	struct dpns_tmu *tmu;
	int ret;

	tmu = devm_kzalloc(priv->dev, sizeof(*tmu), GFP_KERNEL);
	if (!tmu)
		return -ENOMEM;
	tmu->priv = priv;
	mutex_init(&tmu->lock);

	ret = tmu_reset(priv);
	if (ret)
		return ret;
	priv->tmu = tmu;

	return 0;
}

void dpns_tmu_fini(struct dpns_priv *priv)
{
	struct dpns_tmu *tmu = priv->tmu;
	unsigned int port;

	if (!tmu)
		return;
	mutex_lock(&tmu->lock);
	for (port = 0; port < DPNS_PHYS_PORTS; port++)
		dpns_tmu_tbf_disable(tmu, port);
	mutex_unlock(&tmu->lock);
	priv->tmu = NULL;
}
