/*
 *  Xen virtual DRM zero copy device
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
 * Copyright (C) 2017 EPAM Systems Inc.
 *
 * Author: Oleksandr Andrushchenko <Oleksandr_Andrushchenko@epam.com>
 */

#include <drm/drmP.h>
#include <drm/drm_gem.h>

#include <linux/dma-buf.h>
#include <linux/platform_device.h>

#ifdef CONFIG_DRM_XEN_ZCOPY_CMA
#include <asm/xen/hypercall.h>
#include <xen/interface/memory.h>
#include <xen/page.h>
#else
#include <xen/balloon.h>
#endif
#include <xen/grant_table.h>
#include <asm/xen/page.h>

#include <drm/xen_zcopy_drm.h>

#define GRANT_INVALID_REF	0

struct xen_gem_object {
	struct drm_gem_object base;
	uint32_t dumb_handle;

	/* Xen related */
	int otherend_id;
	uint32_t num_pages;
	grant_ref_t *grefs;
	/* these are pages from Xen balloon for allocated Xen GEM object */
	struct page **pages;
#ifdef CONFIG_DRM_XEN_ZCOPY_CMA
	void *vaddr;
	dma_addr_t dev_bus_addr;
#endif
	/* this will be set if we have imported a GEM object */
	struct sg_table *sgt;
	/* map grant handles */
	grant_handle_t *map_handles;
};

static inline struct xen_gem_object *to_xen_gem_obj(
	struct drm_gem_object *gem_obj)
{
	return container_of(gem_obj, struct xen_gem_object, base);
}

#ifdef CONFIG_DRM_XEN_ZCOPY_CMA
static int xen_alloc_ballooned_pages(struct device *dev,
	struct xen_gem_object *xen_obj)
{
	struct page **pages;
	xen_pfn_t *frame_list;
	int num_pages, i;
	size_t size;
	int ret;
	dma_addr_t dev_addr, cpu_addr;
	void *vaddr = NULL;
	struct xen_memory_reservation reservation = {
		.address_bits = 0,
		.extent_order = 0,
		.domid        = DOMID_SELF
	};

	num_pages = xen_obj->num_pages;
	pages = xen_obj->pages;
	size = num_pages * PAGE_SIZE;
	DRM_DEBUG("Ballooning out %d pages, size %zu\n", num_pages, size);
	frame_list = kcalloc(num_pages, sizeof(*frame_list), GFP_KERNEL);
	if (!frame_list)
		return -ENOMEM;
	vaddr = dma_alloc_wc(dev, size, &dev_addr, GFP_KERNEL | __GFP_NOWARN);
	if (!vaddr) {
		DRM_ERROR("Failed to allocate DMA buffer with size %zu\n",
			size);
		ret = -ENOMEM;
		goto fail;
	}
	cpu_addr = dev_addr;
	for (i = 0; i < num_pages; i++) {
		pages[i] = pfn_to_page(__phys_to_pfn(cpu_addr));
		/* XENMEM_populate_physmap requires a PFN based on Xen
		 * granularity.
		 */
		frame_list[i] = page_to_xen_pfn(pages[i]);
		cpu_addr += PAGE_SIZE;
	}
	set_xen_guest_handle(reservation.extent_start, frame_list);
	reservation.nr_extents = num_pages;
	/* rc will hold number of pages processed */
	ret = HYPERVISOR_memory_op(XENMEM_populate_physmap, &reservation);
	if (ret <= 0) {
		DRM_ERROR("Failed to balloon out %d pages (%d), retrying\n",
			num_pages, ret);
		WARN_ON(ret != num_pages);
		ret = -EFAULT;
		goto fail;
	}
	xen_obj->vaddr = vaddr;
	xen_obj->dev_bus_addr = dev_addr;
	kfree(frame_list);
	return 0;

fail:
	if (vaddr)
		dma_free_wc(dev, size, vaddr, dev_addr);
	kfree(frame_list);
	return ret;
}

