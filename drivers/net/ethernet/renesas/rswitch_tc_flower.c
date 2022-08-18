/* SPDX-License-Identifier: GPL-2.0 */
/* Renesas Ethernet Switch Driver tc flower functions
 *
 * Copyright (C) 2022 Renesas Electronics Corporation
 * Copyright (C) 2022 EPAM Systems
 */

#include "rswitch.h"
#include "rswitch_tc_filters.h"

static int rswitch_tc_flower_validate_match(struct flow_rule *rule)
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
		  BIT(FLOW_DISSECTOR_KEY_PORTS) |
		  BIT(FLOW_DISSECTOR_KEY_ETH_ADDRS)
		  )
	    ) {
		return -EOPNOTSUPP;
	}

	return 0;
}

static int rswitch_tc_flower_validate_action(struct rswitch_device *rdev,
				struct flow_rule *rule)
{
	struct flow_action_entry *act;
	struct flow_action *actions = &rule->action;
	int i;
	bool redirect = false, dmac_change = false;

	flow_action_for_each(i, act, actions) {
		switch (act->id) {
		case FLOW_ACTION_DROP:
			if (!flow_offload_has_one_action(actions)) {
				pr_err("Other actions with DROP is not supported\n");
				return -EOPNOTSUPP;
			}
			break;
		case FLOW_ACTION_REDIRECT:
			if (!ndev_is_rswitch_dev(act->dev, rdev->priv)) {
				pr_err("Can not redirect to not R-Switch dev!\n");
				return -EOPNOTSUPP;
			}
			redirect = true;
			break;
		case FLOW_ACTION_MANGLE:
			if (act->mangle.htype != FLOW_ACT_MANGLE_HDR_TYPE_ETH) {
				pr_err("Only dst MAC change is supported for mangle\n");
				return -EOPNOTSUPP;
			}
			dmac_change = true;
			break;
		default:
			pr_err("Unsupported for offload action id = %d\n", act->id);
			return -EOPNOTSUPP;
		}
	}

	if (dmac_change && !redirect) {
		pr_err("dst MAC change is supported only with redirect\n");
		return -EOPNOTSUPP;
	}

	return 0;
}

static int rswitch_tc_flower_setup_drop_action(struct rswitch_tc_filter *f)
{
	f->param.slv = 0x3F;
	/* Explicitly zeroing parameters for drop */
	f->param.dv = 0;
	f->param.csd = 0;

	return 0;
}

static int rswitch_tc_flower_setup_redirect_action(struct rswitch_tc_filter *f)
{
	struct rswitch_device *rdev = f->rdev;

	f->param.slv = BIT(rdev->port);
	f->param.dv = BIT(f->target_rdev->port);
	f->param.csd = 0;
	if (f->action & ACTION_CHANGE_DMAC) {
		f->param.l23_info.priv = rdev->priv;
		ether_addr_copy(f->param.l23_info.dst_mac, f->dmac);
		f->param.l23_info.update_dst_mac = true;
		f->param.l23_info.routing_number = rswitch_rn_get(rdev->priv);
		f->param.l23_info.routing_port_valid = BIT(rdev->port) | BIT(f->target_rdev->port);
	}

	return 0;
}

static int rswitch_tc_flower_setup_action(struct rswitch_tc_filter *f,
				struct flow_rule *rule)
{
	struct flow_action_entry *act;
	struct flow_action *actions = &rule->action;
	int i;

	flow_action_for_each(i, act, actions) {
		switch (act->id) {
		case FLOW_ACTION_DROP:
			f->action = ACTION_DROP;
			break;
		case FLOW_ACTION_REDIRECT:
			f->action |= ACTION_MIRRED_REDIRECT;
			f->target_rdev = netdev_priv(act->dev);
			break;
		case FLOW_ACTION_MANGLE:
			/*
			 * The only FLOW_ACT_MANGLE_HDR_TYPE_ETH is supported,
			 * sanitized by rswitch_tc_flower_validate_action().
			 */
			f->action |= ACTION_CHANGE_DMAC;
			rswitch_parse_pedit(f, act);
			break;
		default:
			/*
			 * Should not come here, such action will be dropped by
			 * rswitch_tc_flower_validate_action().
			 */
			pr_err("Unsupported action for offload!\n");
			return -EOPNOTSUPP;
		}
	}

