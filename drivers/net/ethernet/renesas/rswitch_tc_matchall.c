/* SPDX-License-Identifier: GPL-2.0 */
/* Renesas Ethernet Switch Driver tc matchall functions
 *
 * Copyright (C) 2022 Renesas Electronics Corporation
 * Copyright (C) 2022 EPAM Systems
 */

#include "rswitch.h"
#include "rswitch_tc_filters.h"
#include "rswitch_tc_common.h"

static int rswitch_add_mall_action(struct rswitch_tc_filter *filter,
		struct tc_cls_matchall_offload *cls)
{
	struct rswitch_device *rdev = filter->rdev;
	struct rswitch_pf_param pf_param = {0};
	int rc;

	pf_param.rdev = rdev;
	pf_param.all_sources = false;
	/* Match all packets */
	rc = rswitch_init_mask_pf_entry(&pf_param, PF_FOUR_BYTE, 0, 0, 0);
	if (rc)
		goto end;

	filter->param.pf_cascade_index = rswitch_setup_pf(&pf_param);
	if (filter->param.pf_cascade_index < 0) {
		rc = -E2BIG;
		goto end;
	}

	if (rswitch_add_l3fwd(&filter->param)) {
		rc = -EBUSY;
		goto put_pf;
	}

	list_add(&filter->lh, &rdev->tc_matchall_list);

	return 0;

put_pf:
	rswitch_put_pf(&filter->param);
end:
	return rc;
}

static int rswitch_tc_matchall_replace(struct net_device *ndev,
				  struct tc_cls_matchall_offload *cls_matchall)
{
	struct flow_rule *rule = cls_matchall->rule;
	struct rswitch_device *rdev = netdev_priv(ndev);
	struct rswitch_tc_filter *filter = kzalloc(sizeof(*filter), GFP_KERNEL);
	int rc;

	if (!filter)
		return -ENOMEM;

	filter->cookie = cls_matchall->cookie;
	filter->rdev = rdev;
	filter->param.priv = rdev->priv;

	rc = rswitch_tc_validate_flow_action(rdev, rule);
	if (rc)
		goto free;

	rc = rswitch_tc_setup_flow_action(filter, rule);
	if (rc)
		goto free;

	rc = rswitch_add_mall_action(filter, cls_matchall);
	if (rc)
		goto free;
	return rc;
free:
	kfree(filter);
	return rc;
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
