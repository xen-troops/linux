// SPDX-License-Identifier: GPL-2.0 OR MIT

/*
 *  Xen zero-copy helper DRM device
 *
 * Copyright (C) 2016-2018 EPAM Systems Inc.
 *
 * Author: Oleksandr Andrushchenko <oleksandr_andrushchenko@epam.com>
 */

#include <drm/drmP.h>
#include <drm/drm_gem.h>

#include <linux/dma-buf.h>
#include <linux/of_device.h>
#include <linux/platform_device.h>

#include <xen/grant_table.h>
#include <asm/xen/page.h>

#include <drm/xen_zcopy_drm.h>

#include "xen_drm_zcopy_balloon.h"

struct xen_gem_object {
	struct drm_gem_object base;
	u32 dumb_handle;

	int otherend_id;

	u32 num_pages;
	grant_ref_t *grefs;
	/* these are the pages from Xen balloon for allocated Xen GEM object */
	struct page **pages;

	struct xen_drm_zcopy_balloon balloon;

	/* this will be set if we have imported a PRIME buffer */
	struct sg_table *sgt;
	/* map grant handles */
	grant_handle_t *map_handles;
	/*
	 * these are used for synchronous object deletion, e.g.
	 * when user-space wants to know that the grefs are unmapped
	 */
	struct kref refcount;
	int wait_handle;
};

struct xen_wait_obj {
	struct list_head list;
	struct xen_gem_object *xen_obj;
	struct completion completion;
};

struct xen_drv_info {
	struct drm_device *drm_dev;

	/*
	 * For buffers, created from front's grant references, synchronization
	 * between backend and frontend is needed on buffer deletion as front
	 * expects us to unmap these references after XENDISPL_OP_DBUF_DESTROY
	 * response. This means that when calling DRM_XEN_ZCOPY_DUMB_WAIT_FREE
	 * ioctl user-space has to provide some unique handle, so we can tell
	 * the buffer. For that reason we use IDR to allocate a unique value.
	 * The rationale behind implementing wait handle as IDR:
	 * - dumb buffer handle cannot be used as when the PRIME buffer
	 *   gets exported there are at least 2 handles: one is for the
	 *   backend and another one for the importing application,
	 *   so when backend closes its handle and the other application still
	 *   holds the buffer and then there is no way for the backend to tell
	 *   which buffer we want to wait for while calling xen_ioctl_wait_free
	 * - flink cannot be used as well as it is gone when DRM core
	 *   calls .gem_free_object_unlocked
	 * - sync_file can be used, but it seems to be an overhead to use it
	 *   only to get a unique "handle"
	 */
	struct list_head wait_obj_list;
	struct idr idr;
	spinlock_t idr_lock;
	spinlock_t wait_list_lock;
};

static inline struct xen_gem_object *
to_xen_gem_obj(struct drm_gem_object *gem_obj)
{
	return container_of(gem_obj, struct xen_gem_object, base);
}

static struct xen_wait_obj *wait_obj_new(struct xen_drv_info *drv_info,
					 struct xen_gem_object *xen_obj)
{
	struct xen_wait_obj *wait_obj;

	wait_obj = kzalloc(sizeof(*wait_obj), GFP_KERNEL);
	if (!wait_obj)
		return ERR_PTR(-ENOMEM);

	init_completion(&wait_obj->completion);
	wait_obj->xen_obj = xen_obj;

	spin_lock(&drv_info->wait_list_lock);
	list_add(&wait_obj->list, &drv_info->wait_obj_list);
	spin_unlock(&drv_info->wait_list_lock);

	return wait_obj;
}

static void wait_obj_free(struct xen_drv_info *drv_info,
			  struct xen_wait_obj *wait_obj)
{
	struct xen_wait_obj *cur_wait_obj, *q;

	spin_lock(&drv_info->wait_list_lock);
	list_for_each_entry_safe(cur_wait_obj, q, &drv_info->wait_obj_list, list)
		if (cur_wait_obj == wait_obj) {
			list_del(&wait_obj->list);
			kfree(wait_obj);
			break;
		}
	spin_unlock(&drv_info->wait_list_lock);
}

static void wait_obj_check_pending(struct xen_drv_info *drv_info)
{
	/*
	 * It is intended to be called from .last_close when
	 * no pending wait objects should be on the list.
	 * make sure we don't miss a bug if this is not the case.
	 */
	WARN(!list_empty(&drv_info->wait_obj_list),
	     "Removing with pending wait objects!\n");
}

