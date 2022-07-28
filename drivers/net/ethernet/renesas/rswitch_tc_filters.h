/* SPDX-License-Identifier: GPL-2.0 */
/* Renesas Ethernet Switch Driver tc common header functions
 *
 * Copyright (C) 2022 Renesas Electronics Corporation
 * Copyright (C) 2022 EPAM Systems
 */

#include <linux/netdevice.h>
#include <net/pkt_cls.h>
#include <net/flow_offload.h>

enum rswitch_tc_action {
	ACTION_SKBMOD = 1,
	ACTION_MIRRED_REDIRECT = 2,
	ACTION_DROP = 4,
};

struct rswitch_tc_filter {
	struct rswitch_device *rdev;
	struct rswitch_device *target_rdev;
	unsigned long cookie;
	struct l3_ipv4_fwd_param param;
	struct list_head lh;
	u8 dmac[ETH_ALEN];
	enum rswitch_tc_action action;
};

int rswitch_setup_tc_flower(struct net_device *dev,
				struct flow_cls_offload *cls_flower);

int rswitch_setup_tc_cls_u32(struct net_device *dev,
				 struct tc_cls_u32_offload *cls_u32);
