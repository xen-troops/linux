/* SPDX-License-Identifier: GPL-2.0 */
/* Renesas Ethernet Switch Driver tc common functions
 *
 * Copyright (C) 2022 Renesas Electronics Corporation
 * Copyright (C) 2022 EPAM Systems
 */

#include "rswitch.h"
#include "rswitch_tc_filters.h"
#include "rswitch_tc_common.h"

enum possible_combination {
	/* Can be matched by any filter */
	ONE_B = 1,
	/* Can be matched by any filter */
	TWO_B = 2,
	/* Can be matched by three or four byte filter in mask mode */
	THREE_B = 3,
	/* Can be matched by four byte filter in mask mode or two byte filter in expand mode */
	FOUR_B = 4,
	/* Can be matched by three byte filter in expand mode only */
	SIX_B = 6,
	/* Can be matched by four byte filter in expand mode only */
	EIGHT_B = 8,
};

/* The number of PF used for current iterations for the further setup */
struct used_pf_entries {
	int two_byte;
	int three_byte;
	int four_byte;
};

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

/* Find the least used filter to match four bytes */
static enum pf_type get_four_byte_matcher(struct rswitch_private *priv,
					  struct used_pf_entries *pf_entries)
{
	int used_two_bytes_hw, used_four_bytes_hw;
	int relative_two_bytes_used, relative_four_bytes_used;

	used_two_bytes_hw = get_two_byte_filter(priv);
	used_four_bytes_hw = get_four_byte_filter(priv);
	relative_two_bytes_used =
		((used_two_bytes_hw + pf_entries->two_byte) * 100) / PFL_TWBF_N;
	relative_four_bytes_used =
		((used_four_bytes_hw + pf_entries->four_byte) * 100) / PFL_FOBF_N;

	if (used_two_bytes_hw > 0 &&
	    relative_two_bytes_used < relative_four_bytes_used) {
		return PF_TWO_BYTE;
	}

	return PF_FOUR_BYTE;
}

/* Find the least used filter to match three bytes */
static enum pf_type get_three_byte_matcher(struct rswitch_private *priv,
					   struct used_pf_entries *pf_entries)
{
	int used_three_bytes_hw, used_four_bytes_hw;
	int relative_three_bytes_used, relative_four_bytes_used;

	used_three_bytes_hw = get_three_byte_filter(priv);
	used_four_bytes_hw = get_four_byte_filter(priv);
	relative_three_bytes_used =
		((used_three_bytes_hw + pf_entries->three_byte) * 100) / PFL_THBF_N;
	relative_four_bytes_used =
		((used_four_bytes_hw + pf_entries->four_byte) * 100) / PFL_FOBF_N;

	if (used_three_bytes_hw > 0 &&
	    relative_three_bytes_used < relative_four_bytes_used) {
		return PF_THREE_BYTE;
	}

	return PF_FOUR_BYTE;
}

/* Find the least used filter to match one or two bytes */
static enum pf_type get_one_or_two_byte_matcher(struct rswitch_private *priv,
						struct used_pf_entries *pf_entries)
{
	int used_two_bytes_hw, used_three_bytes_hw, used_four_bytes_hw;
	int relative_two_bytes_used, relative_three_bytes_used, relative_four_bytes_used;

	used_two_bytes_hw = get_two_byte_filter(priv);
	used_three_bytes_hw = get_three_byte_filter(priv);
	used_four_bytes_hw = get_four_byte_filter(priv);
	relative_two_bytes_used =
		((used_two_bytes_hw + pf_entries->two_byte) * 100) / PFL_TWBF_N;
	relative_three_bytes_used =
		((used_three_bytes_hw + pf_entries->three_byte) * 100) / PFL_THBF_N;
	relative_four_bytes_used =
		((used_four_bytes_hw + pf_entries->four_byte) * 100) / PFL_FOBF_N;

	if (used_four_bytes_hw >= 0 && relative_four_bytes_used < relative_two_bytes_used &&
	    relative_four_bytes_used < relative_three_bytes_used) {
		return PF_FOUR_BYTE;
	}

	if (used_three_bytes_hw >= 0 && relative_three_bytes_used < relative_two_bytes_used &&
	    relative_three_bytes_used < relative_four_bytes_used) {
		return PF_THREE_BYTE;
	}

	return PF_TWO_BYTE;
}

