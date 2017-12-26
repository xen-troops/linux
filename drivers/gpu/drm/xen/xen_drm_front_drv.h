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

#ifndef __XEN_DRM_FRONT_DRV_H_
#define __XEN_DRM_FRONT_DRV_H_

#include <drm/drmP.h>

#include "xen_drm_front.h"
#include "xen_drm_front_cfg.h"
#include "xen_drm_front_crtc.h"

struct xen_drm_front_drm_info {
	struct xen_drm_front_info *front_info;
	struct xen_drm_front_ops *front_ops;
	const struct xen_drm_front_gem_ops *gem_ops;
	struct drm_device *drm_dev;
	int num_crtcs;
	struct xen_drm_front_cfg_plat_data *plat_data;
	struct xen_drm_front_crtc crtcs[XEN_DRM_FRONT_MAX_CRTCS];

	/* vblank emulation timer */
	struct timer_list vblank_timer;
	bool vblank_enabled[XEN_DRM_FRONT_MAX_CRTCS];
};

static inline uint64_t xen_drm_front_fb_to_cookie(struct drm_framebuffer *fb)
{
	return (uint64_t)fb;
}

static inline uint64_t xen_drm_front_dbuf_to_cookie(
	struct drm_gem_object *gem_obj)
{
	return (uint64_t)gem_obj;
}

int xen_drm_front_drv_probe(struct platform_device *pdev,
	struct xen_drm_front_ops *xendrm_front_funcs);
int xen_drm_front_drv_remove(struct platform_device *pdev);

bool xen_drm_front_drv_is_used(struct platform_device *pdev);

#endif /* __XEN_DRM_FRONT_DRV_H_ */

