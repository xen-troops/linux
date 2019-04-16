// SPDX-License-Identifier: GPL-2.0 OR MIT

/*
 * Xen para-virtual camera device
 *
 * Based on V4L2 PCI Skeleton Driver: samples/v4l/v4l2-pci-skeleton.c
 *
 * Copyright (C) 2018 EPAM Systems Inc.
 *
 * Author: Oleksandr Andrushchenko <oleksandr_andrushchenko@epam.com>
 */

#include <linux/videodev2.h>

#include <media/v4l2-device.h>
#include <media/v4l2-dev.h>
#include <media/v4l2-ioctl.h>
#include <media/v4l2-ctrls.h>
#include <media/v4l2-event.h>
#include <media/videobuf2-dma-sg.h>
#include <media/videobuf2-v4l2.h>

#include <xen/xenbus.h>

#include <xen/interface/io/cameraif.h>

#include "xen_camera_front.h"
#include "xen_camera_front_v4l2.h"

struct xen_camera_front_v4l2_info {
	struct xen_camera_front_info *front_info;
	/* This will be set if device has been unplugged. */
	bool unplugged;

	struct v4l2_device v4l2_dev;
	struct video_device vdev;
	struct v4l2_ctrl_handler ctrl_handler;
	struct v4l2_ctrl *ctrls[XENCAMERA_MAX_CTRL];
	struct vb2_queue queue;

	/* IOCTL serialization and the rest. */
	struct mutex v4l2_lock;
	/* Queue serialization. */
	struct mutex vb_queue_lock;

	/* Queued buffer list lock. */
	struct mutex bufs_lock;
	struct list_head bufs_list;

	/* Size of a camera buffer. */
	size_t v4l2_buffer_sz;
};

struct xen_camera_buffer {
	struct vb2_v4l2_buffer vb;
	/* Xen shared buffer backing this V4L2 buffer's memory. */
	struct xen_camera_front_shbuf shbuf;
	/* Is this buffer queued or not. */
	bool is_queued;

	struct list_head list;
};

static struct xen_camera_buffer *
to_xen_camera_buffer(struct vb2_buffer *vb)
{
	return container_of(vb, struct xen_camera_buffer, vb.vb2_buf);
}

struct xen_to_v4l2 {
	int xen;
	int v4l2;
};

static const struct xen_to_v4l2 XEN_TYPE_TO_V4L2_CID[] = {
	{
		.xen = XENCAMERA_CTRL_BRIGHTNESS,
		.v4l2 = V4L2_CID_BRIGHTNESS,
	},
	{
		.xen = XENCAMERA_CTRL_CONTRAST,
		.v4l2 = V4L2_CID_CONTRAST,
	},
	{
		.xen = XENCAMERA_CTRL_SATURATION,
		.v4l2 = V4L2_CID_SATURATION,
	},
	{
		.xen = XENCAMERA_CTRL_HUE,
		.v4l2 = V4L2_CID_HUE
	},
};

static const struct xen_to_v4l2 XEN_COLORSPACE_TO_V4L2[] = {
	{
		.xen = XENCAMERA_COLORSPACE_DEFAULT,
		.v4l2 = V4L2_COLORSPACE_DEFAULT,
	},
	{
		.xen = XENCAMERA_COLORSPACE_SMPTE170M,
		.v4l2 = V4L2_COLORSPACE_SMPTE170M,
	},
	{
		.xen = XENCAMERA_COLORSPACE_REC709,
		.v4l2 = V4L2_COLORSPACE_REC709,
	},
	{
		.xen = XENCAMERA_COLORSPACE_SRGB,
		.v4l2 = V4L2_COLORSPACE_SRGB,
	},
	{
		.xen = XENCAMERA_COLORSPACE_OPRGB,
#if (LINUX_VERSION_CODE < KERNEL_VERSION(4, 15, 0))
		.v4l2 = V4L2_COLORSPACE_ADOBERGB,
#else
		.v4l2 = V4L2_COLORSPACE_OPRGB,
#endif
	},
	{
		.xen = XENCAMERA_COLORSPACE_BT2020,
		.v4l2 = V4L2_COLORSPACE_BT2020,
	},
	{
		.xen = XENCAMERA_COLORSPACE_DCI_P3,
		.v4l2 = V4L2_COLORSPACE_DCI_P3,
	},
};

static const struct xen_to_v4l2 XEN_XFER_FUNC_TO_V4L2[] = {
	{
		.xen = XENCAMERA_XFER_FUNC_DEFAULT,
		.v4l2 = V4L2_XFER_FUNC_DEFAULT,
	},
	{
		.xen = XENCAMERA_XFER_FUNC_709,
		.v4l2 = V4L2_XFER_FUNC_709,
	},
	{
		.xen = XENCAMERA_XFER_FUNC_SRGB,
		.v4l2 = V4L2_XFER_FUNC_SRGB,
	},
	{
		.xen = XENCAMERA_XFER_FUNC_OPRGB,
#if (LINUX_VERSION_CODE < KERNEL_VERSION(4, 15, 0))
		.v4l2 = V4L2_XFER_FUNC_ADOBERGB,
#else
		.v4l2 = V4L2_XFER_FUNC_OPRGB,
#endif
	},
	{
		.xen = XENCAMERA_XFER_FUNC_NONE,
		.v4l2 = V4L2_XFER_FUNC_NONE,
	},
	{
		.xen = XENCAMERA_XFER_FUNC_DCI_P3,
		.v4l2 = V4L2_XFER_FUNC_DCI_P3,
	},
	{
		.xen = XENCAMERA_XFER_FUNC_SMPTE2084,
		.v4l2 = V4L2_XFER_FUNC_SMPTE2084,
	},
};

