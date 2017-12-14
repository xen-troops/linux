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

#include "xen_drm_front_gem.h"

#include <drm/drmP.h>
#include <drm/drm_crtc_helper.h>
#include <drm/drm_fb_helper.h>
#include <drm/drm_gem.h>

#include <linux/dma-buf.h>
#include <linux/scatterlist.h>
#include <linux/shmem_fs.h>

#include "xen_drm_balloon.h"
#include "xen_drm_front.h"
#include "xen_drm_front_drv.h"
#include "xen_drm_front_shbuf.h"

struct xen_gem_object {
	struct drm_gem_object base;

	size_t num_pages;
	struct page **pages;

	struct xen_drm_balloon balloon;

	/* set for buffers allocated by the backend */
	bool be_alloc;

	/* this is for imported PRIME buffer */
	struct sg_table *sgt_imported;
};

struct xen_fb {
	struct drm_framebuffer fb;
	struct xen_gem_object *xen_obj;
};

static inline struct xen_gem_object *to_xen_gem_obj(
	struct drm_gem_object *gem_obj)
{
	return container_of(gem_obj, struct xen_gem_object, base);
}

static inline struct xen_fb *to_xen_fb(struct drm_framebuffer *fb)
{
	return container_of(fb, struct xen_fb, fb);
}

static int gem_alloc_pages_array(struct xen_gem_object *xen_obj,
	size_t buf_size)
{
	xen_obj->num_pages = DIV_ROUND_UP(buf_size, PAGE_SIZE);
	xen_obj->pages = drm_malloc_ab(xen_obj->num_pages,
		sizeof(struct page *));
	return xen_obj->pages == NULL ? -ENOMEM : 0;
}

static void gem_free_pages_array(struct xen_gem_object *xen_obj)
{
	drm_free_large(xen_obj->pages);
	xen_obj->pages = NULL;
}

static struct xen_gem_object *gem_create_obj(struct drm_device *dev,
	size_t size)
{
	struct xen_gem_object *xen_obj;
	int ret;

	xen_obj = kzalloc(sizeof(*xen_obj), GFP_KERNEL);
	if (!xen_obj)
		return ERR_PTR(-ENOMEM);

	ret = drm_gem_object_init(dev, &xen_obj->base, size);
	if (ret < 0)
		goto fail;

	return xen_obj;

fail:
	kfree(xen_obj);
	return ERR_PTR(ret);
}

static struct xen_gem_object *gem_create(struct drm_device *dev,
	size_t size)
{
	struct xen_drm_front_drm_info *drm_info = dev->dev_private;
	struct xen_gem_object *xen_obj;
	int ret;

	size = round_up(size, PAGE_SIZE);
	xen_obj = gem_create_obj(dev, size);
	if (IS_ERR_OR_NULL(xen_obj))
		return xen_obj;

	if (drm_info->plat_data->be_alloc) {
		/*
		 * backend will allocate space for this buffer, so
		 * only allocate array of pointers to pages
		 */
		xen_obj->be_alloc = true;
		ret = gem_alloc_pages_array(xen_obj, size);
		if (ret < 0) {
			gem_free_pages_array(xen_obj);
			goto fail;
		}

		ret = xen_drm_ballooned_pages_alloc(dev->dev, &xen_obj->balloon,
			xen_obj->num_pages, xen_obj->pages);
		if (ret < 0) {
			DRM_ERROR("Cannot allocate %zu ballooned pages: %d\n",
				xen_obj->num_pages, ret);
			goto fail;
		}

		return xen_obj;
	}
	/*
	 * need to allocate backing pages now, so we can share those
	 * with the backend
	 */
	xen_obj->num_pages = DIV_ROUND_UP(size, PAGE_SIZE);
	xen_obj->pages = drm_gem_get_pages(&xen_obj->base);
	if (IS_ERR_OR_NULL(xen_obj->pages)) {
		ret = PTR_ERR(xen_obj->pages);
		xen_obj->pages = NULL;
		goto fail;
	}

	return xen_obj;

fail:
	DRM_ERROR("Failed to allocate buffer with size %zu\n", size);
	return ERR_PTR(ret);
}

