/*
 * Copyright (C) 2017 CETITEC GmbH. All Rights Reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#ifndef avbeth_ops_h_
#define avbeth_ops_h_

#include <linux/types.h>
#include <linux/netdevice.h>

typedef unsigned ctc_avbeth_queue_t;

typedef enum {
	ctc_avbeth_class_a = 8000,
	ctc_avbeth_class_b = 4000,
	ctc_avbeth_class_c = 750,
	ctc_avbeth_none = 0
} ctc_avbeth_class_t;


struct ctc_avbeth_ops {
	/* Either get_ethts or get_ethts_and_systs must be implemented. */

	/* Write current ethernet HW timestamp to ethts (unit is ns) */
	int (*get_ethts)(struct net_device *netdev, u_int64_t *ethts);

	/* Write correlated current ethernet HW timestamp to ethts and current
	   system TS to systs. Timestamp unit is ns */
	int (*get_ethts_and_systs)(struct net_device *netdev, u_int64_t *ethts,
							   ktime_t *systs);

	int (*get_queue_for_class)(struct net_device *netdev,
							   ctc_avbeth_class_t class,
							   ctc_avbeth_queue_t *queue);

	int (*queue_add_vlan)(struct net_device *netdev, ctc_avbeth_queue_t queue,
						  u_int16_t vlan_mask, u_int16_t vlan_match);
	int (*queue_remove_vlan)(struct net_device *netdev, u_int16_t vlan_mask,
							 u_int16_t vlan_match);

	int (*queue_adjust_shaper)(struct net_device *netdev,
							   ctc_avbeth_queue_t queue, int32_t bytes);


};



#endif // #ifndef avbeth_ops_h_
