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

#include "xen_drm_front_kms.h"

#include <drm/drmP.h>
#include <drm/drm_atomic_helper.h>
#include <drm/drm_crtc.h>
#include <drm/drm_crtc_helper.h>
#include <drm/drm_gem.h>

#include "xen_drm_front.h"
#include "xen_drm_front_drv.h"
#include "xen_drm_front_gem.h"

static void kms_fb_destroy(struct drm_framebuffer *fb)
{
	struct xen_drm_front_drm_info *drm_info = fb->dev->dev_private;

	drm_info->front_ops->fb_detach(drm_info->front_info,
		xen_drm_front_fb_to_cookie(fb));
	drm_info->gem_ops->fb_destroy(fb);
}

static struct drm_framebuffer_funcs xen_drm_fb_funcs = {
	.destroy = kms_fb_destroy,
};

static struct drm_framebuffer *
kms_fb_create(struct drm_device *dev, struct drm_file *file_priv,
	const struct drm_mode_fb_cmd2 *mode_cmd)
{
	struct xen_drm_front_drm_info *drm_info = dev->dev_private;
	static struct drm_framebuffer *fb;
	struct drm_gem_object *gem_obj;
	int ret;

	fb = drm_info->gem_ops->fb_create_with_funcs(dev, file_priv,
		mode_cmd, &xen_drm_fb_funcs);
	if (IS_ERR(fb))
		return fb;

	gem_obj = drm_gem_object_lookup(file_priv,
		mode_cmd->handles[0]);
	if (!gem_obj) {
		DRM_ERROR("Failed to lookup GEM object\n");
		ret = -ENOENT;
		goto fail;
	}

	drm_gem_object_unreference_unlocked(gem_obj);

	ret = drm_info->front_ops->fb_attach(
		drm_info->front_info, xen_drm_front_dbuf_to_cookie(gem_obj),
		xen_drm_front_fb_to_cookie(fb), fb->width, fb->height,
		fb->pixel_format);
	if (ret < 0) {
		DRM_ERROR("Back failed to attach FB %p: %d\n", fb, ret);
		goto fail;
	}

	return fb;

fail:
	drm_info->gem_ops->fb_destroy(fb);
	return ERR_PTR(ret);
}

static const struct drm_mode_config_funcs xen_drm_kms_config_funcs = {
	.fb_create = kms_fb_create,
	.atomic_check = drm_atomic_helper_check,
	.atomic_commit = drm_atomic_helper_commit,
};

int xen_drm_front_kms_init(struct xen_drm_front_drm_info *drm_info)
{
	struct drm_device *drm_dev = drm_info->drm_dev;
	int i, ret;

	drm_mode_config_init(drm_dev);

	drm_dev->mode_config.min_width = 0;
	drm_dev->mode_config.min_height = 0;
	drm_dev->mode_config.max_width = 4095;
	drm_dev->mode_config.max_height = 2047;
	drm_dev->mode_config.funcs = &xen_drm_kms_config_funcs;

	for (i = 0; i < drm_info->num_crtcs; i++) {
		struct xen_drm_front_crtc *crtc;

		crtc = &drm_info->crtcs[i];
		ret = xen_drm_front_crtc_create(drm_info, crtc, i);
		if (ret < 0)
			goto fail;

		ret = xen_drm_front_crtc_encoder_create(drm_info, crtc);
		if (ret)
			goto fail;

		ret = xen_drm_front_crtc_connector_create(drm_info, crtc,
			&drm_info->plat_data->connectors[i]);
		if (ret)
			goto fail;
	}
	drm_mode_config_reset(drm_dev);

	return 0;

fail:
	drm_mode_config_cleanup(drm_dev);
	return ret;
}
