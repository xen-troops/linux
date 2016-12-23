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
 * Copyright (C) 2016 EPAM Systems Inc.
 *
 * Author: Oleksandr Andrushchenko <Oleksandr_Andrushchenko@epam.com>
 */

#include <drm/drmP.h>
#include <drm/drm_gem.h>

#include <linux/nmi.h>

#include "xen_drm_drv.h"
#include "xen_drm_front.h"
#include "xen_drm_gem.h"
#include "xen_drm_kms.h"

int xendrm_enable_vblank(struct drm_device *dev, unsigned int pipe)
{
	struct xendrm_device *xendrm_dev = dev->dev_private;

	if (unlikely(pipe >= xendrm_dev->num_crtcs))
		return -EINVAL;
	if (atomic_read(&xendrm_dev->vblank_enabled[pipe]) == 0)
		xendrm_timer_start(&xendrm_dev->vblank_timer);
	atomic_set(&xendrm_dev->vblank_enabled[pipe], 1);
	return 0;
}

void xendrm_disable_vblank(struct drm_device *dev, unsigned int pipe)
{
	struct xendrm_device *xendrm_dev = dev->dev_private;

	if (unlikely(pipe >= xendrm_dev->num_crtcs))
		return;
	if (atomic_read(&xendrm_dev->vblank_enabled[pipe]))
		xendrm_timer_stop(&xendrm_dev->vblank_timer, false);
	atomic_set(&xendrm_dev->vblank_enabled[pipe], 0);
}

static int xendrm_dumb_create(struct drm_file *file_priv,
	struct drm_device *dev, struct drm_mode_create_dumb *args)
{
	struct xendrm_device *xendrm_dev = dev->dev_private;
	struct drm_gem_object *gem_obj;
	struct page **pages = NULL;
	struct sg_table *sgt = NULL;
	bool be_alloc;
	int ret;

	ret = xendrm_gem_dumb_create(file_priv, dev, args);
	if (ret < 0)
		goto fail;
	gem_obj = drm_gem_object_lookup(file_priv, args->handle);
	if (!gem_obj) {
		ret = -EINVAL;
		goto fail_destroy;
	}
	drm_gem_object_unreference_unlocked(gem_obj);

	be_alloc = xendrm_dev->platdata->be_alloc;
	/*
	 * if buffers are allocated on backend's side, then
	 * pass NULL for pages and have backend to provide them
	 */
	if (!be_alloc) {
		pages = xendrm_gem_get_pages(gem_obj);
		if (!pages)
			sgt = xendrm_gem_get_sg_table(gem_obj);
	}
	pages = xendrm_dev->front_ops->dbuf_create(
			xendrm_dev->xdrv_info, xendrm_dumb_to_cookie(gem_obj),
			args->width, args->height, args->bpp, args->size,
			pages, sgt);
	if (IS_ERR_OR_NULL(pages)) {
		ret = PTR_ERR(pages);
		goto fail_destroy;
	}
	if (be_alloc)
		xendrm_gem_set_pages(gem_obj, pages);
	return 0;

fail_destroy:
	drm_gem_dumb_destroy(file_priv, dev, args->handle);
fail:
	DRM_ERROR("Failed to create dumb buffer: %d\n", ret);
	return ret;
}

static void xendrm_free_object(struct drm_gem_object *gem_obj)
{
	struct xendrm_device *xendrm_dev = gem_obj->dev->dev_private;

	xendrm_dev->front_ops->dbuf_destroy(xendrm_dev->xdrv_info,
		xendrm_dumb_to_cookie(gem_obj));
	xendrm_gem_free_object(gem_obj);
}

static void xendrm_on_page_flip(struct platform_device *pdev,
	int conn_idx, uint64_t fb_cookie)
{
	struct xendrm_device *xendrm_dev = platform_get_drvdata(pdev);

	if (unlikely(conn_idx >= xendrm_dev->num_crtcs))
		return;
	xendrm_crtc_on_page_flip_done(&xendrm_dev->crtcs[conn_idx], fb_cookie);
}

static void xendrm_handle_vblank(unsigned long data)
{
	struct xendrm_device *xendrm_dev = (struct xendrm_device *)data;
	int i;

	for (i = 0; i < ARRAY_SIZE(xendrm_dev->crtcs); i++) {
		if (atomic_read(&xendrm_dev->vblank_enabled[i])) {
			struct xendrm_crtc *xen_crtc = &xendrm_dev->crtcs[i];

			drm_crtc_handle_vblank(&xen_crtc->crtc);
			/* handle page flip time outs */
			if (likely(atomic_read(&xendrm_dev->pflip_to_cnt_armed[i])))
				if (unlikely(atomic_dec_and_test(
						&xendrm_dev->pflip_to_cnt[i]))) {
					atomic_set(&xendrm_dev->pflip_to_cnt_armed[i], 0);
					xendrm_crtc_on_page_flip_to(xen_crtc);
				}
		}
	}
}

static void xendrm_lastclose(struct drm_device *dev)
{
	struct xendrm_device *xendrm_dev = dev->dev_private;

	xendrm_dev->front_ops->drm_last_close(xendrm_dev->xdrv_info);
}

void xendrm_vtimer_restart_to(struct xendrm_device *xendrm_dev, int index)
{
	atomic_set(&xendrm_dev->pflip_to_cnt[index],
		xendrm_dev->vblank_timer.to_period);
	atomic_set(&xendrm_dev->pflip_to_cnt_armed[index], 1);
}

void xendrm_vtimer_cancel_to(struct xendrm_device *xendrm_dev, int index)
{
	atomic_set(&xendrm_dev->pflip_to_cnt_armed[index], 0);
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
	.mmap           = xendrm_gem_mmap,
};

