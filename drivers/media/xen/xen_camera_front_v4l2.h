/* SPDX-License-Identifier: GPL-2.0 OR MIT */

/*
 * Xen para-virtual camera device
 *
 * Copyright (C) 2018 EPAM Systems Inc.
 *
 * Author: Oleksandr Andrushchenko <oleksandr_andrushchenko@epam.com>
 */

#ifndef __XEN_CAMERA_FRONT_V4L2_H
#define __XEN_CAMERA_FRONT_V4L2_H

struct xen_camera_front_info;

int xen_camera_front_v4l2_init(struct xen_camera_front_info *front_info);

void xen_camera_front_v4l2_fini(struct xen_camera_front_info *front_info);

int xen_camera_front_v4l2_to_v4l2_cid(int xen_type);

int xen_camera_front_v4l2_to_xen_type(int v4l2_cid);

void xen_camera_front_v4l2_on_frame(struct xen_camera_front_info *front_info,
				    struct xencamera_frame_avail_evt *evt);

void xen_camera_front_v4l2_on_ctrl(struct xen_camera_front_info *front_info,
				   struct xencamera_ctrl_value *evt);

#endif /*__XEN_CAMERA_FRONT_V4L2_H */
