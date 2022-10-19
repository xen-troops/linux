/* SPDX-License-Identifier: GPL-2.0 */
/* Renesas Ethernet Switch Driver tc common functions
 *
 * Copyright (C) 2022 Renesas Electronics Corporation
 * Copyright (C) 2022 EPAM Systems
 */

#ifndef __RSWITCH_TC_COMMON_H__
#define __RSWITCH_TC_COMMON_H__

#include "rswitch_tc_filters.h"

int rswitch_tc_validate_flow_action(struct rswitch_device *rdev,
				struct flow_rule *rule);

int rswitch_tc_setup_flow_action(struct rswitch_tc_filter *f,
				struct flow_rule *rule);


#endif
