// SPDX-License-Identifier: GPL-2.0 OR MIT

/*
 * Xen para-virtual camera device
 *
 * Copyright (C) 2018 EPAM Systems Inc.
 *
 * Author: Oleksandr Andrushchenko <oleksandr_andrushchenko@epam.com>
 */

#include <linux/delay.h>
#include <linux/module.h>
#include <linux/of_device.h>

#include <xen/platform_pci.h>
#include <xen/xen.h>
#include <xen/xenbus.h>

#include "xen_camera_front.h"
#include "xen_camera_front_evtchnl.h"
#include "xen_camera_front_v4l2.h"

void xen_camera_front_destroy_shbuf(struct xen_camera_front_shbuf *shbuf)
{
	xen_front_pgdir_shbuf_unmap(&shbuf->pgdir);
	xen_front_pgdir_shbuf_free(&shbuf->pgdir);

	kfree(shbuf->pages);
	shbuf->pages = NULL;
}

static struct xencamera_req *
be_prepare_req(struct xen_camera_front_evtchnl *evtchnl, u8 operation)
{
	struct xencamera_req *req;

	req = RING_GET_REQUEST(&evtchnl->u.req.ring,
			       evtchnl->u.req.ring.req_prod_pvt);
	req->operation = operation;
	req->id = evtchnl->evt_next_id++;
	evtchnl->evt_id = req->id;
	return req;
}

static int be_stream_do_io(struct xen_camera_front_evtchnl *evtchnl,
			   struct xencamera_req *req)
{
	reinit_completion(&evtchnl->u.req.completion);
	if (unlikely(evtchnl->state != EVTCHNL_STATE_CONNECTED))
		return -EIO;

	xen_camera_front_evtchnl_flush(evtchnl);
	return 0;
}

static int be_stream_wait_io(struct xen_camera_front_evtchnl *evtchnl)
{
	if (wait_for_completion_timeout(&evtchnl->u.req.completion,
			msecs_to_jiffies(XEN_CAMERA_FRONT_WAIT_BACK_MS)) <= 0)
		return -ETIMEDOUT;

	return evtchnl->u.req.resp_status;
}

static int set_config_helper(struct xen_camera_front_info *front_info,
			     struct xencamera_config_req *cfg_req,
			     struct xencamera_config_resp *cfg_resp,
			     u8 op)
{
	struct xen_camera_front_evtchnl *evtchnl;
	struct xencamera_req *req;
	unsigned long flags;
	int ret;

	evtchnl = &front_info->evt_pair.req;
	if (unlikely(!evtchnl))
		return -EIO;

	mutex_lock(&evtchnl->u.req.req_io_lock);

	spin_lock_irqsave(&front_info->io_lock, flags);
	req = be_prepare_req(evtchnl, op);
	req->req.config = *cfg_req;

	ret = be_stream_do_io(evtchnl, req);
	spin_unlock_irqrestore(&front_info->io_lock, flags);

	if (ret == 0)
		ret = be_stream_wait_io(evtchnl);

	*cfg_resp = evtchnl->u.req.resp.resp.config;

	mutex_unlock(&evtchnl->u.req.req_io_lock);
	return ret;
}

int xen_camera_front_set_config(struct xen_camera_front_info *front_info,
				struct xencamera_config_req *cfg_req,
				struct xencamera_config_resp *cfg_resp)
{
	return set_config_helper(front_info, cfg_req, cfg_resp,
				 XENCAMERA_OP_CONFIG_SET);
}

int xen_camera_front_validate_config(struct xen_camera_front_info *front_info,
				     struct xencamera_config_req *cfg_req,
				     struct xencamera_config_resp *cfg_resp)
{
	return set_config_helper(front_info, cfg_req, cfg_resp,
				 XENCAMERA_OP_CONFIG_VALIDATE);
}

int xen_camera_front_get_config(struct xen_camera_front_info *front_info,
				struct xencamera_config_resp *cfg_resp)
{
	struct xen_camera_front_evtchnl *evtchnl;
	struct xencamera_req *req;
	unsigned long flags;
	int ret;

	evtchnl = &front_info->evt_pair.req;
	if (unlikely(!evtchnl))
		return -EIO;

	mutex_lock(&evtchnl->u.req.req_io_lock);

	spin_lock_irqsave(&front_info->io_lock, flags);
	req = be_prepare_req(evtchnl, XENCAMERA_OP_CONFIG_GET);

