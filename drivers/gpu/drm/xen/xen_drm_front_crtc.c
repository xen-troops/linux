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

#include "xen_drm_front_crtc.h"

#include <drm/drm_atomic_helper.h>
#include <drm/drm_crtc_helper.h>
#include <drm/drm_plane_helper.h>

#include <video/videomode.h>

#include "xen_drm_front.h"
#include "xen_drm_front_drv.h"

/*
 * page flip complete event can be sent by either on back's
 * page flip completed event or atomic_flush, whatever is the
 * _last_
 */
enum page_flip_event_sources {
	PF_EVT_SOURCE_BACK,
	PF_EVT_SOURCE_FLUSH,
	PF_EVT_SOURCE_MAX,
};

static inline struct xen_drm_front_connector *
to_xendrm_connector(struct drm_connector *connector)
{
	return container_of(connector, struct xen_drm_front_connector, base);
}

static inline struct xen_drm_front_crtc *
to_xendrm_crtc(struct drm_crtc *crtc)
{
	return container_of(crtc, struct xen_drm_front_crtc, crtc);
}

static const struct drm_encoder_funcs xen_drm_encoder_funcs = {
	.destroy = drm_encoder_cleanup,
};

int xen_drm_front_crtc_encoder_create(struct xen_drm_front_drm_info *drm_info,
	struct xen_drm_front_crtc *xen_crtc)
{
	struct drm_encoder *encoder = &xen_crtc->encoder;
	int ret;

	/* only this CRTC w/o any clones */
	encoder->possible_crtcs = 1 << xen_crtc->index;
	encoder->possible_clones = 0;
	ret = drm_encoder_init(drm_info->drm_dev, encoder,
		&xen_drm_encoder_funcs, DRM_MODE_ENCODER_VIRTUAL, NULL);
	if (ret < 0)
		return ret;

	return 0;
}

static enum drm_connector_status crtc_connector_detect(
	struct drm_connector *connector, bool force)
{
	return connector_status_connected;
}

#define XENDRM_NUM_VIDEO_MODES	1

static int crtc_connector_get_modes(struct drm_connector *connector)
{
	struct xen_drm_front_connector *xen_connector;
	struct drm_display_mode *mode;
	struct videomode videomode;
	int width, height;

	mode = drm_mode_create(connector->dev);
	if (!mode)
		return 0;

	memset(&videomode, 0, sizeof(videomode));
	xen_connector = to_xendrm_connector(connector);
	videomode.hactive = xen_connector->width;
	videomode.vactive = xen_connector->height;
	width = videomode.hactive + videomode.hfront_porch +
		videomode.hback_porch + videomode.hsync_len;
	height = videomode.vactive + videomode.vfront_porch +
		videomode.vback_porch + videomode.vsync_len;
	videomode.pixelclock = width * height * XENDRM_CRTC_VREFRESH_HZ;
	mode->type = DRM_MODE_TYPE_PREFERRED | DRM_MODE_TYPE_DRIVER;
	drm_display_mode_from_videomode(&videomode, mode);
	drm_mode_probed_add(connector, mode);
	return XENDRM_NUM_VIDEO_MODES;
}

static int crtc_connector_mode_valid(struct drm_connector *connector,
	struct drm_display_mode *mode)
{
	struct xen_drm_front_connector *xen_connector =
		to_xendrm_connector(connector);

	if (mode->hdisplay != xen_connector->width)
		return MODE_ERROR;

	if (mode->vdisplay != xen_connector->height)
		return MODE_ERROR;

	return MODE_OK;
}

static const struct drm_connector_helper_funcs
		xen_drm_connector_helper_funcs = {
	.get_modes = crtc_connector_get_modes,
	.mode_valid = crtc_connector_mode_valid,
};

static const struct drm_connector_funcs xen_drm_connector_funcs = {
	.atomic_duplicate_state = drm_atomic_helper_connector_duplicate_state,
	.atomic_destroy_state = drm_atomic_helper_connector_destroy_state,
	.destroy = drm_connector_cleanup,
	.detect = crtc_connector_detect,
	.dpms = drm_atomic_helper_connector_dpms,
	.fill_modes = drm_helper_probe_single_connector_modes,
	.reset = drm_atomic_helper_connector_reset,
};