static const struct xen_to_v4l2 XEN_YCBCR_ENC_TO_V4L2[] = {
	{
		.xen = XENCAMERA_YCBCR_ENC_IGNORE,
		.v4l2 = V4L2_YCBCR_ENC_DEFAULT,
	},
	{
		.xen = XENCAMERA_YCBCR_ENC_601,
		.v4l2 = V4L2_YCBCR_ENC_601,
	},
	{
		.xen = XENCAMERA_YCBCR_ENC_709,
		.v4l2 = V4L2_YCBCR_ENC_709,
	},
	{
		.xen = XENCAMERA_YCBCR_ENC_XV601,
		.v4l2 = V4L2_YCBCR_ENC_XV601,
	},
	{
		.xen = XENCAMERA_YCBCR_ENC_XV709,
		.v4l2 = V4L2_YCBCR_ENC_XV709,
	},
	{
		.xen = XENCAMERA_YCBCR_ENC_BT2020,
		.v4l2 = V4L2_YCBCR_ENC_BT2020,
	},
	{
		.xen = XENCAMERA_YCBCR_ENC_BT2020_CONST_LUM,
		.v4l2 = V4L2_YCBCR_ENC_BT2020_CONST_LUM,
	},
};

static const struct xen_to_v4l2 XEN_QUANTIZATION_TO_V4L2[] = {
	{
		.xen = XENCAMERA_QUANTIZATION_DEFAULT,
		.v4l2 = V4L2_QUANTIZATION_DEFAULT,
	},
	{
		.xen = XENCAMERA_QUANTIZATION_FULL_RANGE,
		.v4l2 = V4L2_QUANTIZATION_FULL_RANGE,
	},
	{
		.xen = XENCAMERA_QUANTIZATION_LIM_RANGE,
		.v4l2 = V4L2_QUANTIZATION_LIM_RANGE,
	},
};

static int xen_to_v4l2(int xen, const struct xen_to_v4l2 *table,
		       size_t table_sz)
{
	int i;

	for (i = 0; i < table_sz; i++)
		if (table[i].xen == xen)
			return table[i].v4l2;
	return -EINVAL;
}

static int v4l2_to_xen(int v4l2, const struct xen_to_v4l2 *table,
		       size_t table_sz)
{
	int i;

	for (i = 0; i < table_sz; i++)
		if (table[i].v4l2 == v4l2)
			return table[i].xen;
	return -EINVAL;
}

int xen_camera_front_v4l2_to_v4l2_cid(int xen_type)
{
	return xen_to_v4l2(xen_type, XEN_TYPE_TO_V4L2_CID,
			   ARRAY_SIZE(XEN_TYPE_TO_V4L2_CID));
}

int xen_camera_front_v4l2_to_xen_type(int v4l2_cid)
{
	return v4l2_to_xen(v4l2_cid, XEN_TYPE_TO_V4L2_CID,
			   ARRAY_SIZE(XEN_TYPE_TO_V4L2_CID));
}

static int xen_buf_layout_to_format(struct xen_camera_front_info *front_info,
				    struct v4l2_pix_format *sp)
{
	struct xencamera_buf_get_layout_resp buf_layout;
	int ret;

	ret =  xen_camera_front_get_buf_layout(front_info, &buf_layout);
	if (ret < 0)
		return ret;

	if (buf_layout.num_planes != 1) {
		dev_err(&front_info->xb_dev->dev,
			"Unsupported number of planes %d\n",
			buf_layout.num_planes);
		return -EINVAL;
	}

	sp->bytesperline = buf_layout.plane_stride[0];
	sp->sizeimage = buf_layout.plane_size[0];
	return 0;
}

static void buf_list_return_queued(struct xen_camera_front_v4l2_info *v4l2_info,
				   enum vb2_buffer_state state)
{
	struct xen_camera_buffer *buf;

	mutex_lock(&v4l2_info->bufs_lock);
	list_for_each_entry(buf, &v4l2_info->bufs_list, list) {
		if (buf->is_queued) {
			vb2_buffer_done(&buf->vb.vb2_buf, state);
			buf->is_queued = false;
		}
	}
	mutex_unlock(&v4l2_info->bufs_lock);
}

void xen_camera_front_v4l2_on_frame(struct xen_camera_front_info *front_info,
				    struct xencamera_frame_avail_evt *evt)
{
	struct xen_camera_front_v4l2_info *v4l2_info = front_info->v4l2_info;
	struct xen_camera_buffer *buf, *xen_buf = NULL;

