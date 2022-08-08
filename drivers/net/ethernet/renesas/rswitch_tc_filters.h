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

static inline void rswitch_parse_pedit(struct rswitch_tc_filter *f, struct flow_action_entry *a)
{
	/*
	  MAC comes to pedit action as 2 parts of u32 with offsets,
	  so we have to concatenate it in 2 steps
	*/
	if (!a->mangle.offset) {
		memcpy(f->dmac, &(a->mangle.val), 4);
	} else {
		memcpy(f->dmac + 4, &(a->mangle.val), 2);
	}
}

int rswitch_setup_tc_flower(struct net_device *dev,
				struct flow_cls_offload *cls_flower);

int rswitch_setup_tc_cls_u32(struct net_device *dev,
				 struct tc_cls_u32_offload *cls_u32);

int rswitch_setup_tc_matchall(struct net_device *dev,
				  struct tc_cls_matchall_offload *cls_matchall);
