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

#include "xen_drm_front_drv.h"

#include <drm/drmP.h>
#include <drm/drm_gem.h>

#include "xen_drm_front.h"
#include "xen_drm_front_cfg.h"
#include "xen_drm_front_gem.h"
#include "xen_drm_front_kms.h"

static void rearm_vblank_timer(struct xen_drm_front_drm_info *drm_info)
{
	mod_timer(&drm_info->vblank_timer,
		jiffies + msecs_to_jiffies(1000 / XENDRM_CRTC_VREFRESH_HZ));
}

static int enable_vblank(struct drm_device *dev, unsigned int pipe)
{
	struct xen_drm_front_drm_info *drm_info = dev->dev_private;

	drm_info->vblank_enabled[pipe] = true;
	return 0;
}

static void disable_vblank(struct drm_device *dev, unsigned int pipe)
{
	struct xen_drm_front_drm_info *drm_info = dev->dev_private;

	drm_info->vblank_enabled[pipe] = false;
}

static void emulate_vblank_interrupt(unsigned long data)
{
	struct xen_drm_front_drm_info *drm_info =
		(struct xen_drm_front_drm_info *)data;
	int i;

	/*
	 * we are not synchronized with enable/disable vblank,
	 * but calling drm_crtc_handle_vblank is safe with this respect,
	 * e.g. checks if vblank is enabled for the crtc given are made in
	 * the DRM core
	 */
	for (i = 0; i < ARRAY_SIZE(drm_info->vblank_enabled); i++)
		if (drm_info->vblank_enabled[i])
			drm_crtc_handle_vblank(&drm_info->crtcs[i].crtc);
	rearm_vblank_timer(drm_info);
}

static int dumb_create(struct drm_file *file_priv,
	struct drm_device *dev, struct drm_mode_create_dumb *args)
{
	struct xen_drm_front_drm_info *drm_info = dev->dev_private;
	struct drm_gem_object *gem_obj;
	int ret;

	ret = drm_info->gem_ops->dumb_create(file_priv, dev, args);
	if (ret < 0)
		goto fail;

	gem_obj = drm_gem_object_lookup(file_priv, args->handle);
	if (!gem_obj) {
		ret = -EINVAL;
		goto fail_destroy;
	}

	drm_gem_object_unreference_unlocked(gem_obj);

	/*
	 * in case of CONFIG_DRM_XEN_FRONTEND_CMA gem_obj is constructed
	 * via DRM CMA helpers and doesn't have ->pages allocated
	 * (xendrm_gem_get_pages will return NULL), but instead can provide
	 * sg table
	 */
	if (drm_info->gem_ops->get_pages(gem_obj))
		ret = drm_info->front_ops->dbuf_create(
				drm_info->front_info,
				xen_drm_front_dbuf_to_cookie(gem_obj),
				args->width, args->height, args->bpp,
				args->size,
				drm_info->gem_ops->get_pages(gem_obj));
	else
		ret = drm_info->front_ops->dbuf_create_from_sgt(
				drm_info->front_info,
				xen_drm_front_dbuf_to_cookie(gem_obj),
				args->width, args->height, args->bpp,
				args->size,
				drm_info->gem_ops->prime_get_sg_table(gem_obj));
	if (ret < 0)
		goto fail_destroy;

	return 0;

fail_destroy:
	drm_gem_dumb_destroy(file_priv, dev, args->handle);
fail:
	DRM_ERROR("Failed to create dumb buffer: %d\n", ret);
	return ret;
}

static void free_object(struct drm_gem_object *gem_obj)
{
	struct xen_drm_front_drm_info *drm_info = gem_obj->dev->dev_private;

	drm_info->front_ops->dbuf_destroy(drm_info->front_info,
		xen_drm_front_dbuf_to_cookie(gem_obj));
	drm_info->gem_ops->free_object_unlocked(gem_obj);
}

static void on_page_flip(struct platform_device *pdev,
	int conn_idx, uint64_t fb_cookie)
{
	struct xen_drm_front_drm_info *drm_info = platform_get_drvdata(pdev);

	if (unlikely(conn_idx >= drm_info->num_crtcs))
		return;

	xen_drm_front_crtc_on_page_flip_done(&drm_info->crtcs[conn_idx], fb_cookie);
}