	ret = be_stream_do_io(evtchnl, req);
	spin_unlock_irqrestore(&front_info->io_lock, flags);

	if (ret == 0)
		ret = be_stream_wait_io(evtchnl);

	*cfg_resp = evtchnl->u.req.resp.resp.config;

	mutex_unlock(&evtchnl->u.req.req_io_lock);
	return ret;
}

int xen_camera_front_set_frame_rate(struct xen_camera_front_info *front_info,
				    struct xencamera_frame_rate_req *frame_rate)
{
	struct xen_camera_front_evtchnl *evtchnl;
	struct xencamera_req *req;
	unsigned long flags;
	int ret;

	evtchnl = &front_info->evt_pair.req;
	if (unlikely(!evtchnl))
		return -EIO;

	mutex_lock(&evtchnl->u.req.req_io_lock);

	spin_lock_irqsave(&front_info->io_lock, flags);
	req = be_prepare_req(evtchnl, XENCAMERA_OP_FRAME_RATE_SET);
	req->req.frame_rate = *frame_rate;

	ret = be_stream_do_io(evtchnl, req);
	spin_unlock_irqrestore(&front_info->io_lock, flags);

	if (ret == 0)
		ret = be_stream_wait_io(evtchnl);

	mutex_unlock(&evtchnl->u.req.req_io_lock);
	return ret;
}

int xen_camera_front_set_control(struct xen_camera_front_info *front_info,
				 int v4l2_cid, s64 value)
{
	struct xen_camera_front_evtchnl *evtchnl;
	struct xencamera_req *req;
	unsigned long flags;
	int ret, xen_type;

	evtchnl = &front_info->evt_pair.req;
	if (unlikely(!evtchnl))
		return -EIO;

	xen_type = xen_camera_front_v4l2_to_xen_type(v4l2_cid);
	if (xen_type < 0)
		return xen_type;

	mutex_lock(&evtchnl->u.req.req_io_lock);

	spin_lock_irqsave(&front_info->io_lock, flags);
	req = be_prepare_req(evtchnl, XENCAMERA_OP_CTRL_SET);

	req->req.ctrl_value.type = xen_type;
	req->req.ctrl_value.value = value;

	ret = be_stream_do_io(evtchnl, req);
	spin_unlock_irqrestore(&front_info->io_lock, flags);

	if (ret == 0)
		ret = be_stream_wait_io(evtchnl);

	mutex_unlock(&evtchnl->u.req.req_io_lock);
	return ret;
}

int xen_camera_front_get_control(struct xen_camera_front_info *front_info,
				 int v4l2_cid, s64 *value)
{
	struct xen_camera_front_evtchnl *evtchnl;
	struct xencamera_req *req;
	unsigned long flags;
	int ret, xen_type;

	evtchnl = &front_info->evt_pair.req;
	if (unlikely(!evtchnl))
		return -EIO;

	xen_type = xen_camera_front_v4l2_to_xen_type(v4l2_cid);
	if (xen_type < 0)
		return xen_type;

	mutex_lock(&evtchnl->u.req.req_io_lock);

	spin_lock_irqsave(&front_info->io_lock, flags);
	req = be_prepare_req(evtchnl, XENCAMERA_OP_CTRL_GET);

	req->req.get_ctrl.type = xen_type;

	ret = be_stream_do_io(evtchnl, req);
	spin_unlock_irqrestore(&front_info->io_lock, flags);

	if (ret == 0)
		ret = be_stream_wait_io(evtchnl);

	*value = evtchnl->u.req.resp.resp.ctrl_value.value;

	mutex_unlock(&evtchnl->u.req.req_io_lock);
	return ret;
}

static int be_enum_control(struct xen_camera_front_info *front_info, int index,
			   struct xencamera_ctrl_enum_resp *resp)
{
	struct xen_camera_front_evtchnl *evtchnl;
	struct xencamera_req *req;
	unsigned long flags;
	int ret;

	evtchnl = &front_info->evt_pair.req;
	if (unlikely(!evtchnl))
		return -EIO;

	mutex_lock(&evtchnl->u.req.req_io_lock);

	spin_lock_irqsave(&front_info->io_lock, flags);
	req = be_prepare_req(evtchnl, XENCAMERA_OP_CTRL_ENUM);
	req->req.index.index = index;

	ret = be_stream_do_io(evtchnl, req);
	spin_unlock_irqrestore(&front_info->io_lock, flags);

	if (ret == 0)
		ret = be_stream_wait_io(evtchnl);

