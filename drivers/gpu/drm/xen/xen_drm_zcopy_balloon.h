/* SPDX-License-Identifier: GPL-2.0 OR MIT */

/*
 *  Xen zero-copy helper DRM device
 *
 * Copyright (C) 2016-2018 EPAM Systems Inc.
 *
 * Author: Oleksandr Andrushchenko <oleksandr_andrushchenko@epam.com>
 */

#ifndef __XEN_DRM_ZCOPY_BALLOON_H_
#define __XEN_DRM_ZCOPY_BALLOON_H_

#include <linux/types.h>

#ifndef GRANT_INVALID_REF
/*
 * Note on usage of grant reference 0 as invalid grant reference:
 * grant reference 0 is valid, but never exposed to a PV driver,
 * because of the fact it is already in use/reserved by the PV console.
 */
#define GRANT_INVALID_REF	0
#endif

struct xen_drm_zcopy_balloon {
	void *vaddr;
	dma_addr_t dev_bus_addr;
};

int xen_drm_zcopy_ballooned_pages_alloc(struct device *dev,
					struct xen_drm_zcopy_balloon *obj,
					int num_pages,
					struct page **pages);

void xen_drm_zcopy_ballooned_pages_free(struct device *dev,
					struct xen_drm_zcopy_balloon *obj,
					int num_pages,
					struct page **pages);

#endif /* __XEN_DRM_ZCOPY_BALLOON_H_ */
