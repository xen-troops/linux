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

#ifndef __XEN_DRM_FRONT_H_
#define __XEN_DRM_FRONT_H_

struct xdrv_info;
struct platform_device;
struct xendrm_crtc;
struct sg_table;

/* timeout in ms to wait for backend to respond */
#define VDRM_WAIT_BACK_MS	3000

struct xendispl_front_ops {
	int (*mode_set)(struct xendrm_crtc *xen_crtc, uint32_t x, uint32_t y,
		uint32_t width, uint32_t height, uint32_t bpp,
		uint64_t fb_cookie);
	struct page **(*dbuf_create)(struct xdrv_info *drv_info,
		uint64_t dumb_cookie, uint32_t width, uint32_t height,
		uint32_t bpp, uint64_t size, struct page **pages,
		struct sg_table *sgt);
	int (*dbuf_destroy)(struct xdrv_info *drv_info, uint64_t dumb_cookie);
	int (*fb_attach)(struct xdrv_info *drv_info, uint64_t dumb_cookie,
		uint64_t fb_cookie, uint32_t width, uint32_t height,
		uint32_t pixel_format);
	int (*fb_detach)(struct xdrv_info *drv_info, uint64_t fb_cookie);
	int (*page_flip)(struct xdrv_info *drv_info, int conn_idx,
		uint64_t fb_cookie);
	/* CAUTION! this is called with a spin_lock held! */
	void (*on_page_flip)(struct platform_device *pdev, int conn_idx,
		uint64_t fb_cookie);
	void (*drm_last_close)(struct xdrv_info *drv_info);
};

#endif /* __XEN_DRM_FRONT_H_ */
