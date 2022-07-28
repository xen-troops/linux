/* SPDX-License-Identifier: GPL-2.0 */
/* Renesas Ethernet Switch Driver tc flower functions
 *
 * Copyright (C) 2022 Renesas Electronics Corporation
 * Copyright (C) 2022 EPAM Systems
 */

#include "rswitch.h"
#include "rswitch_tc_filters.h"

static int rswitch_tc_flower_validate_match(struct net_device *dev,
				struct flow_rule *rule)
{
	struct flow_dissector *dissector = rule->match.dissector;

	/*
	 * Note: IPV6 dissector is always set for IPV4 rules for some reason,
	 * but true IPV6 rules are not currently supported for offload.
	 */
	if (dissector->used_keys &
		~(BIT(FLOW_DISSECTOR_KEY_CONTROL) |
		  BIT(FLOW_DISSECTOR_KEY_BASIC) |
		  BIT(FLOW_DISSECTOR_KEY_IPV4_ADDRS) |
		  BIT(FLOW_DISSECTOR_KEY_IPV6_ADDRS))) {
		return -EOPNOTSUPP;
	}

	return 0;
}

static int rswitch_tc_flower_validate_action(struct net_device *dev,
				struct flow_rule *rule)
{
	/* TODO: only drop is currently supported */
	if (flow_action_first_entry_get(&rule->action)->id == FLOW_ACTION_DROP
		&& flow_offload_has_one_action(&rule->action)) {
		return 0;
	}

	return -EOPNOTSUPP;
}

static int rswitch_tc_flower_replace(struct net_device *dev,
				struct flow_cls_offload *cls_flower)
{
	struct rswitch_device *rdev = netdev_priv(dev);
	struct rswitch_private *priv = rdev->priv;
	struct rswitch_tc_filter *f;
	struct rswitch_pf_param pf_param = {0};
	struct flow_rule *rule = flow_cls_offload_flow_rule(cls_flower);

	u16 addr_type = 0;

	if (rswitch_tc_flower_validate_match(dev, rule) ||
		rswitch_tc_flower_validate_action(dev, rule)) {
		return -EOPNOTSUPP;
	}

	f = kzalloc(sizeof(*f), GFP_KERNEL);
	if (!f) {
		pr_err("Failed to allocate memory for tc flower filter\n");
		return -ENOMEM;
	}

	f->cookie = cls_flower->cookie;
	if (flow_rule_match_key(rule, FLOW_DISSECTOR_KEY_CONTROL)) {
		struct flow_match_control match;

		flow_rule_match_control(rule, &match);
		addr_type = match.key->addr_type;
	} else if (flow_rule_match_key(rule, FLOW_DISSECTOR_KEY_IPV4_ADDRS)) {
		addr_type = FLOW_DISSECTOR_KEY_IPV4_ADDRS;
	} else if (flow_rule_match_key(rule, FLOW_DISSECTOR_KEY_IPV6_ADDRS)) {
		addr_type = FLOW_DISSECTOR_KEY_IPV6_ADDRS;
	}

	if (addr_type == FLOW_DISSECTOR_KEY_IPV4_ADDRS) {
		struct flow_match_ipv4_addrs match;

		flow_rule_match_ipv4_addrs(rule, &match);

		f->param.priv = priv;
		f->param.slv = 0x3F;

		/* Explicitly zeroing parameters for drop */
		f->param.dv = 0;
		f->param.csd = 0;

		/* Using cascade filter, src_ip field is not used */
		f->param.src_ip = 0;

		pf_param.rdev = rdev;
		pf_param.all_sources = false;
		pf_param.used_entries = 2;

		pf_param.entries[0].val = be32_to_cpu(match.key->src);
		pf_param.entries[0].mask = be32_to_cpu(match.mask->src);
		pf_param.entries[0].off = IPV4_SRC_OFFSET;
		pf_param.entries[0].type = PF_FOUR_BYTE;

		pf_param.entries[1].val = be32_to_cpu(match.key->dst);
		pf_param.entries[1].mask = be32_to_cpu(match.mask->dst);
		pf_param.entries[1].off = IPV4_DST_OFFSET;
		pf_param.entries[1].type = PF_FOUR_BYTE;

		f->param.pf_cascade_index = rswitch_setup_pf(&pf_param);
		if (f->param.pf_cascade_index < 0) {
			kfree(f);
			return -EOPNOTSUPP;
		}

		if (rswitch_add_l3fwd(&f->param)) {
			rswitch_put_pf(&f->param);
			kfree(f);
			return -EOPNOTSUPP;
		}

		list_add(&f->lh, &rdev->tc_flower_list);

		return 0;
	}

	kfree(f);
	return -EOPNOTSUPP;

}

static int rswitch_tc_flower_destroy(struct net_device *dev,
				struct flow_cls_offload *cls_flower)
{
	struct rswitch_device *rdev = netdev_priv(dev);
	struct rswitch_tc_filter *f = NULL;

	list_for_each_entry(f, &rdev->tc_flower_list, lh) {
		if (f->cookie == cls_flower->cookie) {
			rswitch_remove_l3fwd(&f->param);
			list_del(&f->lh);
			kfree(f);

			return 0;
		}
	}

	return -ENOENT;
}

static int rswitch_tc_flower_stats(struct net_device *dev,
				struct flow_cls_offload *cls_flower)
{
	return -EOPNOTSUPP;
}

int rswitch_setup_tc_flower(struct net_device *dev,
				struct flow_cls_offload *cls_flower)
{
	switch (cls_flower->command) {
	case FLOW_CLS_REPLACE:
		return rswitch_tc_flower_replace(dev, cls_flower);
	case FLOW_CLS_DESTROY:
		return rswitch_tc_flower_destroy(dev, cls_flower);
	case FLOW_CLS_STATS:
		return rswitch_tc_flower_stats(dev, cls_flower);
	default:
		return -EOPNOTSUPP;
	}
}