static void xen_free_ballooned_pages(struct device *dev,
	struct xen_gem_object *xen_obj)
{
	struct page **pages;
	xen_pfn_t *frame_list;
	int num_pages, i;
	int ret;
	size_t size;
	struct xen_memory_reservation reservation = {
		.address_bits = 0,
		.extent_order = 0,
		.domid        = DOMID_SELF
	};

	num_pages = xen_obj->num_pages;
	pages = xen_obj->pages;
	if (!pages)
		return;
	if (!xen_obj->vaddr)
		return;
	frame_list = kcalloc(num_pages, sizeof(*frame_list), GFP_KERNEL);
	if (!frame_list) {
		DRM_ERROR("Failed to balloon in %d pages\n", num_pages);
		return;
	}
	DRM_DEBUG("Ballooning in %d pages\n", num_pages);
	size = num_pages * PAGE_SIZE;
	for (i = 0; i < num_pages; i++) {
		/*
		 * XENMEM_populate_physmap requires a PFN based on Xen
		 * granularity.
		 */
		frame_list[i] = page_to_xen_pfn(pages[i]);
	}
	set_xen_guest_handle(reservation.extent_start, frame_list);
	reservation.nr_extents = num_pages;
	/* rc will hold number of pages processed */
	ret = HYPERVISOR_memory_op(XENMEM_decrease_reservation, &reservation);
	if (ret <= 0) {
		DRM_ERROR("Failed to balloon in %d pages\n", num_pages);
		WARN_ON(ret != num_pages);
	}
	if (xen_obj->vaddr)
		dma_free_wc(dev, size, xen_obj->vaddr, xen_obj->dev_bus_addr);
	xen_obj->vaddr = NULL;
	xen_obj->dev_bus_addr = 0;
	kfree(frame_list);
}
#else
static inline int xen_alloc_ballooned_pages(struct device *dev,
	struct xen_gem_object *xen_obj)
{
	return alloc_xenballooned_pages(xen_obj->num_pages, xen_obj->pages);
}

static inline void xen_free_ballooned_pages(struct device *dev,
	struct xen_gem_object *xen_obj)
{
	free_xenballooned_pages(xen_obj->num_pages, xen_obj->pages);
}
#endif /* CONFIG_DRM_XEN_ZCOPY_CMA */

#define xen_page_to_vaddr(page) \
	((phys_addr_t)pfn_to_kaddr(page_to_xen_pfn(page)))

static int xen_from_refs_map(struct device *dev, struct xen_gem_object *xen_obj)
{
	struct gnttab_map_grant_ref *map_ops = NULL;
	int ret, i;

	if (xen_obj->pages) {
		DRM_ERROR("Mapping already mapped pages?\n");
		return -EINVAL;
	}
	xen_obj->pages = kcalloc(xen_obj->num_pages, sizeof(*xen_obj->pages),
		GFP_KERNEL);
	if (!xen_obj->pages) {
		ret = -ENOMEM;
		goto fail;
	}
	xen_obj->map_handles = kcalloc(xen_obj->num_pages,
		sizeof(*xen_obj->map_handles), GFP_KERNEL);
	if (!xen_obj->map_handles) {
		ret = -ENOMEM;
		goto fail;
	}
	map_ops = kcalloc(xen_obj->num_pages, sizeof(*map_ops), GFP_KERNEL);
	if (!map_ops) {
		ret = -ENOMEM;
		goto fail;
	}
	ret = xen_alloc_ballooned_pages(dev, xen_obj);
	if (ret < 0) {
		DRM_ERROR("Cannot allocate %d ballooned pages: %d\n",
			xen_obj->num_pages, ret);
		goto fail;
	}

	for (i = 0; i < xen_obj->num_pages; i++) {
		phys_addr_t addr;

		addr = xen_page_to_vaddr(xen_obj->pages[i]);
		gnttab_set_map_op(&map_ops[i], addr,
#if defined(CONFIG_X86)
			GNTMAP_host_map | GNTMAP_device_map,
#else
			GNTMAP_host_map,
#endif
			xen_obj->grefs[i], xen_obj->otherend_id);
	}
	ret = gnttab_map_refs(map_ops, NULL, xen_obj->pages,
		xen_obj->num_pages);
	BUG_ON(ret);
	for (i = 0; i < xen_obj->num_pages; i++) {
		xen_obj->map_handles[i] = map_ops[i].handle;
		if (unlikely(map_ops[i].status != GNTST_okay))
			DRM_ERROR("Failed to map page %d with ref %d: %d\n",
				i, xen_obj->grefs[i], map_ops[i].status);
	}
	kfree(map_ops);
	return 0;

fail:
	kfree(xen_obj->pages);
	xen_obj->pages = NULL;
	kfree(xen_obj->map_handles);
	xen_obj->map_handles = NULL;
	kfree(map_ops);
	return ret;

}