static int add_param_entry(struct rswitch_pf_param *param, int offset, struct filtering_vector *fv,
			   u8 mask_lb, int len, struct used_pf_entries *pf_entries)
{
	enum pf_type pf_type;
	u32 val32 = 0, ext_val32 = 0, mask;
	struct rswitch_private *priv = param->rdev->priv;
	int rc;

	switch (len) {
	case EIGHT_B:
		memcpy(&val32, &fv->values[offset], FOUR_B);
		memcpy(&ext_val32, &fv->values[offset + FOUR_B], FOUR_B);
		val32 = ntohl(val32);
		ext_val32 = ntohl(ext_val32);
		rc = rswitch_init_expand_pf_entry(param, PF_FOUR_BYTE, val32, ext_val32, offset);
		if (!rc)
			pf_entries->four_byte++;
		break;
	case SIX_B:
		memcpy(&val32, &fv->values[offset], THREE_B);
		memcpy(&ext_val32, &fv->values[offset + THREE_B], THREE_B);
		val32 = ntohl(val32) >> 8;
		ext_val32 = ntohl(ext_val32) >> 8;
		rc = rswitch_init_expand_pf_entry(param, PF_THREE_BYTE, val32, ext_val32, offset);
		pf_entries->three_byte++;
		break;
	case FOUR_B:
		pf_type = get_four_byte_matcher(priv, pf_entries);
		if (pf_type == PF_TWO_BYTE && mask_lb == 0xff) {
			memcpy(&val32, &fv->values[offset], TWO_B);
			memcpy(&ext_val32, &fv->values[offset + TWO_B], TWO_B);
			val32 = ntohs(val32);
			ext_val32 = ntohs(ext_val32);
			rc = rswitch_init_expand_pf_entry(param, PF_TWO_BYTE, val32, ext_val32,
							  offset);
			if (!rc)
				pf_entries->two_byte++;
		} else {
			memcpy(&val32, &fv->values[offset], FOUR_B);
			mask = ntohl(0xffffff00 | mask_lb);
			val32 = ntohl(val32);
			rc = rswitch_init_mask_pf_entry(param, PF_FOUR_BYTE, val32, mask, offset);
			if (!rc)
				pf_entries->four_byte++;
		}
		break;
	case THREE_B:
		pf_type = get_three_byte_matcher(priv, pf_entries);
		memcpy(&val32, &fv->values[offset], THREE_B);
		mask = ntohl(0xffff00 | mask_lb);
		if (pf_type == PF_FOUR_BYTE) {
			val32 = ntohl(val32);
			rc = rswitch_init_mask_pf_entry(param, PF_FOUR_BYTE, val32, mask,
							offset);
			if (!rc)
				pf_entries->four_byte++;
		} else {
			val32 = ntohl(val32) >> 8;
			mask = mask >> 8;
			rc = rswitch_init_mask_pf_entry(param, PF_THREE_BYTE, val32, mask, offset);
			if (!rc)
				pf_entries->three_byte++;
		}
		break;
	case TWO_B:
		pf_type = get_one_or_two_byte_matcher(priv, pf_entries);
		memcpy(&val32, &fv->values[offset], TWO_B);
		if (pf_type == PF_TWO_BYTE) {
			val32 = ntohs(val32);
			mask = ntohs(0xff00 | mask_lb);
			rc = rswitch_init_mask_pf_entry(param, pf_type, val32, mask, offset);
			if (!rc)
				pf_entries->two_byte++;
		} else if (pf_type == PF_THREE_BYTE) {
			val32 = ntohl(val32) >> 8;
			mask = ntohl(0xff00 | mask_lb) >> 8;
			rc = rswitch_init_mask_pf_entry(param, pf_type, val32, mask, offset);
			if (!rc)
				pf_entries->three_byte++;
		} else {
			val32 = ntohl(val32);
			mask = ntohl(0xff00 | mask_lb);
			rc = rswitch_init_mask_pf_entry(param, pf_type, val32, mask, offset);
			if (!rc)
				pf_entries->four_byte++;
		}
		break;
	case ONE_B:
		pf_type = get_one_or_two_byte_matcher(priv, pf_entries);
		memcpy(&val32, &fv->values[offset], ONE_B);
		if (pf_type == PF_TWO_BYTE) {
			val32 = ntohs(val32);
			mask = ntohs(mask_lb);
			rc = rswitch_init_mask_pf_entry(param, pf_type, val32, mask, offset);
			if (!rc)
				pf_entries->two_byte++;
		} else if (pf_type == PF_THREE_BYTE) {
			val32 = ntohl(val32) >> 8;
			mask = ntohl(mask_lb) >> 8;
			rc = rswitch_init_mask_pf_entry(param, pf_type, val32, mask, offset);
			if (!rc)
				pf_entries->three_byte++;
		} else {
			val32 = ntohl(val32);
			mask = ntohl(mask_lb);
			rc = rswitch_init_mask_pf_entry(param, pf_type, val32, mask, offset);
			if (!rc)
				pf_entries->four_byte++;
		}
		break;
	default:
		/* Can't be matched (e.g. 5 or 7 len) by one filter
		 * so separate it on two entries
		 */
		rc = add_param_entry(param, offset, fv, 0xff, FOUR_B, pf_entries);
		if (!rc) {
			rc = add_param_entry(param, offset + FOUR_B, fv, mask_lb,
					     len - FOUR_B, pf_entries);
		}
		break;
	}

	return rc;
}

