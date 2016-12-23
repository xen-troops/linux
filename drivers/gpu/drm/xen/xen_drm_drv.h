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
 * Copyright (C) 2016 EPAM Systems Inc.
 *
 * Author: Oleksandr Andrushchenko <Oleksandr_Andrushchenko@epam.com>
 */

#ifndef __XEN_DRM_H
#define __XEN_DRM_H

#include "xen_drm_crtc.h"
#include "xen_drm_timer.h"

#define XENDRM_MAX_CRTCS	4

struct xendispl_front_ops;
struct platform_device;
struct drm_framebuffer;
struct drm_gem_object;

struct xendrm_device {
	struct xdrv_info *xdrv_info;
	struct xendispl_front_ops *front_ops;
	struct drm_device *drm;
	int num_crtcs;
	struct xendrm_plat_data *platdata;
	struct xendrm_crtc crtcs[XENDRM_MAX_CRTCS];

	/* vblank and page flip handling */
	struct xendrm_timer vblank_timer;
	atomic_t pflip_to_cnt[XENDRM_MAX_CRTCS];
	atomic_t pflip_to_cnt_armed[XENDRM_MAX_CRTCS];
	atomic_t vblank_enabled[XENDRM_MAX_CRTCS];
};

struct xendrm_cfg_connector {
	int width;
	int height;
	char *xenstore_path;
};

struct xendrm_plat_data {
	struct xdrv_info *xdrv_info;
	/* number of connectors in this configuration */
	int num_connectors;
	/* connector configurations */
	struct xendrm_cfg_connector connectors[XENDRM_MAX_CRTCS];
	/* set if dumb buffers are allocated externally on backend side */
	bool be_alloc;
};

static inline uint64_t xendrm_fb_to_cookie(struct drm_framebuffer *fb)
{
	return (uint64_t)fb;
}

static inline uint64_t xendrm_dumb_to_cookie(struct drm_gem_object *gem_obj)
{
	return (uint64_t)gem_obj;
}

int xendrm_probe(struct platform_device *pdev,
	struct xendispl_front_ops *xendrm_front_funcs);
int xendrm_remove(struct platform_device *pdev);
bool xendrm_is_used(struct platform_device *pdev);

void xendrm_vtimer_restart_to(struct xendrm_device *xendrm_dev, int index);
void xendrm_vtimer_cancel_to(struct xendrm_device *xendrm_dev, int index);

#endif /* __XEN_DRM_H*/