static struct xen_gem_object *gem_create_with_handle(
	struct drm_file *file_priv, struct drm_device *dev,
	size_t size, uint32_t *handle)
{
	struct xen_gem_object *xen_obj;
	struct drm_gem_object *gem_obj;
	int ret;

	xen_obj = gem_create(dev, size);
	if (IS_ERR_OR_NULL(xen_obj))
		return xen_obj;

	gem_obj = &xen_obj->base;
	ret = drm_gem_handle_create(file_priv, gem_obj, handle);
	/* handle holds the reference */
	drm_gem_object_unreference_unlocked(gem_obj);
	if (ret < 0)
		return ERR_PTR(ret);

	return xen_obj;
}

static int gem_dumb_create(struct drm_file *file_priv,
	struct drm_device *dev, struct drm_mode_create_dumb *args)
{
	struct xen_gem_object *xen_obj;

	args->pitch = DIV_ROUND_UP(args->width * args->bpp, 8);
	args->size = args->pitch * args->height;

	xen_obj = gem_create_with_handle(file_priv, dev, args->size,
		&args->handle);
	if (IS_ERR_OR_NULL(xen_obj))
		return xen_obj == NULL ? -ENOMEM : PTR_ERR(xen_obj);

	return 0;
}

static void gem_free_object(struct drm_gem_object *gem_obj)
{
	struct xen_gem_object *xen_obj = to_xen_gem_obj(gem_obj);

	if (xen_obj->base.import_attach) {
		drm_prime_gem_destroy(&xen_obj->base, xen_obj->sgt_imported);
		if (xen_obj->pages)
			gem_free_pages_array(xen_obj);
	} else {
		if (xen_obj->pages) {
			if (xen_obj->be_alloc) {
				xen_drm_ballooned_pages_free(gem_obj->dev->dev,
					&xen_obj->balloon,
					xen_obj->num_pages, xen_obj->pages);
				gem_free_pages_array(xen_obj);
			} else {
				drm_gem_put_pages(&xen_obj->base,
					xen_obj->pages, true, false);
			}
		}
	}
	drm_gem_object_release(gem_obj);
	kfree(xen_obj);
}

static struct page **gem_get_pages(struct drm_gem_object *gem_obj)
{
	struct xen_gem_object *xen_obj = to_xen_gem_obj(gem_obj);

	return xen_obj->pages;
}

static struct sg_table *gem_get_sg_table(struct drm_gem_object *gem_obj)
{
	struct xen_gem_object *xen_obj = to_xen_gem_obj(gem_obj);

	if (!xen_obj->pages)
		return NULL;

	return drm_prime_pages_to_sg(xen_obj->pages, xen_obj->num_pages);
}

static struct drm_gem_object *gem_import_sg_table(struct drm_device *dev,
	struct dma_buf_attachment *attach, struct sg_table *sgt)
{
	struct xen_drm_front_drm_info *drm_info = dev->dev_private;
	struct xen_gem_object *xen_obj;
	size_t size;
	int ret;

	size = attach->dmabuf->size;
	xen_obj = gem_create_obj(dev, size);
	if (IS_ERR(xen_obj))
		return ERR_CAST(xen_obj);

	ret = gem_alloc_pages_array(xen_obj, size);
	if (ret < 0)
		return ERR_PTR(ret);

	xen_obj->sgt_imported = sgt;

	ret = drm_prime_sg_to_page_addr_arrays(sgt, xen_obj->pages,
		NULL, xen_obj->num_pages);
	if (ret < 0)
		return ERR_PTR(ret);

	/*
	 * N.B. Although we have an API to create display buffer from sgt
	 * we use pages API, because we still need these for GEM handling,
	 * e.g. for mapping etc.
	 */
	ret = drm_info->front_ops->dbuf_create(
		drm_info->front_info,
		xen_drm_front_dbuf_to_cookie(&xen_obj->base),
		0, 0, 0, size, xen_obj->pages);
	if (ret < 0)
		return ERR_PTR(ret);

