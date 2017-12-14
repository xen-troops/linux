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

#ifndef __XEN_DRM_FRONT_CFG_H_
#define __XEN_DRM_FRONT_CFG_H_

#include <linux/types.h>

#define XEN_DRM_FRONT_MAX_CRTCS	4

struct xen_drm_front_cfg_connector {
	int width;
	int height;
	char *xenstore_path;
};

struct xen_drm_front_cfg_plat_data {
	struct xen_drm_front_info *front_info;
	/* number of connectors in this configuration */
	int num_connectors;
	/* connector configurations */
	struct xen_drm_front_cfg_connector connectors[XEN_DRM_FRONT_MAX_CRTCS];
	/* set if dumb buffers are allocated externally on backend side */
	bool be_alloc;
};

int xen_drm_front_cfg_card(struct xen_drm_front_info *front_info,
	struct xen_drm_front_cfg_plat_data *plat_data);

#endif /* __XEN_DRM_FRONT_CFG_H_ */