	if (f->action & ACTION_DROP) {
		return rswitch_tc_flower_setup_drop_action(f);
	}

	if (f->action & ACTION_MIRRED_REDIRECT) {
		return rswitch_tc_flower_setup_redirect_action(f);
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
	unsigned int pf_index = 0;
	struct flow_rule *rule = flow_cls_offload_flow_rule(cls_flower);

	u16 addr_type = 0;

	if (rswitch_tc_flower_validate_match(rule) ||
		rswitch_tc_flower_validate_action(rdev, rule)) {
		return -EOPNOTSUPP;
	}

	f = kzalloc(sizeof(*f), GFP_KERNEL);
	if (!f) {
		pr_err("Failed to allocate memory for tc flower filter\n");
		return -ENOMEM;
	}

	f->rdev = rdev;
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
			pf_param.entries[pf_index].mode = RSWITCH_PF_MASK_MODE;
			pf_index++;
		}

		if (match.mask->ip_proto) {
			pf_param.entries[pf_index].val = match.key->ip_proto;
			pf_param.entries[pf_index].mask = match.mask->ip_proto;
			/* Using one byte in two-byte filter, make offset correction */
			pf_param.entries[pf_index].off = RSWITCH_IPV4_PROTO_OFFSET - 1;
			pf_param.entries[pf_index].type = PF_TWO_BYTE;
			pf_param.entries[pf_index].mode = RSWITCH_PF_MASK_MODE;
			pf_index++;
		}
	}

	if (flow_rule_match_key(rule, FLOW_DISSECTOR_KEY_ETH_ADDRS)) {
		struct flow_match_eth_addrs match;
		int filters_needed = 0;

		flow_rule_match_eth_addrs(rule, &match);

		if (!is_zero_ether_addr(match.mask->src)) {
			/*
			 * There two possible scenarios for both src and dst MAC
			 * matching (when mask is non-zero):
			 * - MAC is not masked (mask is ff:...:ff) and we can use
			 *   1 three-byte filter in expand mode
			 * - MAC is masked and we need to use 2 three-byte filters.
			 *   Both MAC and mask will be divided into 2 parts and will
			 *   be placed to separate filters.
			 */
			if (is_broadcast_ether_addr(match.mask->src)) {
				filters_needed++;
			} else {
				filters_needed += 2;
			}
		}

		if (!is_zero_ether_addr(match.mask->dst)) {
			if (is_broadcast_ether_addr(match.mask->dst)) {
				filters_needed++;
			} else {
				filters_needed += 2;
			}
		}

		if ((MAX_PF_ENTRIES - pf_index) < filters_needed) {
			/* Not enough perfect filters left for matching */
			goto err;
		}

		if (!is_zero_ether_addr(match.mask->src)) {
			if (is_broadcast_ether_addr(match.mask->src)) {
				pf_param.entries[pf_index].val = rswitch_mac_left_half(match.key->src);
				pf_param.entries[pf_index].ext_val = rswitch_mac_right_half(match.key->src);
				pf_param.entries[pf_index].off = RSWITCH_MAC_SRC_OFFSET;
				pf_param.entries[pf_index].type = PF_THREE_BYTE;
				pf_param.entries[pf_index].mode = RSWITCH_PF_EXPAND_MODE;
				pf_index++;
			} else {
				pf_param.entries[pf_index].val = rswitch_mac_left_half(match.key->src);
				pf_param.entries[pf_index].mask = rswitch_mac_left_half(match.mask->src);
				pf_param.entries[pf_index].off = RSWITCH_MAC_SRC_OFFSET;
				pf_param.entries[pf_index].type = PF_THREE_BYTE;
				pf_param.entries[pf_index].mode = RSWITCH_PF_MASK_MODE;
				pf_index++;

				pf_param.entries[pf_index].val = rswitch_mac_right_half((match.key->src));
				pf_param.entries[pf_index].mask = rswitch_mac_right_half(match.mask->src);
				pf_param.entries[pf_index].off = RSWITCH_MAC_SRC_OFFSET + 3;
				pf_param.entries[pf_index].type = PF_THREE_BYTE;
				pf_param.entries[pf_index].mode = RSWITCH_PF_MASK_MODE;
				pf_index++;
			}
		}

		if (!is_zero_ether_addr(match.mask->dst)) {
			if (is_broadcast_ether_addr(match.mask->dst)) {
				pf_param.entries[pf_index].val = rswitch_mac_left_half(match.key->dst);
				pf_param.entries[pf_index].ext_val = rswitch_mac_right_half(match.key->dst);
				pf_param.entries[pf_index].off = RSWITCH_MAC_DST_OFFSET;
				pf_param.entries[pf_index].type = PF_THREE_BYTE;
				pf_param.entries[pf_index].mode = RSWITCH_PF_EXPAND_MODE;
				pf_index++;
			} else {
				pf_param.entries[pf_index].val = rswitch_mac_left_half(match.key->dst);
				pf_param.entries[pf_index].mask = rswitch_mac_left_half(match.mask->dst);
				pf_param.entries[pf_index].off = RSWITCH_MAC_DST_OFFSET;
				pf_param.entries[pf_index].type = PF_THREE_BYTE;
				pf_param.entries[pf_index].mode = RSWITCH_PF_MASK_MODE;
				pf_index++;

				pf_param.entries[pf_index].val = rswitch_mac_right_half((match.key->dst));
				pf_param.entries[pf_index].mask = rswitch_mac_right_half(match.mask->dst);
				pf_param.entries[pf_index].off = RSWITCH_MAC_DST_OFFSET + 3;
				pf_param.entries[pf_index].type = PF_THREE_BYTE;
				pf_param.entries[pf_index].mode = RSWITCH_PF_MASK_MODE;
				pf_index++;
			}
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
			pf_param.entries[pf_index].mode = RSWITCH_PF_MASK_MODE;
			pf_index++;
		}

		if (match.mask->dst) {
			pf_param.entries[pf_index].val = be32_to_cpu(match.key->dst);
			pf_param.entries[pf_index].mask = be32_to_cpu(match.mask->dst);
			pf_param.entries[pf_index].off = RSWITCH_IPV4_DST_OFFSET;
			pf_param.entries[pf_index].type = PF_FOUR_BYTE;
			pf_param.entries[pf_index].mode = RSWITCH_PF_MASK_MODE;
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
			pf_param.entries[pf_index].mode = RSWITCH_PF_MASK_MODE;
			pf_index++;
		}

		if (match.mask->ttl) {
			pf_param.entries[pf_index].val = match.key->ttl;
			pf_param.entries[pf_index].mask = match.mask->ttl;
			/* Using one byte in two-byte filter, make offset correction */
			pf_param.entries[pf_index].off = RSWITCH_IPV4_TTL_OFFSET - 1;
			pf_param.entries[pf_index].type = PF_TWO_BYTE;
			pf_param.entries[pf_index].mode = RSWITCH_PF_MASK_MODE;
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
			pf_param.entries[pf_index].mode = RSWITCH_PF_MASK_MODE;
			pf_index++;
		}

		if (match.mask->dst) {
			pf_param.entries[pf_index].val = be16_to_cpu(match.key->dst);
			pf_param.entries[pf_index].mask = be16_to_cpu(match.mask->dst);
			pf_param.entries[pf_index].off = RSWITCH_L4_DST_PORT_OFFSET;
			pf_param.entries[pf_index].type = PF_TWO_BYTE;
			pf_param.entries[pf_index].mode = RSWITCH_PF_MASK_MODE;
			pf_index++;
		}
	}


	if (!pf_index) {
		/* No parameters matched in rule */
		goto err;
	}

	if (rswitch_tc_flower_setup_action(f, rule)) {
		goto err;
	}

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