static int wait_obj_wait(struct xen_wait_obj *wait_obj, u32 wait_to_ms)
{
	if (wait_for_completion_timeout(&wait_obj->completion,
			msecs_to_jiffies(wait_to_ms)) <= 0)
		return -ETIMEDOUT;

	return 0;
}

static void wait_obj_signal(struct xen_drv_info *drv_info,
			    struct xen_gem_object *xen_obj)
{
	struct xen_wait_obj *wait_obj, *q;

	spin_lock(&drv_info->wait_list_lock);
	list_for_each_entry_safe(wait_obj, q, &drv_info->wait_obj_list, list)
		if (wait_obj->xen_obj == xen_obj) {
			DRM_DEBUG("Found xen_obj in the wait list, wake\n");
			complete_all(&wait_obj->completion);
		}
	spin_unlock(&drv_info->wait_list_lock);
}

static int wait_obj_handle_new(struct xen_drv_info *drv_info,
			       struct xen_gem_object *xen_obj)
{
	int ret;

	idr_preload(GFP_KERNEL);
	spin_lock(&drv_info->idr_lock);
	ret = idr_alloc(&drv_info->idr, xen_obj, 1, 0, GFP_NOWAIT);
	spin_unlock(&drv_info->idr_lock);
	idr_preload_end();
	return ret;
}

static void wait_obj_handle_free(struct xen_drv_info *drv_info,
				 struct xen_gem_object *xen_obj)
{
	spin_lock(&drv_info->idr_lock);
	idr_remove(&drv_info->idr, xen_obj->wait_handle);
	spin_unlock(&drv_info->idr_lock);
}

static struct xen_gem_object *
get_obj_by_wait_handle(struct xen_drv_info *drv_info, int wait_handle)
{
	struct xen_gem_object *xen_obj;

	spin_lock(&drv_info->idr_lock);
	/* check if xen_obj still exists */
	xen_obj = idr_find(&drv_info->idr, wait_handle);
	if (xen_obj)
		kref_get(&xen_obj->refcount);
	spin_unlock(&drv_info->idr_lock);
	return xen_obj;
}

#define xen_page_to_vaddr(page) \
		((phys_addr_t)pfn_to_kaddr(page_to_xen_pfn(page)))

static int from_refs_unmap(struct device *dev,
			   struct xen_gem_object *xen_obj)
{
	struct gnttab_unmap_grant_ref *unmap_ops;
	int i, ret;

	if (!xen_obj->pages || !xen_obj->map_handles)
		return 0;

	unmap_ops = kcalloc(xen_obj->num_pages, sizeof(*unmap_ops), GFP_KERNEL);
	if (!unmap_ops)
		return -ENOMEM;

