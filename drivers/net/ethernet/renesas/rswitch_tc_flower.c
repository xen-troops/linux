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
		  BIT(FLOW_DISSECTOR_KEY_IPV6_ADDRS) |
		  BIT(FLOW_DISSECTOR_KEY_IP) |
		  BIT(FLOW_DISSECTOR_KEY_PORTS)
		  )
	    ) {
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

static int rswitch_tc_flower_setup_drop_action(struct rswitch_tc_filter *f)
{
	f->param.slv = 0x3F;
	/* Explicitly zeroing parameters for drop */
	f->param.dv = 0;
	f->param.csd = 0;

	return 0;
}

static int rswitch_tc_flower_replace(struct net_device *dev,
				struct flow_cls_offload *cls_flower)
{
	struct rswitch_device *rdev = netdev_priv(dev);
	struct rswitch_private *priv = rdev->priv;
	struct rswitch_tc_filter *f;
	struct rswitch_pf_param pf_param = {0};
	unsigned int pf_index = 0;
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

	f->param.priv = priv;
	/* Using cascade filter, src_ip field is not used */
	f->param.src_ip = 0;
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

	if (flow_rule_match_key(rule, FLOW_DISSECTOR_KEY_BASIC)) {
		struct flow_match_basic match;
		int filters_needed;

		flow_rule_match_basic(rule, &match);

		filters_needed = !!(match.mask->n_proto) + !!(match.mask->ip_proto);
		if ((MAX_PF_ENTRIES - pf_index) < filters_needed) {
			/* Not enough perfect filters left for matching */
			goto err;
		}

		pr_err("FLOW_DISSECTOR_KEY_BASIC: n_proto = 0x%x, ip_proto = 0x%x\n",
				ntohs(match.key->n_proto), match.key->ip_proto);

		if (match.mask->n_proto) {
			pf_param.entries[pf_index].val = ntohs(match.key->n_proto);
			pf_param.entries[pf_index].mask = ntohs(match.mask->n_proto);
			pf_param.entries[pf_index].off = RSWITCH_IP_VERSION_OFFSET;
			pf_param.entries[pf_index].type = PF_TWO_BYTE;
			pf_index++;
		}

		if (match.mask->ip_proto) {
			pf_param.entries[pf_index].val = match.key->ip_proto;
			pf_param.entries[pf_index].mask = match.mask->ip_proto;
			/* Using one byte in two-byte filter, make offset correction */
			pf_param.entries[pf_index].off = RSWITCH_IPV4_PROTO_OFFSET - 1;
			pf_param.entries[pf_index].type = PF_TWO_BYTE;
			pf_index++;
		}
	}

	if (addr_type == FLOW_DISSECTOR_KEY_IPV4_ADDRS) {
		struct flow_match_ipv4_addrs match;
		int filters_needed;

		flow_rule_match_ipv4_addrs(rule, &match);

		filters_needed = !!(match.mask->src) + !!(match.mask->dst);
		if ((MAX_PF_ENTRIES - pf_index) < filters_needed) {
			/* Not enough perfect filters left for matching */
			goto err;
		}

		if (match.mask->src) {
			pf_param.entries[pf_index].val = be32_to_cpu(match.key->src);
			pf_param.entries[pf_index].mask = be32_to_cpu(match.mask->src);
			pf_param.entries[pf_index].off = RSWITCH_IPV4_SRC_OFFSET;
			pf_param.entries[pf_index].type = PF_FOUR_BYTE;
			pf_index++;
		}

		if (match.mask->dst) {
			pf_param.entries[pf_index].val = be32_to_cpu(match.key->dst);
			pf_param.entries[pf_index].mask = be32_to_cpu(match.mask->dst);
			pf_param.entries[pf_index].off = RSWITCH_IPV4_DST_OFFSET;
			pf_param.entries[pf_index].type = PF_FOUR_BYTE;
			pf_index++;
		}
	}

	if (flow_rule_match_key(rule, FLOW_DISSECTOR_KEY_IP)) {
		struct flow_match_ip match;
		int filters_needed;

		flow_rule_match_ip(rule, &match);

		filters_needed = !!(match.mask->tos) + !!(match.mask->ttl);
		if ((MAX_PF_ENTRIES - pf_index) < filters_needed) {
			/* Not enough perfect filters left for matching */
			goto err;
		}

		pr_err("FLOW_DISSECTOR_KEY_IP: tos = 0x%x, ttl = %d\n",
			match.key->tos, match.key->ttl);

		if (match.mask->tos) {
			pf_param.entries[pf_index].val = match.key->tos;
			pf_param.entries[pf_index].mask = match.mask->tos;
			/* Using one byte in two-byte filter, make offset correction */
			pf_param.entries[pf_index].off = RSWITCH_IPV4_TOS_OFFSET - 1;
			pf_param.entries[pf_index].type = PF_TWO_BYTE;
			pf_index++;
		}

		if (match.mask->ttl) {
			pf_param.entries[pf_index].val = match.key->ttl;
			pf_param.entries[pf_index].mask = match.mask->ttl;
			/* Using one byte in two-byte filter, make offset correction */
			pf_param.entries[pf_index].off = RSWITCH_IPV4_TTL_OFFSET - 1;
			pf_param.entries[pf_index].type = PF_TWO_BYTE;
			pf_index++;
		}
	}

	if (flow_rule_match_key(rule, FLOW_DISSECTOR_KEY_PORTS)) {
		struct flow_match_ports match;
		int filters_needed;

		flow_rule_match_ports(rule, &match);

		filters_needed = !!(match.mask->src) + !!(match.mask->dst);
		if ((MAX_PF_ENTRIES - pf_index) < filters_needed) {
			/* Not enough perfect filters left for matching */
			goto err;
		}
		pr_err("FLOW_DISSECTOR_KEY_PORTS: src = 0x%x, dst = 0x%x\n",
			be16_to_cpu(match.key->src), be16_to_cpu(match.key->dst));

		if (match.mask->src) {
			pf_param.entries[pf_index].val = be16_to_cpu(match.key->src);
			pf_param.entries[pf_index].mask = be16_to_cpu(match.mask->src);
			pf_param.entries[pf_index].off = RSWITCH_L4_SRC_PORT_OFFSET;
			pf_param.entries[pf_index].type = PF_TWO_BYTE;
			pf_index++;
		}

		if (match.mask->dst) {
			pf_param.entries[pf_index].val = be16_to_cpu(match.key->dst);
			pf_param.entries[pf_index].mask = be16_to_cpu(match.mask->dst);
			pf_param.entries[pf_index].off = RSWITCH_L4_DST_PORT_OFFSET;
			pf_param.entries[pf_index].type = PF_TWO_BYTE;
			pf_index++;
		}
	}

	/* Check if any parameters matched and setup l3fwd */
	if (pf_index) {
		rswitch_tc_flower_setup_drop_action(f);

		pf_param.rdev = rdev;
		pf_param.all_sources = false;

		pf_param.used_entries = pf_index;
		f->param.pf_cascade_index = rswitch_setup_pf(&pf_param);
		if (f->param.pf_cascade_index < 0) {
			goto err;
		}

		if (rswitch_add_l3fwd(&f->param)) {
			rswitch_put_pf(&f->param);
			goto err;
		}

		list_add(&f->lh, &rdev->tc_flower_list);

		return 0;
	}

err:
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
