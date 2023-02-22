/* SPDX-License-Identifier: GPL-2.0 */
/* Renesas Ethernet Switch Driver tc flower functions
 *
 * Copyright (C) 2022 Renesas Electronics Corporation
 * Copyright (C) 2022 EPAM Systems
 */

#include "rswitch.h"
#include "rswitch_tc_filters.h"
#include "rswitch_tc_common.h"

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
		  BIT(FLOW_DISSECTOR_KEY_ETH_ADDRS) |
		  BIT(FLOW_DISSECTOR_KEY_VLAN) |
		  BIT(FLOW_DISSECTOR_KEY_CVLAN)
		  )
	    ) {
		return -EOPNOTSUPP;
	}

	return 0;
}

static int rswitch_flower_gen_fv_callback(struct filtering_vector *fv, void *filter_param)
{
	struct flow_rule *rule = filter_param;
	u16 addr_type = 0;

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
			memcpy(&fv->values[RSWITCH_IP_VERSION_OFFSET], &match.key->n_proto,
			       sizeof(match.key->n_proto));
			memcpy(&fv->masks[RSWITCH_IP_VERSION_OFFSET], &match.mask->n_proto,
			       sizeof(match.mask->n_proto));
		}

		if (match.mask->ip_proto) {
			memcpy(&fv->values[RSWITCH_IPV4_PROTO_OFFSET], &match.key->ip_proto,
			       sizeof(match.key->ip_proto));
			memcpy(&fv->masks[RSWITCH_IPV4_PROTO_OFFSET], &match.mask->ip_proto,
			       sizeof(match.mask->ip_proto));
		}
	}

	if (flow_rule_match_key(rule, FLOW_DISSECTOR_KEY_ETH_ADDRS)) {
		struct flow_match_eth_addrs match;

		flow_rule_match_eth_addrs(rule, &match);

		if (!is_zero_ether_addr(match.mask->src)) {
			memcpy(&fv->values[RSWITCH_MAC_SRC_OFFSET], match.key->src, ETH_ALEN);
			memcpy(&fv->masks[RSWITCH_MAC_SRC_OFFSET], match.mask->src, ETH_ALEN);
		}

		if (!is_zero_ether_addr(match.mask->dst)) {
			memcpy(&fv->values[RSWITCH_MAC_DST_OFFSET], match.key->dst, ETH_ALEN);
			memcpy(&fv->masks[RSWITCH_MAC_DST_OFFSET], match.mask->dst, ETH_ALEN);
		}
	}

	if (addr_type == FLOW_DISSECTOR_KEY_IPV4_ADDRS) {
		struct flow_match_ipv4_addrs match;

		flow_rule_match_ipv4_addrs(rule, &match);

		if (match.mask->src) {
			memcpy(&fv->values[RSWITCH_IPV4_SRC_OFFSET], &match.key->src,
			       sizeof(match.key->src));
			memcpy(&fv->masks[RSWITCH_IPV4_SRC_OFFSET], &match.mask->src,
			       sizeof(match.mask->src));
		}

		if (match.mask->dst) {
			memcpy(&fv->values[RSWITCH_IPV4_DST_OFFSET], &match.key->dst,
			       sizeof(match.key->src));
			memcpy(&fv->masks[RSWITCH_IPV4_DST_OFFSET], &match.mask->dst,
			       sizeof(match.mask->src));
		}
	}

	if (addr_type == FLOW_DISSECTOR_KEY_IPV6_ADDRS) {
		struct flow_match_ipv6_addrs match;
		int i;

		flow_rule_match_ipv6_addrs(rule, &match);

		if (!rswitch_ipv6_all_zero(&match.mask->src)) {
			/* Walk through all 128-bit or until we reach zero mask in addr */
			for (i = 0; (i < 4) && match.mask->src.s6_addr32[i]; i++) {
				memcpy(&fv->values[RSWITCH_IPV6_SRC_OFFSET + (4 * i)],
				       &match.key->src.s6_addr32[i],
				       sizeof(match.key->src.s6_addr32[i]));
				memcpy(&fv->masks[RSWITCH_IPV6_SRC_OFFSET + (4 * i)],
				       &match.mask->src.s6_addr32[i],
				       sizeof(match.mask->src.s6_addr32[i]));
			}
		}

		if (!rswitch_ipv6_all_zero((&match.mask->dst))) {
			/* Walk through all 128-bit or until we reach zero mask in addr */
			for (i = 0; (i < 4) && match.mask->dst.s6_addr32[i]; i++) {
				memcpy(&fv->values[RSWITCH_IPV6_DST_OFFSET + (4 * i)],
				       &match.key->dst.s6_addr32[i],
				       sizeof(match.key->dst.s6_addr32[i]));
				memcpy(&fv->masks[RSWITCH_IPV6_DST_OFFSET + (4 * i)],
				       &match.mask->dst.s6_addr32[i],
				       sizeof(match.mask->dst.s6_addr32[i]));
			}
		}
	}

	if (flow_rule_match_key(rule, FLOW_DISSECTOR_KEY_IP)) {
		struct flow_match_ip match;

		flow_rule_match_ip(rule, &match);

		if (match.mask->tos) {
			memcpy(&fv->values[RSWITCH_IPV4_TOS_OFFSET], &match.key->tos,
			       sizeof(match.key->tos));
			memcpy(&fv->masks[RSWITCH_IPV4_TOS_OFFSET], &match.mask->tos,
			       sizeof(match.mask->tos));
		}

		if (match.mask->ttl) {
			memcpy(&fv->values[RSWITCH_IPV4_TTL_OFFSET], &match.key->ttl,
			       sizeof(match.key->ttl));
			memcpy(&fv->masks[RSWITCH_IPV4_TTL_OFFSET], &match.mask->ttl,
			       sizeof(match.mask->ttl));
		}
	}

	if (flow_rule_match_key(rule, FLOW_DISSECTOR_KEY_PORTS)) {
		struct flow_match_ports match;

		flow_rule_match_ports(rule, &match);

		if (match.mask->src) {
			memcpy(&fv->values[RSWITCH_L4_SRC_PORT_OFFSET], &match.key->src,
			       sizeof(match.key->src));
			memcpy(&fv->masks[RSWITCH_L4_SRC_PORT_OFFSET], &match.mask->src,
			       sizeof(match.mask->src));
		}

		if (match.mask->dst) {
			memcpy(&fv->values[RSWITCH_L4_DST_PORT_OFFSET], &match.key->dst,
			       sizeof(match.key->dst));
			memcpy(&fv->masks[RSWITCH_L4_DST_PORT_OFFSET], &match.mask->dst,
			       sizeof(match.mask->dst));
		}
	}

	/*
	 * In R-Switch terminology we have C-Tags and S-Tags (TPIDs 0x8100 and 0x88A8 by default).
	 *
	 * There 3 supported cases for VLAN matching:
	 * - single tagged traffic - VLAN dissector contains TPID 0x8100, CVLAN dissector not used
	 * - double tagged traffic (match outer VLAN) - VLAN dissector contains TPID 0x88A8,
	 *   CVLAN dissector none
	 * - double tagged traffic (match both VLANs) - VLAN dissector contains TPID 0x88A8,
	 *   CVLAN dissector contains TPID 0x8100
	 */
	if (flow_rule_match_key(rule, FLOW_DISSECTOR_KEY_VLAN)) {
		struct flow_match_vlan vlan;
		u16 vlan_tpid;
		u16 vlan_tci, vlan_mask;
		bool has_cvlan = flow_rule_match_key(rule, FLOW_DISSECTOR_KEY_CVLAN);

		flow_rule_match_vlan(rule, &vlan);

		vlan_tpid = be16_to_cpu(vlan.key->vlan_tpid);
		vlan_tci = vlan.key->vlan_id | (vlan.key->vlan_priority << VLAN_PRIO_SHIFT);
		vlan_mask = vlan.mask->vlan_id | (vlan.mask->vlan_priority << VLAN_PRIO_SHIFT);

		switch (vlan_tpid) {
		case ETH_P_8021Q:
			if (has_cvlan)
				return -EOPNOTSUPP;

			if (vlan_mask) {
				memcpy(&fv->vlan_values[RSWITCH_VLAN_CTAG_OFFSET], &vlan_tci,
				       sizeof(vlan_tci));
				memcpy(&fv->vlan_masks[RSWITCH_VLAN_CTAG_OFFSET], &vlan_mask,
				       sizeof(vlan_mask));
				fv->set_vlan = true;
			}
			break;
		case ETH_P_8021AD:
			if (vlan_mask) {
				memcpy(&fv->vlan_values[RSWITCH_VLAN_STAG_OFFSET], &vlan_tci,
				       sizeof(vlan_tci));
				memcpy(&fv->vlan_masks[RSWITCH_VLAN_STAG_OFFSET], &vlan_mask,
				       sizeof(vlan_mask));
				fv->set_vlan = true;
			} else {
				/*
				 * We can not verify if traffic has 802.1ad TPID in outer VLAN,
				 * because S-tag contains only TCI value (TPID dropped
				 * automatically after packet matching). This will cause
				 * pure 802.1q traffic match by rule below, instead of double
				 * tagged traffic where S-tag is not matter.
				 */
				return -EOPNOTSUPP;
			}

			if (has_cvlan) {
				struct flow_match_vlan cvlan;
				u16 cvlan_tpid;
				u16 cvlan_tci, cvlan_mask;

				flow_rule_match_cvlan(rule, &cvlan);
				cvlan_tpid = be16_to_cpu(cvlan.key->vlan_tpid);
				if (cvlan_tpid != ETH_P_8021Q)
					return -EOPNOTSUPP;

				cvlan_tci = cvlan.key->vlan_id |
						(cvlan.key->vlan_priority << VLAN_PRIO_SHIFT);
				cvlan_mask = cvlan.mask->vlan_id |
						(cvlan.mask->vlan_priority << VLAN_PRIO_SHIFT);

				if (cvlan_mask) {
					memcpy(&fv->vlan_values[RSWITCH_VLAN_CTAG_OFFSET],
					       &cvlan_tci, sizeof(cvlan_tci));
					memcpy(&fv->vlan_masks[RSWITCH_VLAN_CTAG_OFFSET],
					       &cvlan_mask, sizeof(cvlan_mask));
					fv->set_vlan = true;
				}
			}
			break;
		default:
			return -EOPNOTSUPP;

		}
	}

	return 0;
}