int xen_drm_front_crtc_connector_create(struct xen_drm_front_drm_info *drm_info,
	struct xen_drm_front_crtc *xen_crtc,
	struct xen_drm_front_cfg_connector *cfg)
{
	struct drm_encoder *encoder = &xen_crtc->encoder;
	struct drm_connector *connector = &xen_crtc->connector.base;
	struct drm_mode_config *mode_config = &drm_info->drm_dev->mode_config;
	int ret;

	xen_crtc->connector.width = cfg->width;
	xen_crtc->connector.height = cfg->height;
	ret = drm_connector_init(drm_info->drm_dev, connector,
		&xen_drm_connector_funcs, DRM_MODE_CONNECTOR_VIRTUAL);
	if (ret < 0)
		return ret;

	drm_connector_helper_add(connector, &xen_drm_connector_helper_funcs);

	ret = drm_mode_connector_attach_encoder(connector, encoder);
	if (ret < 0)
		goto fail;

	drm_object_property_set_value(&connector->base,
		mode_config->dpms_property, DRM_MODE_DPMS_ON);
	return 0;

fail:
	drm_connector_cleanup(connector);
	return ret;
}

static const uint32_t xen_drm_plane_formats[] = {
	DRM_FORMAT_RGB565,
	DRM_FORMAT_RGB888,
	DRM_FORMAT_XRGB8888,
	DRM_FORMAT_ARGB8888,
	DRM_FORMAT_XRGB4444,
	DRM_FORMAT_ARGB4444,
	DRM_FORMAT_XRGB1555,
	DRM_FORMAT_ARGB1555,
};

static int crtc_plane_atomic_check(struct drm_plane *plane,
	struct drm_plane_state *state)
{
	struct drm_framebuffer *fb = state->fb;
	int i;

	if (!state->fb || !state->crtc)
		return 0;

	for (i = 0; i < ARRAY_SIZE(xen_drm_plane_formats); i++)
		if (fb->pixel_format == xen_drm_plane_formats[i])
			return 0;

	return -EINVAL;
}

static void crtc_plane_atomic_update(struct drm_plane *plane,
	struct drm_plane_state *old_state)
{
	/* nothing to do */
}

static const struct drm_plane_helper_funcs xen_drm_plane_helper_funcs = {
	.atomic_check = crtc_plane_atomic_check,
	.atomic_update = crtc_plane_atomic_update,
};

static const struct drm_plane_funcs xen_drm_crtc_drm_plane_funcs = {
	.update_plane = drm_atomic_helper_update_plane,
	.disable_plane = drm_atomic_helper_disable_plane,
	.destroy = drm_plane_cleanup,
	.reset = drm_atomic_helper_plane_reset,
	.atomic_duplicate_state = drm_atomic_helper_plane_duplicate_state,
	.atomic_destroy_state = drm_atomic_helper_plane_destroy_state,
};

static struct drm_plane *crtc_create_primary(
	struct xen_drm_front_drm_info *drm_info, struct xen_drm_front_crtc *xen_crtc)
{
	struct drm_plane *primary = &xen_crtc->primary;
	int ret;

	ret = drm_universal_plane_init(drm_info->drm_dev, primary, 0,
		&xen_drm_crtc_drm_plane_funcs,
		xen_drm_plane_formats,
		ARRAY_SIZE(xen_drm_plane_formats),
		DRM_PLANE_TYPE_PRIMARY, NULL);
	if (ret < 0)
		return NULL;

	drm_plane_helper_add(primary, &xen_drm_plane_helper_funcs);
	return primary;
}

static int crtc_props_init(struct xen_drm_front_drm_info *drm_info,
	struct xen_drm_front_crtc *xen_crtc)
{
	xen_crtc->props.alpha = drm_property_create_range(drm_info->drm_dev,
		0, "alpha", 0, 255);
	if (!xen_crtc->props.alpha)
		return -ENOMEM;
	return 0;
}

static inline bool crtc_page_flip_pending(
	struct xen_drm_front_crtc *xen_crtc)
{
	struct drm_device *dev = xen_crtc->crtc.dev;
	bool pending;
	unsigned long flags;

	spin_lock_irqsave(&dev->event_lock, flags);
	pending = xen_crtc->pg_flip_event != NULL;
	spin_unlock_irqrestore(&dev->event_lock, flags);
	return pending;
}

