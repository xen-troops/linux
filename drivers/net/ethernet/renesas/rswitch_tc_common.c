/* SPDX-License-Identifier: GPL-2.0 */
/* Renesas Ethernet Switch Driver tc common functions
 *
 * Copyright (C) 2022 Renesas Electronics Corporation
 * Copyright (C) 2022 EPAM Systems
 */

#include "rswitch.h"
#include "rswitch_tc_filters.h"
#include "rswitch_tc_common.h"

int rswitch_tc_validate_flow_action(struct rswitch_device *rdev,
				struct flow_rule *rule)
{
	struct flow_action_entry *act;
	struct flow_action *actions = &rule->action;
	int i;
	bool redirect = false, dmac_change = false, vlan_change = false;

	flow_action_for_each(i, act, actions) {
		switch (act->id) {
		case FLOW_ACTION_DROP:
			if (!flow_offload_has_one_action(actions)) {
				pr_err("Other actions with DROP is not supported\n");
				return -EOPNOTSUPP;
			}
			break;
		case FLOW_ACTION_REDIRECT:
			if (!ndev_is_tsn_dev(act->dev, rdev->priv)) {
				pr_err("Can not redirect to not R-Switch TSN dev!\n");
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
		case FLOW_ACTION_VLAN_MANGLE:
			if (be16_to_cpu(act->vlan.proto) != ETH_P_8021Q) {
				pr_err("Unsupported VLAN proto for offload!\n");
				return -EOPNOTSUPP;
			}

			vlan_change = true;
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

	if (vlan_change & !redirect) {
		pr_err("VLAN mangle is supported only with redirect\n");
		return -EOPNOTSUPP;
	}

	return 0;
}

static int rswitch_tc_setup_redirect_action(struct rswitch_tc_filter *f)
{
	struct rswitch_device *rdev = f->rdev;

	f->param.slv = BIT(rdev->port);
	f->param.dv = BIT(f->target_rdev->port);
	f->param.csd = 0;

	if (f->action & (ACTION_CHANGE_DMAC | ACTION_VLAN_CHANGE)) {
		f->param.l23_info.priv = rdev->priv;
		f->param.l23_info.routing_number = rswitch_rn_get(rdev->priv);
		f->param.l23_info.routing_port_valid = BIT(rdev->port) | BIT(f->target_rdev->port);

		if (f->action & ACTION_CHANGE_DMAC) {
			ether_addr_copy(f->param.l23_info.dst_mac, f->dmac);
			f->param.l23_info.update_dst_mac = true;
		}

		if (f->action & ACTION_VLAN_CHANGE) {
			f->param.l23_info.update_ctag_vlan_id = true;
			f->param.l23_info.update_ctag_vlan_prio = true;
			f->param.l23_info.vlan_id = f->vlan_id;
			f->param.l23_info.vlan_prio = f->vlan_prio;
		}
	}

	return 0;
}
static int rswitch_tc_setup_drop_action(struct rswitch_tc_filter *f)
{
	f->param.slv = BIT(f->rdev->port);
	/* Explicitly zeroing parameters for drop */
	f->param.dv = 0;
	f->param.csd = 0;

	return 0;
}

int rswitch_tc_setup_flow_action(struct rswitch_tc_filter *f,
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
			 * sanitized by rswitch_tc_validate_flow_action().
			 */
			f->action |= ACTION_CHANGE_DMAC;
			rswitch_parse_pedit(f, act);
			break;
		case FLOW_ACTION_VLAN_MANGLE:
			f->action |= ACTION_VLAN_CHANGE;
			f->vlan_id = act->vlan.vid;
			f->vlan_prio = act->vlan.prio;
			break;
		default:
			/*
			 * Should not come here, such action will be dropped by
			 * rswitch_tc_validate_flow_action().
			 */
			pr_err("Unsupported action for offload!\n");
			return -EOPNOTSUPP;
		}
	}

	if (f->action & ACTION_DROP) {
		return rswitch_tc_setup_drop_action(f);
	}

	if (f->action & ACTION_MIRRED_REDIRECT) {
		return rswitch_tc_setup_redirect_action(f);
	}

	return -EOPNOTSUPP;
}