	*resp = evtchnl->u.req.resp.resp.ctrl_enum;

	mutex_unlock(&evtchnl->u.req.req_io_lock);
	return ret;
}

static int be_enum_controls(struct xen_camera_front_info *front_info)
{
	struct xen_camera_front_cfg_card *cfg = &front_info->cfg;
	struct xencamera_ctrl_enum_resp resp;
	int i, ret;

	cfg->num_controls = 0;
	for (i = 0; i < XENCAMERA_MAX_CTRL; i++) {
		ret = be_enum_control(front_info, i, &resp);
		/*
		 * We enumerate assigned controls here until EINVAL is
		 * returned by the backend meaning that the requested control
		 * with that index is not supported/assigned to the frontend.
		 */
		if (ret == -EINVAL)
			break;

		if (ret < 0)
			return ret;

		ret = xen_camera_front_v4l2_to_v4l2_cid(resp.type);
		if (ret < 0)
			return -EINVAL;

		cfg->ctrl[i].v4l2_cid = ret;
		cfg->ctrl[i].flags = resp.flags;
		cfg->ctrl[i].minimum = resp.min;
		cfg->ctrl[i].maximum = resp.max;
		cfg->ctrl[i].default_value = resp.def_val;
		cfg->ctrl[i].step = resp.step;

		dev_info(&front_info->xb_dev->dev, "Control CID %x\n", ret);

		cfg->num_controls++;
	}

	dev_info(&front_info->xb_dev->dev, "Assigned %d control(s)\n",
		 cfg->num_controls);
	return 0;
}

int xen_camera_front_buf_request(struct xen_camera_front_info *front_info,
				 int num_bufs)
{
	struct xen_camera_front_evtchnl *evtchnl;
	struct xencamera_req *req;
	unsigned long flags;
	int ret;

	evtchnl = &front_info->evt_pair.req;
	if (unlikely(!evtchnl))
		return -EIO;

	mutex_lock(&evtchnl->u.req.req_io_lock);

	spin_lock_irqsave(&front_info->io_lock, flags);
	req = be_prepare_req(evtchnl, XENCAMERA_OP_BUF_REQUEST);

	req->req.buf_request.num_bufs = num_bufs;

	ret = be_stream_do_io(evtchnl, req);
	spin_unlock_irqrestore(&front_info->io_lock, flags);

	if (ret == 0)
		ret = be_stream_wait_io(evtchnl);

	num_bufs = evtchnl->u.req.resp.resp.buf_request.num_bufs;

	mutex_unlock(&evtchnl->u.req.req_io_lock);
	if (ret < 0)
		return ret;

	return num_bufs;
}

static int shbuf_setup_pages(struct xen_camera_front_shbuf *shbuf)
{
	struct sg_page_iter sg_iter;
	int i, num_pages;

	num_pages = 0;
	for_each_sg_page(shbuf->sgt->sgl, &sg_iter, shbuf->sgt->nents, 0)
		num_pages++;
	if (!num_pages)
		return -EINVAL;

	shbuf->pages = kcalloc(num_pages, sizeof(struct page *),
			       GFP_KERNEL);
	if (!shbuf->pages)
		return -ENOMEM;

	i = 0;
	for_each_sg_page(shbuf->sgt->sgl, &sg_iter, shbuf->sgt->nents, 0)
		shbuf->pages[i++] = sg_page_iter_page(&sg_iter);

	return num_pages;
}