	for (i = 0; i < xen_obj->num_pages; i++) {
		phys_addr_t addr;
		unsigned long mfn;

		/*
		 * When unmapping the grant entry for access by host CPUs:
		 * if <host_addr> or <dev_bus_addr> is zero, that
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
		mfn = __pfn_to_mfn(page_to_pfn(xen_obj->pages[i]));
		unmap_ops[i].dev_bus_addr = __pfn_to_phys(mfn);
	}

	ret = gnttab_unmap_refs(unmap_ops, NULL, xen_obj->pages,
				xen_obj->num_pages);
	/*
	 * Even if we didn't unmap properly - continue to rescue whatever
	 * resources we can.
	 */
	if (ret)
		DRM_ERROR("Failed to unmap grant references, ret %d", ret);

	for (i = 0; i < xen_obj->num_pages; i++) {
		if (unlikely(unmap_ops[i].status != GNTST_okay))
			DRM_ERROR("Failed to unmap page %d with ref %d: %d\n",
				  i, xen_obj->grefs[i],
				  unmap_ops[i].status);
	}

	xen_drm_zcopy_ballooned_pages_free(dev, &xen_obj->balloon,
					   xen_obj->num_pages, xen_obj->pages);

	kfree(xen_obj->pages);
	xen_obj->pages = NULL;
	kfree(xen_obj->map_handles);
	xen_obj->map_handles = NULL;
	kfree(unmap_ops);
	kfree(xen_obj->grefs);
	xen_obj->grefs = NULL;
	return ret;
}

static int from_refs_map(struct device *dev, struct xen_gem_object *xen_obj)
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
				       sizeof(*xen_obj->map_handles),
				       GFP_KERNEL);
	if (!xen_obj->map_handles) {
		ret = -ENOMEM;
		goto fail;
	}

	map_ops = kcalloc(xen_obj->num_pages, sizeof(*map_ops), GFP_KERNEL);
	if (!map_ops) {
		ret = -ENOMEM;
		goto fail;
	}

	ret = xen_drm_zcopy_ballooned_pages_alloc(dev, &xen_obj->balloon,
						  xen_obj->num_pages,
						  xen_obj->pages);
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

	/* save handles even if error, so we can unmap */
	for (i = 0; i < xen_obj->num_pages; i++) {
		xen_obj->map_handles[i] = map_ops[i].handle;
		if (unlikely(map_ops[i].status != GNTST_okay))
			DRM_ERROR("Failed to map page %d with ref %d: %d\n",
				  i, xen_obj->grefs[i], map_ops[i].status);
	}

	if (ret) {
		DRM_ERROR("Failed to map grant references, ret %d", ret);
		from_refs_unmap(dev, xen_obj);
		goto fail;
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

static void to_refs_end_foreign_access(struct xen_gem_object *xen_obj)
{
	int i;

	if (xen_obj->grefs)
		for (i = 0; i < xen_obj->num_pages; i++)
			if (xen_obj->grefs[i] != GRANT_INVALID_REF)
				gnttab_end_foreign_access(xen_obj->grefs[i],
							  0, 0UL);

	kfree(xen_obj->grefs);
	xen_obj->grefs = NULL;
	xen_obj->sgt = NULL;
}

static int to_refs_grant_foreign_access(struct xen_gem_object *xen_obj)
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
						xen_obj->otherend_id,
						xen_page_to_gfn(page), 0);
		xen_obj->grefs[j++] = cur_ref;
		num_pages--;
	}

	WARN_ON(num_pages != 0);

	gnttab_free_grant_references(priv_gref_head);
	return 0;
}

static int gem_create_with_handle(struct xen_gem_object *xen_obj,
				  struct drm_file *filp, struct drm_device *dev,
				  int size)
{
	struct drm_gem_object *gem_obj;
	int ret;

	drm_gem_private_object_init(dev, &xen_obj->base, size);
	gem_obj = &xen_obj->base;
	ret = drm_gem_handle_create(filp, gem_obj, &xen_obj->dumb_handle);
	/* drop reference from allocate - handle holds it now. */
	drm_gem_object_put_unlocked(gem_obj);
	return ret;
}

static int gem_create_obj(struct xen_gem_object *xen_obj,
			  struct drm_device *dev, struct drm_file *filp,
			  int size)
{
	struct drm_gem_object *gem_obj;
	int ret;

	ret = gem_create_with_handle(xen_obj, filp, dev, size);
	if (ret < 0)
		goto fail;

	gem_obj = drm_gem_object_lookup(filp, xen_obj->dumb_handle);
	if (!gem_obj) {
		DRM_ERROR("Lookup for handle %d failed\n",
			  xen_obj->dumb_handle);
		ret = -EINVAL;
		goto fail_destroy;
	}

	drm_gem_object_put_unlocked(gem_obj);
	return 0;

fail_destroy:
	drm_gem_dumb_destroy(filp, dev, xen_obj->dumb_handle);
fail:
	DRM_ERROR("Failed to create dumb buffer: %d\n", ret);
	xen_obj->dumb_handle = 0;
	return ret;
}

