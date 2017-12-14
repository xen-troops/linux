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

#ifndef __XEN_DRM_FRONT_SHBUF_H_
#define __XEN_DRM_FRONT_SHBUF_H_

#include <linux/kernel.h>
#include <linux/scatterlist.h>

#include <xen/grant_table.h>

struct xen_drm_front_shbuf {
	struct list_head list;
	uint64_t dbuf_cookie;
	uint64_t fb_cookie;
	/*
	 * number of references granted for the backend use:
	 *  - for allocated/imported dma-buf's this holds number of grant
	 *    references for the page directory and pages of the buffer
	 *  - for the buffer provided by the backend this holds number of
	 *    grant references for the page directory as grant references for
	 *    the buffer will be provided by the backend
	 */
	int num_grefs;
	grant_ref_t *grefs;
	unsigned char *vdirectory;

	/*
	 * there are 2 ways to provide backing storage for this shared buffer:
	 * either pages or an sgt. if buffer created from the sgt then we own
	 * the pages and must free those ourselves on closure
	 */
	int num_pages;
	struct page **pages;

	struct sg_table *sgt;

	struct xenbus_device *xb_dev;

	/* set if this buffer was allocated by the backend */
	bool be_alloc;
	/* Xen map handles for the buffer allocated by the backend */
	grant_handle_t *be_alloc_map_handles;
};

struct xen_drm_front_shbuf_alloc {
	struct xenbus_device *xb_dev;
	struct list_head *dbuf_list;
	uint64_t dbuf_cookie;

	size_t size;
	struct page **pages;
	struct sg_table *sgt;

	bool be_alloc;
};

grant_ref_t xen_drm_front_shbuf_get_dir_start(struct xen_drm_front_shbuf *buf);
struct xen_drm_front_shbuf *xen_drm_front_shbuf_alloc(
	struct xen_drm_front_shbuf_alloc *info);
int xen_drm_front_shbuf_be_alloc_map(struct xen_drm_front_shbuf *buf);
struct xen_drm_front_shbuf *xen_drm_front_shbuf_get_by_dbuf_cookie(
	struct list_head *dbuf_list, uint64_t dbuf_cookie);
void xen_drm_front_shbuf_flush_fb(struct list_head *dbuf_list,
	uint64_t fb_cookie);
void xen_drm_front_shbuf_free_by_dbuf_cookie(struct list_head *dbuf_list,
	uint64_t dbuf_cookie);
void xen_drm_front_shbuf_free_all(struct list_head *dbuf_list);

#endif /* __XEN_DRM_FRONT_SHBUF_H_ */