int xen_camera_front_buf_create(struct xen_camera_front_info *front_info,
				struct xen_camera_front_shbuf *shbuf,
				u8 index, struct sg_table *sgt)
{
	struct xen_camera_front_evtchnl *evtchnl;
	struct xencamera_req *req;
	struct xen_front_pgdir_shbuf_cfg buf_cfg;
	unsigned long flags;
	int ret, num_pages;

	memset(shbuf, 0, sizeof(*shbuf));

	evtchnl = &front_info->evt_pair.req;
	if (unlikely(!evtchnl))
		return -EIO;

	shbuf->sgt = sgt;
	num_pages = shbuf_setup_pages(shbuf);
	if (num_pages < 0)
		return num_pages;

	/* Remember the offset to the data of this buffer. */
	shbuf->data_offset = sgt->sgl->offset;

	memset(&buf_cfg, 0, sizeof(buf_cfg));
	buf_cfg.xb_dev = front_info->xb_dev;
	buf_cfg.pgdir = &shbuf->pgdir;
	buf_cfg.num_pages = num_pages;
	buf_cfg.pages = shbuf->pages;
	buf_cfg.be_alloc = front_info->cfg.be_alloc;

	ret = xen_front_pgdir_shbuf_alloc(&buf_cfg);
	if (ret < 0)
		goto fail_pgdir_alloc;

	mutex_lock(&evtchnl->u.req.req_io_lock);

	spin_lock_irqsave(&front_info->io_lock, flags);
	req = be_prepare_req(evtchnl, XENCAMERA_OP_BUF_CREATE);
	req->req.buf_create.gref_directory =
		xen_front_pgdir_shbuf_get_dir_start(&shbuf->pgdir);
	req->req.buf_create.index = index;
	req->req.buf_create.plane_offset[0] = shbuf->data_offset;

	ret = be_stream_do_io(evtchnl, req);
	spin_unlock_irqrestore(&front_info->io_lock, flags);

	if (ret < 0)
		goto fail;

	ret = be_stream_wait_io(evtchnl);
	if (ret < 0)
		goto fail;

	ret = xen_front_pgdir_shbuf_map(&shbuf->pgdir);
	if (ret < 0)
		goto fail;

	mutex_unlock(&evtchnl->u.req.req_io_lock);
	return 0;

fail:
	mutex_unlock(&evtchnl->u.req.req_io_lock);
fail_pgdir_alloc:
	xen_camera_front_destroy_shbuf(shbuf);
	return ret;
}

int xen_camera_front_buf_destroy(struct xen_camera_front_info *front_info,
				 struct xen_camera_front_shbuf *shbuf,
				 u8 index)
{
	struct xen_camera_front_evtchnl *evtchnl;
	struct xencamera_req *req;
	unsigned long flags;
	bool be_alloc;
	int ret;

	evtchnl = &front_info->evt_pair.req;
	if (unlikely(!evtchnl))
		return -EIO;

	be_alloc = front_info->cfg.be_alloc;

	/*
	 * For the backend allocated buffer release references now, so backend
	 * can free the buffer.
	 */
	if (be_alloc)
		xen_camera_front_destroy_shbuf(shbuf);

	mutex_lock(&evtchnl->u.req.req_io_lock);

	spin_lock_irqsave(&front_info->io_lock, flags);
	req = be_prepare_req(evtchnl, XENCAMERA_OP_BUF_DESTROY);
	req->req.index.index = index;

	ret = be_stream_do_io(evtchnl, req);
	spin_unlock_irqrestore(&front_info->io_lock, flags);

	if (ret == 0)
		ret = be_stream_wait_io(evtchnl);

	/*
	 * Do this regardless of communication status with the backend:
	 * if we cannot remove remote resources remove what we can locally.
	 */
	if (!be_alloc)
		xen_camera_front_destroy_shbuf(shbuf);

	mutex_unlock(&evtchnl->u.req.req_io_lock);
	return ret;
}

static int buf_queue_helper(struct xen_camera_front_info *front_info,
			    u8 index, u8 op)
{
	struct xen_camera_front_evtchnl *evtchnl;
	struct xencamera_req *req;
	unsigned long flags;
	int ret;

	evtchnl = &front_info->evt_pair.req;
	if (unlikely(!evtchnl))
		return -EIO;

	mutex_lock(&evtchnl->u.req.req_io_lock);

	spin_lock_irqsave(&front_info->io_lock, flags);
	req = be_prepare_req(evtchnl, op);

	req->req.index.index = index;

	ret = be_stream_do_io(evtchnl, req);
	spin_unlock_irqrestore(&front_info->io_lock, flags);

	if (ret == 0)
		ret = be_stream_wait_io(evtchnl);

	mutex_unlock(&evtchnl->u.req.req_io_lock);
	return ret;
}

int xen_camera_front_buf_queue(struct xen_camera_front_info *front_info,
			       u8 index)
{
	return buf_queue_helper(front_info, index, XENCAMERA_OP_BUF_QUEUE);
}

int xen_camera_front_buf_dequeue(struct xen_camera_front_info *front_info,
				 u8 index)
{
	return buf_queue_helper(front_info, index, XENCAMERA_OP_BUF_DEQUEUE);
}