static int xen_from_refs_unmap(struct device *dev,
	struct xen_gem_object *xen_obj)
{
	struct gnttab_unmap_grant_ref *unmap_ops;
	int i;

	if (!xen_obj->pages || !xen_obj->map_handles)
		return 0;

	unmap_ops = kcalloc(xen_obj->num_pages, sizeof(*unmap_ops), GFP_KERNEL);
	if (!unmap_ops)
		return -ENOMEM;
	for (i = 0; i < xen_obj->num_pages; i++) {
		phys_addr_t addr;

		/*
		 * Map the grant entry for access by host CPUs.
		 * If <host_addr> or <dev_bus_addr> is zero, that
		 * field is ignored. If non-zero, they must refer to
		 * a device/host mapping that is tracked by <handle>
		 */
		addr = xen_page_to_vaddr(xen_obj->pages[i]);
		gnttab_set_unmap_op(&unmap_ops[i], addr,
#if defined(CONFIG_X86)
			GNTMAP_host_map | GNTMAP_device_map,
#else
			GNTMAP_host_map,
#endif
			xen_obj->map_handles[i]);
		unmap_ops[i].dev_bus_addr = __pfn_to_phys(__pfn_to_mfn(
			page_to_pfn(xen_obj->pages[i])));
	}
	BUG_ON(gnttab_unmap_refs(unmap_ops, NULL, xen_obj->pages,
		xen_obj->num_pages));
	for (i = 0; i < xen_obj->num_pages; i++) {
		if (unlikely(unmap_ops[i].status != GNTST_okay))
			DRM_ERROR("Failed to unmap page %d with ref %d: %d\n",
				i, xen_obj->grefs[i], unmap_ops[i].status);
	}
	xen_free_ballooned_pages(dev, xen_obj);
	kfree(xen_obj->pages);
	xen_obj->pages = NULL;
	kfree(xen_obj->map_handles);
	xen_obj->map_handles = NULL;
	kfree(unmap_ops);
	kfree(xen_obj->grefs);
	xen_obj->grefs = NULL;
	return 0;
}

static void xen_to_refs_release_refs(struct xen_gem_object *xen_obj)
{
	int i;

	if (xen_obj->grefs)
		for (i = 0; i < xen_obj->num_pages; i++)
			if (xen_obj->grefs[i] != GRANT_INVALID_REF)
				gnttab_end_foreign_access(xen_obj->grefs[i],
					0, 0UL);
	kfree(xen_obj->grefs);
	xen_obj->grefs = NULL;
	sg_free_table(xen_obj->sgt);
	xen_obj->sgt = NULL;
}

