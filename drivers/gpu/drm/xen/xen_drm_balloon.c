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

#if defined(CONFIG_DRM_XEN_ZCOPY_CMA)
#include <asm/xen/hypercall.h>
#include <xen/interface/memory.h>
#include <xen/page.h>
#else
#include <xen/balloon.h>
#endif

#include "xen_drm_balloon.h"

#if defined(CONFIG_DRM_XEN_ZCOPY_CMA)
int xen_drm_ballooned_pages_alloc(struct device *dev,
	struct xen_drm_balloon *obj, int num_pages, struct page **pages)
{
	xen_pfn_t *frame_list;
	size_t size;
	int i, ret;
	dma_addr_t dev_addr, cpu_addr;
	void *vaddr = NULL;
	struct xen_memory_reservation reservation = {
		.address_bits = 0,
		.extent_order = 0,
		.domid        = DOMID_SELF
	};

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
	ret = HYPERVISOR_memory_op(XENMEM_decrease_reservation, &reservation);
	if (ret <= 0) {
		DRM_ERROR("Failed to balloon out %d pages (%d), retrying\n",
			num_pages, ret);
		WARN_ON(ret != num_pages);
		ret = -EFAULT;
		goto fail;
	}

	obj->vaddr = vaddr;
	obj->dev_bus_addr = dev_addr;
	kfree(frame_list);
	return 0;

fail:
	if (vaddr)
		dma_free_wc(dev, size, vaddr, dev_addr);
	kfree(frame_list);
	return ret;
}

void xen_drm_ballooned_pages_free(struct device *dev,
	struct xen_drm_balloon *obj, int num_pages, struct page **pages)
{
	xen_pfn_t *frame_list;
	int i, ret;
	size_t size;
	struct xen_memory_reservation reservation = {
		.address_bits = 0,
		.extent_order = 0,
		.domid        = DOMID_SELF
	};

	if (!pages)
		return;

	if (!obj->vaddr)
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
	ret = HYPERVISOR_memory_op(XENMEM_populate_physmap, &reservation);
	if (ret <= 0) {
		DRM_ERROR("Failed to balloon in %d pages\n", num_pages);
		WARN_ON(ret != num_pages);
	}

	if (obj->vaddr)
		dma_free_wc(dev, size, obj->vaddr, obj->dev_bus_addr);

	obj->vaddr = NULL;
	obj->dev_bus_addr = 0;
	kfree(frame_list);
}
#else
int xen_drm_ballooned_pages_alloc(struct device *dev,
	struct xen_drm_balloon *obj, int num_pages, struct page **pages)
{
	return alloc_xenballooned_pages(num_pages, pages);
}

void xen_drm_ballooned_pages_free(struct device *dev,
	struct xen_drm_balloon *obj, int num_pages, struct page **pages)
{
	free_xenballooned_pages(num_pages, pages);
}
#endif /* defined(CONFIG_DRM_XEN_ZCOPY_CMA) */
