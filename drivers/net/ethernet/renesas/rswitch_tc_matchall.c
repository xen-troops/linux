/* SPDX-License-Identifier: GPL-2.0 */
/* Renesas Ethernet Switch Driver tc matchall functions
 *
 * Copyright (C) 2022 Renesas Electronics Corporation
 * Copyright (C) 2022 EPAM Systems
 */

#include "rswitch.h"
#include "rswitch_tc_filters.h"

static int rswitch_add_drop_action(struct rswitch_tc_filter *filter, struct tc_cls_matchall_offload *cls)
{
	struct rswitch_device *rdev = filter->rdev;
	struct rswitch_private *priv = rdev->priv;
	struct rswitch_pf_param pf_param = {0};
	struct rswitch_tc_filter *cfg = kzalloc(sizeof(*cfg), GFP_KERNEL);
	int rc;

	if (!cfg)
		return -ENOMEM;

	cfg->cookie = cls->cookie;
	cfg->action = ACTION_DROP;
	cfg->rdev = rdev;
	/* Leave other paramters as zero */
	cfg->param.priv = priv;
	cfg->param.slv = 0x3F;

	pf_param.rdev = rdev;
	pf_param.all_sources = false;
	/* Match all packets */
	rc = rswitch_init_mask_pf_entry(&pf_param, PF_FOUR_BYTE, 0, 0, 0);
	if (rc)
		goto free;

	cfg->param.pf_cascade_index = rswitch_setup_pf(&pf_param);
	if (cfg->param.pf_cascade_index < 0) {
		rc = -E2BIG;
		goto free;
	}

	if (rswitch_add_l3fwd(&cfg->param)) {
		rc = -EBUSY;
		goto put_pf;
	}

	list_add(&cfg->lh, &rdev->tc_matchall_list);

	return 0;

put_pf:
	rswitch_put_pf(&cfg->param);
free:
	kfree(cfg);
	return rc;
}

static int rswitch_add_redirect_action(struct rswitch_tc_filter *filter, struct tc_cls_matchall_offload *cls)
{
	struct rswitch_device *rdev = filter->rdev;
	struct rswitch_private *priv = rdev->priv;
	struct rswitch_pf_param pf_param = {0};
	struct rswitch_tc_filter *cfg = kzalloc(sizeof(*cfg), GFP_KERNEL);
	int rc;

	if (!cfg)
		return -ENOMEM;

	cfg->cookie = cls->cookie;
	cfg->action = filter->action;
	cfg->rdev = rdev;

	cfg->param.priv = priv;
	cfg->param.slv = BIT(rdev->port);
	cfg->param.dv = BIT(filter->target_rdev->port);
	if (filter->action & ACTION_CHANGE_DMAC) {
		cfg->param.l23_info.priv = priv;
		ether_addr_copy(cfg->param.l23_info.dst_mac, filter->dmac);
		ether_addr_copy(cfg->dmac, filter->dmac);
		cfg->param.l23_info.update_dst_mac = true;
		cfg->param.l23_info.routing_number = rswitch_rn_get(priv);
		cfg->param.l23_info.routing_port_valid = BIT(rdev->port) | BIT(filter->target_rdev->port);
	}

	pf_param.rdev = rdev;
	pf_param.all_sources = false;
	/* Match all packets */
	rc = rswitch_init_mask_pf_entry(&pf_param, PF_FOUR_BYTE, 0, 0, 0);
	if (rc)
		goto free;

	cfg->param.pf_cascade_index = rswitch_setup_pf(&pf_param);
	if (cfg->param.pf_cascade_index < 0) {
		rc = -E2BIG;
		goto free;
	}

	if (rswitch_add_l3fwd(&cfg->param)) {
		rc = -EBUSY;
		goto put_pf;
	}

	list_add(&cfg->lh, &rdev->tc_matchall_list);

	return 0;

put_pf:
	rswitch_put_pf(&cfg->param);
free:
	kfree(cfg);
	return rc;
}

static int rswitch_tc_matchall_replace(struct net_device *ndev,
				  struct tc_cls_matchall_offload *cls_matchall)
{
	struct flow_action_entry *entry;
	struct flow_action *actions = &cls_matchall->rule->action;
	struct rswitch_device *rdev = netdev_priv(ndev);
	struct rswitch_tc_filter filter = {
		.action = 0,
	};
	int i;

	filter.rdev = rdev;

	flow_action_for_each(i, entry, actions) {
		/* Several actions with drop cannot be offloaded */
		if (filter.action != 0 && filter.action & ACTION_DROP)
			return -EOPNOTSUPP;

		if (entry->id == FLOW_ACTION_REDIRECT) {
			struct net_device *target_dev = entry->dev;

			if (!ndev_is_rswitch_dev(target_dev, rdev->priv)) {
				pr_err("Can not redirect to not R-Switch dev!\n");
				return -EOPNOTSUPP;
			}

			filter.action |= ACTION_MIRRED_REDIRECT;
			filter.target_rdev = netdev_priv(target_dev);
			continue;
		}

		if (entry->id == FLOW_ACTION_MANGLE &&
			entry->mangle.htype == FLOW_ACT_MANGLE_HDR_TYPE_ETH) {
			filter.action |= ACTION_CHANGE_DMAC;
			rswitch_parse_pedit(&filter, entry);
			continue;
		}

		/* Drop in hardware */
		if (entry->id == FLOW_ACTION_DROP)
			filter.action |= ACTION_DROP;
	}

	/* skbmod cannot be offloaded without redirect */
	if ((filter.action & (ACTION_CHANGE_DMAC | ACTION_MIRRED_REDIRECT)) == ACTION_CHANGE_DMAC)
		return -EOPNOTSUPP;
	if (filter.action & ACTION_DROP)
		return rswitch_add_drop_action(&filter, cls_matchall);
	if (filter.action & ACTION_MIRRED_REDIRECT)
		return rswitch_add_redirect_action(&filter, cls_matchall);

	return -EOPNOTSUPP;
}

static int rswitch_tc_matchall_destroy(struct net_device *ndev,
				  struct tc_cls_matchall_offload *cls_matchall)
{
	struct rswitch_device *rdev = netdev_priv(ndev);
	struct list_head *cur, *tmp;
	bool removed = false;

	list_for_each_safe(cur, tmp, &rdev->tc_matchall_list) {
		struct rswitch_tc_filter *cfg = list_entry(cur, struct rswitch_tc_filter, lh);

		if (cls_matchall->cookie == cfg->cookie) {
			removed = true;
			rswitch_remove_l3fwd(&cfg->param);
			list_del(&cfg->lh);
			kfree(cfg);
		}
	}

	if (removed)
		return 0;

	return -ENOENT;
}

int rswitch_setup_tc_matchall(struct net_device *dev,
				  struct tc_cls_matchall_offload *cls_matchall)
{
	switch (cls_matchall->command) {
	case TC_CLSMATCHALL_REPLACE:
		return rswitch_tc_matchall_replace(dev, cls_matchall);
	case TC_CLSMATCHALL_DESTROY:
		return rswitch_tc_matchall_destroy(dev, cls_matchall);
	default:
		return -EOPNOTSUPP;
	}

	return -EOPNOTSUPP;
}
