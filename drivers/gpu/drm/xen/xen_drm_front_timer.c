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

#include "xen_drm_front_timer.h"

#include <drm/drmP.h>


void xen_drm_front_timer_start(struct xen_drm_front_timer *timer)
{
	unsigned long flags;

	spin_lock_irqsave(&timer->lock, flags);
	atomic_inc(&timer->running);
	if (atomic_read(&timer->running) == 1)
		mod_timer(&timer->timer, jiffies + timer->period);
	spin_unlock_irqrestore(&timer->lock, flags);
}

void xen_drm_front_timer_stop(struct xen_drm_front_timer *timer, bool force)
{
	unsigned long flags;

	if (!atomic_read(&timer->running))
		return;

	spin_lock_irqsave(&timer->lock, flags);
	if (force || atomic_dec_and_test(&timer->running))
		del_timer_sync(&timer->timer);
	spin_unlock_irqrestore(&timer->lock, flags);
}

static void timer_callback(unsigned long data)
{
	struct xen_drm_front_timer *timer = (struct xen_drm_front_timer *)data;

	if (likely(atomic_read(&timer->running))) {
		unsigned long flags;

		spin_lock_irqsave(&timer->lock, flags);
		timer->clb->on_period(timer->clb_private);
		spin_unlock_irqrestore(&timer->lock, flags);
		mod_timer(&timer->timer, jiffies + timer->period);
	}
}

int xen_drm_front_timer_init(struct xen_drm_front_timer *timer,
	unsigned long clb_private, struct xen_drm_front_timer_ops *clb)
{
	if (!clb)
		return -EINVAL;

	timer->clb = clb;
	timer->clb_private = clb_private;

	setup_timer(&timer->timer, timer_callback, (unsigned long)timer);
	spin_lock_init(&timer->lock);
	return 0;
}

void xen_drm_front_timer_setup(struct xen_drm_front_timer *timer,
	int freq_hz, int to_ms)
{
	timer->period = msecs_to_jiffies(1000 / freq_hz);
	timer->to_period = to_ms * freq_hz / 1000;
}

void xen_drm_front_timer_cleanup(struct xen_drm_front_timer *timer)
{
	xen_drm_front_timer_stop(timer, true);
}