	mutex_lock(&v4l2_info->bufs_lock);
	list_for_each_entry(buf, &v4l2_info->bufs_list, list) {
		if (buf->vb.vb2_buf.index == evt->index) {
			xen_buf = buf;
			break;
		}
	}

	if (unlikely(!xen_buf)) {
		dev_err(&front_info->xb_dev->dev,
			"Buffer with index %d not found\n", evt->index);
		goto out;
	}

	/*
	 * This is not an error, but because we can temporarily get
	 * out of sync with the backend (for example when we disconnect),
	 * so then just drop the event.
	 */
	if (unlikely(!xen_buf->is_queued))
		goto out;

	xen_buf->is_queued = false;
	xen_buf->vb.vb2_buf.timestamp = ktime_get_ns();
	xen_buf->vb.sequence = evt->seq_num;
	vb2_buffer_done(&xen_buf->vb.vb2_buf, VB2_BUF_STATE_DONE);

out:
	mutex_unlock(&v4l2_info->bufs_lock);
}

void xen_camera_front_v4l2_on_ctrl(struct xen_camera_front_info *front_info,
				   struct xencamera_ctrl_value *evt)
{
	struct xen_camera_front_v4l2_info *v4l2_info = front_info->v4l2_info;
	struct xen_camera_front_cfg_card *cfg = &front_info->cfg;
	int v4l2_cid, i;

	v4l2_cid = xen_camera_front_v4l2_to_v4l2_cid(evt->type);
	if (v4l2_cid < 0) {
		dev_err(&front_info->xb_dev->dev,
			"Drop event with wrong Xen control type: %d\n",
			evt->type);
		return;
	}

	for (i = 0; i < cfg->num_controls; i++)
		if (v4l2_info->ctrls[i]->id == v4l2_cid)
			v4l2_ctrl_s_ctrl(v4l2_info->ctrls[i], evt->value);
}

/*
 * Called from VIDIOC_REQBUFS() and VIDIOC_CREATE_BUFS()
 * handlers before memory allocation. It can be called
 * twice: if the original number of requested buffers
 * could not be allocated, then it will be called a
 * second time with the actually allocated number of
 * buffers to verify if that is OK.
 * The driver should return the required number of buffers
 * in \*num_buffers, the required number of planes per
 * buffer in \*num_planes, the size of each plane should be
 * set in the sizes\[\] array and optional per-plane
 * allocator specific device in the alloc_devs\[\] array.
 * When called from VIDIOC_REQBUFS(), \*num_planes == 0,
 * the driver has to use the currently configured format to
 * determine the plane sizes and \*num_buffers is the total
 * number of buffers that are being allocated. When called
 * from VIDIOC_CREATE_BUFS(), \*num_planes != 0 and it
 * describes the requested number of planes and sizes\[\]
 * contains the requested plane sizes. In this case
 * \*num_buffers are being allocated additionally to
 * q->num_buffers. If either \*num_planes or the requested
 * sizes are invalid callback must return %-EINVAL.
 */
static int queue_setup(struct vb2_queue *vq,
		       unsigned int *nbuffers, unsigned int *nplanes,
		       unsigned int sizes[], struct device *alloc_devs[])
{
	struct xen_camera_front_v4l2_info *v4l2_info = vb2_get_drv_priv(vq);
	int min_bufs, max_bufs, ret;

	min_bufs = vq->min_buffers_needed;
	max_bufs = v4l2_info->front_info->cfg.max_buffers;
	if (*nbuffers < min_bufs)
		*nbuffers = min_bufs;
	if (*nbuffers > max_bufs)
		*nbuffers = max_bufs;

	/* Check if backend can handle that many buffers. */
	if (likely(!v4l2_info->unplugged)) {
		ret = xen_camera_front_buf_request(v4l2_info->front_info,
						   *nbuffers);
		if (ret < 0)
			return ret;
		*nbuffers = ret;
	} else {
		ret = 0;
	}

	*nbuffers = ret;

	if (*nplanes)
		return sizes[0] < v4l2_info->v4l2_buffer_sz ? -EINVAL : 0;

	*nplanes = 1;

	sizes[0] = v4l2_info->v4l2_buffer_sz;

	return 0;
}

static int buffer_init(struct vb2_buffer *vb)
{
	struct xen_camera_front_v4l2_info *v4l2_info =
		vb2_get_drv_priv(vb->vb2_queue);
	struct xen_camera_buffer *xen_buf = to_xen_camera_buffer(vb);
	struct sg_table *sgt;
	int ret;

	if (unlikely(v4l2_info->unplugged))
		return -ENODEV;

	if (vb2_plane_size(vb, 0) < v4l2_info->v4l2_buffer_sz) {
		dev_err(&v4l2_info->front_info->xb_dev->dev,
			"Buffer too small (%lu < %lu)\n",
			vb2_plane_size(vb, 0), v4l2_info->v4l2_buffer_sz);
		return -EINVAL;
	}

	/* We only support a single plane. */
	sgt = vb2_dma_sg_plane_desc(vb, 0);
	if (!sgt)
		return -EFAULT;

	ret = xen_camera_front_buf_create(v4l2_info->front_info,
					  &xen_buf->shbuf, vb->index, sgt);
	if (ret < 0)
		return ret;

	mutex_lock(&v4l2_info->bufs_lock);
	list_add(&xen_buf->list, &v4l2_info->bufs_list);
	mutex_unlock(&v4l2_info->bufs_lock);
	return 0;
}