static void lastclose(struct drm_device *dev)
{
	struct xen_drm_front_drm_info *drm_info = dev->dev_private;

	drm_info->front_ops->drm_last_close(drm_info->front_info);
}

static int gem_mmap(struct file *filp, struct vm_area_struct *vma)
{
	struct drm_file *file_priv = filp->private_data;
	struct drm_device *dev = file_priv->minor->dev;
	struct xen_drm_front_drm_info *drm_info = dev->dev_private;

	return drm_info->gem_ops->mmap(filp, vma);
}

static struct sg_table *prime_get_sg_table(struct drm_gem_object *obj)
{
	struct xen_drm_front_drm_info *drm_info;

	drm_info = obj->dev->dev_private;
	return drm_info->gem_ops->prime_get_sg_table(obj);
}

static struct drm_gem_object *prime_import_sg_table(struct drm_device *dev,
	struct dma_buf_attachment *attach, struct sg_table *sgt)
{
	struct xen_drm_front_drm_info *drm_info;

	drm_info = dev->dev_private;
	return drm_info->gem_ops->prime_import_sg_table(dev, attach, sgt);
}

static void *prime_vmap(struct drm_gem_object *obj)
{
	struct xen_drm_front_drm_info *drm_info;

	drm_info = obj->dev->dev_private;
	return drm_info->gem_ops->prime_vmap(obj);
}

static void prime_vunmap(struct drm_gem_object *obj, void *vaddr)
{
	struct xen_drm_front_drm_info *drm_info;

	drm_info = obj->dev->dev_private;
	return drm_info->gem_ops->prime_vunmap(obj, vaddr);
}

static int prime_mmap(struct drm_gem_object *obj,
	struct vm_area_struct *vma)
{
	struct xen_drm_front_drm_info *drm_info;

	drm_info = obj->dev->dev_private;
	return drm_info->gem_ops->prime_mmap(obj, vma);
}

static int dumb_map_offset(struct drm_file *file_priv,
	struct drm_device *dev, uint32_t handle, uint64_t *offset)
{
	struct xen_drm_front_drm_info *drm_info;

	drm_info = dev->dev_private;
	return drm_info->gem_ops->dumb_map_offset(file_priv, dev,
		handle, offset);
}

static const struct file_operations xendrm_fops = {
	.owner          = THIS_MODULE,
	.open           = drm_open,
	.release        = drm_release,
	.unlocked_ioctl = drm_ioctl,
#ifdef CONFIG_COMPAT
	.compat_ioctl   = drm_compat_ioctl,
#endif
	.poll           = drm_poll,
	.read           = drm_read,
	.llseek         = no_llseek,
	.mmap           = gem_mmap,
};

static const struct vm_operations_struct xendrm_vm_ops = {
	.open           = drm_gem_vm_open,
	.close          = drm_gem_vm_close,
};

struct drm_driver xendrm_driver = {
	.driver_features           = DRIVER_GEM | DRIVER_MODESET |
				     DRIVER_PRIME | DRIVER_ATOMIC,
	.lastclose                 = lastclose,
	.get_vblank_counter        = drm_vblank_no_hw_counter,
	.enable_vblank             = enable_vblank,
	.disable_vblank            = disable_vblank,
	.get_vblank_counter        = drm_vblank_no_hw_counter,
	.gem_free_object_unlocked  = free_object,
	.gem_vm_ops                = &xendrm_vm_ops,
	.prime_handle_to_fd        = drm_gem_prime_handle_to_fd,
	.prime_fd_to_handle        = drm_gem_prime_fd_to_handle,
	.gem_prime_import          = drm_gem_prime_import,
	.gem_prime_export          = drm_gem_prime_export,
	.gem_prime_get_sg_table    = prime_get_sg_table,
	.gem_prime_import_sg_table = prime_import_sg_table,
	.gem_prime_vmap            = prime_vmap,
	.gem_prime_vunmap          = prime_vunmap,
	.gem_prime_mmap            = prime_mmap,
	.dumb_create               = dumb_create,
	.dumb_map_offset           = dumb_map_offset,
	.dumb_destroy              = drm_gem_dumb_destroy,
	.fops                      = &xendrm_fops,
	.name                      = "xendrm-du",
	.desc                      = "Xen PV DRM Display Unit",
	.date                      = "20161109",
	.major                     = 1,
	.minor                     = 0,
};

