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

#ifndef __XEN_DRM_FRONT_H_
#define __XEN_DRM_FRONT_H_

#include <linux/platform_device.h>
#include <linux/mutex.h>
#include <linux/scatterlist.h>
#include <linux/spinlock.h>
#include <linux/types.h>

#include "xen_drm_front_cfg.h"

struct xen_drm_front_crtc;

/* timeout in ms to wait for backend to respond */
#define VDRM_WAIT_BACK_MS	3000

struct xen_drm_front_ops {
	int (*mode_set)(struct xen_drm_front_crtc *xen_crtc,
		uint32_t x, uint32_t y, uint32_t width, uint32_t height,
		uint32_t bpp, uint64_t fb_cookie);
	int (*dbuf_create)(struct xen_drm_front_info *front_info,
		uint64_t dbuf_cookie, uint32_t width, uint32_t height,
		uint32_t bpp, uint64_t size, struct page **pages);
	int (*dbuf_create_from_sgt)(struct xen_drm_front_info *front_info,
		uint64_t dbuf_cookie, uint32_t width, uint32_t height,
		uint32_t bpp, uint64_t size, struct sg_table *sgt);
	int (*dbuf_destroy)(struct xen_drm_front_info *front_info,
		uint64_t dbuf_cookie);
	int (*fb_attach)(struct xen_drm_front_info *front_info,
		uint64_t dbuf_cookie, uint64_t fb_cookie, uint32_t width,
		uint32_t height, uint32_t pixel_format);
	int (*fb_detach)(struct xen_drm_front_info *front_info,
		uint64_t fb_cookie);
	int (*page_flip)(struct xen_drm_front_info *front_info, int conn_idx,
		uint64_t fb_cookie);
	/* CAUTION! this is called with a spin_lock held! */
	void (*on_page_flip)(struct platform_device *pdev, int conn_idx,
		uint64_t fb_cookie);
	void (*drm_last_close)(struct xen_drm_front_info *front_info);
};

struct xen_drm_front_info {
	struct xenbus_device *xb_dev;
	spinlock_t io_lock;
	struct mutex mutex;
	bool drm_pdrv_registered;
	/* virtual DRM platform device */
	struct platform_device *drm_pdev;

	int num_evt_pairs;
	struct xen_drm_front_evtchnl_pair *evt_pairs;
	struct xen_drm_front_cfg_plat_data cfg_plat_data;

	/* display buffers */
	struct list_head dbuf_list;
};

#endif /* __XEN_DRM_FRONT_H_ */