static void buffer_cleanup(struct vb2_buffer *vb)
{
	struct xen_camera_front_v4l2_info *v4l2_info =
		vb2_get_drv_priv(vb->vb2_queue);
	struct xen_camera_buffer *xen_buf = to_xen_camera_buffer(vb);
	int ret;

	if (likely(!v4l2_info->unplugged)) {
		ret = xen_camera_front_buf_destroy(v4l2_info->front_info,
						   &xen_buf->shbuf, vb->index);
		if (ret < 0)
			dev_err(&v4l2_info->front_info->xb_dev->dev,
				"Failed to cleanup buffer with index %d: %d\n",
				vb->index, ret);
	}

	mutex_lock(&v4l2_info->bufs_lock);
	list_del(&xen_buf->list);
	mutex_unlock(&v4l2_info->bufs_lock);
}

static int buffer_prepare(struct vb2_buffer *vb)
{
	struct xen_camera_front_v4l2_info *v4l2_info =
		vb2_get_drv_priv(vb->vb2_queue);
	struct xen_camera_buffer *xen_buf = to_xen_camera_buffer(vb);
	size_t size = v4l2_info->v4l2_buffer_sz;
	int ret;

	if (unlikely(v4l2_info->unplugged))
		return -ENODEV;

	if (vb2_plane_size(vb, 0) < size) {
		dev_err(&v4l2_info->front_info->xb_dev->dev,
			"Buffer too small (%lu < %lu)\n",
			vb2_plane_size(vb, 0), size);
		return -EINVAL;
	}

	vb2_set_plane_payload(vb, 0, size);

	/*
	 * FIXME: we can have an error here while communicating to
	 * the backend, but .buf_queue callback doesn't allow us to return
	 * any error code: queue the buffer to the backend now, so we can
	 * make sure we do not fail later on.
	 */
	ret = xen_camera_front_buf_queue(v4l2_info->front_info, vb->index);
	if (ret < 0) {
		dev_err(&v4l2_info->front_info->xb_dev->dev,
			"Failed to queue buffer with index %d: %d\n",
			vb->index, ret);
		return ret;
	}

	mutex_lock(&v4l2_info->bufs_lock);
	xen_buf->is_queued = true;
	mutex_unlock(&v4l2_info->bufs_lock);
	return 0;
}

static void buffer_finish(struct vb2_buffer *vb)
{
	struct xen_camera_front_v4l2_info *v4l2_info =
		vb2_get_drv_priv(vb->vb2_queue);
	int ret;

	if (unlikely(v4l2_info->unplugged))
		return;

	ret = xen_camera_front_buf_dequeue(v4l2_info->front_info, vb->index);
	if (ret < 0)
		dev_err(&v4l2_info->front_info->xb_dev->dev,
			"Failed to dequeue buffer with index %d: %d\n",
			vb->index, ret);
}

static void buffer_queue(struct vb2_buffer *vb)
{
	struct xen_camera_front_v4l2_info *v4l2_info =
		vb2_get_drv_priv(vb->vb2_queue);
	struct xen_camera_buffer *xen_buf = to_xen_camera_buffer(vb);

	if (unlikely(v4l2_info->unplugged)) {
		vb2_buffer_done(&xen_buf->vb.vb2_buf, VB2_BUF_STATE_ERROR);
		return;
	}

	mutex_lock(&v4l2_info->bufs_lock);
	xen_buf->is_queued = true;
	mutex_unlock(&v4l2_info->bufs_lock);
}

static int streaming_start(struct vb2_queue *vq, unsigned int count)
{
	struct xen_camera_front_v4l2_info *v4l2_info = vb2_get_drv_priv(vq);
	int ret;


	if (unlikely(v4l2_info->unplugged))
		return -ENODEV;

	ret = xen_camera_front_stream_start(v4l2_info->front_info);
	if (ret < 0)
		buf_list_return_queued(v4l2_info, VB2_BUF_STATE_QUEUED);
	return ret;
}

static void streaming_stop(struct vb2_queue *vq)
{
	struct xen_camera_front_v4l2_info *v4l2_info = vb2_get_drv_priv(vq);

	buf_list_return_queued(v4l2_info, VB2_BUF_STATE_ERROR);

	if (likely(!v4l2_info->unplugged))
		xen_camera_front_stream_stop(v4l2_info->front_info);
}

static const struct vb2_ops qops = {
	.queue_setup		= queue_setup,

	.buf_prepare		= buffer_prepare,
	.buf_queue		= buffer_queue,
	.buf_finish		= buffer_finish,
	.buf_init		= buffer_init,
	.buf_cleanup		= buffer_cleanup,

	.start_streaming	= streaming_start,
	.stop_streaming		= streaming_stop,

	.wait_prepare		= vb2_ops_wait_prepare,
	.wait_finish		= vb2_ops_wait_finish,
};

