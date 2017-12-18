/*
 *  Xen para-virtual DRM device
 *
 *   This program is free software; you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation; either version 2 of the License, or
 *   (at your option) any later version.
 *
 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 *
 * Copyright (C) 2016-2017 EPAM Systems Inc.
 *
 * Author: Oleksandr Andrushchenko <oleksandr_andrushchenko@epam.com>
 */

#ifndef __XEN_DRM_FRONT_TIMER_H_
#define __XEN_DRM_FRONT_TIMER_H_

#include <linux/time.h>
#include <linux/interrupt.h>

struct xen_drm_front_timer_ops {
	void (*on_period)(unsigned long data);
};

struct xen_drm_front_timer {
	struct timer_list timer;
	unsigned long period;
	spinlock_t lock;
	int to_period;
	unsigned long clb_private;
	struct xen_drm_front_timer_ops *clb;
	atomic_t running;
};

int xen_drm_front_timer_init(struct xen_drm_front_timer *timer,
	unsigned long clb_private, struct xen_drm_front_timer_ops *clb);
void xen_drm_front_timer_setup(struct xen_drm_front_timer *timer,
	int freq_hz, int to_ms);
void xen_drm_front_timer_cleanup(struct xen_drm_front_timer *timer);
void xen_drm_front_timer_start(struct xen_drm_front_timer *timer);
void xen_drm_front_timer_stop(struct xen_drm_front_timer *timer, bool force);

#endif /* __XEN_DRM_FRONT_TIMER_H_ */
