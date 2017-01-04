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
 * Copyright (C) 2016 EPAM Systems Inc.
 *
 * Author: Oleksandr Andrushchenko <Oleksandr_Andrushchenko@epam.com>
 */

#include <drm/drmP.h>
#include <drm/drm_gem.h>

#include <linux/dma-buf.h>
#include <linux/platform_device.h>

#ifdef CONFIG_XEN_HAVE_PVMMU
#include <xen/balloon.h>
#else
#include <xen/interface/memory.h>
#include <asm/xen/hypercall.h>
#include <xen/page.h>
#endif
#include <xen/grant_table.h>

#include <drm/xen_zcopy_drm.h>

struct xen_info {
	struct drm_device *drm_dev;
};

struct xen_gem_object {
	struct drm_gem_object base;
	uint32_t dumb_handle;

	/* Xen related */
	int otherend_id;
	uint32_t num_pages;
	grant_ref_t *grefs;
	/* these are pages from Xen balloon */
	struct page **pages;
	/* and their map grant handles and addresses */
	struct map_info {
		grant_handle_t handle;
		uint64_t dev_bus_addr;
	} *map_info;
#ifndef CONFIG_XEN_HAVE_PVMMU
	dma_addr_t paddr;
	void *vaddr;
#endif
};

static inline struct xen_gem_object *
to_xen_gem_obj(struct drm_gem_object *gem_obj)
{
	return container_of(gem_obj, struct xen_gem_object, base);
}

#ifdef CONFIG_XEN_HAVE_PVMMU
/* FIXME: ARM platform has no concept of PVMMU,
 * so, most probably, drivers for ARM will require CMA
 */
static int xen_alloc_ballooned_pages(struct xen_gem_object *xen_obj)
{
	return alloc_xenballooned_pages(xen_obj->num_pages, xen_obj->pages);
}

static void xen_free_ballooned_pages(struct xen_gem_object *xen_obj)
{
	free_xenballooned_pages(xen_obj->num_pages, xen_obj->pages);
}
#else
static int xen_alloc_ballooned_pages(struct xen_gem_object *xen_obj)
{
	struct page **pages;
	xen_pfn_t *frame_list;
	int num_pages, i, tries_left;
	int ret;
	dma_addr_t paddr, cpu_addr;
	void *vaddr = NULL;
	size_t size;
	struct xen_memory_reservation reservation = {
		.address_bits = 0,
		.extent_order = 0,
		.domid        = DOMID_SELF
	};

	num_pages = xen_obj->num_pages;
	pages = xen_obj->pages;
	DRM_DEBUG("Ballooning out %d pages\n", num_pages);
	frame_list = kcalloc(num_pages, sizeof(*frame_list), GFP_KERNEL);
	if (!frame_list)
		return -ENOMEM;
	size = num_pages * PAGE_SIZE;
	vaddr = dma_alloc_wc(xen_obj->base.dev->dev, size, &paddr,
		GFP_KERNEL | __GFP_NOWARN);
	if (!vaddr) {
		DRM_ERROR("Failed to allocate DMA buffer with size %zu\n",
			size);
		ret = -ENOMEM;
		goto fail;
	}
	cpu_addr = paddr;
	for (i = 0; i < num_pages; i++) {
		pages[i] = virt_to_page(cpu_addr);
		/* XENMEM_populate_physmap requires a PFN based on Xen
		 * granularity.
		 */
		frame_list[i] = page_to_xen_pfn(pages[i]);
		cpu_addr += PAGE_SIZE;
	}
	tries_left = 3;
again:
	set_xen_guest_handle(reservation.extent_start, frame_list);
	reservation.nr_extents = num_pages;
	/* rc will hold number of pages processed */
	ret = HYPERVISOR_memory_op(XENMEM_populate_physmap, &reservation);
	if (ret <= 0) {
		DRM_ERROR("Failed to balloon out %d pages, retrying\n",
			num_pages);
		if (--tries_left)
			goto again;
		WARN_ON(ret != num_pages);
		ret = -EFAULT;
		goto fail;
	}
	xen_obj->vaddr = vaddr;
	xen_obj->paddr = paddr;
	kfree(frame_list);
	return 0;

fail:
	if (vaddr)
		dma_free_wc(xen_obj->base.dev->dev, size, vaddr, paddr);
	kfree(frame_list);
	return ret;
}