static int ioctl_querycap(struct file *file, void *fh,
			  struct v4l2_capability *cap)
{
	strlcpy(cap->driver, KBUILD_MODNAME, sizeof(cap->driver));
	strlcpy(cap->card, "V4L2 para-virtualized camera", sizeof(cap->card));
	strlcpy(cap->bus_info, "platform:xen_bus", sizeof(cap->bus_info));
	return 0;
}

static struct xen_camera_front_cfg_format *
enum_get_format(struct xen_camera_front_cfg_card *cfg, u32 pixel_format)
{
	int i;

	for (i = 0; i < cfg->num_formats; i++) {
		struct xen_camera_front_cfg_format *format = &cfg->format[i];

		if (format->pixel_format == pixel_format)
			return format;
	}
	return NULL;
}

static struct xen_camera_front_cfg_resolution *
enum_get_resolution(struct xen_camera_front_cfg_format *format,
		    int width, int height)
{
	int i;

	for (i = 0; i < format->num_resolutions; i++) {
		struct xen_camera_front_cfg_resolution *r =
			&format->resolution[i];

		if ((r->width == width) && (r->height == height))
			return r;
	}
	return NULL;
}

static int xen_cfg_to_v4l2_fmt(struct xen_camera_front_v4l2_info *v4l2_info,
			       struct xencamera_config_resp *cfg_resp,
			       struct v4l2_format *f)
{
	struct v4l2_pix_format *sp = &f->fmt.pix;
	int ret;

	sp->width = cfg_resp->width;
	sp->height = cfg_resp->height;
	sp->pixelformat = cfg_resp->pixel_format;

	sp->field = V4L2_FIELD_NONE;

	ret = xen_to_v4l2(cfg_resp->colorspace, XEN_COLORSPACE_TO_V4L2,
			  ARRAY_SIZE(XEN_COLORSPACE_TO_V4L2));
	if (ret < 0)
		return ret;
	sp->colorspace = ret;

	ret = xen_to_v4l2(cfg_resp->xfer_func, XEN_XFER_FUNC_TO_V4L2,
			  ARRAY_SIZE(XEN_XFER_FUNC_TO_V4L2));
	if (ret < 0)
		return ret;
	sp->xfer_func = ret;

	ret = xen_to_v4l2(cfg_resp->ycbcr_enc, XEN_YCBCR_ENC_TO_V4L2,
			  ARRAY_SIZE(XEN_YCBCR_ENC_TO_V4L2));
	if (ret < 0)
		return ret;
	sp->ycbcr_enc = ret;

	ret = xen_to_v4l2(cfg_resp->quantization, XEN_QUANTIZATION_TO_V4L2,
			  ARRAY_SIZE(XEN_QUANTIZATION_TO_V4L2));
	if (ret < 0)
		return ret;
	sp->quantization = ret;

	return 0;
}

static int v4l2_fmt_to_xen_cfg(struct xen_camera_front_v4l2_info *v4l2_info,
			       struct v4l2_format *f,
			       struct xencamera_config_req *cfg_req)
{
	struct v4l2_pix_format *sp = &f->fmt.pix;

	cfg_req->width = sp->width;
	cfg_req->height = sp->height;
	cfg_req->pixel_format = sp->pixelformat;
	return 0;
}

static int set_get_fmt_tail(struct xen_camera_front_v4l2_info *v4l2_info,
			    struct xencamera_config_resp *cfg_resp,
			    struct v4l2_format *f,
			    bool with_layout)
{
	struct v4l2_pix_format *sp = &f->fmt.pix;
	int ret;

	ret =  xen_cfg_to_v4l2_fmt(v4l2_info, cfg_resp, f);
	if (ret < 0)
		return ret;

	if (with_layout) {
		ret = xen_buf_layout_to_format(v4l2_info->front_info, sp);
		if (ret < 0)
			return ret;
	}

	return 0;
}

static int get_format_helper(struct xen_camera_front_v4l2_info *v4l2_info,
			     struct xencamera_config_resp *cfg_resp,
			     struct v4l2_format *f, bool with_layout)
{
	int ret;

	ret = xen_camera_front_get_config(v4l2_info->front_info, cfg_resp);
	if (ret < 0)
		return ret;

	return set_get_fmt_tail(v4l2_info, cfg_resp, f, with_layout);
}

static int set_format(struct xen_camera_front_v4l2_info *v4l2_info,
		      struct v4l2_format *f,
		      bool is_cfg_validate)
{
	struct xencamera_config_req cfg_req;
	struct xencamera_config_resp cfg_resp;
	int ret;

	/*
	 * It is not allowed to change the format while buffers used for
	 * streaming have already been allocated.
	 */
	if (!is_cfg_validate && vb2_is_busy(&v4l2_info->queue))
		return -EBUSY;

	ret = v4l2_fmt_to_xen_cfg(v4l2_info, f, &cfg_req);
	/*
	 * If the requested format is obviously wrong, then return
	 * the current format as seen by the backend.
	 */
	if (ret < 0)
		return get_format_helper(v4l2_info, &cfg_resp, f, true);

	/*
	 * N.B. During format set/validate if we fail because of backend
	 * communication error, then return error code.
	 * If the format is not accepted by the backend then comply
	 * to V4L2 spec which says we shouldn't return an error here,
	 * but instead provide the user-space with what we think is ok.
	 */
	if (is_cfg_validate)
		ret = xen_camera_front_validate_config(v4l2_info->front_info,
						       &cfg_req, &cfg_resp);
	else
		ret = xen_camera_front_set_config(v4l2_info->front_info,
						  &cfg_req, &cfg_resp);

	if (ret < 0) {
		if (ret == -EIO || ret == -ETIMEDOUT)
			return ret;

		return get_format_helper(v4l2_info, &cfg_resp, f, true);
	}

	ret = set_get_fmt_tail(v4l2_info, &cfg_resp, f, true);
	if (ret < 0)
		return ret;

	/* Remember the negotiated buffer size. */
	v4l2_info->v4l2_buffer_sz = f->fmt.pix.sizeimage;

	return 0;
}