static int crtc_do_page_flip(struct drm_crtc *crtc,
	struct drm_framebuffer *fb, struct drm_pending_vblank_event *event,
	uint32_t drm_flags)
{
	struct xen_drm_front_crtc *xen_crtc = to_xendrm_crtc(crtc);
	struct drm_device *dev = xen_crtc->crtc.dev;
	struct xen_drm_front_drm_info *drm_info;
	unsigned long flags;
	int ret;

	if (unlikely(crtc_page_flip_pending(xen_crtc))) {
		/* this can happen if user space doesn't honor
		 * page flip completed events
		 */
		DRM_WARN("Already have pending page flip\n");
		return -EBUSY;
	}

	/* There are 2 possible cases:
	 *   1. backend sends page flip completed before atomic_flush
	 *   2. backend is clumsy and sends event later than atomic_flush
	 * FIXME: drm_pending_vblank_event is not yet fully initialized
	 *   by the DRM core, so it cannot be used to send events right now
	 *   (see drm_ioctl), so use it as a placeholder which will not
	 *   allow concurrent flips
	 */
	spin_lock_irqsave(&dev->event_lock, flags);
	xen_crtc->pg_flip_event = event;
	atomic_set(&xen_crtc->pg_flip_source_cnt, PF_EVT_SOURCE_MAX);
	xen_crtc->fb_cookie = xen_drm_front_fb_to_cookie(fb);
	spin_unlock_irqrestore(&dev->event_lock, flags);

	drm_info = xen_crtc->drm_info;

	ret = drm_info->front_ops->page_flip(
		drm_info->front_info, xen_crtc->index,
		xen_drm_front_fb_to_cookie(fb));
	if (unlikely(ret < 0))
		goto fail;

	/* at this stage back was armed and will send page flip event,
	 * so if we now fail then we have to drop incoming event
	 */
	ret = drm_atomic_helper_page_flip(crtc, fb, event, drm_flags);
	if (unlikely(ret < 0)) {
		xen_crtc->fb_cookie = -1;
		goto fail;
	}

	/* restart page flip time-out counter */
	xen_drm_front_drv_vtimer_restart_to(drm_info, xen_crtc->index);
	return 0;

fail:
	spin_lock_irqsave(&dev->event_lock, flags);
	atomic_set(&xen_crtc->pg_flip_source_cnt, 0);
	xen_crtc->pg_flip_event = NULL;
	xen_crtc->fb_cookie = xen_drm_front_fb_to_cookie(NULL);
	spin_unlock_irqrestore(&dev->event_lock, flags);
	return ret;
}

static void crtc_ntfy_page_flip_completed(
	struct xen_drm_front_crtc *xen_crtc)
{
	struct drm_device *dev = xen_crtc->crtc.dev;
	unsigned long flags;

	xen_drm_front_drv_vtimer_cancel_to(xen_crtc->drm_info, xen_crtc->index);
	if (unlikely(!crtc_page_flip_pending(xen_crtc)))
		return;

	spin_lock_irqsave(&dev->event_lock, flags);
	drm_crtc_send_vblank_event(&xen_crtc->crtc, xen_crtc->pg_flip_event);
	xen_crtc->pg_flip_event = NULL;
	wake_up(&xen_crtc->flip_wait);
	spin_unlock_irqrestore(&dev->event_lock, flags);
	drm_crtc_vblank_put(&xen_crtc->crtc);
}

void xen_drm_front_crtc_on_page_flip_done(struct xen_drm_front_crtc *xen_crtc,
	uint64_t fb_cookie)
{
	if (unlikely(xen_crtc->fb_cookie != fb_cookie)) {
		DRM_ERROR("Drop page flip event: current %llx != %llx\n",
			xen_crtc->fb_cookie, fb_cookie);
		return;
	}

	WARN_ON(atomic_read(&xen_crtc->pg_flip_source_cnt) == 0);

	if (atomic_dec_and_test(&xen_crtc->pg_flip_source_cnt))
		crtc_ntfy_page_flip_completed(xen_crtc);
}

void xen_drm_front_crtc_on_page_flip_to(struct xen_drm_front_crtc *xen_crtc)
{
	if (crtc_page_flip_pending(xen_crtc)) {
		DRM_ERROR("Flip event timed-out, releasing\n");
		crtc_ntfy_page_flip_completed(xen_crtc);
		atomic_set(&xen_crtc->pg_flip_source_cnt, 0);
	}
}

