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

#ifndef __XEN_DRM_FRONT_CRTC_H_
#define __XEN_DRM_FRONT_CRTC_H_

#include <drm/drmP.h>
#include <drm/drm_crtc.h>

#include <linux/wait.h>

#define XENDRM_CRTC_VREFRESH_HZ	60

struct xen_drm_front_drm_info;
struct xen_drm_front_cfg_connector;

struct xen_drm_front_connector {
	struct drm_connector base;
	int width, height;
};

struct xen_drm_front_crtc {
	int index;
	struct xen_drm_front_drm_info *drm_info;
	struct drm_plane primary;
	struct drm_crtc crtc;
	struct drm_encoder encoder;
	struct xen_drm_front_connector connector;

	/* vblank and flip handling */
	atomic_t pg_flip_source_cnt;
	struct drm_pending_vblank_event *pg_flip_event;
	wait_queue_head_t flip_wait;
	/* page flip event time-out handling */
	struct timer_list pg_flip_to_timer;
	/* current fb cookie */
	uint64_t fb_cookie;

	struct {
		struct drm_property *alpha;
	} props;
};

int xen_drm_front_crtc_create(struct xen_drm_front_drm_info *drm_info,
	struct xen_drm_front_crtc *xen_crtc, unsigned int index);
int xen_drm_front_crtc_encoder_create(struct xen_drm_front_drm_info *drm_info,
	struct xen_drm_front_crtc *xen_crtc);
int xen_drm_front_crtc_connector_create(struct xen_drm_front_drm_info *drm_info,
	struct xen_drm_front_crtc *xen_crtc,
	struct xen_drm_front_cfg_connector *cfg);

void xen_drm_front_crtc_on_page_flip_done(struct xen_drm_front_crtc *xen_crtc,
	uint64_t fb_cookie);

#endif /* __XEN_DRM_FRONT_CRTC_H_ */
