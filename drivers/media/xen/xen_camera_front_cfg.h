/* SPDX-License-Identifier: GPL-2.0 OR MIT */

/*
 * Xen para-virtual camera device
 *
 * Copyright (C) 2018 EPAM Systems Inc.
 *
 * Author: Oleksandr Andrushchenko <oleksandr_andrushchenko@epam.com>
 */

#ifndef __XEN_CAMERA_FRONT_CFG_H
#define __XEN_CAMERA_FRONT_CFG_H

#include <xen/interface/io/cameraif.h>

struct xen_camera_front_info;

struct xen_camera_front_cfg_ctrl {
	u32 v4l2_cid;

	u16 flags;
	s64 minimum;
	s64 maximum;
	s64 default_value;
	s64 step;
};

struct xen_camera_front_cfg_fract {
	u32 numerator;
	u32 denominator;
};

struct xen_camera_front_cfg_resolution {
	u32 width;
	u32 height;
	int num_frame_rates;
	struct xen_camera_front_cfg_fract *frame_rate;
};

struct xen_camera_front_cfg_format {
	u32 pixel_format;
	int num_resolutions;
	struct xen_camera_front_cfg_resolution *resolution;
};

struct xen_camera_front_cfg_card {
	bool be_alloc;

	int max_buffers;

	int num_formats;
	struct xen_camera_front_cfg_format *format;
	int num_controls;
	struct xen_camera_front_cfg_ctrl ctrl[XENCAMERA_MAX_CTRL];
};

int xen_camera_front_cfg_init(struct xen_camera_front_info *front_info);

#endif /* __XEN_CAMERA_FRONT_CFG_H */