static int buf_stream_helper(struct xen_camera_front_info *front_info, u8 op)
{
	struct xen_camera_front_evtchnl *evtchnl;
	struct xencamera_req *req;
	unsigned long flags;
	int ret;

	evtchnl = &front_info->evt_pair.req;
	if (unlikely(!evtchnl))
		return -EIO;

	mutex_lock(&evtchnl->u.req.req_io_lock);

	spin_lock_irqsave(&front_info->io_lock, flags);
	req = be_prepare_req(evtchnl, op);

	ret = be_stream_do_io(evtchnl, req);
	spin_unlock_irqrestore(&front_info->io_lock, flags);

	if (ret == 0)
		ret = be_stream_wait_io(evtchnl);

	mutex_unlock(&evtchnl->u.req.req_io_lock);
	return ret;
}

int xen_camera_front_stream_start(struct xen_camera_front_info *front_info)
{
	return buf_stream_helper(front_info, XENCAMERA_OP_STREAM_START);
}

int xen_camera_front_stream_stop(struct xen_camera_front_info *front_info)
{
	return buf_stream_helper(front_info, XENCAMERA_OP_STREAM_STOP);
}

int xen_camera_front_get_buf_layout(struct xen_camera_front_info *front_info,
				    struct xencamera_buf_get_layout_resp *resp)
{
	struct xen_camera_front_evtchnl *evtchnl;
	struct xencamera_req *req;
	unsigned long flags;
	int ret;

	evtchnl = &front_info->evt_pair.req;
	if (unlikely(!evtchnl))
		return -EIO;

	mutex_lock(&evtchnl->u.req.req_io_lock);

	spin_lock_irqsave(&front_info->io_lock, flags);
	req = be_prepare_req(evtchnl, XENCAMERA_OP_BUF_GET_LAYOUT);

	ret = be_stream_do_io(evtchnl, req);
	spin_unlock_irqrestore(&front_info->io_lock, flags);

	if (ret == 0)
		ret = be_stream_wait_io(evtchnl);

	*resp = evtchnl->u.req.resp.resp.buf_layout;

	mutex_unlock(&evtchnl->u.req.req_io_lock);
	return ret;
}

static void xen_camera_drv_fini(struct xen_camera_front_info *front_info)
{
	struct xen_camera_front_v4l2_info *v4l2_info = front_info->v4l2_info;

	if (!v4l2_info)
		return;

	xen_camera_front_v4l2_fini(front_info);

	front_info->v4l2_info = NULL;

	xen_camera_front_evtchnl_free_all(front_info);

	xenbus_switch_state(front_info->xb_dev, XenbusStateInitialising);
}

static int cameraback_initwait(struct xen_camera_front_info *front_info)
{
	int ret;

	ret = xen_camera_front_cfg_init(front_info);
	if (ret < 0)
		return ret;

	/* Create all event channels and publish. */
	ret = xen_camera_front_evtchnl_create_all(front_info);
	if (ret < 0)
		return ret;

	return xen_camera_front_evtchnl_publish_all(front_info);
}

static int cameraback_connect(struct xen_camera_front_info *front_info)
{
	int ret;

	xen_camera_front_evtchnl_pair_set_connected(&front_info->evt_pair,
						    true);
	/*
	 * Event channels are all set now, so we can read detailed
	 * configuration for each assigned control.
	 */
	ret = be_enum_controls(front_info);
	if (ret < 0)
		return ret;

	return xen_camera_front_v4l2_init(front_info);
}

static void cameraback_disconnect(struct xen_camera_front_info *front_info)
{
	if (!front_info->v4l2_info)
		return;

	/* Tell the backend to wait until we release the V4L2 driver. */
	xenbus_switch_state(front_info->xb_dev, XenbusStateReconfiguring);

	xen_camera_drv_fini(front_info);
}

static void cameraback_changed(struct xenbus_device *xb_dev,
			    enum xenbus_state backend_state)
{
	struct xen_camera_front_info *front_info = dev_get_drvdata(&xb_dev->dev);
	int ret;

	dev_dbg(&xb_dev->dev, "Backend state is %s, front is %s\n",
		xenbus_strstate(backend_state),
		xenbus_strstate(xb_dev->state));

