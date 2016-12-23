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
 * Author: Oleksandr Andrushchenko <Oleksandr_Andrushchenko@epam.com>
 */

#include <drm/drmP.h>
#include <drm/drm_crtc_helper.h>
#include <drm/drm_fb_helper.h>
#include <drm/drm_gem.h>

#include <linux/dma-buf.h>
#include <linux/scatterlist.h>
#include <linux/shmem_fs.h>

#include "xen_drm_drv.h"
#include "xen_drm_gem.h"

struct xen_gem_object {
	struct drm_gem_object base;

	/*
	 * for buffer pages allocated either by front or by the backend,
	 * imported PRIME will never be here
	 */
	struct page **pages;
	size_t num_pages;

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

static struct xen_gem_object *xendrm_gem_create_obj(struct drm_device *dev,
	size_t size)
{
	struct xen_gem_object *xen_obj;
	int ret;

	xen_obj = kzalloc(sizeof(*xen_obj), GFP_KERNEL);
	if (!xen_obj)
		return ERR_PTR(-ENOMEM);
	ret = drm_gem_object_init(dev, &xen_obj->base, size);
	if (ret < 0)
		goto error;
	return xen_obj;

error:
	kfree(xen_obj);
	return ERR_PTR(ret);
}

static struct xen_gem_object *xendrm_gem_create(struct drm_device *dev,
	size_t size)
{
	struct xendrm_device *xendrm_dev = dev->dev_private;
	struct xen_gem_object *xen_obj;

	size = round_up(size, PAGE_SIZE);
	xen_obj = xendrm_gem_create_obj(dev, size);
	if (IS_ERR_OR_NULL(xen_obj))
		return xen_obj;
	xen_obj->num_pages = DIV_ROUND_UP(size, PAGE_SIZE);
	if (xendrm_dev->platdata->be_alloc) {
		/*
		 * backend will allocate space for this buffer, so
		 * we are done: pages array will be set later
		 */
		xen_obj->be_alloc = true;
		return xen_obj;
	}
	/*
	 * need to allocate this buffer now, so we can share its pages
	 * with the backend
	 */
	xen_obj->pages = drm_gem_get_pages(&xen_obj->base);
	if (IS_ERR_OR_NULL(xen_obj->pages)) {
		int ret;

		if (!xen_obj->pages)
			ret = -ENOMEM;
		else
			ret = PTR_ERR(xen_obj->pages);
		xen_obj->pages = NULL;
		DRM_ERROR("Failed to allocate buffer with size %zu\n", size);
		drm_gem_object_unreference_unlocked(&xen_obj->base);
		return ERR_PTR(ret);
	}
	return xen_obj;
}

static struct xen_gem_object *xendrm_gem_create_with_handle(
	struct drm_file *file_priv, struct drm_device *dev,
	size_t size, uint32_t *handle)
{
	struct xen_gem_object *xen_obj;
	struct drm_gem_object *gem_obj;
	int ret;

	xen_obj = xendrm_gem_create(dev, size);
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

int xendrm_gem_dumb_create(struct drm_file *file_priv,
	struct drm_device *dev, struct drm_mode_create_dumb *args)
{
	struct xen_gem_object *xen_obj;

	args->pitch = DIV_ROUND_UP(args->width * args->bpp, 8);
	args->size = args->pitch * args->height;

	xen_obj = xendrm_gem_create_with_handle(file_priv, dev, args->size,
		&args->handle);
	return PTR_ERR_OR_ZERO(xen_obj);
}

void xendrm_gem_free_object(struct drm_gem_object *gem_obj)
{
	struct xen_gem_object *xen_obj = to_xen_gem_obj(gem_obj);

	if (xen_obj->pages) {
		if (!xen_obj->be_alloc)
			drm_gem_put_pages(&xen_obj->base, xen_obj->pages,
				true, false);
	}
	if (xen_obj->base.import_attach)
		drm_prime_gem_destroy(&xen_obj->base, xen_obj->sgt_imported);
	drm_gem_object_release(gem_obj);
	kfree(xen_obj);
}

struct page **xendrm_gem_get_pages(struct drm_gem_object *gem_obj)
{
	struct xen_gem_object *xen_obj = to_xen_gem_obj(gem_obj);

	return xen_obj->pages;
}

struct sg_table *xendrm_gem_get_sg_table(struct drm_gem_object *gem_obj)
{
	struct xen_gem_object *xen_obj = to_xen_gem_obj(gem_obj);

	if (!xen_obj->pages)
		return NULL;
	return drm_prime_pages_to_sg(xen_obj->pages, xen_obj->num_pages);
}

struct drm_gem_object *xendrm_gem_import_sg_table(struct drm_device *dev,
	struct dma_buf_attachment *attach, struct sg_table *sgt)
{
	struct xen_gem_object *xen_obj;

	xen_obj = xendrm_gem_create_obj(dev, attach->dmabuf->size);
	if (IS_ERR(xen_obj))
		return ERR_CAST(xen_obj);
	xen_obj->sgt_imported = sgt;
	/***********************************************************************
	 * TODO: talk to backend and create dumb,
	 * convert sgt to pages, so we can return those on xendrm_gem_get_pages
	 **********************************************************************/
	BUG();
	return &xen_obj->base;
}

void xendrm_gem_set_pages(struct drm_gem_object *gem_obj,
	struct page **pages)
{
	struct xen_gem_object *xen_obj = to_xen_gem_obj(gem_obj);

	xen_obj->pages = pages;
}

static struct xen_fb *xendrm_gem_fb_alloc(struct drm_device *dev,
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

struct drm_framebuffer *xendrm_gem_fb_create_with_funcs(struct drm_device *dev,
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

	xen_fb = xendrm_gem_fb_alloc(dev, mode_cmd, xen_obj, funcs);
	if (IS_ERR(xen_fb)) {
		ret = PTR_ERR(xen_fb);
		goto fail;
	}
	return &xen_fb->fb;

fail:
	drm_gem_object_unreference_unlocked(gem_obj);
	return ERR_PTR(ret);
}

void xendrm_gem_fb_destroy(struct drm_framebuffer *fb)
{
	struct xen_fb *xen_fb = to_xen_fb(fb);

	if (xen_fb->xen_obj)
		drm_gem_object_unreference_unlocked(&xen_fb->xen_obj->base);
	drm_framebuffer_cleanup(fb);
	kfree(xen_fb);
}

int xendrm_gem_dumb_map_offset(struct drm_file *file_priv,
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

static inline void xendrm_gem_mmap_obj(struct xen_gem_object *xen_obj,
	struct vm_area_struct *vma)
{
	/*
	 * Clear the VM_PFNMAP flag that was set by drm_gem_mmap(), and set the
	 * vm_pgoff (used as a fake buffer offset by DRM) to 0 as we want to map
	 * the whole buffer.
	 */
	vma->vm_flags &= ~VM_PFNMAP;
	vma->vm_flags |= VM_MIXEDMAP;
	vma->vm_pgoff = 0;
	/* this is the only way we can map in unprivileged domain */
	vma->vm_page_prot = PAGE_SHARED;
}

int xendrm_gem_mmap(struct file *filp, struct vm_area_struct *vma)
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
	xendrm_gem_mmap_obj(xen_obj, vma);
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

void *xendrm_gem_prime_vmap(struct drm_gem_object *gem_obj)
{
	struct xen_gem_object *xen_obj = to_xen_gem_obj(gem_obj);

	if (!xen_obj->pages)
		return NULL;
	return vmap(xen_obj->pages, xen_obj->num_pages,
		GFP_KERNEL, PAGE_SHARED);
}

void xendrm_gem_prime_vunmap(struct drm_gem_object *gem_obj, void *vaddr)
{
	vunmap(vaddr);
}

int xendrm_gem_prime_mmap(struct drm_gem_object *gem_obj,
	struct vm_area_struct *vma)
{
	struct xen_gem_object *xen_obj;
	int ret;

	ret = drm_gem_mmap_obj(gem_obj, gem_obj->size, vma);
	if (ret < 0)
		return ret;
	xen_obj = to_xen_gem_obj(gem_obj);
	xendrm_gem_mmap_obj(xen_obj, vma);
	return 0;
}
