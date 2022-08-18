/* SPDX-License-Identifier: GPL-2.0 */
/* Renesas Ethernet Switch Driver tc u32 functions
 *
 * Copyright (C) 2022 Renesas Electronics Corporation
 * Copyright (C) 2022 EPAM Systems
 */

#include <net/tc_act/tc_mirred.h>
#include <net/tc_act/tc_skbmod.h>
#include <net/tc_act/tc_gact.h>

#include "rswitch.h"
#include "rswitch_tc_filters.h"

static int rswitch_add_drop_action_knode(struct rswitch_tc_filter *filter, struct tc_cls_u32_offload *cls)
{
	struct rswitch_device *rdev = filter->rdev;
	struct rswitch_private *priv = rdev->priv;
	struct rswitch_pf_param pf_param;
	struct rswitch_tc_filter *tc_u32_cfg = kzalloc(sizeof(*tc_u32_cfg), GFP_KERNEL);
	if (!tc_u32_cfg)
		return -ENOMEM;

	tc_u32_cfg->cookie = cls->knode.handle;
	tc_u32_cfg->action = ACTION_DROP;
	tc_u32_cfg->rdev = rdev;
	/* Leave other paramters as zero */
	tc_u32_cfg->param.priv = priv;
	tc_u32_cfg->param.slv = 0x3F;

	pf_param.rdev = rdev;
	pf_param.all_sources = false;
	pf_param.used_entries = 1;
	pf_param.entries[0].val = be32_to_cpu(cls->knode.sel->keys[0].val);
	pf_param.entries[0].mask = be32_to_cpu(cls->knode.sel->keys[0].mask);
	pf_param.entries[0].off = cls->knode.sel->keys[0].off + RSWITCH_IPV4_HEADER_OFFSET;
	pf_param.entries[0].type = PF_FOUR_BYTE;
	pf_param.entries[0].mode = RSWITCH_PF_MASK_MODE;

	tc_u32_cfg->param.pf_cascade_index = rswitch_setup_pf(&pf_param);
	if (tc_u32_cfg->param.pf_cascade_index < 0) {
		kfree(tc_u32_cfg);
		return -E2BIG;
	}

	if (rswitch_add_l3fwd(&tc_u32_cfg->param)) {
		rswitch_put_pf(&tc_u32_cfg->param);
		kfree(tc_u32_cfg);
		return -EBUSY;
	}

	list_add(&tc_u32_cfg->lh, &rdev->tc_u32_list);

	return 0;
}

static int rswitch_add_redirect_action_knode(struct rswitch_tc_filter *filter, struct tc_cls_u32_offload *cls)
{
	struct rswitch_device *rdev = filter->rdev;
	struct rswitch_private *priv = rdev->priv;
	struct rswitch_pf_param pf_param;
	struct rswitch_tc_filter *tc_u32_cfg = kzalloc(sizeof(*tc_u32_cfg), GFP_KERNEL);
	if (!tc_u32_cfg)
		return -ENOMEM;

	tc_u32_cfg->cookie = cls->knode.handle;
	tc_u32_cfg->action = filter->action;
	tc_u32_cfg->rdev = rdev;

	tc_u32_cfg->param.priv = priv;
	tc_u32_cfg->param.slv = BIT(rdev->port);
	tc_u32_cfg->param.dv = BIT(filter->target_rdev->port);
	if (filter->action & ACTION_CHANGE_DMAC) {
		tc_u32_cfg->param.l23_info.priv = priv;
		ether_addr_copy(tc_u32_cfg->param.l23_info.dst_mac, filter->dmac);
		ether_addr_copy(tc_u32_cfg->dmac, filter->dmac);
		tc_u32_cfg->param.l23_info.update_dst_mac = true;
		tc_u32_cfg->param.l23_info.routing_number = rswitch_rn_get(priv);
		tc_u32_cfg->param.l23_info.routing_port_valid = BIT(rdev->port) | BIT(filter->target_rdev->port);
	}

	pf_param.rdev = rdev;
	pf_param.all_sources = false;
	pf_param.used_entries = 1;
	pf_param.entries[0].val = be32_to_cpu(cls->knode.sel->keys[0].val);
	pf_param.entries[0].mask = be32_to_cpu(cls->knode.sel->keys[0].mask);
	pf_param.entries[0].off = cls->knode.sel->keys[0].off + RSWITCH_IPV4_HEADER_OFFSET;
	pf_param.entries[0].type = PF_FOUR_BYTE;
	pf_param.entries[0].mode = RSWITCH_PF_MASK_MODE;

	tc_u32_cfg->param.pf_cascade_index = rswitch_setup_pf(&pf_param);
	if (tc_u32_cfg->param.pf_cascade_index < 0) {
		kfree(tc_u32_cfg);
		return -E2BIG;
	}

	if (rswitch_add_l3fwd(&tc_u32_cfg->param)) {
		rswitch_put_pf(&tc_u32_cfg->param);
		kfree(tc_u32_cfg);
		return -EBUSY;
	}

	list_add(&tc_u32_cfg->lh, &rdev->tc_u32_list);

	return 0;
}

