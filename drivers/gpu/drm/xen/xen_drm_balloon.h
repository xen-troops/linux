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

#ifndef __XEN_DRM_BALLOON_H_
#define __XEN_DRM_BALLOON_H_

#define GRANT_INVALID_REF	0

#include <linux/types.h>

struct xen_drm_balloon {
	void *vaddr;
	dma_addr_t dev_bus_addr;
};

int xen_drm_ballooned_pages_alloc(struct device *dev,
	struct xen_drm_balloon *obj, int num_pages, struct page **pages);
void xen_drm_ballooned_pages_free(struct device *dev,
	struct xen_drm_balloon *obj,int num_pages, struct page **pages);

#endif /* __XEN_DRM_BALLOON_H_ */