static const struct vm_operations_struct xendrm_vm_ops = {
	.open           = drm_gem_vm_open,
	.close          = drm_gem_vm_close,
};

struct drm_driver xendrm_driver = {
	.driver_features           = DRIVER_GEM | DRIVER_MODESET |
				     DRIVER_PRIME | DRIVER_ATOMIC,
	.lastclose                 = xendrm_lastclose,
	.get_vblank_counter        = drm_vblank_no_hw_counter,
	.enable_vblank             = xendrm_enable_vblank,
	.disable_vblank            = xendrm_disable_vblank,
	.get_vblank_counter        = drm_vblank_no_hw_counter,
	.gem_free_object_unlocked  = xendrm_free_object,
	.gem_vm_ops                = &xendrm_vm_ops,
	.prime_handle_to_fd        = drm_gem_prime_handle_to_fd,
	.prime_fd_to_handle        = drm_gem_prime_fd_to_handle,
	.gem_prime_import          = drm_gem_prime_import,
	.gem_prime_export          = drm_gem_prime_export,
	.gem_prime_get_sg_table    = xendrm_gem_get_sg_table,
	.gem_prime_import_sg_table = xendrm_gem_import_sg_table,
	.gem_prime_vmap            = xendrm_gem_prime_vmap,
	.gem_prime_vunmap          = xendrm_gem_prime_vunmap,
	.gem_prime_mmap            = xendrm_gem_prime_mmap,
	.dumb_create               = xendrm_dumb_create,
	.dumb_map_offset           = xendrm_gem_dumb_map_offset,
	.dumb_destroy              = drm_gem_dumb_destroy,
	.fops                      = &xendrm_fops,
	.name                      = "xendrm-du",
	.desc                      = "Xen PV DRM Display Unit",
	.date                      = "20161109",
	.major                     = 1,
	.minor                     = 0,
};

static struct xendrm_timer_callbacks vblank_timer_ops = {
	.on_period = xendrm_handle_vblank,
};

int xendrm_probe(struct platform_device *pdev,
	struct xendispl_front_ops *xendrm_front_funcs)
{
	struct xendrm_plat_data *platdata;
	struct xendrm_device *xendrm_dev;
	struct drm_device *ddev;
	int ret;

	platdata = dev_get_platdata(&pdev->dev);
	DRM_INFO("Creating %s\n", xendrm_driver.desc);
	/* Allocate and initialize the DRM and xendrm device structures. */
	xendrm_dev = devm_kzalloc(&pdev->dev, sizeof(*xendrm_dev), GFP_KERNEL);
	if (!xendrm_dev)
		return -ENOMEM;

	xendrm_dev->front_ops = xendrm_front_funcs;
	xendrm_dev->front_ops->on_page_flip = xendrm_on_page_flip;
	xendrm_dev->xdrv_info = platdata->xdrv_info;

	ddev = drm_dev_alloc(&xendrm_driver, &pdev->dev);
	if (!ddev)
		return -ENOMEM;

	xendrm_dev->drm = ddev;
	/*
	 * FIXME: assume 1 CRTC and 1 Encoder per each connector
	 */
	xendrm_dev->num_crtcs = platdata->num_connectors;
	xendrm_dev->platdata = platdata;
	ddev->dev_private = xendrm_dev;
	platform_set_drvdata(pdev, xendrm_dev);

	ret = drm_vblank_init(ddev, xendrm_dev->num_crtcs);
	if (ret < 0)
		goto fail_vblank;
	/* DRM/KMS objects */
	ret = xendrm_kms_init(xendrm_dev);
	if (ret < 0) {
		if (ret != -EPROBE_DEFER)
			dev_err(&pdev->dev,
				"failed to initialize DRM/KMS (%d)\n", ret);
		goto fail_modeset;
	}
	/* setup vblank emulation: all CRTCs are set for
	 * XENDRM_CRTC_VREFRESH_HZ and lots of operations during vblank
	 * interrupt are handled under drm_dev->event_lock. This allows
	 * having a single vblank "interrupt"
	 */
	xendrm_timer_init(&xendrm_dev->vblank_timer,
		(unsigned long)xendrm_dev, &vblank_timer_ops);
	xendrm_timer_setup(&xendrm_dev->vblank_timer,
		XENDRM_CRTC_VREFRESH_HZ, XENDRM_CRTC_PFLIP_TO_MS);
	ddev->irq_enabled = 1;

	/* Register the DRM device with the core and the connectors,
	 * encoders, planes with sysfs.
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
	xendrm_timer_cleanup(&xendrm_dev->vblank_timer);
	drm_dev_unregister(ddev);
fail_modeset:
	drm_mode_config_cleanup(ddev);
fail_vblank:
	drm_vblank_cleanup(ddev);
	return ret;
}

int xendrm_remove(struct platform_device *pdev)
{
	struct xendrm_device *xendrm_dev = platform_get_drvdata(pdev);
	struct drm_device *drm_dev = xendrm_dev->drm;

	xendrm_timer_cleanup(&xendrm_dev->vblank_timer);
	drm_dev_unregister(drm_dev);
	drm_vblank_cleanup(drm_dev);
	drm_mode_config_cleanup(drm_dev);
	drm_dev_unref(drm_dev);
	return 0;
}

bool xendrm_is_used(struct platform_device *pdev)
{
	struct xendrm_device *xendrm_dev = platform_get_drvdata(pdev);
	struct drm_device *drm_dev;

	if (!xendrm_dev)
		return false;
	drm_dev = xendrm_dev->drm;
	if (!drm_dev)
		return false;

	/* FIXME: the code below must be protected by drm_global_mutex,
	 * but it is not accessible to us and anyways there is a
	 * race condition.
	 */
	return drm_dev->open_count != 0;
}