static int xen_to_refs_grant_refs(struct xen_gem_object *xen_obj)
{
	grant_ref_t priv_gref_head;
	int ret, j, cur_ref, num_pages;
	struct sg_page_iter sg_iter;

	ret = gnttab_alloc_grant_references(xen_obj->num_pages,
		&priv_gref_head);
	if (ret < 0) {
		DRM_ERROR("Cannot allocate grant references\n");
		return ret;
	}
	j = 0;
	num_pages = xen_obj->num_pages;
	for_each_sg_page(xen_obj->sgt->sgl, &sg_iter, xen_obj->sgt->nents, 0) {
		struct page *page;

		page = sg_page_iter_page(&sg_iter);
		cur_ref = gnttab_claim_grant_reference(&priv_gref_head);
		if (cur_ref < 0)
			return cur_ref;
		gnttab_grant_foreign_access_ref(cur_ref,
			xen_obj->otherend_id, xen_page_to_gfn(page), 0);
		xen_obj->grefs[j++] = cur_ref;
		num_pages--;
	}
	WARN_ON(num_pages != 0);
	gnttab_free_grant_references(priv_gref_head);
	return 0;
}

static int xen_gem_create_with_handle(
	struct xen_gem_object *xen_obj, struct drm_file *file_priv,
	struct drm_device *dev, int size)
{
	struct drm_gem_object *gem_obj;
	int ret;

	drm_gem_private_object_init(dev, &xen_obj->base, size);
	gem_obj = &xen_obj->base;
	ret = drm_gem_handle_create(file_priv, gem_obj, &xen_obj->dumb_handle);
	/* drop reference from allocate - handle holds it now. */
	drm_gem_object_unreference_unlocked(gem_obj);
	return ret;
}

static int xen_gem_create_obj(struct xen_gem_object *xen_obj,
	struct drm_device *dev, struct drm_file *file_priv, int size)
{
	struct drm_gem_object *gem_obj;
	int ret;

	ret = xen_gem_create_with_handle(xen_obj, file_priv, dev, size);
	if (ret < 0)
		goto fail;
	gem_obj = drm_gem_object_lookup(file_priv, xen_obj->dumb_handle);
	if (!gem_obj) {
		DRM_ERROR("Lookup for handle %d failed\n",
			xen_obj->dumb_handle);
		ret = -EINVAL;
		goto fail_destroy;
	}
	drm_gem_object_unreference_unlocked(gem_obj);
	return 0;

fail_destroy:
	drm_gem_dumb_destroy(file_priv, dev, xen_obj->dumb_handle);
fail:
	DRM_ERROR("Failed to create dumb buffer: %d\n", ret);
	xen_obj->dumb_handle = 0;
	return ret;
}

static int xen_gem_init_obj(struct xen_gem_object *xen_obj,
	struct drm_device *dev, int size)
{
	struct drm_gem_object *gem_obj = &xen_obj->base;
	int ret;

	ret = drm_gem_object_init(dev, gem_obj, size);
	if (ret < 0)
		return ret;

	ret = drm_gem_create_mmap_offset(gem_obj);
	if (ret < 0) {
		drm_gem_object_release(gem_obj);
		return ret;
	}
	return 0;
}

static void xen_gem_free_object(struct drm_gem_object *gem_obj)
{
	struct xen_gem_object *xen_obj = to_xen_gem_obj(gem_obj);

	if (xen_obj->grefs) {
		if (xen_obj->sgt) {
			if (xen_obj->base.import_attach)
				drm_prime_gem_destroy(&xen_obj->base,
					xen_obj->sgt);
			xen_to_refs_release_refs(xen_obj);
		} else {
			xen_from_refs_unmap(gem_obj->dev->dev, xen_obj);
		}
	}
	drm_gem_object_release(gem_obj);
	kfree(xen_obj);
}

#ifdef CONFIG_DRM_XEN_ZCOPY_WA_SWIOTLB
#define swiotlb_active() swiotlb_nr_tbl()
#else
#define swiotlb_active() 0
#endif

static struct sg_table *xen_gem_prime_get_sg_table(
	struct drm_gem_object *gem_obj)
{
	struct xen_gem_object *xen_obj = to_xen_gem_obj(gem_obj);
	struct sg_table *sgt = NULL;