static int crtc_set_config(struct drm_mode_set *set)
{
	struct drm_crtc *crtc = set->crtc;
	struct xen_drm_front_crtc *xen_crtc = to_xendrm_crtc(crtc);
	struct xen_drm_front_drm_info *drm_info = xen_crtc->drm_info;
	int ret;

	if (set->mode) {
		ret = drm_info->front_ops->mode_set(xen_crtc, set->x, set->y,
			set->fb->width, set->fb->height,
			set->fb->bits_per_pixel, xen_drm_front_fb_to_cookie(set->fb));
		if (ret < 0) {
			DRM_ERROR("Failed to set mode to back: %d\n", ret);
			return ret;
		}
	} else {
		ret = drm_info->front_ops->mode_set(xen_crtc,
			0, 0, 0, 0, 0, 0);
		if (ret < 0)
			DRM_ERROR("Failed to set mode to back: %d\n", ret);
		/* fall through - at least try to set mode locally */
	}
	return drm_atomic_helper_set_config(set);
}

static void crtc_disable(struct drm_crtc *crtc)
{
	struct xen_drm_front_crtc *xen_crtc = to_xendrm_crtc(crtc);

	xen_drm_front_drv_vtimer_cancel_to(xen_crtc->drm_info, xen_crtc->index);
	if (wait_event_timeout(xen_crtc->flip_wait,
			!crtc_page_flip_pending(xen_crtc),
			msecs_to_jiffies(XENDRM_CRTC_PFLIP_TO_MS)) == 0) {
		crtc_ntfy_page_flip_completed(xen_crtc);
	}
	drm_crtc_vblank_off(crtc);
}

static void crtc_atomic_flush(struct drm_crtc *crtc,
	struct drm_crtc_state *old_crtc_state)
{
	struct xen_drm_front_crtc *xen_crtc = to_xendrm_crtc(crtc);
	struct drm_pending_vblank_event *event;
	struct drm_device *dev = crtc->dev;
	unsigned long flags;

	spin_lock_irqsave(&dev->event_lock, flags);
	event = crtc->state->event;
	crtc->state->event = NULL;
	if (event) {
		if (event->event.base.type == DRM_EVENT_FLIP_COMPLETE) {
			WARN_ON(drm_crtc_vblank_get(crtc) != 0);
			xen_crtc->pg_flip_event = event;
			WARN_ON(atomic_read(&xen_crtc->pg_flip_source_cnt) == 0);
			if (atomic_dec_and_test(&xen_crtc->pg_flip_source_cnt)) {
				spin_unlock_irqrestore(&dev->event_lock, flags);
				crtc_ntfy_page_flip_completed(xen_crtc);
				return;
			}
		} else {
			if (drm_crtc_vblank_get(crtc) == 0)
				drm_crtc_arm_vblank_event(crtc, event);
			else
				drm_crtc_send_vblank_event(crtc, event);
		}
	}
	spin_unlock_irqrestore(&dev->event_lock, flags);
}

static const struct drm_crtc_helper_funcs xen_drm_crtc_helper_funcs = {
	.atomic_flush = crtc_atomic_flush,
	.enable = drm_crtc_vblank_on,
	.disable = crtc_disable,
};

static const struct drm_crtc_funcs xen_drm_crtc_funcs = {
	.atomic_duplicate_state = drm_atomic_helper_crtc_duplicate_state,
	.atomic_destroy_state = drm_atomic_helper_crtc_destroy_state,
	.destroy = drm_crtc_cleanup,
	.page_flip = crtc_do_page_flip,
	.reset = drm_atomic_helper_crtc_reset,
	.set_config = crtc_set_config,
};

int xen_drm_front_crtc_create(struct xen_drm_front_drm_info *drm_info,
	struct xen_drm_front_crtc *xen_crtc, unsigned int index)
{
	struct drm_plane *primary;
	int ret;

	memset(xen_crtc, 0, sizeof(*xen_crtc));
	xen_crtc->drm_info = drm_info;
	xen_crtc->index = index;
	init_waitqueue_head(&xen_crtc->flip_wait);

	ret = crtc_props_init(drm_info, xen_crtc);
	if (ret < 0)
		return ret;

	primary = crtc_create_primary(drm_info, xen_crtc);
	if (!primary)
		return -ENOMEM;

	/* only primary plane, no cursor */
	ret = drm_crtc_init_with_planes(drm_info->drm_dev, &xen_crtc->crtc,
		primary, NULL, &xen_drm_crtc_funcs, NULL);
	if (ret) {
		primary->funcs->destroy(primary);
		return ret;
	}

	drm_crtc_helper_add(&xen_crtc->crtc, &xen_drm_crtc_helper_funcs);
	return 0;
}