	DRM_DEBUG("Imported buffer of size %zu with nents %u\n",
		size, sgt->nents);

	return &xen_obj->base;
}

static struct xen_fb *gem_fb_alloc(struct drm_device *dev,
	const struct drm_mode_fb_cmd2 *mode_cmd,
	struct xen_gem_object *xen_obj,
	const struct drm_framebuffer_funcs *funcs)
{
	struct xen_fb *xen_fb;
	int ret;

	xen_fb = kzalloc(sizeof(*xen_fb), GFP_KERNEL);
	if (!xen_fb)
		return ERR_PTR(-ENOMEM);

	drm_helper_mode_fill_fb_struct(&xen_fb->fb, mode_cmd);
	xen_fb->xen_obj = xen_obj;
	ret = drm_framebuffer_init(dev, &xen_fb->fb, funcs);
	if (ret < 0) {
		DRM_ERROR("Failed to initialize framebuffer: %d\n", ret);
		kfree(xen_fb);
		return ERR_PTR(ret);
	}

	return xen_fb;
}

static struct drm_framebuffer *gem_fb_create_with_funcs(struct drm_device *dev,
	struct drm_file *file_priv, const struct drm_mode_fb_cmd2 *mode_cmd,
	const struct drm_framebuffer_funcs *funcs)
{
	struct xen_fb *xen_fb;
	struct xen_gem_object *xen_obj;
	struct drm_gem_object *gem_obj;
	unsigned int hsub;
	unsigned int vsub;
	unsigned int min_size;
	int ret;

	/* we do not support formats that require more than 1 plane */
	if (drm_format_num_planes(mode_cmd->pixel_format) != 1) {
		DRM_ERROR("Unsupported pixel format 0x%04x\n",
			mode_cmd->pixel_format);
		return ERR_PTR(-EINVAL);
	}

	hsub = drm_format_horz_chroma_subsampling(mode_cmd->pixel_format);
	vsub = drm_format_vert_chroma_subsampling(mode_cmd->pixel_format);

	gem_obj = drm_gem_object_lookup(file_priv, mode_cmd->handles[0]);
	if (!gem_obj) {
		DRM_ERROR("Failed to lookup GEM object\n");
		return ERR_PTR(-ENXIO);
	}

	min_size = (mode_cmd->height - 1) * mode_cmd->pitches[0] +
		mode_cmd->width *
		drm_format_plane_cpp(mode_cmd->pixel_format, 0) +
		mode_cmd->offsets[0];
	if (gem_obj->size < min_size) {
		drm_gem_object_unreference_unlocked(gem_obj);
		return ERR_PTR(-EINVAL);
	}
	xen_obj = to_xen_gem_obj(gem_obj);

	xen_fb = gem_fb_alloc(dev, mode_cmd, xen_obj, funcs);
	if (IS_ERR(xen_fb)) {
		ret = PTR_ERR(xen_fb);
		goto fail;
	}

	return &xen_fb->fb;

fail:
	drm_gem_object_unreference_unlocked(gem_obj);
	return ERR_PTR(ret);
}

static void gem_fb_destroy(struct drm_framebuffer *fb)
{
	struct xen_fb *xen_fb = to_xen_fb(fb);

	if (xen_fb->xen_obj)
		drm_gem_object_unreference_unlocked(&xen_fb->xen_obj->base);

	drm_framebuffer_cleanup(fb);
	kfree(xen_fb);
}

static int gem_dumb_map_offset(struct drm_file *file_priv,
	struct drm_device *dev, uint32_t handle, uint64_t *offset)
{
	struct drm_gem_object *gem_obj;
	struct xen_gem_object *xen_obj;
	int ret = 0;

	gem_obj = drm_gem_object_lookup(file_priv, handle);
	if (!gem_obj) {
		DRM_ERROR("Failed to lookup GEM object\n");
		return -ENOENT;
	}

