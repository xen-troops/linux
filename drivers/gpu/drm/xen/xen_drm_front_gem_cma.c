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

#include <drm/drmP.h>
#include <drm/drm_gem.h>
#include <drm/drm_fb_cma_helper.h>
#include <drm/drm_gem_cma_helper.h>

#include "xen_drm_front.h"
#include "xen_drm_front_drv.h"
#include "xen_drm_front_gem.h"

static struct drm_gem_object *gem_import_sg_table(struct drm_device *dev,
	struct dma_buf_attachment *attach, struct sg_table *sgt)
{
	struct xen_drm_front_drm_info *drm_info = dev->dev_private;
	struct drm_gem_object *gem_obj;
	struct drm_gem_cma_object *cma_obj;
	int ret;

	gem_obj = drm_gem_cma_prime_import_sg_table(dev, attach, sgt);
	if (IS_ERR_OR_NULL(gem_obj))
		return gem_obj;

	cma_obj = to_drm_gem_cma_obj(gem_obj);

	ret = drm_info->front_ops->dbuf_create_from_sgt(
		drm_info->front_info,
		xen_drm_front_dbuf_to_cookie(gem_obj),
		0, 0, 0, gem_obj->size,
		drm_gem_cma_prime_get_sg_table(gem_obj));
	if (ret < 0)
		return ERR_PTR(ret);

	DRM_DEBUG("Imported CMA buffer of size %zu\n", gem_obj->size);

	return gem_obj;
}

static struct page **gem_get_pages(struct drm_gem_object *gem_obj)
{
	return NULL;
}

static const struct xen_drm_front_gem_ops xen_drm_front_gem_cma_ops = {
	.free_object_unlocked  = drm_gem_cma_free_object,
	.prime_get_sg_table    = drm_gem_cma_prime_get_sg_table,
	.prime_import_sg_table = gem_import_sg_table,

	.prime_vmap            = drm_gem_cma_prime_vmap,
	.prime_vunmap          = drm_gem_cma_prime_vunmap,
	.prime_mmap            = drm_gem_cma_prime_mmap,

	.dumb_create           = drm_gem_cma_dumb_create,
	.dumb_map_offset       = drm_gem_cma_dumb_map_offset,

	.fb_create_with_funcs  = drm_fb_cma_create_with_funcs,
	.fb_destroy            = drm_fb_cma_destroy,

	.mmap                  = drm_gem_cma_mmap,

	.get_pages             = gem_get_pages,
};

const struct xen_drm_front_gem_ops *xen_drm_front_gem_get_ops(void)
{
	return &xen_drm_front_gem_cma_ops;
}