static void xen_free_ballooned_pages(struct xen_gem_object *xen_obj)
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
		/* XENMEM_populate_physmap requires a PFN based on Xen
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
		dma_free_wc(xen_obj->base.dev->dev, size,
			xen_obj->vaddr, xen_obj->paddr);
	xen_obj->vaddr = NULL;
	xen_obj->paddr = 0;
	kfree(frame_list);
}
#endif

#define xen_page_to_vaddr(page) \
	((phys_addr_t)pfn_to_kaddr(page_to_xen_pfn(page)))

static int xen_do_map(struct xen_gem_object *xen_obj)
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
	xen_obj->map_info = kcalloc(xen_obj->num_pages,
		sizeof(*xen_obj->map_info), GFP_KERNEL);
	if (!xen_obj->map_info) {
		ret = -ENOMEM;
		goto fail;
	}
	map_ops = kcalloc(xen_obj->num_pages, sizeof(*map_ops), GFP_KERNEL);
	if (!map_ops) {
		ret = -ENOMEM;
		goto fail;
	}
	ret = xen_alloc_ballooned_pages(xen_obj);
	if (ret < 0) {
		DRM_ERROR("Cannot allocate %d ballooned pages: %d\n",
			xen_obj->num_pages, ret);
		goto fail;
	}
	for (i = 0; i < xen_obj->num_pages; i++) {
		phys_addr_t addr;

		/* Map the grant entry for access by I/O devices. */
		/* Map the grant entry for access by host CPUs. */
		addr = xen_page_to_vaddr(xen_obj->pages[i]);
		gnttab_set_map_op(&map_ops[i], addr,
			GNTMAP_host_map | GNTMAP_device_map,
			xen_obj->grefs[i], xen_obj->otherend_id);
	}
	ret = gnttab_map_refs(map_ops, NULL, xen_obj->pages,
		xen_obj->num_pages);
	BUG_ON(ret);
	for (i = 0; i < xen_obj->num_pages; i++) {
		xen_obj->map_info[i].handle = map_ops[i].handle;
		xen_obj->map_info[i].dev_bus_addr = map_ops[i].dev_bus_addr;
		if (unlikely(map_ops[i].status != GNTST_okay))
			DRM_ERROR("Failed to map page %d with ref %d: %d\n",
				i, xen_obj->grefs[i], map_ops[i].status);
	}
	kfree(map_ops);
	return 0;

fail:
	kfree(xen_obj->pages);
	xen_obj->pages = NULL;
	kfree(xen_obj->map_info);
	xen_obj->map_info = NULL;
	kfree(map_ops);
	return ret;

}

static int xen_do_unmap(struct xen_gem_object *xen_obj)
{
	struct gnttab_unmap_grant_ref *unmap_ops;
	int i;

	if (!xen_obj->pages || !xen_obj->map_info)
		return 0;

	unmap_ops = kcalloc(xen_obj->num_pages, sizeof(*unmap_ops), GFP_KERNEL);
	if (!unmap_ops)
		return -ENOMEM;
	for (i = 0; i < xen_obj->num_pages; i++) {
		phys_addr_t addr;

		/* Map the grant entry for access by I/O devices.
		 * Map the grant entry for access by host CPUs.
		 * If <host_addr> or <dev_bus_addr> is zero, that
		 * field is ignored. If non-zero, they must refer to
		 * a device/host mapping that is tracked by <handle>
		 */
		addr = xen_page_to_vaddr(xen_obj->pages[i]);
		gnttab_set_unmap_op(&unmap_ops[i], addr,
			GNTMAP_host_map | GNTMAP_device_map,
			xen_obj->map_info[i].handle);
		unmap_ops[i].dev_bus_addr = xen_obj->map_info[i].dev_bus_addr;
	}
	BUG_ON(gnttab_unmap_refs(unmap_ops, NULL, xen_obj->pages,
		xen_obj->num_pages));
	for (i = 0; i < xen_obj->num_pages; i++) {
		if (unlikely(unmap_ops[i].status != GNTST_okay))
			DRM_ERROR("Failed to unmap page %d with ref %d: %d\n",
				i, xen_obj->grefs[i], unmap_ops[i].status);
	}
	xen_free_ballooned_pages(xen_obj);
	kfree(xen_obj->pages);
	xen_obj->pages = NULL;
	kfree(xen_obj->map_info);
	xen_obj->map_info = NULL;
	kfree(unmap_ops);
	return 0;
}

static void xen_gem_close_object(struct drm_gem_object *gem_obj,
	struct drm_file *file_priv)
{
	struct xen_gem_object *xen_obj = to_xen_gem_obj(gem_obj);