	switch (backend_state) {
	case XenbusStateReconfiguring:
		/* fall through */
	case XenbusStateReconfigured:
		/* fall through */
	case XenbusStateInitialised:
		/* fall through */
		break;

	case XenbusStateInitialising:
		/* Recovering after backend unexpected closure. */
		cameraback_disconnect(front_info);
		break;

	case XenbusStateInitWait:
		/* Recovering after backend unexpected closure. */
		cameraback_disconnect(front_info);

		ret = cameraback_initwait(front_info);
		if (ret < 0)
			xenbus_dev_fatal(xb_dev, ret, "initializing frontend");
		else
			xenbus_switch_state(xb_dev, XenbusStateInitialised);

		break;

	case XenbusStateConnected:
		if (xb_dev->state != XenbusStateInitialised)
			break;

		ret = cameraback_connect(front_info);
		if (ret < 0)
			xenbus_dev_fatal(xb_dev, ret, "initializing frontend");
		else
			xenbus_switch_state(xb_dev, XenbusStateConnected);
		break;

	case XenbusStateClosing:
		/*
		 * In this state backend starts freeing resources,
		 * so let it go into closed state first, so we can also
		 * remove ours.
		 */
		break;

	case XenbusStateUnknown:
		/* fall through */
	case XenbusStateClosed:
		if (xb_dev->state == XenbusStateClosed)
			break;

		cameraback_disconnect(front_info);
		break;

	default:
		break;
	}
}

static int xen_drv_probe(struct xenbus_device *xb_dev,
			 const struct xenbus_device_id *id)
{
	struct device *dev = &xb_dev->dev;
	struct xen_camera_front_info *front_info;
	int ret;

	/*
	 * The device is not spawn from a device tree, so arch_setup_dma_ops
	 * is not called, thus leaving the device with dummy DMA ops.
	 * This makes the device return error on PRIME buffer import, which
	 * is not correct: to fix this call of_dma_configure() with a NULL
	 * node to set default DMA ops.
	 */
	dev->coherent_dma_mask = DMA_BIT_MASK(64);
	ret = of_dma_configure(dev, NULL, true);
	if (ret < 0) {
		xenbus_dev_fatal(xb_dev, ret, "setting up DMA ops");
		return ret;
	}

	front_info = devm_kzalloc(&xb_dev->dev,
				  sizeof(*front_info), GFP_KERNEL);
	if (!front_info)
		return -ENOMEM;

	front_info->xb_dev = xb_dev;
	spin_lock_init(&front_info->io_lock);
	dev_set_drvdata(&xb_dev->dev, front_info);

	return xenbus_switch_state(xb_dev, XenbusStateInitialising);
}

static int xen_drv_remove(struct xenbus_device *dev)
{
	struct xen_camera_front_info *front_info = dev_get_drvdata(&dev->dev);
	int to = 100;

	xenbus_switch_state(dev, XenbusStateClosing);

	/*
	 * On driver removal it is disconnected from XenBus,
	 * so no backend state change events come via .otherend_changed
	 * callback. This prevents us from exiting gracefully, e.g.
	 * signaling the backend to free event channels, waiting for its
	 * state to change to XenbusStateClosed and cleaning at our end.
	 * Normally when front driver removed backend will finally go into
	 * XenbusStateInitWait state.
	 *
	 * Workaround: read backend's state manually and wait with time-out.
	 */
	while ((xenbus_read_unsigned(front_info->xb_dev->otherend, "state",
				     XenbusStateUnknown) != XenbusStateInitWait) &&
				     --to)
		msleep(10);

	if (!to) {
		unsigned int state;

		state = xenbus_read_unsigned(front_info->xb_dev->otherend,
					     "state", XenbusStateUnknown);
		pr_err("Backend state is %s while removing driver\n",
		       xenbus_strstate(state));
	}

	xen_camera_drv_fini(front_info);
	xenbus_frontend_closed(dev);
	return 0;
}

static const struct xenbus_device_id xen_drv_ids[] = {
	{ XENCAMERA_DRIVER_NAME },
	{ "" }
};

static struct xenbus_driver xen_driver = {
	.ids = xen_drv_ids,
	.probe = xen_drv_probe,
	.remove = xen_drv_remove,
	.otherend_changed = cameraback_changed,
};

static int __init xen_drv_init(void)
{
	if (!xen_domain())
		return -ENODEV;

	if (!xen_has_pv_devices())
		return -ENODEV;

	pr_info("Initialising Xen " XENCAMERA_DRIVER_NAME " frontend driver\n");
	return xenbus_register_frontend(&xen_driver);
}

static void __exit xen_drv_fini(void)
{
	pr_info("Unregistering Xen " XENCAMERA_DRIVER_NAME " frontend driver\n");
	xenbus_unregister_driver(&xen_driver);
}

module_init(xen_drv_init);
module_exit(xen_drv_fini);

MODULE_DESCRIPTION("Xen virtual camera device frontend");
MODULE_LICENSE("GPL");
MODULE_ALIAS("xen:" XENCAMERA_DRIVER_NAME);