static bool is_tcf_act_skbmod(const struct tc_action *a)
{
	if (a->ops && a->ops->id == TCA_ACT_SKBMOD)
		return true;

	return false;
}

static bool rswitch_skbmod_can_offload(const struct tc_action *a)
{
	struct tcf_skbmod *skmod = to_skbmod(a);
	struct tcf_skbmod_params *p = rcu_dereference_bh(skmod->skbmod_p);

	/* Only updating MAC destination can be offloaded */
	if ((p->flags & SKBMOD_F_SMAC || p->flags & SKBMOD_F_ETYPE ||
		p->flags & SKBMOD_F_SWAPMAC) && (!(p->flags & SKBMOD_F_DMAC))) {
		return false;
	}

	return true;
}

static void rswitch_tc_skbmod_get_dmac(const struct tc_action *a, u8 *dmac)
{
	struct tcf_skbmod *skmod = to_skbmod(a);
	struct tcf_skbmod_params *p = rcu_dereference_bh(skmod->skbmod_p);

	ether_addr_copy(dmac, p->eth_dst);
}

static int rswitch_del_knode(struct net_device *ndev, struct tc_cls_u32_offload *cls)
{
	struct rswitch_device *rdev = netdev_priv(ndev);
	struct list_head *cur, *tmp;
	bool removed = false;

	list_for_each_safe(cur, tmp, &rdev->tc_u32_list) {
		struct rswitch_tc_filter *tc_u32_cfg = list_entry(cur, struct rswitch_tc_filter, lh);

		if (cls->knode.handle == tc_u32_cfg->cookie) {
			removed = true;
			rswitch_remove_l3fwd(&tc_u32_cfg->param);
			list_del(&tc_u32_cfg->lh);
			kfree(tc_u32_cfg);
		}
	}

	if (removed)
		return 0;

	return -ENOENT;
}

static int rswitch_add_knode(struct net_device *ndev, struct tc_cls_u32_offload *cls)
{
	struct rswitch_device *rdev = netdev_priv(ndev);
	const struct tc_action *a;
	struct tcf_exts *exts;
	struct rswitch_tc_filter filter = {
		.action = 0,
	};
	int i;

	exts = cls->knode.exts;
	if (!tcf_exts_has_actions(exts))
		return -EINVAL;

	filter.rdev = rdev;

	tcf_exts_for_each_action(i, a, exts) {
		/* Several actions with drop cannot be offloaded */
		if (filter.action != 0 && filter.action & ACTION_DROP)
			return -EOPNOTSUPP;

		if (is_tcf_act_skbmod(a)) {
			/* skbmod dmac action can be offloaded only if placed before redirrect */
			if (!rswitch_skbmod_can_offload(a) || filter.action != 0)
				return -EOPNOTSUPP;
			filter.action |= ACTION_CHANGE_DMAC;
			rswitch_tc_skbmod_get_dmac(a, filter.dmac);
			continue;
		}

		if (is_tcf_mirred_egress_redirect(a)) {
			struct net_device *target_dev;

			target_dev = tcf_mirred_dev(a);
			filter.action |= ACTION_MIRRED_REDIRECT;
			filter.target_rdev = netdev_priv(target_dev);
			continue;
		}

		/* Drop in hardware */
		if (is_tcf_gact_shot(a))
			filter.action |= ACTION_DROP;
	}

	/* skbmod cannot be offloaded without redirect */
	if ((filter.action & (ACTION_CHANGE_DMAC | ACTION_MIRRED_REDIRECT)) == ACTION_CHANGE_DMAC)
		return -EOPNOTSUPP;
	if (filter.action & ACTION_DROP)
		return rswitch_add_drop_action_knode(&filter, cls);
	if (filter.action & ACTION_MIRRED_REDIRECT)
		return rswitch_add_redirect_action_knode(&filter, cls);

	return -EOPNOTSUPP;
}

int rswitch_setup_tc_cls_u32(struct net_device *dev,
				 struct tc_cls_u32_offload *cls_u32)
{
	switch (cls_u32->command) {
	case TC_CLSU32_NEW_KNODE:
	case TC_CLSU32_REPLACE_KNODE:
		return rswitch_add_knode(dev, cls_u32);
	case TC_CLSU32_DELETE_KNODE:
		return rswitch_del_knode(dev, cls_u32);
	default:
		return -EOPNOTSUPP;
	}

	return -EOPNOTSUPP;
}