	if (unlikely(!xen_obj->pages))
		return NULL;
	/* N.B. there will be a single entry in the table if buffer
	 * is contiguous. otherwise CMA drivers will not accept
	 * the buffer
	 */
	if (swiotlb_active()) {
		struct scatterlist *sg;
		int i, ret;

		sgt = kmalloc(sizeof(*sgt), GFP_KERNEL);
		if (!sgt)
			return NULL;

		ret = sg_alloc_table(sgt, xen_obj->num_pages, GFP_KERNEL);
		if (ret < 0) {
			kfree(sgt);
			return ERR_PTR(ret);
		}
		/*
		 * insert individual pages, so we don't make pressure
		 * on SWIOTLB
		 */
		for_each_sg(sgt->sgl, sg, xen_obj->num_pages, i) {
			sg_set_page(sg, xen_obj->pages[i],
				PAGE_SIZE, 0);
		}

	} else {
		sgt = drm_prime_pages_to_sg(xen_obj->pages, xen_obj->num_pages);
	}
	if (unlikely(!sgt))
		DRM_ERROR("Failed to export sgt\n");
	else
		DRM_DEBUG("Exporting %scontiguous buffer nents %d\n",
			sgt->nents == 1 ? "" : "non-", sgt->nents);
	return sgt;
}

struct drm_gem_object *xen_gem_prime_import_sg_table(struct drm_device *dev,
	struct dma_buf_attachment *attach, struct sg_table *sgt)
{
	struct xen_gem_object *xen_obj;
	int ret;

	/* Create a Xen GEM buffer */
	xen_obj = kzalloc(sizeof(*xen_obj), GFP_KERNEL);
	if (!xen_obj)
		return ERR_PTR(-ENOMEM);
	ret = xen_gem_init_obj(xen_obj, dev, attach->dmabuf->size);
	if (ret < 0)
		goto fail;
	xen_obj->sgt = sgt;
	xen_obj->num_pages = DIV_ROUND_UP(attach->dmabuf->size, PAGE_SIZE);
	DRM_DEBUG("Imported buffer of size %zu with nents %u\n",
		attach->dmabuf->size, sgt->nents);
	return &xen_obj->base;

fail:
	kfree(xen_obj);
	return ERR_PTR(ret);
}

static int xen_do_ioctl_from_refs(struct drm_device *dev,
	struct drm_xen_zcopy_dumb_from_refs *req,
	struct drm_file *file_priv)
{
	struct xen_gem_object *xen_obj;
	int ret;

	xen_obj = kzalloc(sizeof(*xen_obj), GFP_KERNEL);
	if (!xen_obj)
		return -ENOMEM;
	xen_obj->num_pages = req->num_grefs;
	xen_obj->otherend_id = req->otherend_id;
	xen_obj->grefs = kcalloc(xen_obj->num_pages, sizeof(grant_ref_t),
		GFP_KERNEL);
	if (!xen_obj->grefs) {
		ret = -ENOMEM;
		goto fail;
	}
	if (copy_from_user(xen_obj->grefs, req->grefs,
			xen_obj->num_pages * sizeof(grant_ref_t))) {
		ret = -EINVAL;
		goto fail;
	}
	ret = xen_from_refs_map(dev->dev, xen_obj);
	if (ret < 0)
		goto fail;
	ret = xen_gem_create_obj(xen_obj, dev, file_priv,
		round_up(req->dumb.size, PAGE_SIZE));
	if (ret < 0)
		goto fail;
	/* return handle */
	req->dumb.handle = xen_obj->dumb_handle;
	return 0;

fail:
	kfree(xen_obj->grefs);
	xen_obj->grefs = NULL;
	return ret;
}

