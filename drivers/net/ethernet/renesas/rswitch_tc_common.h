/* SPDX-License-Identifier: GPL-2.0 */
/* Renesas Ethernet Switch Driver tc common functions
 *
 * Copyright (C) 2022 Renesas Electronics Corporation
 * Copyright (C) 2022 EPAM Systems
 */

#ifndef __RSWITCH_TC_COMMON_H__
#define __RSWITCH_TC_COMMON_H__

#include "rswitch_tc_filters.h"

#define MAX_MATCH_LEN (256)
#define MAX_VLAN_MATCH_LEN (4)

struct filtering_vector {
	u8 values[MAX_MATCH_LEN];
	u8 masks[MAX_MATCH_LEN];
	u8 vlan_values[MAX_VLAN_MATCH_LEN];
	u8 vlan_masks[MAX_VLAN_MATCH_LEN];
	bool set_vlan;
};

typedef int (*fv_gen)(struct filtering_vector *, void *);

int rswitch_tc_validate_flow_action(struct rswitch_device *rdev,
				struct flow_rule *rule);

int rswitch_tc_setup_flow_action(struct rswitch_tc_filter *f,
				struct flow_rule *rule);

int rswitch_fill_pf_param(struct rswitch_pf_param *pf_param, fv_gen gen_fn,
			  void *filter_param);

int rswitch_u32_restore_l3(struct rswitch_device *rdev);
int rswitch_matchall_restore_l3(struct rswitch_device *rdev);
int rswitch_flower_restore_l3(struct rswitch_device *rdev);

#endif