static int ioctl_s_fmt_vid_cap(struct file *file, void *fh,
			       struct v4l2_format *f)
{
	struct xen_camera_front_v4l2_info *v4l2_info = video_drvdata(file);

	return set_format(v4l2_info, f, false);
}

static int ioctl_try_fmt_vid_cap(struct file *file, void *fh,
				 struct v4l2_format *f)
{
	struct xen_camera_front_v4l2_info *v4l2_info = video_drvdata(file);

	return set_format(v4l2_info, f, true);
}

static int ioctl_g_fmt_vid_cap(struct file *file,
			       void *fh, struct v4l2_format *f)
{
	struct xen_camera_front_v4l2_info *v4l2_info = video_drvdata(file);
	struct xencamera_config_resp cfg_resp;

	return get_format_helper(v4l2_info, &cfg_resp, f, true);
}

static int ioctl_enum_fmt_vid_cap(struct file *file, void *fh,
				  struct v4l2_fmtdesc *f)
{
	struct xen_camera_front_v4l2_info *v4l2_info = video_drvdata(file);
	struct xen_camera_front_cfg_card *cfg = &v4l2_info->front_info->cfg;

	if (f->index >= cfg->num_formats)
		return -EINVAL;

	f->pixelformat = cfg->format[f->index].pixel_format;
	return 0;
}

int ioctl_enum_framesizes(struct file *file, void *fh,
			  struct v4l2_frmsizeenum *fsize)
{
	struct xen_camera_front_v4l2_info *v4l2_info = video_drvdata(file);
	struct xen_camera_front_cfg_card *cfg = &v4l2_info->front_info->cfg;
	struct xen_camera_front_cfg_format *format;

	format = enum_get_format(cfg, fsize->pixel_format);
	if (!format || (fsize->index >= format->num_resolutions))
		return -EINVAL;

	fsize->type = V4L2_FRMSIZE_TYPE_DISCRETE;
	fsize->discrete.width = format->resolution[fsize->index].width;
	fsize->discrete.height = format->resolution[fsize->index].height;
	return 0;
}

int ioctl_enum_frameintervals(struct file *file, void *fh,
			      struct v4l2_frmivalenum *fival)
{
	struct xen_camera_front_v4l2_info *v4l2_info = video_drvdata(file);
	struct xen_camera_front_cfg_card *cfg = &v4l2_info->front_info->cfg;
	struct xen_camera_front_cfg_format *format;
	struct xen_camera_front_cfg_resolution *resolution;

	format = enum_get_format(cfg, fival->pixel_format);
	if (!format)
		return -EINVAL;

	resolution = enum_get_resolution(format, fival->width, fival->height);
	if (!resolution || (fival->index >= resolution->num_frame_rates))
		return -EINVAL;

	fival->type = V4L2_FRMIVAL_TYPE_DISCRETE;
	/* Interval is inverse to frame rate. */
	fival->discrete.denominator =
		resolution->frame_rate[fival->index].numerator;
	fival->discrete.numerator =
		resolution->frame_rate[fival->index].denominator;
	return 0;
}

static int ioctl_enum_input(struct file *file, void *fh,
			    struct v4l2_input *inp)
{
	if (inp->index > 0)
		return -EINVAL;

	strlcpy(inp->name, "Xen PV camera", sizeof(inp->name));
	inp->type = V4L2_INPUT_TYPE_CAMERA;
	return 0;
}

static int ioctl_g_input(struct file *file, void *fh, unsigned int *i)
{
	*i = 0;
	return 0;
}

static int ioctl_s_input(struct file *file, void *fh, unsigned int i)
{
	return (i > 0) ? -EINVAL : 0;
}

static int set_get_param_tail(struct xen_camera_front_v4l2_info *v4l2_info,
			      struct v4l2_streamparm *parm)
{
	struct xencamera_config_resp cfg_resp;
	struct v4l2_format f;
	int ret;

	/*
	 * We are only interested in the frame rate, no need to request
	 * buffer layout then.
	 */
	ret = get_format_helper(v4l2_info, &cfg_resp, &f, false);
	if (ret < 0)
		return ret;

	parm->parm.capture.capability = V4L2_CAP_TIMEPERFRAME;
	/* Interval is inverse to frame rate. */
	parm->parm.capture.timeperframe.denominator =
		cfg_resp.frame_rate_numer;
	parm->parm.capture.timeperframe.numerator =
		cfg_resp.frame_rate_denom;
	parm->parm.capture.readbuffers = 0;

