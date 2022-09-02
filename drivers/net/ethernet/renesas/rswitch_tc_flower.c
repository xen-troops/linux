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
	struct flow_rule *rule = flow_cls_offload_flow_rule(cls_flower);
	struct rswitch_device *rdev = netdev_priv(dev);
	struct rswitch_private *priv = rdev->priv;
	struct rswitch_tc_filter *f;
	struct rswitch_pf_param pf_param = {0};
	int rc = 0, i;
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

		flow_rule_match_basic(rule, &match);

		if (match.mask->n_proto) {
			rc = rswitch_init_mask_pf_entry(&pf_param, PF_TWO_BYTE,
					ntohs(match.key->n_proto), ntohs(match.mask->n_proto),
					RSWITCH_IP_VERSION_OFFSET);
			if (rc)
				goto free;
		}

		if (match.mask->ip_proto) {
			/* Using one byte in two-byte filter, make offset correction */
			rc = rswitch_init_mask_pf_entry(&pf_param, PF_TWO_BYTE,
					match.key->ip_proto, match.mask->ip_proto,
					RSWITCH_IPV4_PROTO_OFFSET - 1);
			if (rc)
				goto free;
		}
	}

	if (flow_rule_match_key(rule, FLOW_DISSECTOR_KEY_ETH_ADDRS)) {
		struct flow_match_eth_addrs match;

		flow_rule_match_eth_addrs(rule, &match);

		/*
		 * There two possible scenarios for both src and dst MAC
		 * matching (when mask is non-zero):
		 * - MAC is not masked (mask is ff:...:ff) and we can use
		 *   1 three-byte filter in expand mode
		 * - MAC is masked and we need to use 2 three-byte filters.
		 *   Both MAC and mask will be divided into 2 parts and will
		 *   be placed to separate filters.
		 */
		if (!is_zero_ether_addr(match.mask->src)) {
			if (is_broadcast_ether_addr(match.mask->src)) {
				rc = rswitch_init_expand_pf_entry(&pf_param, PF_THREE_BYTE,
						rswitch_mac_left_half(match.key->src),
						rswitch_mac_right_half(match.key->src),
						RSWITCH_MAC_SRC_OFFSET);
				if (rc)
					goto free;
			} else {
				rc = rswitch_init_mask_pf_entry(&pf_param, PF_THREE_BYTE,
						rswitch_mac_left_half(match.key->src),
						rswitch_mac_left_half(match.mask->src),
						RSWITCH_MAC_SRC_OFFSET);
				if (rc)
					goto free;

				rc = rswitch_init_mask_pf_entry(&pf_param, PF_THREE_BYTE,
						rswitch_mac_right_half(match.key->src),
						rswitch_mac_right_half(match.mask->src),
						RSWITCH_MAC_SRC_OFFSET + 3);
				if (rc)
					goto free;
			}
		}

		if (!is_zero_ether_addr(match.mask->dst)) {
			if (is_broadcast_ether_addr(match.mask->dst)) {
				rc = rswitch_init_expand_pf_entry(&pf_param, PF_THREE_BYTE,
							rswitch_mac_left_half(match.key->dst),
							rswitch_mac_right_half(match.key->dst),
							RSWITCH_MAC_DST_OFFSET);
				if (rc)
					goto free;
			} else {
				rc = rswitch_init_mask_pf_entry(&pf_param, PF_THREE_BYTE,
						rswitch_mac_left_half(match.key->dst),
						rswitch_mac_left_half(match.mask->dst),
						RSWITCH_MAC_DST_OFFSET);
				if (rc)
					goto free;;

				rc = rswitch_init_mask_pf_entry(&pf_param, PF_THREE_BYTE,
						rswitch_mac_right_half(match.key->dst),
						rswitch_mac_right_half(match.mask->dst),
						RSWITCH_MAC_DST_OFFSET + 3);
				if (rc)
					goto free;
			}
		}
	}

	if (addr_type == FLOW_DISSECTOR_KEY_IPV4_ADDRS) {
		struct flow_match_ipv4_addrs match;

		flow_rule_match_ipv4_addrs(rule, &match);

		if (match.mask->src) {
			rc = rswitch_init_mask_pf_entry(&pf_param, PF_FOUR_BYTE,
					be32_to_cpu(match.key->src), be32_to_cpu(match.mask->src),
					RSWITCH_IPV4_SRC_OFFSET);
			if (rc)
				goto free;
		}

		if (match.mask->dst) {
			rc = rswitch_init_mask_pf_entry(&pf_param, PF_FOUR_BYTE,
					be32_to_cpu(match.key->dst), be32_to_cpu(match.mask->dst),
					RSWITCH_IPV4_DST_OFFSET);
			if (rc)
				goto free;
		}
	}

	if (addr_type == FLOW_DISSECTOR_KEY_IPV6_ADDRS) {
		struct flow_match_ipv6_addrs match;

		flow_rule_match_ipv6_addrs(rule, &match);

		/*
		 * Approach is the same as for MAC addresses:
		 * - 2 four-byte filters in expand mode for exact IPv6
		 * - 4 four-byte filter in mask mode for masked IPv6 (or less
		 *   if mask has more than 32 zeros at the end)
		 */
		if (!rswitch_ipv6_all_zero(&match.mask->src)) {
			if (rswitch_ipv6_all_set(&match.mask->src)) {
				rc = rswitch_init_expand_pf_entry(&pf_param, PF_FOUR_BYTE,
						be32_to_cpu(match.key->src.s6_addr32[0]),
						be32_to_cpu(match.key->src.s6_addr32[1]),
						RSWITCH_IPV6_SRC_OFFSET);
				if (rc)
					goto free;

				rc = rswitch_init_expand_pf_entry(&pf_param, PF_FOUR_BYTE,
						be32_to_cpu(match.key->src.s6_addr32[2]),
						be32_to_cpu(match.key->src.s6_addr32[3]),
						RSWITCH_IPV6_SRC_OFFSET + 8);
				if (rc)
					goto free;
			} else {
				/* Walk through all 128-bit or until we reach zero mask in addr */
				for (i = 0; (i < 4) && match.mask->src.s6_addr32[i]; i++) {
					rc = rswitch_init_mask_pf_entry(&pf_param, PF_FOUR_BYTE,
							be32_to_cpu(match.key->src.s6_addr32[i]),
							be32_to_cpu(match.mask->src.s6_addr32[i]),
							RSWITCH_IPV6_SRC_OFFSET + 4 * i);
					if (rc)
						goto free;
				}
			}
		}

		if (!rswitch_ipv6_all_zero((&match.mask->dst))) {
			if (rswitch_ipv6_all_set(&match.mask->dst)) {
				rc = rswitch_init_expand_pf_entry(&pf_param, PF_FOUR_BYTE,
						be32_to_cpu(match.key->dst.s6_addr32[0]),
						be32_to_cpu(match.key->dst.s6_addr32[1]),
						RSWITCH_IPV6_DST_OFFSET);
				if (rc)
					goto free;

				rc = rswitch_init_expand_pf_entry(&pf_param, PF_FOUR_BYTE,
						be32_to_cpu(match.key->dst.s6_addr32[2]),
						be32_to_cpu(match.key->dst.s6_addr32[3]),
						RSWITCH_IPV6_DST_OFFSET + 8);
				if (rc)
					goto free;
			} else {
				/* Walk through all 128-bit or until we reach zero mask in addr */
				for (i = 0; (i < 4) && match.mask->dst.s6_addr32[i]; i++) {
					rc = rswitch_init_mask_pf_entry(&pf_param, PF_FOUR_BYTE,
							be32_to_cpu(match.key->dst.s6_addr32[i]),
							be32_to_cpu(match.mask->dst.s6_addr32[i]),
							RSWITCH_IPV6_DST_OFFSET + 4 * i);
					if (rc)
						goto free;
				}
			}
		}
	}

	if (flow_rule_match_key(rule, FLOW_DISSECTOR_KEY_IP)) {
		struct flow_match_ip match;

		flow_rule_match_ip(rule, &match);

		if (match.mask->tos) {
			/* Using one byte in two-byte filter, make offset correction */
			rc = rswitch_init_mask_pf_entry(&pf_param, PF_TWO_BYTE, match.key->tos,
					match.mask->tos, RSWITCH_IPV4_TOS_OFFSET - 1);
			if (rc)
				goto free;
		}

		if (match.mask->ttl) {
			/* Using one byte in two-byte filter, make offset correction */
			rc = rswitch_init_mask_pf_entry(&pf_param, PF_TWO_BYTE, match.key->ttl,
					match.mask->ttl, RSWITCH_IPV4_TTL_OFFSET - 1);
			if (rc)
				goto free;
		}
	}

	if (flow_rule_match_key(rule, FLOW_DISSECTOR_KEY_PORTS)) {
		struct flow_match_ports match;

		flow_rule_match_ports(rule, &match);

		if (match.mask->src) {
			rc = rswitch_init_mask_pf_entry(&pf_param, PF_TWO_BYTE,
					be16_to_cpu(match.key->src), be16_to_cpu(match.mask->src),
					RSWITCH_L4_SRC_PORT_OFFSET);
			if (rc)
				goto free;
		}

		if (match.mask->dst) {
			rc = rswitch_init_mask_pf_entry(&pf_param, PF_TWO_BYTE,
					be16_to_cpu(match.key->dst), be16_to_cpu(match.mask->dst),
					RSWITCH_L4_DST_PORT_OFFSET);
			if (rc)
				goto free;
		}
	}

	rc = rswitch_tc_flower_setup_action(f, rule);
	if (rc) {
		goto free;
	}

	pf_param.rdev = rdev;
	pf_param.all_sources = false;

	f->param.pf_cascade_index = rswitch_setup_pf(&pf_param);
	if (f->param.pf_cascade_index < 0) {
		rc = -E2BIG;
		goto free;
	}

	if (rswitch_add_l3fwd(&f->param)) {
		rc = -EBUSY;
		goto put_pf;
	}

	list_add(&f->lh, &rdev->tc_flower_list);

	return 0;

put_pf:
	rswitch_put_pf(&f->param);
free:
	kfree(f);
	return rc;

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
