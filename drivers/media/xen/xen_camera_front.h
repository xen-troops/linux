/* SPDX-License-Identifier: GPL-2.0 OR MIT */

/*
 * Xen para-virtual camera device
 *
 * Copyright (C) 2018 EPAM Systems Inc.
 *
 * Author: Oleksandr Andrushchenko <oleksandr_andrushchenko@epam.com>
 */

#ifndef __XEN_CAMERA_FRONT_H
#define __XEN_CAMERA_FRONT_H

#include <media/v4l2-ctrls.h>
#include <media/v4l2-device.h>
#include <media/videobuf2-v4l2.h>

#include <xen/grant_table.h>
#include <xen/xen-front-pgdir-shbuf.h>

#include <xen/interface/io/cameraif.h>

#include "xen_camera_front_cfg.h"
#include "xen_camera_front_evtchnl.h"

struct xen_camera_front_info {
	struct xenbus_device *xb_dev;
	struct xen_camera_front_v4l2_info *v4l2_info;

	struct xen_camera_front_evtchnl_pair evt_pair;

	/* To protect data between backend IO code and interrupt handler. */
	spinlock_t io_lock;

	struct xen_camera_front_cfg_card cfg;
};

struct xen_camera_front_shbuf {
	struct xen_front_pgdir_shbuf pgdir;

	unsigned int data_offset;

	struct sg_table *sgt;
	struct page **pages;
};

int xen_camera_front_set_config(struct xen_camera_front_info *front_info,
				struct xencamera_config_req *cfg_req,
				struct xencamera_config_resp *cfg_resp);

int xen_camera_front_get_config(struct xen_camera_front_info *front_info,
				struct xencamera_config_resp *cfg_resp);

int xen_camera_front_validate_config(struct xen_camera_front_info *front_info,
				     struct xencamera_config_req *cfg_req,
				     struct xencamera_config_resp *cfg_resp);

int xen_camera_front_set_frame_rate(struct xen_camera_front_info *front_info,
				    struct xencamera_frame_rate_req *frame_rate);

int xen_camera_front_set_control(struct xen_camera_front_info *front_info,
				 int v4l2_cid, s64 value);

int xen_camera_front_get_control(struct xen_camera_front_info *front_info,
				 int v4l2_cid, s64 *value);

int xen_camera_front_get_buf_layout(struct xen_camera_front_info *front_info,
				    struct xencamera_buf_get_layout_resp *resp);

int xen_camera_front_buf_request(struct xen_camera_front_info *front_info,
				 int num_bufs);

int xen_camera_front_buf_create(struct xen_camera_front_info *front_info,
				struct xen_camera_front_shbuf *shbuf,
				u8 index, struct sg_table *sgt);

int xen_camera_front_buf_destroy(struct xen_camera_front_info *front_info,
				 struct xen_camera_front_shbuf *shbuf,
				 u8 index);

int xen_camera_front_buf_queue(struct xen_camera_front_info *front_info,
			       u8 index);

int xen_camera_front_buf_dequeue(struct xen_camera_front_info *front_info,
				 u8 index);

int xen_camera_front_stream_start(struct xen_camera_front_info *front_info);

int xen_camera_front_stream_stop(struct xen_camera_front_info *front_info);

void xen_camera_front_destroy_shbuf(struct xen_camera_front_shbuf *shbuf);

#endif /* __XEN_CAMERA_FRONT_H */