static int xen_ioctl_from_refs(struct drm_device *dev,
	void *data, struct drm_file *file_priv)
{
	struct drm_xen_zcopy_dumb_from_refs *req =
		(struct drm_xen_zcopy_dumb_from_refs *)data;
	struct drm_mode_create_dumb *args = &req->dumb;
	uint32_t cpp, stride, size;

	/*
	 * FIXME: this kind of mapping will need extra care on platforms where
	 * XENFEAT_auto_translated_physmap == 0 and user-space needs
	 * to access these pages (see gntdev driver).
	 * As we only use the pages to feed the real display HW
	 * (no mmap) ignoring XENFEAT_auto_translated_physmap is ok
	 */
	if (!xen_feature(XENFEAT_auto_translated_physmap))
		DRM_DEBUG("Buffer must not be accessed by user-space:"\
			"platform has no XENFEAT_auto_translated_physmap\n");

	if (!req->num_grefs || !req->grefs)
		return -EINVAL;
	if (!args->width || !args->height || !args->bpp)
		return -EINVAL;

	/* overflow checks for 32bit size calculations */
	/* NOTE: DIV_ROUND_UP() can overflow */
	cpp = DIV_ROUND_UP(args->bpp, 8);
	if (!cpp || cpp > 0xffffffffU / args->width)
		return -EINVAL;
	stride = cpp * args->width;
	if (args->height > 0xffffffffU / stride)
		return -EINVAL;

	/* test for wrap-around */
	size = args->height * stride;
	if (PAGE_ALIGN(size) == 0)
		return -EINVAL;

	/* this are the output parameters */
	args->pitch = DIV_ROUND_UP(args->width * args->bpp, 8);
	args->size = args->pitch * args->height;
	args->handle = 0;
	if (req->num_grefs < DIV_ROUND_UP(args->size, PAGE_SIZE)) {
		DRM_ERROR("Provided %d pages, need %d\n", req->num_grefs,
			(int)DIV_ROUND_UP(args->size, PAGE_SIZE));
		return -EINVAL;
	}
	return xen_do_ioctl_from_refs(dev, req, file_priv);
}

static int xen_ioctl_to_refs(struct drm_device *dev,
	void *data, struct drm_file *file_priv)
{
	struct xen_gem_object *xen_obj;
	struct drm_gem_object *gem_obj;
	struct drm_xen_zcopy_dumb_to_refs *req =
		(struct drm_xen_zcopy_dumb_to_refs *)data;
	int ret;

	if (!req->num_grefs || !req->grefs)
		return -EINVAL;

	gem_obj = drm_gem_object_lookup(file_priv, req->handle);
	if (!gem_obj) {
		DRM_ERROR("Lookup for handle %d failed\n", req->handle);
		return -EINVAL;
	}
	drm_gem_object_unreference_unlocked(gem_obj);
	xen_obj = to_xen_gem_obj(gem_obj);

	if (xen_obj->num_pages != req->num_grefs) {
		DRM_ERROR("Provided %d pages, need %d\n", req->num_grefs,
			xen_obj->num_pages);
		return -EINVAL;
	}
	xen_obj->otherend_id = req->otherend_id;
	xen_obj->grefs = kcalloc(xen_obj->num_pages, sizeof(grant_ref_t),
		GFP_KERNEL);
	if (!xen_obj->grefs) {
		ret = -ENOMEM;
		goto fail;
	}
	ret = xen_to_refs_grant_refs(xen_obj);
	if (ret < 0)
		goto fail;
	if (copy_to_user(req->grefs, xen_obj->grefs,
			xen_obj->num_pages * sizeof(grant_ref_t))) {
		ret = -EINVAL;
		goto fail;
	}
	return 0;

fail:
	xen_to_refs_release_refs(xen_obj);
	return ret;
}

static const struct drm_ioctl_desc xen_ioctls[] = {
	DRM_IOCTL_DEF_DRV(XEN_ZCOPY_DUMB_FROM_REFS,
		xen_ioctl_from_refs,
		DRM_AUTH | DRM_CONTROL_ALLOW | DRM_UNLOCKED),
	DRM_IOCTL_DEF_DRV(XEN_ZCOPY_DUMB_TO_REFS,
		xen_ioctl_to_refs,
		DRM_AUTH | DRM_CONTROL_ALLOW | DRM_UNLOCKED),
};