static int rswitch_tc_flower_setup_match(struct rswitch_tc_filter *f,
					 struct flow_rule *rule)
{
	struct rswitch_pf_param pf_param = {0};
	int rc = 0;

	pf_param.rdev = f->rdev;
	pf_param.all_sources = false;

	rc = rswitch_fill_pf_param(&pf_param, rswitch_flower_gen_fv_callback, rule);
	if (rc)
		return rc;

	f->param.pf_cascade_index = rswitch_setup_pf(&pf_param);
	if (f->param.pf_cascade_index < 0)
		return -E2BIG;

	return 0;
}

static int rswitch_tc_flower_replace(struct net_device *ndev,
				     struct flow_cls_offload *cls_flower)
{
	struct flow_rule *rule = flow_cls_offload_flow_rule(cls_flower);
	struct rswitch_device *rdev = netdev_priv(ndev);
	struct rswitch_private *priv = rdev->priv;
	struct rswitch_tc_filter *f;
	int rc;

	if (rswitch_tc_flower_validate_match(rule) ||
		rswitch_tc_validate_flow_action(rdev, rule)) {
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

	rc = rswitch_tc_flower_setup_match(f, rule);
	if (rc)
		goto free;

	rc = rswitch_tc_setup_flow_action(f, rule);
	if (rc)
		goto put_pf;


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

static int rswitch_tc_flower_destroy(struct net_device *ndev,
				     struct flow_cls_offload *cls_flower)
{
	struct rswitch_device *rdev = netdev_priv(ndev);
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

static int rswitch_tc_flower_stats(struct net_device *ndev,
				   struct flow_cls_offload *cls_flower)
{
	return -EOPNOTSUPP;
}

int rswitch_setup_tc_flower(struct net_device *ndev,
			    struct flow_cls_offload *cls_flower)
{
	switch (cls_flower->command) {
	case FLOW_CLS_REPLACE:
		return rswitch_tc_flower_replace(ndev, cls_flower);
	case FLOW_CLS_DESTROY:
		return rswitch_tc_flower_destroy(ndev, cls_flower);
	case FLOW_CLS_STATS:
		return rswitch_tc_flower_stats(ndev, cls_flower);
	default:
		return -EOPNOTSUPP;
	}
}