int xen_drm_front_drv_probe(struct platform_device *pdev,
	struct xen_drm_front_ops *xendrm_front_funcs)
{
	struct xen_drm_front_cfg_plat_data *platdata;
	struct xen_drm_front_drm_info *drm_info;
	struct drm_device *ddev;
	int ret;

	platdata = dev_get_platdata(&pdev->dev);
	DRM_INFO("Creating %s\n", xendrm_driver.desc);
	/* Allocate and initialize the DRM and xendrm device structures. */
	drm_info = devm_kzalloc(&pdev->dev, sizeof(*drm_info), GFP_KERNEL);
	if (!drm_info)
		return -ENOMEM;

	drm_info->front_ops = xendrm_front_funcs;
	drm_info->front_ops->on_page_flip = on_page_flip;
	drm_info->gem_ops = xen_drm_front_gem_get_ops();
	drm_info->front_info = platdata->front_info;

	ddev = drm_dev_alloc(&xendrm_driver, &pdev->dev);
	if (!ddev)
		return -ENOMEM;

	drm_info->drm_dev = ddev;

	/* assume 1 CRTC and 1 Encoder per each connector */
	drm_info->num_crtcs = platdata->num_connectors;
	drm_info->plat_data = platdata;
	ddev->dev_private = drm_info;
	platform_set_drvdata(pdev, drm_info);

	ret = drm_vblank_init(ddev, drm_info->num_crtcs);
	if (ret < 0)
		goto fail_vblank;

	/* DRM/KMS objects */
	ret = xen_drm_front_kms_init(drm_info);
	if (ret < 0) {
		if (ret != -EPROBE_DEFER)
			dev_err(&pdev->dev,
				"failed to initialize DRM/KMS (%d)\n", ret);
		goto fail_modeset;
	}

	setup_timer(&drm_info->vblank_timer, emulate_vblank_interrupt,
		(unsigned long)drm_info);
	rearm_vblank_timer(drm_info);

	ddev->irq_enabled = 1;

	/*
	 * register the DRM device with the core and the connectors,
	 * encoders, planes with sysfs
	 */
	ret = drm_dev_register(ddev, 0);
	if (ret)
		goto fail_register;

	DRM_INFO("Initialized %s %d.%d.%d %s on minor %d\n",
		xendrm_driver.name, xendrm_driver.major,
		xendrm_driver.minor, xendrm_driver.patchlevel,
		xendrm_driver.date, ddev->primary->index);

	return 0;

fail_register:
	del_timer_sync(&drm_info->vblank_timer);
	drm_dev_unregister(ddev);
fail_modeset:
	drm_mode_config_cleanup(ddev);
fail_vblank:
	drm_vblank_cleanup(ddev);
	return ret;
}

int xen_drm_front_drv_remove(struct platform_device *pdev)
{
	struct xen_drm_front_drm_info *drm_info = platform_get_drvdata(pdev);
	struct drm_device *drm_dev = drm_info->drm_dev;

	del_timer_sync(&drm_info->vblank_timer);
	drm_dev_unregister(drm_dev);
	drm_vblank_cleanup(drm_dev);
	drm_mode_config_cleanup(drm_dev);
	drm_dev_unref(drm_dev);
	return 0;
}

bool xen_drm_front_drv_is_used(struct platform_device *pdev)
{
	struct xen_drm_front_drm_info *drm_info = platform_get_drvdata(pdev);
	struct drm_device *drm_dev;

	if (!drm_info)
		return false;
	drm_dev = drm_info->drm_dev;
	if (!drm_dev)
		return false;

	/* FIXME: the code below must be protected by drm_global_mutex,
	 * but it is not accessible to us and anyways there is a
	 * race condition.
	 */
	return drm_dev->open_count != 0;
}