static const struct file_operations xen_fops = {
	.owner          = THIS_MODULE,
	.open           = drm_open,
	.release        = drm_release,
	.unlocked_ioctl = drm_ioctl,
};

static struct drm_driver xen_driver = {
	.driver_features           = DRIVER_GEM | DRIVER_PRIME,
	.prime_handle_to_fd        = drm_gem_prime_handle_to_fd,
	.gem_prime_export          = drm_gem_prime_export,
	.gem_prime_get_sg_table    = xen_gem_prime_get_sg_table,
	.prime_fd_to_handle        = drm_gem_prime_fd_to_handle,
	.gem_prime_import          = drm_gem_prime_import,
	.gem_prime_import_sg_table = xen_gem_prime_import_sg_table,
	.gem_free_object_unlocked  = xen_gem_free_object,
	.fops                      = &xen_fops,
	.ioctls                    = xen_ioctls,
	.num_ioctls                = ARRAY_SIZE(xen_ioctls),
	.name                      = XENDRM_ZCOPY_DRIVER_NAME,
	.desc                      = "Xen PV DRM zero copy",
	.date                      = "20161207",
	.major                     = 1,
	.minor                     = 0,
};

static int xen_remove(struct platform_device *pdev)
{
	struct drm_device *drm_dev = platform_get_drvdata(pdev);

	if (drm_dev) {
		drm_dev_unregister(drm_dev);
		drm_dev_unref(drm_dev);
	}
	return 0;
}

static int xen_probe(struct platform_device *pdev)
{
	struct drm_device *drm_dev;
	int ret;

	DRM_INFO("Creating %s\n", xen_driver.desc);
#ifdef CONFIG_DRM_XEN_ZCOPY_CMA
	arch_setup_dma_ops(&pdev->dev, 0, 0, NULL, false);
#endif
	drm_dev = drm_dev_alloc(&xen_driver, &pdev->dev);
	if (!drm_dev)
		return -ENOMEM;

	ret = drm_dev_register(drm_dev, 0);
	if (ret < 0)
		goto fail;

	platform_set_drvdata(pdev, drm_dev);

	DRM_INFO("Initialized %s %d.%d.%d %s on minor %d\n",
		xen_driver.name, xen_driver.major,
		xen_driver.minor, xen_driver.patchlevel,
		xen_driver.date, drm_dev->primary->index);
	return 0;

fail:
	drm_dev_unref(drm_dev);
	return ret;
}

static struct platform_driver xen_ddrv_info = {
	.probe		= xen_probe,
	.remove		= xen_remove,
	.driver		= {
		.name	= XENDRM_ZCOPY_DRIVER_NAME,
	},
};

struct platform_device_info xen_ddrv_platform_info = {
	.name = XENDRM_ZCOPY_DRIVER_NAME,
	.id = 0,
	.num_res = 0,
	.dma_mask = DMA_BIT_MASK(32),
};

static struct platform_device *xen_pdev;

static int __init xen_init(void)
{
	int ret;

	xen_pdev = platform_device_register_full(&xen_ddrv_platform_info);
	if (!xen_pdev) {
		DRM_ERROR("Failed to register " XENDRM_ZCOPY_DRIVER_NAME \
			" device\n");
		return -ENODEV;
	}
	ret = platform_driver_register(&xen_ddrv_info);
	if (ret != 0) {
		DRM_ERROR("Failed to register " XENDRM_ZCOPY_DRIVER_NAME \
			" driver: %d\n", ret);
		platform_device_unregister(xen_pdev);
		return ret;
	}
	return 0;
}

static void __exit xen_cleanup(void)
{
	if (xen_pdev)
		platform_device_unregister(xen_pdev);
	platform_driver_unregister(&xen_ddrv_info);
}

module_init(xen_init);
module_exit(xen_cleanup);

MODULE_DESCRIPTION("Xen DRM zero copy");
MODULE_LICENSE("GPL");