static int rswitch_fill_vlan_pf_param(struct rswitch_pf_param *pf_param,
				      struct filtering_vector *fv,
				      struct used_pf_entries *pf_entries)
{
	int rc;
	u32 mask;
	u16 val16, mask16, ext_val16;

	memcpy(&mask, fv->vlan_masks, sizeof(mask));

	if (mask == 0xffffffff) {
		memcpy(&val16, &fv->vlan_values[0], TWO_B);
		memcpy(&ext_val16, &fv->vlan_values[TWO_B], TWO_B);
		rc = rswitch_init_tag_expand_pf_entry(pf_param, ntohs(val16), ntohs(ext_val16));
		if (!rc)
			pf_entries->two_byte++;
		return rc;
	}

	if (fv->vlan_masks[0] || fv->vlan_masks[1]) {
		memcpy(&val16, &fv->vlan_values[0], TWO_B);
		memcpy(&mask16, &fv->vlan_masks[0], TWO_B);
		rc = rswitch_init_tag_mask_pf_entry(pf_param, val16, mask16, 0);
		if (rc)
			return rc;
		pf_entries->two_byte++;
	}

	if (fv->vlan_masks[2] || fv->vlan_masks[3]) {
		memcpy(&val16, &fv->vlan_values[2], TWO_B);
		memcpy(&mask16, &fv->vlan_masks[2], TWO_B);
		rc = rswitch_init_tag_mask_pf_entry(pf_param, val16, mask16, 2);
		if (rc)
			return rc;
		pf_entries->two_byte++;
	}

	return 0;
}

int rswitch_fill_pf_param(struct rswitch_pf_param *pf_param, fv_gen gen_fn,
			  void *filter_param)
{
	struct used_pf_entries pf_entries = { 0 };
	struct filtering_vector fv = { 0 };
	int continues_mask = 0, mask_length = 0, i, rc;

	rc = gen_fn(&fv, filter_param);
	if (rc)
		return rc;

	/* Set VLAN first to let know how many two byte filters needed for filtering */
	if (fv.set_vlan) {
		rc = rswitch_fill_vlan_pf_param(pf_param, &fv, &pf_entries);
		if (rc)
			return rc;
	}

	for (i = 0; i < MAX_MATCH_LEN; i++) {
		if (fv.masks[i] == 0xff) {
			if (!continues_mask)
				continues_mask = 1;

			/* The maximum length that can be matched by one filter is 8 */
			if (mask_length >= EIGHT_B) {
				rc = add_param_entry(pf_param, i - mask_length,
						     &fv, 0xff, mask_length, &pf_entries);
				if (rc)
					return rc;
				mask_length = 0;
			}
			mask_length++;
		} else {
			/* If the last byte of continues mask is not equal 0xff we have
			 * to use only mask mode to filter this.
			 */
			if (fv.masks[i]) {
				if (mask_length >= FOUR_B) {
					rc = add_param_entry(pf_param, i - mask_length,
							     &fv, 0xff, FOUR_B, &pf_entries);
					if (rc)
						return rc;
					mask_length -= FOUR_B;
				} else {
					mask_length++;
				}
				rc = add_param_entry(pf_param, i - mask_length + 1,
						     &fv, fv.masks[i], mask_length, &pf_entries);
				if (rc)
					return rc;
			} else if (continues_mask) {
				rc = add_param_entry(pf_param, i - mask_length,
						     &fv, 0xff, mask_length, &pf_entries);
				if (rc)
					return rc;
			}

			continues_mask = 0;
			mask_length = 0;
		}
	}

	if (continues_mask) {
		rc = add_param_entry(pf_param, i - mask_length, &fv, fv.masks[i - 1], mask_length,
				     &pf_entries);
		if (rc)
			return rc;
	}

	return 0;
}