	return 0;
}

static int ioctl_g_parm(struct file *file, void *priv,
			struct v4l2_streamparm *parm)
{
	struct xen_camera_front_v4l2_info *v4l2_info = video_drvdata(file);

	if (parm->type != V4L2_BUF_TYPE_VIDEO_CAPTURE)
		return -EINVAL;

	return set_get_param_tail(v4l2_info, parm);
}

static int ioctl_s_parm(struct file *file, void *priv,
			struct v4l2_streamparm *parm)
{
	struct xen_camera_front_v4l2_info *v4l2_info = video_drvdata(file);
	struct xencamera_frame_rate_req frame_rate_req;
	int ret;

	if (parm->type != V4L2_BUF_TYPE_VIDEO_CAPTURE)
		return -EINVAL;

	if (vb2_is_streaming(&v4l2_info->queue))
		return -EBUSY;

	/* Interval is inverse to frame rate. */
	frame_rate_req.frame_rate_denom =
		parm->parm.capture.timeperframe.numerator;
	frame_rate_req.frame_rate_numer =
		parm->parm.capture.timeperframe.denominator;

	ret = xen_camera_front_set_frame_rate(v4l2_info->front_info,
					      &frame_rate_req);
	if (ret < 0)
		return ret;

	/* Read back the configuration and report the actual frame rate set. */
	return set_get_param_tail(v4l2_info, parm);
}

static const struct v4l2_ioctl_ops ioctl_ops = {
	.vidioc_querycap = ioctl_querycap,
	.vidioc_s_fmt_vid_cap = ioctl_s_fmt_vid_cap,
	.vidioc_try_fmt_vid_cap = ioctl_try_fmt_vid_cap,
	.vidioc_g_fmt_vid_cap = ioctl_g_fmt_vid_cap,
	.vidioc_enum_fmt_vid_cap = ioctl_enum_fmt_vid_cap,

	.vidioc_enum_framesizes = ioctl_enum_framesizes,
	.vidioc_enum_frameintervals = ioctl_enum_frameintervals,

	.vidioc_enum_input = ioctl_enum_input,
	.vidioc_g_input = ioctl_g_input,
	.vidioc_s_input = ioctl_s_input,

	.vidioc_g_parm = ioctl_g_parm,
	.vidioc_s_parm = ioctl_s_parm,

	.vidioc_reqbufs = vb2_ioctl_reqbufs,
	.vidioc_create_bufs = vb2_ioctl_create_bufs,
	.vidioc_querybuf = vb2_ioctl_querybuf,
	.vidioc_qbuf = vb2_ioctl_qbuf,
	.vidioc_dqbuf = vb2_ioctl_dqbuf,
	.vidioc_expbuf = vb2_ioctl_expbuf,
	.vidioc_streamon = vb2_ioctl_streamon,
	.vidioc_streamoff = vb2_ioctl_streamoff,

	.vidioc_log_status = v4l2_ctrl_log_status,
	.vidioc_subscribe_event = v4l2_ctrl_subscribe_event,
	.vidioc_unsubscribe_event = v4l2_event_unsubscribe,
};

static const struct v4l2_file_operations fops = {
	.owner = THIS_MODULE,
	.open = v4l2_fh_open,
	.release = vb2_fop_release,
	.unlocked_ioctl = video_ioctl2,
	.read = vb2_fop_read,
	.mmap = vb2_fop_mmap,
	.poll = vb2_fop_poll,
};

static int s_ctrl(struct v4l2_ctrl *ctrl)
{
	struct xen_camera_front_v4l2_info *v4l2_info =
		container_of(ctrl->handler, struct xen_camera_front_v4l2_info,
			     ctrl_handler);

	switch (ctrl->id) {
	case V4L2_CID_BRIGHTNESS:
		/* fall-through */
	case V4L2_CID_CONTRAST:
		/* fall-through */
	case V4L2_CID_SATURATION:
		/* fall-through */
	case V4L2_CID_HUE:
		return xen_camera_front_set_control(v4l2_info->front_info,
						    ctrl->id, ctrl->val);

	default:
		break;
	}

	return -EINVAL;
}

static const struct v4l2_ctrl_ops ctrl_ops = {
	.s_ctrl = s_ctrl,
};

static int init_controls(struct xen_camera_front_cfg_card *cfg,
			 struct xen_camera_front_v4l2_info *v4l2_info)
{
	struct v4l2_ctrl_handler *hdl = &v4l2_info->ctrl_handler;
	int i, ret;

	v4l2_ctrl_handler_init(hdl, cfg->num_controls);

	for (i = 0; i < cfg->num_controls; i++)
		v4l2_info->ctrls[i] =
			v4l2_ctrl_new_std(hdl, &ctrl_ops,
					  cfg->ctrl[i].v4l2_cid,
					  cfg->ctrl[i].minimum,
					  cfg->ctrl[i].maximum,
					  cfg->ctrl[i].step,
					  cfg->ctrl[i].default_value);
	if (hdl->error) {
		ret = hdl->error;
		v4l2_ctrl_handler_free(&v4l2_info->ctrl_handler);
		return ret;
	}
	v4l2_info->v4l2_dev.ctrl_handler = hdl;
	return 0;
}