static int gem_init_obj(struct xen_gem_object *xen_obj,
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

static void obj_release(struct kref *kref)
{
	struct xen_gem_object *xen_obj =
			container_of(kref, struct xen_gem_object, refcount);
	struct xen_drv_info *drv_info = xen_obj->base.dev->dev_private;

	wait_obj_signal(drv_info, xen_obj);
	kfree(xen_obj);
}

static void gem_free_object_unlocked(struct drm_gem_object *gem_obj)
{
	struct xen_gem_object *xen_obj = to_xen_gem_obj(gem_obj);
	struct xen_drv_info *drv_info = gem_obj->dev->dev_private;

	DRM_DEBUG("Freeing dumb with handle %d\n", xen_obj->dumb_handle);
	if (xen_obj->grefs) {
		if (xen_obj->sgt) {
			drm_prime_gem_destroy(&xen_obj->base, xen_obj->sgt);
			to_refs_end_foreign_access(xen_obj);
		} else {
			from_refs_unmap(gem_obj->dev->dev, xen_obj);
		}
	}

	drm_gem_object_release(gem_obj);

	wait_obj_handle_free(drv_info, xen_obj);
	kref_put(&xen_obj->refcount, obj_release);
}

static struct sg_table *gem_prime_get_sg_table(struct drm_gem_object *gem_obj)
{
	struct xen_gem_object *xen_obj = to_xen_gem_obj(gem_obj);
	struct sg_table *sgt = NULL;

	if (unlikely(!xen_obj->pages))
		return NULL;

	sgt = drm_prime_pages_to_sg(xen_obj->pages, xen_obj->num_pages);

	if (unlikely(!sgt))
		DRM_ERROR("Failed to export sgt\n");
	else
		DRM_DEBUG("Exporting %scontiguous buffer nents %d\n",
			  sgt->nents == 1 ? "" : "non-", sgt->nents);
	return sgt;
}

struct drm_gem_object *
gem_prime_import_sg_table(struct drm_device *dev,
			  struct dma_buf_attachment *attach,
			  struct sg_table *sgt)
{
	struct xen_gem_object *xen_obj;
	int ret;

	xen_obj = kzalloc(sizeof(*xen_obj), GFP_KERNEL);
	if (!xen_obj)
		return ERR_PTR(-ENOMEM);

	ret = gem_init_obj(xen_obj, dev, attach->dmabuf->size);
	if (ret < 0)
		goto fail;

	kref_init(&xen_obj->refcount);
	xen_obj->sgt = sgt;
	xen_obj->num_pages = DIV_ROUND_UP(attach->dmabuf->size, PAGE_SIZE);

	DRM_DEBUG("Imported buffer of size %zu with nents %u\n",
		  attach->dmabuf->size, sgt->nents);
	return &xen_obj->base;

fail:
	kfree(xen_obj);
	return ERR_PTR(ret);
}

static int do_ioctl_from_refs(struct drm_device *dev,
			      struct drm_xen_zcopy_dumb_from_refs *req,
			      struct drm_file *filp)
{
	struct xen_drv_info *drv_info = dev->dev_private;
	struct xen_gem_object *xen_obj;
	int ret;

	xen_obj = kzalloc(sizeof(*xen_obj), GFP_KERNEL);
	if (!xen_obj)
		return -ENOMEM;

	kref_init(&xen_obj->refcount);
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

	ret = from_refs_map(dev->dev, xen_obj);
	if (ret < 0)
		goto fail;

	ret = gem_create_obj(xen_obj, dev, filp,
			     round_up(req->dumb.size, PAGE_SIZE));
	if (ret < 0)
		goto fail;

	req->dumb.handle = xen_obj->dumb_handle;

	/*
	 * Get user-visible handle for this GEM object.
	 * the wait object is not allocated at the moment,
	 * but if need be it will be allocated at the time of
	 * DRM_XEN_ZCOPY_DUMB_WAIT_FREE IOCTL
	 */
	ret = wait_obj_handle_new(drv_info, xen_obj);
	if (ret < 0)
		goto fail;

	req->wait_handle = ret;
	xen_obj->wait_handle = ret;
	return 0;

fail:
	kfree(xen_obj->grefs);
	xen_obj->grefs = NULL;
	kfree(xen_obj);
	return ret;
}

static int ioctl_from_refs(struct drm_device *dev,
			   void *data, struct drm_file *filp)
{
	struct drm_xen_zcopy_dumb_from_refs *req =
			(struct drm_xen_zcopy_dumb_from_refs *)data;
	struct drm_mode_create_dumb *args = &req->dumb;
	u32 cpp, stride, size;

	if (!req->num_grefs || !req->grefs)
		return -EINVAL;

	if (!args->width || !args->height || !args->bpp)
		return -EINVAL;

	cpp = DIV_ROUND_UP(args->bpp, 8);
	if (!cpp || cpp > 0xffffffffU / args->width)
		return -EINVAL;

	stride = cpp * args->width;
	if (args->height > 0xffffffffU / stride)
		return -EINVAL;

	size = args->height * stride;
	if (PAGE_ALIGN(size) == 0)
		return -EINVAL;

	args->pitch = DIV_ROUND_UP(args->width * args->bpp, 8);
	args->size = args->pitch * args->height;
	args->handle = 0;
	if (req->num_grefs < DIV_ROUND_UP(args->size, PAGE_SIZE)) {
		DRM_ERROR("Provided %d pages, need %d\n", req->num_grefs,
			  (int)DIV_ROUND_UP(args->size, PAGE_SIZE));
		return -EINVAL;
	}

	return do_ioctl_from_refs(dev, req, filp);
}

static int ioctl_to_refs(struct drm_device *dev,
			 void *data, struct drm_file *filp)
{
	struct xen_gem_object *xen_obj;
	struct drm_gem_object *gem_obj;
	struct drm_xen_zcopy_dumb_to_refs *req =
			(struct drm_xen_zcopy_dumb_to_refs *)data;
	int ret;

	if (!req->num_grefs || !req->grefs)
		return -EINVAL;

	gem_obj = drm_gem_object_lookup(filp, req->handle);
	if (!gem_obj) {
		DRM_ERROR("Lookup for handle %d failed\n", req->handle);
		return -EINVAL;
	}

	drm_gem_object_put_unlocked(gem_obj);
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

	ret = to_refs_grant_foreign_access(xen_obj);
	if (ret < 0)
		goto fail;

	if (copy_to_user(req->grefs, xen_obj->grefs,
			 xen_obj->num_pages * sizeof(grant_ref_t))) {
		ret = -EINVAL;
		goto fail;
	}

	return 0;

fail:
	to_refs_end_foreign_access(xen_obj);
	return ret;
}

static int ioctl_wait_free(struct drm_device *dev,
			   void *data, struct drm_file *file_priv)
{
	struct drm_xen_zcopy_dumb_wait_free *req =
			(struct drm_xen_zcopy_dumb_wait_free *)data;
	struct xen_drv_info *drv_info = dev->dev_private;
	struct xen_gem_object *xen_obj;
	struct xen_wait_obj *wait_obj;
	int wait_handle, ret;

	wait_handle = req->wait_handle;
	/*
	 * try to find the wait handle: if not found means that
	 * either the handle has already been freed or wrong
	 */
	xen_obj = get_obj_by_wait_handle(drv_info, wait_handle);
	if (!xen_obj)
		return -ENOENT;

	/*
	 * xen_obj still exists and is reference count locked by us now, so
	 * prepare to wait: allocate wait object and add it to the wait list,
	 * so we can find it on release
	 */
	wait_obj = wait_obj_new(drv_info, xen_obj);
	/* put our reference and wait for xen_obj release to fire */
	kref_put(&xen_obj->refcount, obj_release);
	ret = PTR_ERR_OR_ZERO(wait_obj);
	if (ret < 0) {
		DRM_ERROR("Failed to setup wait object, ret %d\n", ret);
		return ret;
	}

	ret = wait_obj_wait(wait_obj, req->wait_to_ms);
	wait_obj_free(drv_info, wait_obj);
	return ret;
}

static void lastclose(struct drm_device *dev)
{
	struct xen_drv_info *drv_info = dev->dev_private;

	wait_obj_check_pending(drv_info);
}

static const struct drm_ioctl_desc xen_drm_ioctls[] = {
	DRM_IOCTL_DEF_DRV(XEN_ZCOPY_DUMB_FROM_REFS,
			  ioctl_from_refs,
			  DRM_AUTH | DRM_CONTROL_ALLOW | DRM_UNLOCKED),
	DRM_IOCTL_DEF_DRV(XEN_ZCOPY_DUMB_TO_REFS,
			  ioctl_to_refs,
			  DRM_AUTH | DRM_CONTROL_ALLOW | DRM_UNLOCKED),
	DRM_IOCTL_DEF_DRV(XEN_ZCOPY_DUMB_WAIT_FREE,
			  ioctl_wait_free,
			  DRM_AUTH | DRM_CONTROL_ALLOW | DRM_UNLOCKED),
};

static const struct file_operations xen_drm_fops = {
	.owner          = THIS_MODULE,
	.open           = drm_open,
	.release        = drm_release,
	.unlocked_ioctl = drm_ioctl,
};

static struct drm_driver xen_drm_driver = {
	.driver_features           = DRIVER_GEM | DRIVER_PRIME,
	.lastclose                 = lastclose,
	.prime_handle_to_fd        = drm_gem_prime_handle_to_fd,
	.gem_prime_export          = drm_gem_prime_export,
	.gem_prime_get_sg_table    = gem_prime_get_sg_table,
	.prime_fd_to_handle        = drm_gem_prime_fd_to_handle,
	.gem_prime_import          = drm_gem_prime_import,
	.gem_prime_import_sg_table = gem_prime_import_sg_table,
	.gem_free_object_unlocked  = gem_free_object_unlocked,
	.fops                      = &xen_drm_fops,
	.ioctls                    = xen_drm_ioctls,
	.num_ioctls                = ARRAY_SIZE(xen_drm_ioctls),
	.name                      = XENDRM_ZCOPY_DRIVER_NAME,
	.desc                      = "Xen PV DRM zero copy",
	.date                      = "20180221",
	.major                     = 1,
	.minor                     = 0,
};

static int xen_drm_drv_remove(struct platform_device *pdev)
{
	struct xen_drv_info *drv_info = platform_get_drvdata(pdev);

	if (drv_info && drv_info->drm_dev) {
		drm_dev_unregister(drv_info->drm_dev);
		drm_dev_unref(drv_info->drm_dev);
		idr_destroy(&drv_info->idr);
	}
	return 0;
}

static int xen_drm_drv_probe(struct platform_device *pdev)
{
	struct xen_drv_info *drv_info;
	int ret;

	DRM_INFO("Creating %s\n", xen_drm_driver.desc);
	drv_info = kzalloc(sizeof(*drv_info), GFP_KERNEL);
	if (!drv_info)
		return -ENOMEM;

	idr_init(&drv_info->idr);
	spin_lock_init(&drv_info->idr_lock);
	spin_lock_init(&drv_info->wait_list_lock);
	INIT_LIST_HEAD(&drv_info->wait_obj_list);

	/*
	 * The device is not spawn from a device tree, so arch_setup_dma_ops
	 * is not called, thus leaving the device with dummy DMA ops.
	 * This makes the device return error on PRIME buffer import, which
	 * is not correct: to fix this call of_dma_configure() with a NULL
	 * node to set default DMA ops.
	 */
	of_dma_configure(&pdev->dev, NULL);

	drv_info->drm_dev = drm_dev_alloc(&xen_drm_driver, &pdev->dev);
	if (!drv_info->drm_dev)
		return -ENOMEM;

	ret = drm_dev_register(drv_info->drm_dev, 0);
	if (ret < 0)
		goto fail;

	drv_info->drm_dev->dev_private = drv_info;
	platform_set_drvdata(pdev, drv_info);

	DRM_INFO("Initialized %s %d.%d.%d %s on minor %d\n",
		 xen_drm_driver.name, xen_drm_driver.major,
		 xen_drm_driver.minor, xen_drm_driver.patchlevel,
		 xen_drm_driver.date, drv_info->drm_dev->primary->index);
	return 0;

fail:
	drm_dev_unref(drv_info->drm_dev);
	kfree(drv_info);
	return ret;
}

static struct platform_driver zcopy_platform_drv_info = {
	.probe		= xen_drm_drv_probe,
	.remove		= xen_drm_drv_remove,
	.driver		= {
		.name	= XENDRM_ZCOPY_DRIVER_NAME,
	},
};

struct platform_device_info zcopy_dev_info = {
	.name = XENDRM_ZCOPY_DRIVER_NAME,
	.id = 0,
	.num_res = 0,
	.dma_mask = DMA_BIT_MASK(32),
};

static struct platform_device *xen_pdev;

static int __init xen_drv_init(void)
{
	int ret;

	/* At the moment we only support case with XEN_PAGE_SIZE == PAGE_SIZE */
	if (XEN_PAGE_SIZE != PAGE_SIZE) {
		DRM_ERROR(XENDRM_ZCOPY_DRIVER_NAME ": different kernel and Xen page sizes are not supported: XEN_PAGE_SIZE (%lu) != PAGE_SIZE (%lu)\n",
			  XEN_PAGE_SIZE, PAGE_SIZE);
		return -ENODEV;
	}

	if (!xen_domain())
		return -ENODEV;

	xen_pdev = platform_device_register_full(&zcopy_dev_info);
	if (!xen_pdev) {
		DRM_ERROR("Failed to register " XENDRM_ZCOPY_DRIVER_NAME " device\n");
		return -ENODEV;
	}

	ret = platform_driver_register(&zcopy_platform_drv_info);
	if (ret != 0) {
		DRM_ERROR("Failed to register " XENDRM_ZCOPY_DRIVER_NAME " driver: %d\n",
			  ret);
		platform_device_unregister(xen_pdev);
		return ret;
	}

	return 0;
}

static void __exit xen_drv_fini(void)
{
	if (xen_pdev)
		platform_device_unregister(xen_pdev);
	platform_driver_unregister(&zcopy_platform_drv_info);
}

module_init(xen_drv_init);
module_exit(xen_drv_fini);

MODULE_DESCRIPTION("Xen zero-copy helper DRM device");
MODULE_LICENSE("GPL");
