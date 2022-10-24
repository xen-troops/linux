/* SPDX-License-Identifier: GPL-2.0 */
/* Renesas Ethernet Switch Driver tc u32 functions
 *
 * Copyright (C) 2022 Renesas Electronics Corporation
 * Copyright (C) 2022 EPAM Systems
 */

#include <net/tc_act/tc_mirred.h>
#include <net/tc_act/tc_skbmod.h>
#include <net/tc_act/tc_gact.h>
#include <net/tc_act/tc_vlan.h>

#include "rswitch.h"
#include "rswitch_tc_filters.h"

static void rswitch_init_u32_drop_action(struct rswitch_tc_filter *cfg,
		struct rswitch_tc_filter *f, struct tc_cls_u32_offload *cls)
{
	cfg->action = ACTION_DROP;
	/* Leave other paramters as zero */
	cfg->param.slv = BIT(f->rdev->port);
}

static void rswitch_init_u32_redirect_action(struct rswitch_tc_filter *cfg,
		struct rswitch_tc_filter *f, struct tc_cls_u32_offload *cls)
{
	cfg->action = f->action;
	cfg->param.slv = BIT(f->rdev->port);
	cfg->param.dv = BIT(f->target_rdev->port);

	if (f->action & (ACTION_CHANGE_DMAC | ACTION_VLAN_CHANGE)) {
		cfg->param.l23_info.priv = f->rdev->priv;
		cfg->param.l23_info.routing_number = rswitch_rn_get(f->rdev->priv);
		cfg->param.l23_info.routing_port_valid = BIT(f->rdev->port) | BIT(f->target_rdev->port);

		if (f->action & ACTION_CHANGE_DMAC) {
			ether_addr_copy(cfg->param.l23_info.dst_mac, f->dmac);
			ether_addr_copy(cfg->dmac, f->dmac);
			cfg->param.l23_info.update_dst_mac = true;
		}

		if (f->action & ACTION_VLAN_CHANGE) {
			cfg->param.l23_info.update_ctag_vlan_id = true;
			cfg->param.l23_info.update_ctag_vlan_prio = true;
			cfg->param.l23_info.vlan_id = f->vlan_id;
			cfg->param.l23_info.vlan_prio = f->vlan_prio;
		}
	}
}

static int rswitch_add_action_knode(struct rswitch_tc_filter *f, struct tc_cls_u32_offload *cls)
{
	struct rswitch_device *rdev = f->rdev;
	struct rswitch_private *priv = rdev->priv;
	struct rswitch_pf_param pf_param = {0};
	struct rswitch_tc_filter *tc_u32_cfg = kzalloc(sizeof(*tc_u32_cfg), GFP_KERNEL);
	u16 protocol = be16_to_cpu(cls->common.protocol);
	int rc, i;

	if (!tc_u32_cfg)
		return -ENOMEM;

	if ((protocol != ETH_P_IP) && (protocol != ETH_P_IPV6)) {
		rc = -EOPNOTSUPP;
		goto free;
	}

	tc_u32_cfg->cookie = cls->knode.handle;
	tc_u32_cfg->rdev = rdev;
	tc_u32_cfg->param.priv = priv;

	if (f->action & ACTION_DROP) {
		rswitch_init_u32_drop_action(tc_u32_cfg, f, cls);
	} else if (f->action & ACTION_MIRRED_REDIRECT) {
		rswitch_init_u32_redirect_action(tc_u32_cfg, f, cls);
	} else {
		rc = -EOPNOTSUPP;
		goto free;
	}

	pf_param.rdev = rdev;
	pf_param.all_sources = false;

	/* Check packets EthType to prevent spurious match on different than IP protos */
	rc = rswitch_init_mask_pf_entry(&pf_param, PF_TWO_BYTE, protocol, 0xffff,
				RSWITCH_IP_VERSION_OFFSET);
	if (rc)
		goto free;

	for (i = 0; i < cls->knode.sel->nkeys; i++) {
		rc = rswitch_init_mask_pf_entry(&pf_param, PF_FOUR_BYTE,
				be32_to_cpu(cls->knode.sel->keys[i].val),
				be32_to_cpu(cls->knode.sel->keys[i].mask),
				cls->knode.sel->keys[i].off + RSWITCH_MAC_HEADER_LEN);
		if (rc)
			goto free;
	}

	tc_u32_cfg->param.pf_cascade_index = rswitch_setup_pf(&pf_param);
	if (tc_u32_cfg->param.pf_cascade_index < 0) {
		rc = -E2BIG;
		goto free;
	}

	if (rswitch_add_l3fwd(&tc_u32_cfg->param)) {
		rc = -EBUSY;
		goto put_pf;
	}

	list_add(&tc_u32_cfg->lh, &rdev->tc_u32_list);

	return 0;

put_pf:
	rswitch_put_pf(&tc_u32_cfg->param);
free:
	kfree(tc_u32_cfg);
	return rc;
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
			if (!rswitch_skbmod_can_offload(a) || (filter.action & ACTION_MIRRED_REDIRECT) != 0)
				return -EOPNOTSUPP;
			filter.action |= ACTION_CHANGE_DMAC;
			rswitch_tc_skbmod_get_dmac(a, filter.dmac);
			continue;
		}

		if (is_tcf_mirred_egress_redirect(a)) {
			struct net_device *target_dev = tcf_mirred_dev(a);

			if (!ndev_is_rswitch_dev(target_dev, rdev->priv)) {
				pr_err("Can not redirect to not R-Switch dev!\n");
				return -EOPNOTSUPP;
			}

			filter.action |= ACTION_MIRRED_REDIRECT;
			filter.target_rdev = netdev_priv(target_dev);
			continue;
		}

		if (is_tcf_vlan(a)) {
			/* vlan change action can be offloaded only if placed before redirrect */
			if ((filter.action & ACTION_MIRRED_REDIRECT) != 0)
				return -EOPNOTSUPP;

			switch (tcf_vlan_action(a)) {
			case TCA_VLAN_ACT_MODIFY:
				if (be16_to_cpu(tcf_vlan_push_proto(a)) != ETH_P_8021Q) {
					pr_err("Unsupported VLAN proto for offload!\n");
					return -EOPNOTSUPP;
				}
				filter.action |= ACTION_VLAN_CHANGE;
				filter.vlan_id = tcf_vlan_push_vid(a);
				filter.vlan_prio = tcf_vlan_push_prio(a);
				break;
			default:
				return -EOPNOTSUPP;
			}
			continue;
		}

		/* Drop in hardware */
		if (is_tcf_gact_shot(a)) {
			filter.action |= ACTION_DROP;
			continue;
		}

		return -EOPNOTSUPP;
	}

	/* skbmod cannot be offloaded without redirect */
	if ((filter.action & (ACTION_CHANGE_DMAC | ACTION_MIRRED_REDIRECT)) == ACTION_CHANGE_DMAC)
		return -EOPNOTSUPP;

	return rswitch_add_action_knode(&filter, cls);
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