static void xen_video_device_release(struct video_device *vdev)
{
	struct xen_camera_front_v4l2_info *v4l2_info = video_get_drvdata(vdev);

	v4l2_ctrl_handler_free(v4l2_info->v4l2_dev.ctrl_handler);
	v4l2_info->v4l2_dev.ctrl_handler = NULL;
	v4l2_device_unregister(&v4l2_info->v4l2_dev);
	kfree(v4l2_info);
}

int xen_camera_front_v4l2_init(struct xen_camera_front_info *front_info)
{
	struct device *dev = &front_info->xb_dev->dev;
	struct xen_camera_front_v4l2_info *v4l2_info;
	struct video_device *vdev;
	struct vb2_queue *q;
	int ret;

	v4l2_info = kzalloc(sizeof(*v4l2_info), GFP_KERNEL);
	if (!v4l2_info)
		return -ENOMEM;

	v4l2_info->front_info = front_info;

	mutex_init(&v4l2_info->v4l2_lock);
	mutex_init(&v4l2_info->vb_queue_lock);

	INIT_LIST_HEAD(&v4l2_info->bufs_list);
	mutex_init(&v4l2_info->bufs_lock);

	ret = v4l2_device_register(dev, &v4l2_info->v4l2_dev);
	if (ret < 0)
		goto fail_device_register;

	if (front_info->cfg.num_controls) {
		ret = init_controls(&front_info->cfg, v4l2_info);
		if (ret < 0)
			goto fail_init_controls;
	}

	q = &v4l2_info->queue;

	q->type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	q->io_modes = VB2_MMAP | VB2_DMABUF | VB2_USERPTR;
	q->dev = dev;
	q->drv_priv = v4l2_info;
	q->buf_struct_size = sizeof(struct xen_camera_buffer);
	q->ops = &qops;
	/*
	 * It is better for us to work with vb2_dma_sg_memops
	 * rather than vb2_dma_contig_memops as this might relax
	 * memory subsystem pressure.
	 */
	q->mem_ops = &vb2_dma_sg_memops;
	q->timestamp_flags = V4L2_BUF_FLAG_TIMESTAMP_MONOTONIC;
	q->min_buffers_needed = 2;
	q->lock = &v4l2_info->vb_queue_lock;
	ret = vb2_queue_init(q);
	if (ret)
		goto fail_queue_init;

	vdev = &v4l2_info->vdev;
	strlcpy(vdev->name, KBUILD_MODNAME, sizeof(vdev->name));
	vdev->release = xen_video_device_release;
	vdev->fops = &fops;
	vdev->ioctl_ops = &ioctl_ops;
	vdev->device_caps = V4L2_CAP_VIDEO_CAPTURE | V4L2_CAP_STREAMING;
	vdev->lock = &v4l2_info->v4l2_lock;
	vdev->queue = q;
	vdev->v4l2_dev = &v4l2_info->v4l2_dev;
	video_set_drvdata(vdev, v4l2_info);

	ret = video_register_device(vdev, VFL_TYPE_GRABBER, -1);
	if (ret < 0)
		goto fail_register_video;

	front_info->v4l2_info = v4l2_info;

	dev_info(dev, "V4L2 " XENCAMERA_DRIVER_NAME " driver loaded\n");

	return 0;

fail_register_video:
	vb2_queue_release(q);
fail_queue_init:
	v4l2_ctrl_handler_free(v4l2_info->v4l2_dev.ctrl_handler);
	v4l2_info->v4l2_dev.ctrl_handler = NULL;
fail_init_controls:
	v4l2_device_unregister(&v4l2_info->v4l2_dev);
fail_device_register:
	kfree(v4l2_info);
	return ret;
}

void xen_camera_front_v4l2_fini(struct xen_camera_front_info *front_info)
{
	struct xen_camera_front_v4l2_info *v4l2_info = front_info->v4l2_info;
	struct xen_camera_buffer *buf;

	if (!v4l2_info)
		return;

	mutex_lock(&v4l2_info->vb_queue_lock);
	mutex_lock(&v4l2_info->v4l2_lock);

	if (v4l2_info->unplugged) {
		mutex_unlock(&v4l2_info->v4l2_lock);
		mutex_unlock(&v4l2_info->vb_queue_lock);
	}
	v4l2_info->unplugged = true;
	v4l2_device_disconnect(&v4l2_info->v4l2_dev);

	/* Destroy all shared buffers if any. */
	mutex_lock(&v4l2_info->bufs_lock);
	list_for_each_entry(buf, &v4l2_info->bufs_list, list)
		xen_camera_front_destroy_shbuf(&buf->shbuf);
	mutex_unlock(&v4l2_info->bufs_lock);

	video_unregister_device(&v4l2_info->vdev);

	mutex_unlock(&v4l2_info->v4l2_lock);
	mutex_unlock(&v4l2_info->vb_queue_lock);

	v4l2_device_put(&v4l2_info->v4l2_dev);
}