	/* from drm_prime.c:
	 * On the export the dma_buf holds a reference to the exporting GEM
	 * object. It takes this reference in handle_to_fd_ioctl, when it
	 * first calls .prime_export and stores the exporting GEM object in
	 * the dma_buf priv. This reference is released when the dma_buf
	 * object goes away in the driver .release function.
	 * FIXME: this is too late, as we have to unmap now, so front
	 * can release granted references
	 * FIXME: if handle_count is 1 then dma_buf is not in use anymore
	 * waiting for the driver's .release. Otherwise it is a bug in
	 * the backend, e.g. the handle was not closed in the driver which
	 * imported our dma_buf
	 */
	mutex_lock(&gem_obj->dev->object_name_lock);
	WARN_ON(gem_obj->handle_count != 1);
	if (gem_obj->handle_count == 1) {
		xen_do_unmap(xen_obj);
		kfree(xen_obj->grefs);
		xen_obj->grefs = NULL;
	}
	mutex_unlock(&gem_obj->dev->object_name_lock);
}

static void xen_gem_free_object(struct drm_gem_object *gem_obj)
{
	struct xen_gem_object *xen_obj = to_xen_gem_obj(gem_obj);

	/* FIXME: this gets called on driver .release because of
	 * .handle_to_fd_ioctl + .prime_export
	 */
	if (unlikely(xen_obj->grefs)) {
		/* leftovers due to backend crash? */
		xen_do_unmap(xen_obj);
		kfree(xen_obj->grefs);
	}
	drm_gem_object_release(gem_obj);
	kfree(xen_obj);
}

#ifdef CONFIG_DRM_XENZCOPY_WA_SWIOTLB
#define swiotlb_active() swiotlb_nr_tbl()
#else
#define swiotlb_active() 0
#endif

static struct sg_table *xen_gem_prime_get_sg_table(
	struct drm_gem_object *gem_obj)
{
	struct xen_gem_object *xen_obj = to_xen_gem_obj(gem_obj);
	struct sg_table *sgt;

	if (unlikely(!xen_obj->pages))
		return NULL;
	/* N.B. there will be a single entry in the table if buffer
	 * is contiguous. otherwise CMA drivers will not accept
	 * the buffer
	 */
	if (swiotlb_active()) {
		struct scatterlist *sg;
		int i;

		sgt = kmalloc(sizeof(*sgt), GFP_KERNEL);
		if (!sgt)
			return NULL;

		if (sg_alloc_table(sgt, xen_obj->num_pages, GFP_KERNEL) < 0) {
			kfree(sgt);
			return NULL;
		}
		for_each_sg(sgt->sgl, sg, xen_obj->num_pages, i)
			sg_set_page(sg, xen_obj->pages[i], PAGE_SIZE, 0);

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

static int xen_zcopy_create_dumb_obj(struct xen_gem_object *xen_obj,
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

static int xen_do_dumb_create(struct drm_device *dev,
	struct drm_xen_zcopy_create_dumb *req,
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
	ret = xen_do_map(xen_obj);
	if (ret < 0)
		goto fail;
	ret = xen_zcopy_create_dumb_obj(xen_obj, dev, file_priv,
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

static int xen_create_dumb_ioctl(struct drm_device *dev,
	void *data, struct drm_file *file_priv)
{
	struct drm_xen_zcopy_create_dumb *req =
		(struct drm_xen_zcopy_create_dumb *)data;
	struct drm_mode_create_dumb *args = &req->dumb;
	uint32_t cpp, stride, size;

	if (!req->num_grefs || !req->grefs || !req->otherend_id)
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
	return xen_do_dumb_create(dev, req, file_priv);
}

static const struct drm_ioctl_desc xen_ioctls[] = {
	DRM_IOCTL_DEF_DRV(XEN_ZCOPY_CREATE_DUMB, xen_create_dumb_ioctl,
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
	.gem_close_object          = xen_gem_close_object,
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
	struct xen_info *info = platform_get_drvdata(pdev);
	struct drm_device *drm_dev = info->drm_dev;

	if (drm_dev) {
		drm_dev_unregister(drm_dev);
		drm_dev_unref(drm_dev);
	}
	return 0;
}

static struct xen_info xen_info;

static int xen_probe(struct platform_device *pdev)
{
	struct xen_info *info;
	struct drm_device *drm_dev;
	int ret;

	DRM_INFO("Creating %s\n", xen_driver.desc);
	info = &xen_info;
	drm_dev = drm_dev_alloc(&xen_driver, &pdev->dev);
	if (!drm_dev)
		return -ENOMEM;

	drm_dev->dev_private = info;
	platform_set_drvdata(pdev, info);

	ret = drm_dev_register(drm_dev, 0);
	if (ret)
		goto fail;
	info->drm_dev = drm_dev;

	DRM_INFO("Initialized %s %d.%d.%d %s on minor %d\n",
		xen_driver.name, xen_driver.major,
		xen_driver.minor, xen_driver.patchlevel,
		xen_driver.date, drm_dev->primary->index);
	return 0;

fail:
	xen_remove(pdev);
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