	xen_obj = to_xen_gem_obj(gem_obj);
	/* do not allow mapping of the imported buffers */
	if (xen_obj->base.import_attach) {
		ret = -EINVAL;
	} else {
		ret = drm_gem_create_mmap_offset(gem_obj);
		if (ret < 0)
			*offset = 0;
		else
			*offset = drm_vma_node_offset_addr(&gem_obj->vma_node);
	}

	drm_gem_object_unreference_unlocked(gem_obj);
	return ret;
}

static inline void gem_mmap_obj(struct xen_gem_object *xen_obj,
	struct vm_area_struct *vma)
{
	/*
	 * clear the VM_PFNMAP flag that was set by drm_gem_mmap(), and set the
	 * vm_pgoff (used as a fake buffer offset by DRM) to 0 as we want to map
	 * the whole buffer.
	 */
	vma->vm_flags &= ~VM_PFNMAP;
	vma->vm_flags |= VM_MIXEDMAP;
	vma->vm_pgoff = 0;
	/* this is the only way we can map in unprivileged domain */
	vma->vm_page_prot = PAGE_SHARED;
}

static int gem_mmap(struct file *filp, struct vm_area_struct *vma)
{
	struct xen_gem_object *xen_obj;
	struct drm_gem_object *gem_obj;
	unsigned long addr = vma->vm_start;
	int ret, i;

	ret = drm_gem_mmap(filp, vma);
	if (ret < 0)
		return ret;

	gem_obj = vma->vm_private_data;
	xen_obj = to_xen_gem_obj(gem_obj);
	gem_mmap_obj(xen_obj, vma);
	/*
	 * vm_operations_struct.fault handler will be called if CPU access
	 * to VM is here. For GPUs this isn't the case, because CPU
	 * doesn't touch the memory. Insert pages now, so both CPU and GPU are
	 * happy.
	 * FIXME: as we insert all the pages now then no .fault handler must
	 * be called, so don't provide one
	 */
	for (i = 0; i < xen_obj->num_pages; i++) {
		ret = vm_insert_page(vma, addr, xen_obj->pages[i]);
		if (ret < 0) {
			DRM_ERROR("Failed to insert pages into vma: %d\n", ret);
			return ret;
		}

		addr += PAGE_SIZE;
	}

	return 0;

}

static void *gem_prime_vmap(struct drm_gem_object *gem_obj)
{
	struct xen_gem_object *xen_obj = to_xen_gem_obj(gem_obj);

	if (!xen_obj->pages)
		return NULL;

	return vmap(xen_obj->pages, xen_obj->num_pages,
		GFP_KERNEL, PAGE_SHARED);
}

static void gem_prime_vunmap(struct drm_gem_object *gem_obj, void *vaddr)
{
	vunmap(vaddr);
}

static int gem_prime_mmap(struct drm_gem_object *gem_obj,
	struct vm_area_struct *vma)
{
	struct xen_gem_object *xen_obj;
	int ret;

	ret = drm_gem_mmap_obj(gem_obj, gem_obj->size, vma);
	if (ret < 0)
		return ret;

	xen_obj = to_xen_gem_obj(gem_obj);
	gem_mmap_obj(xen_obj, vma);
	return 0;
}

static const struct xen_drm_front_gem_ops xen_drm_gem_ops = {
	.free_object_unlocked  = gem_free_object,
	.prime_get_sg_table    = gem_get_sg_table,
	.prime_import_sg_table = gem_import_sg_table,

	.prime_vmap            = gem_prime_vmap,
	.prime_vunmap          = gem_prime_vunmap,
	.prime_mmap            = gem_prime_mmap,

	.dumb_create           = gem_dumb_create,
	.dumb_map_offset       = gem_dumb_map_offset,

	.fb_create_with_funcs  = gem_fb_create_with_funcs,
	.fb_destroy            = gem_fb_destroy,

	.mmap                  = gem_mmap,

	.get_pages             = gem_get_pages,
};

const struct xen_drm_front_gem_ops *xen_drm_front_gem_get_ops(void)
{
	return &xen_drm_gem_ops;
}
