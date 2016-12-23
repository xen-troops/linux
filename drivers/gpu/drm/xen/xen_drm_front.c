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

#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/platform_device.h>
#include <linux/irq.h>
#include <linux/scatterlist.h>

#include <asm/xen/hypervisor.h>
#include <xen/xen.h>
#include <xen/platform_pci.h>
#include <xen/xenbus.h>
#include <xen/events.h>
#include <xen/grant_table.h>
#include <xen/interface/io/ring.h>
#include <xen/interface/io/displif.h>

#include "xen_drm_drv.h"
#include "xen_drm_front.h"
#include "xen_drm_shbuf.h"

/* all operations which are not connector oriented use this ctrl event channel,
 * e.g. fb_attach/destroy which belong to a DRM device, not to a CRTC
 */
#define GENERIC_OP_EVT_CHNL	0

enum xdrv_evtchnl_state {
	EVTCHNL_STATE_DISCONNECTED,
	EVTCHNL_STATE_CONNECTED,
};

enum xdrv_evtchnl_type {
	EVTCHNL_TYPE_REQ,
	EVTCHNL_TYPE_EVT,
};

struct xdrv_evtchnl_info {
	struct xdrv_info *drv_info;
	int gref;
	int port;
	int irq;
	int index;
	/* state of the event channel */
	enum xdrv_evtchnl_state state;
	enum xdrv_evtchnl_type type;
	/* either response id or incoming event id */
	uint16_t evt_id;
	/* next request id or next expected event id */
	uint16_t evt_next_id;
	union {
		struct {
			struct xen_displif_front_ring ring;
			struct completion completion;
			/* latest response status */
			int resp_status;
		} req;
		struct {
			struct xendispl_event_page *page;
		} evt;
	} u;
};

struct xdrv_evtchnl_pair_info {
	struct xdrv_evtchnl_info req;
	struct xdrv_evtchnl_info evt;
};

struct xdrv_info {
	struct xenbus_device *xb_dev;
	spinlock_t io_lock;
	struct mutex mutex;
	bool drm_pdrv_registered;
	/* virtual DRM platform device */
	struct platform_device *drm_pdev;

	int num_evt_pairs;
	struct xdrv_evtchnl_pair_info *evt_pairs;
	struct xendrm_plat_data cfg_plat_data;

	/* dumb buffers */
	struct list_head dumb_buf_list;
};

static inline void xdrv_evtchnl_flush(
		struct xdrv_evtchnl_info *channel);
static void xdrv_drm_unload(struct xdrv_info *drv_info);

static inline struct xendispl_req *ddrv_be_prepare_req(
	struct xdrv_evtchnl_info *evtchnl, uint8_t operation)
{
	struct xendispl_req *req;

	req = RING_GET_REQUEST(&evtchnl->u.req.ring,
		evtchnl->u.req.ring.req_prod_pvt);
	req->operation = operation;
	req->id = evtchnl->evt_next_id++;
	evtchnl->evt_id = req->id;
	return req;
}

/* CAUTION!!! Call this with the spin lock held.
 * This function will release it
 */
static int ddrv_be_stream_do_io(struct xdrv_evtchnl_info *evtchnl,
	struct xendispl_req *req, unsigned long flags)
{
	int ret;

	reinit_completion(&evtchnl->u.req.completion);
	if (unlikely(evtchnl->state != EVTCHNL_STATE_CONNECTED)) {
		spin_unlock_irqrestore(&evtchnl->drv_info->io_lock, flags);
		return -EIO;
	}
	xdrv_evtchnl_flush(evtchnl);
	spin_unlock_irqrestore(&evtchnl->drv_info->io_lock, flags);
	ret = 0;
	if (wait_for_completion_timeout(
			&evtchnl->u.req.completion,
			msecs_to_jiffies(VDRM_WAIT_BACK_MS)) <= 0)
		ret = -ETIMEDOUT;
	if (ret < 0)
		return ret;
	return evtchnl->u.req.resp_status;
}

int xendispl_front_mode_set(struct xendrm_crtc *xen_crtc, uint32_t x,
	uint32_t y, uint32_t width, uint32_t height, uint32_t bpp,
	uint64_t fb_cookie)

{
	struct xdrv_evtchnl_info *evtchnl;
	struct xdrv_info *drv_info;
	struct xendispl_req *req;
	unsigned long flags;

	drv_info = xen_crtc->xendrm_dev->xdrv_info;
	evtchnl = &drv_info->evt_pairs[xen_crtc->index].req;
	if (unlikely(!evtchnl))
		return -EIO;
	spin_lock_irqsave(&drv_info->io_lock, flags);
	req = ddrv_be_prepare_req(evtchnl, XENDISPL_OP_SET_CONFIG);
	req->op.set_config.x = x;
	req->op.set_config.y = y;
	req->op.set_config.width = width;
	req->op.set_config.height = height;
	req->op.set_config.bpp = bpp;
	req->op.set_config.fb_cookie = fb_cookie;
	return ddrv_be_stream_do_io(evtchnl, req, flags);
}

struct page **xendispl_front_dbuf_create(struct xdrv_info *drv_info,
	uint64_t dumb_cookie, uint32_t width, uint32_t height,
	uint32_t bpp, uint64_t size, struct page **pages, struct sg_table *sgt)
{
	struct xdrv_evtchnl_info *evtchnl;
	struct xdrv_shared_buffer_info *buf;
	struct xendispl_req *req;
	struct xdrv_shared_buffer_alloc_info alloc_info;
	unsigned long flags;
	bool be_alloc;
	int ret;

	evtchnl = &drv_info->evt_pairs[GENERIC_OP_EVT_CHNL].req;
	if (unlikely(!evtchnl))
		return ERR_PTR(-EIO);
	be_alloc = drv_info->cfg_plat_data.be_alloc;

	memset(&alloc_info, 0, sizeof(alloc_info));
	alloc_info.xb_dev = drv_info->xb_dev;
	alloc_info.dumb_buf_list = &drv_info->dumb_buf_list;
	alloc_info.dumb_cookie = dumb_cookie;
	alloc_info.pages = pages;
	alloc_info.num_pages = DIV_ROUND_UP(size, XEN_PAGE_SIZE);
	alloc_info.sgt = sgt;
	alloc_info.be_alloc = be_alloc;
	buf = xdrv_shbuf_alloc(&alloc_info);
	if (!buf)
		return ERR_PTR(-ENOMEM);

	spin_lock_irqsave(&drv_info->io_lock, flags);
	req = ddrv_be_prepare_req(evtchnl, XENDISPL_OP_DBUF_CREATE);
	req->op.dbuf_create.gref_directory = xdrv_shbuf_get_dir_start(buf);
	req->op.dbuf_create.buffer_sz = size;
	req->op.dbuf_create.dbuf_cookie = dumb_cookie;
	req->op.dbuf_create.width = width;
	req->op.dbuf_create.height = height;
	req->op.dbuf_create.bpp = bpp;
	if (be_alloc)
		req->op.dbuf_create.flags |= XENDISPL_DBUF_FLG_REQ_ALLOC;
	ret = ddrv_be_stream_do_io(evtchnl, req, flags);
	if (ret < 0)
		goto fail;
	if (be_alloc) {
		ret = xdrv_shbuf_be_alloc_map(buf);
		if (ret < 0)
			goto fail;
	}
	return xdrv_shbuf_get_pages(buf);
fail:
	xdrv_shbuf_free_by_dumb_cookie(&drv_info->dumb_buf_list, dumb_cookie);
	return ERR_PTR(ret);
}

int xendispl_front_dbuf_destroy(struct xdrv_info *drv_info,
	uint64_t dumb_cookie)
{
	struct xdrv_evtchnl_info *evtchnl;
	struct xendispl_req *req;
	unsigned long flags;
	bool be_alloc;
	int ret;

	evtchnl = &drv_info->evt_pairs[GENERIC_OP_EVT_CHNL].req;
	if (unlikely(!evtchnl))
		return -EIO;
	spin_lock_irqsave(&drv_info->io_lock, flags);
	req = ddrv_be_prepare_req(evtchnl, XENDISPL_OP_DBUF_DESTROY);
	req->op.dbuf_destroy.dbuf_cookie = dumb_cookie;
	be_alloc = drv_info->cfg_plat_data.be_alloc;
	if (be_alloc)
		xdrv_shbuf_free_by_dumb_cookie(&drv_info->dumb_buf_list,
			dumb_cookie);
	ret = ddrv_be_stream_do_io(evtchnl, req, flags);
	if (!be_alloc)
		xdrv_shbuf_free_by_dumb_cookie(&drv_info->dumb_buf_list,
			dumb_cookie);
	return ret;
}

int xendispl_front_fb_attach(struct xdrv_info *drv_info,
	uint64_t dumb_cookie, uint64_t fb_cookie, uint32_t width,
	uint32_t height, uint32_t pixel_format)
{
	struct xdrv_evtchnl_info *evtchnl;
	struct xdrv_shared_buffer_info *buf;
	struct xendispl_req *req;
	unsigned long flags;

	evtchnl = &drv_info->evt_pairs[GENERIC_OP_EVT_CHNL].req;
	if (unlikely(!evtchnl))
		return -EIO;
	buf = xdrv_shbuf_get_by_dumb_cookie(&drv_info->dumb_buf_list,
		dumb_cookie);
	if (!buf)
		return -EINVAL;
	buf->fb_cookie = fb_cookie;
	spin_lock_irqsave(&drv_info->io_lock, flags);
	req = ddrv_be_prepare_req(evtchnl, XENDISPL_OP_FB_ATTACH);
	req->op.fb_attach.dbuf_cookie = dumb_cookie;
	req->op.fb_attach.fb_cookie = fb_cookie;
	req->op.fb_attach.width = width;
	req->op.fb_attach.height = height;
	req->op.fb_attach.pixel_format = pixel_format;
	return ddrv_be_stream_do_io(evtchnl, req, flags);
}

int xendispl_front_fb_detach(struct xdrv_info *drv_info, uint64_t fb_cookie)
{
	struct xdrv_evtchnl_info *evtchnl;
	struct xendispl_req *req;
	unsigned long flags;

	evtchnl = &drv_info->evt_pairs[GENERIC_OP_EVT_CHNL].req;
	if (unlikely(!evtchnl))
		return -EIO;
	spin_lock_irqsave(&drv_info->io_lock, flags);
	req = ddrv_be_prepare_req(evtchnl, XENDISPL_OP_FB_DETACH);
	req->op.fb_detach.fb_cookie = fb_cookie;
	return ddrv_be_stream_do_io(evtchnl, req, flags);
}

int xendispl_front_page_flip(struct xdrv_info *drv_info, int conn_idx,
	uint64_t fb_cookie)
{
	struct xdrv_evtchnl_info *evtchnl;
	struct xendispl_req *req;
	unsigned long flags;

	if (unlikely(conn_idx >= drv_info->num_evt_pairs))
		return -EINVAL;
	xdrv_shbuf_flush_fb(&drv_info->dumb_buf_list, fb_cookie);
	evtchnl = &drv_info->evt_pairs[conn_idx].req;
	spin_lock_irqsave(&drv_info->io_lock, flags);
	req = ddrv_be_prepare_req(evtchnl, XENDISPL_OP_PG_FLIP);
	req->op.pg_flip.fb_cookie = fb_cookie;
	return ddrv_be_stream_do_io(evtchnl, req, flags);
}

static struct xendispl_front_ops xendispl_front_funcs = {
	.mode_set = xendispl_front_mode_set,
	.dbuf_create = xendispl_front_dbuf_create,
	.dbuf_destroy = xendispl_front_dbuf_destroy,
	.fb_attach = xendispl_front_fb_attach,
	.fb_detach = xendispl_front_fb_detach,
	.page_flip = xendispl_front_page_flip,
	.drm_last_close = xdrv_drm_unload,
};

static int ddrv_probe(struct platform_device *pdev)
{
#ifdef CONFIG_DRM_XEN_FRONTEND_CMA
	struct device *dev = &pdev->dev;

	/* make sure we have DMA ops set up, so no dummy ops are in use */
	arch_setup_dma_ops(dev, 0, *dev->dma_mask, NULL, false);
#endif
	return xendrm_probe(pdev, &xendispl_front_funcs);
}

struct platform_device_info ddrv_platform_info = {
	.name = XENDISPL_DRIVER_NAME,
	.id = 0,
	.num_res = 0,
	.dma_mask = DMA_BIT_MASK(32),
};

static struct platform_driver ddrv_info = {
	.probe		= ddrv_probe,
	.remove		= xendrm_remove,
	.driver		= {
		.name	= XENDISPL_DRIVER_NAME,
	},
};

static void ddrv_cleanup(struct xdrv_info *drv_info)
{
	if (!drv_info->drm_pdrv_registered)
		return;
	if (drv_info->drm_pdev)
		platform_device_unregister(drv_info->drm_pdev);
	platform_driver_unregister(&ddrv_info);
	drv_info->drm_pdrv_registered = false;
	drv_info->drm_pdev = NULL;
}

static int ddrv_init(struct xdrv_info *drv_info)
{
	struct xendrm_plat_data *platdata;
	int ret;

	ret = platform_driver_register(&ddrv_info);
	if (ret < 0)
		return ret;
	drv_info->drm_pdrv_registered = true;
	platdata = &drv_info->cfg_plat_data;
	/* pass card configuration via platform data */
	ddrv_platform_info.data = platdata;
	ddrv_platform_info.size_data = sizeof(struct xendrm_plat_data);
	drv_info->drm_pdev = platform_device_register_full(&ddrv_platform_info);
	if (IS_ERR(drv_info->drm_pdev)) {
		drv_info->drm_pdev = NULL;
		goto fail;
	}
	return 0;

fail:
	DRM_ERROR("Failed to register DRM driver\n");
	ddrv_cleanup(drv_info);
	return -ENODEV;
}

static irqreturn_t xdrv_evtchnl_interrupt_ctrl(int irq, void *dev_id)
{
	struct xdrv_evtchnl_info *channel = dev_id;
	struct xdrv_info *drv_info = channel->drv_info;
	struct xendispl_resp *resp;
	RING_IDX i, rp;
	unsigned long flags;

	spin_lock_irqsave(&drv_info->io_lock, flags);
	if (unlikely(channel->state != EVTCHNL_STATE_CONNECTED))
		goto out;
again:
	rp = channel->u.req.ring.sring->rsp_prod;
	/* Ensure we see queued responses up to 'rp'. */
	virt_rmb();
	for (i = channel->u.req.ring.rsp_cons; i != rp; i++) {
		resp = RING_GET_RESPONSE(&channel->u.req.ring, i);
		if (unlikely(resp->id != channel->evt_id))
			continue;
		switch (resp->operation) {
		case XENDISPL_OP_PG_FLIP:
		case XENDISPL_OP_FB_ATTACH:
		case XENDISPL_OP_FB_DETACH:
		case XENDISPL_OP_DBUF_CREATE:
		case XENDISPL_OP_DBUF_DESTROY:
		case XENDISPL_OP_SET_CONFIG:
			channel->u.req.resp_status = resp->status;
			complete(&channel->u.req.completion);
			break;
		default:
			DRM_ERROR("Operation %d is not supported\n",
				resp->operation);
			break;
		}
	}
	channel->u.req.ring.rsp_cons = i;
	if (i != channel->u.req.ring.req_prod_pvt) {
		int more_to_do;

		RING_FINAL_CHECK_FOR_RESPONSES(&channel->u.req.ring,
			more_to_do);
		if (more_to_do)
			goto again;
	} else
		channel->u.req.ring.sring->rsp_event = i + 1;
out:
	spin_unlock_irqrestore(&drv_info->io_lock, flags);
	return IRQ_HANDLED;
}

static irqreturn_t xdrv_evtchnl_interrupt_evt(int irq, void *dev_id)
{
	struct xdrv_evtchnl_info *channel = dev_id;
	struct xdrv_info *drv_info = channel->drv_info;
	struct xendispl_event_page *page = channel->u.evt.page;
	uint32_t cons, prod;
	unsigned long flags;

	spin_lock_irqsave(&drv_info->io_lock, flags);
	if (unlikely(channel->state != EVTCHNL_STATE_CONNECTED))
		goto out;
	prod = page->in_prod;
	/* ensure we see ring contents up to prod */
	virt_rmb();
	if (prod == page->in_cons)
		goto out;
	for (cons = page->in_cons; cons != prod; cons++) {
		struct xendispl_evt *event;

		event = &XENDISPL_IN_RING_REF(page, cons);
		if (unlikely(event->id != channel->evt_id++))
			continue;
		switch (event->type) {
		case XENDISPL_EVT_PG_FLIP:
			if (likely(xendispl_front_funcs.on_page_flip)) {
				xendispl_front_funcs.on_page_flip(
					drv_info->drm_pdev, channel->index,
					event->op.pg_flip.fb_cookie);
			}
			break;
		}
	}
	page->in_cons = cons;
	/* ensure ring contents */
	virt_wmb();
out:
	spin_unlock_irqrestore(&drv_info->io_lock, flags);
	return IRQ_HANDLED;
}

static void xdrv_evtchnl_free(struct xdrv_info *drv_info,
		struct xdrv_evtchnl_info *channel)
{
	unsigned long page = 0;

	if (channel->type == EVTCHNL_TYPE_REQ)
		page = (unsigned long)channel->u.req.ring.sring;
	else if (channel->type == EVTCHNL_TYPE_EVT)
		page = (unsigned long)channel->u.evt.page;
	if (!page)
		return;
	channel->state = EVTCHNL_STATE_DISCONNECTED;
	if (channel->type == EVTCHNL_TYPE_REQ) {
		/* release all who still waits for response if any */
		channel->u.req.resp_status = -EIO;
		complete_all(&channel->u.req.completion);
	}
	if (channel->irq)
		unbind_from_irqhandler(channel->irq, channel);
	if (channel->port)
		xenbus_free_evtchn(drv_info->xb_dev, channel->port);
	/* End access and free the pages */
	if (channel->gref != GRANT_INVALID_REF)
		gnttab_end_foreign_access(channel->gref, 0, page);
	if (channel->type == EVTCHNL_TYPE_REQ)
		channel->u.req.ring.sring = NULL;
	else
		channel->u.evt.page = NULL;
	memset(channel, 0, sizeof(*channel));
}

static void xdrv_evtchnl_free_all(struct xdrv_info *drv_info)
{
	int i;

	if (!drv_info->evt_pairs)
		return;
	for (i = 0; i < drv_info->num_evt_pairs; i++) {
		xdrv_evtchnl_free(drv_info,
			&drv_info->evt_pairs[i].req);
		xdrv_evtchnl_free(drv_info,
			&drv_info->evt_pairs[i].evt);
	}
	devm_kfree(&drv_info->xb_dev->dev, drv_info->evt_pairs);
	drv_info->evt_pairs = NULL;
}

static int xdrv_evtchnl_alloc(struct xdrv_info *drv_info, int index,
	struct xdrv_evtchnl_info *evt_channel,
	enum xdrv_evtchnl_type type)
{
	struct xenbus_device *xb_dev = drv_info->xb_dev;
	unsigned long page;
	grant_ref_t gref;
	irq_handler_t handler;
	int ret;

	memset(evt_channel, 0, sizeof(*evt_channel));
	evt_channel->type = type;
	evt_channel->index = index;
	evt_channel->drv_info = drv_info;
	evt_channel->state = EVTCHNL_STATE_DISCONNECTED;
	evt_channel->gref = GRANT_INVALID_REF;
	page = get_zeroed_page(GFP_NOIO | __GFP_HIGH);
	if (!page) {
		ret = -ENOMEM;
		goto fail;
	}
	if (type == EVTCHNL_TYPE_REQ) {
		struct xen_displif_sring *sring;

		init_completion(&evt_channel->u.req.completion);
		sring = (struct xen_displif_sring *)page;
		SHARED_RING_INIT(sring);
		FRONT_RING_INIT(&evt_channel->u.req.ring,
			sring, XEN_PAGE_SIZE);

		ret = xenbus_grant_ring(xb_dev, sring, 1, &gref);
		if (ret < 0)
			goto fail;
		handler = xdrv_evtchnl_interrupt_ctrl;
	} else {
		evt_channel->u.evt.page = (struct xendispl_event_page *)page;
		ret = gnttab_grant_foreign_access(xb_dev->otherend_id,
			virt_to_gfn((void *)page), 0);
		if (ret < 0)
			goto fail;
		gref = ret;
		handler = xdrv_evtchnl_interrupt_evt;
	}
	evt_channel->gref = gref;

	ret = xenbus_alloc_evtchn(xb_dev, &evt_channel->port);
	if (ret < 0)
		goto fail;

	ret = bind_evtchn_to_irqhandler(evt_channel->port,
		handler, 0, xb_dev->devicetype, evt_channel);
	if (ret < 0)
		goto fail;
	evt_channel->irq = ret;
	return 0;

fail:
	DRM_ERROR("Failed to allocate ring: %d\n", ret);
	return ret;
}

static int xdrv_evtchnl_publish(struct xenbus_transaction xbt,
	struct xdrv_evtchnl_info *evt_channel,
	const char *path, const char *node_ring,
	const char *node_chnl)
{
	const char *message;
	int ret;

	/* write control channel ring reference */
	ret = xenbus_printf(xbt, path, node_ring, "%u",
			evt_channel->gref);
	if (ret < 0) {
		message = "writing ring-ref";
		goto fail;
	}
	/* write event channel ring reference */
	ret = xenbus_printf(xbt, path, node_chnl, "%u",
		evt_channel->port);
	if (ret < 0) {
		message = "writing event channel";
		goto fail;
	}
	return 0;

fail:
	DRM_ERROR("Error %s: %d\n", message, ret);
	return ret;
}

static inline void xdrv_evtchnl_flush(
		struct xdrv_evtchnl_info *channel)
{
	int notify;

	channel->u.req.ring.req_prod_pvt++;
	RING_PUSH_REQUESTS_AND_CHECK_NOTIFY(&channel->u.req.ring, notify);
	if (notify)
		notify_remote_via_irq(channel->irq);
}

static int xdrv_evtchnl_create_all(struct xdrv_info *drv_info)
{
	struct xendrm_plat_data *plat_data;
	int ret, conn;

	plat_data = &drv_info->cfg_plat_data;
	drv_info->evt_pairs = devm_kcalloc(&drv_info->xb_dev->dev,
		plat_data->num_connectors,
		sizeof(struct xdrv_evtchnl_pair_info), GFP_KERNEL);
	if (!drv_info->evt_pairs) {
		ret = -ENOMEM;
		goto fail;
	}
	for (conn = 0; conn < plat_data->num_connectors; conn++) {
		ret = xdrv_evtchnl_alloc(drv_info, conn,
			&drv_info->evt_pairs[conn].req, EVTCHNL_TYPE_REQ);
		if (ret < 0) {
			DRM_ERROR("Error allocating control channel\n");
			goto fail;
		}
		ret = xdrv_evtchnl_alloc(drv_info, conn,
			&drv_info->evt_pairs[conn].evt, EVTCHNL_TYPE_EVT);
		if (ret < 0) {
			DRM_ERROR("Error allocating in-event channel\n");
			goto fail;
		}
	}
	drv_info->num_evt_pairs = plat_data->num_connectors;
	return 0;
fail:
	xdrv_evtchnl_free_all(drv_info);
	return ret;
}

static int xdrv_evtchnl_publish_all(struct xdrv_info *drv_info)
{
	struct xenbus_transaction xbt;
	struct xendrm_plat_data *plat_data;
	int ret, conn;

	plat_data = &drv_info->cfg_plat_data;
again:
	ret = xenbus_transaction_start(&xbt);
	if (ret < 0) {
		xenbus_dev_fatal(drv_info->xb_dev, ret, "starting transaction");
		return ret;
	}
	for (conn = 0; conn < plat_data->num_connectors; conn++) {
		ret = xdrv_evtchnl_publish(xbt,
			&drv_info->evt_pairs[conn].req,
			plat_data->connectors[conn].xenstore_path,
			XENDISPL_FIELD_REQ_RING_REF,
			XENDISPL_FIELD_REQ_CHANNEL);
		if (ret < 0)
			goto fail;
		ret = xdrv_evtchnl_publish(xbt,
			&drv_info->evt_pairs[conn].evt,
			plat_data->connectors[conn].xenstore_path,
			XENDISPL_FIELD_EVT_RING_REF,
			XENDISPL_FIELD_EVT_CHANNEL);
		if (ret < 0)
			goto fail;
	}
	ret = xenbus_transaction_end(xbt, 0);
	if (ret < 0) {
		if (ret == -EAGAIN)
			goto again;
		xenbus_dev_fatal(drv_info->xb_dev, ret,
			"completing transaction");
		goto fail_to_end;
	}
	return 0;
fail:
	xenbus_transaction_end(xbt, 1);
fail_to_end:
	xenbus_dev_fatal(drv_info->xb_dev, ret, "writing XenStore");
	return ret;
}

static void xdrv_evtchnl_set_state(struct xdrv_info *drv_info,
	enum xdrv_evtchnl_state state)
{
	unsigned long flags;
	int i;

	if (!drv_info->evt_pairs)
		return;
	spin_lock_irqsave(&drv_info->io_lock, flags);
	for (i = 0; i < drv_info->num_evt_pairs; i++) {
		drv_info->evt_pairs[i].req.state = state;
		drv_info->evt_pairs[i].evt.state = state;
	}
	spin_unlock_irqrestore(&drv_info->io_lock, flags);

}

static int xdrv_cfg_connector(struct xdrv_info *drv_info,
	struct xendrm_cfg_connector *connector,
	const char *path, int index)
{
	char *connector_path;
	int ret;

	connector_path = devm_kasprintf(&drv_info->xb_dev->dev,
		GFP_KERNEL, "%s/%d", path, index);
	if (!connector_path)
		return -ENOMEM;
	connector->xenstore_path = connector_path;
	if (xenbus_scanf(XBT_NIL, connector_path, XENDISPL_FIELD_RESOLUTION,
			"%d" XENDISPL_RESOLUTION_SEPARATOR "%d",
			&connector->width, &connector->height) < 0) {
		/* either no entry configured or wrong resolution set */
		connector->width = 0;
		connector->height = 0;
		ret = -EINVAL;
		goto fail;
	}
	DRM_INFO("Connector %s: resolution %dx%d\n",
		connector_path, connector->width, connector->height);
	ret = 0;
fail:
	return ret;
}

static int xdrv_cfg_card(struct xdrv_info *drv_info,
	struct xendrm_plat_data *plat_data)
{
	struct xenbus_device *xb_dev = drv_info->xb_dev;
	int ret, i;

	if (xenbus_read_unsigned(drv_info->xb_dev->nodename,
			XENDISPL_FIELD_BE_ALLOC, 0)) {
		DRM_INFO("Backend can provide dumb buffers\n");
#ifdef CONFIG_DRM_XEN_FRONTEND_CMA
		DRM_WARN("Cannot use backend's buffers with Xen CMA enabled\n");
#else
		/*
		 * FIXME: this mapping will need extra care in case of
		 * XENFEAT_auto_translated_physmap == 0 (see gntdev driver).
		 * For now, only support BE allocated buffers on platforms,
		 * that do auto translation
		 */
		if (!xen_feature(XENFEAT_auto_translated_physmap))
			DRM_WARN("Cannot use backend's buffers on this platform\n");
		else
			plat_data->be_alloc = true;
#endif
	}
	plat_data->num_connectors = 0;
	for (i = 0; i < XENDRM_MAX_CRTCS; i++) {
		ret = xdrv_cfg_connector(drv_info,
			&plat_data->connectors[i], xb_dev->nodename, i);
		if (ret < 0)
			break;
		plat_data->num_connectors++;
	}
	if (!plat_data->num_connectors) {
		DRM_ERROR("No connector(s) configured at %s\n",
			xb_dev->nodename);
		return -ENODEV;
	}
	return 0;
}

static void xdrv_remove_internal(struct xdrv_info *drv_info)
{
	ddrv_cleanup(drv_info);
	xdrv_evtchnl_free_all(drv_info);
	xdrv_shbuf_free_all(&drv_info->dumb_buf_list);
}

static int xdrv_probe(struct xenbus_device *xb_dev,
	const struct xenbus_device_id *id)
{
	struct xdrv_info *drv_info;
	int ret;

	drv_info = devm_kzalloc(&xb_dev->dev, sizeof(*drv_info), GFP_KERNEL);
	if (!drv_info) {
		ret = -ENOMEM;
		goto fail;
	}
	xenbus_switch_state(xb_dev, XenbusStateInitialising);
	drv_info->xb_dev = xb_dev;
	spin_lock_init(&drv_info->io_lock);
	INIT_LIST_HEAD(&drv_info->dumb_buf_list);
	mutex_init(&drv_info->mutex);
	drv_info->drm_pdrv_registered = false;
	dev_set_drvdata(&xb_dev->dev, drv_info);
	return 0;
fail:
	xenbus_dev_fatal(xb_dev, ret, "allocating device memory");
	return ret;
}

static int xdrv_remove(struct xenbus_device *dev)
{
	struct xdrv_info *drv_info = dev_get_drvdata(&dev->dev);
	int to = 10;

	/*
	 * FIXME: on driver removal it is disconnected from XenBus,
	 * so no backend state change events come in via .otherend_changed
	 * callback. This prevents us from exiting gracefully, e.g.
	 * signaling the backend to free event channels, waiting for its
	 * state change to closed and cleaning at our end.
	 * Workaround: read backend's state manually
	 */
	xenbus_switch_state(dev, XenbusStateClosing);
	while ((xenbus_read_unsigned(drv_info->xb_dev->otherend,
		"state", XenbusStateUnknown) != XenbusStateInitWait) && to--)
		msleep(10);
	if (!to)
		DRM_ERROR("Backend state is %s while removing driver\n",
			xenbus_strstate(xenbus_read_unsigned(
					drv_info->xb_dev->otherend,
					"state", XenbusStateUnknown)));
	mutex_lock(&drv_info->mutex);
	xdrv_remove_internal(drv_info);
	mutex_unlock(&drv_info->mutex);
	xenbus_switch_state(dev, XenbusStateClosed);
	return 0;
}

static int xdrv_be_on_initwait(struct xdrv_info *drv_info)
{
	struct xendrm_plat_data *cfg_plat_data;
	int ret;

	cfg_plat_data = &drv_info->cfg_plat_data;
	cfg_plat_data->xdrv_info = drv_info;
	ret = xdrv_cfg_card(drv_info, cfg_plat_data);
	if (ret < 0)
		return ret;
	DRM_INFO("Have %d conector(s)\n", cfg_plat_data->num_connectors);
	/* create event channels for all streams and publish */
	ret = xdrv_evtchnl_create_all(drv_info);
	if (ret < 0)
		return ret;
	return xdrv_evtchnl_publish_all(drv_info);
}

static int xdrv_be_on_connected(struct xdrv_info *drv_info)
{
	xdrv_evtchnl_set_state(drv_info, EVTCHNL_STATE_CONNECTED);
	return ddrv_init(drv_info);
}

static void xdrv_be_on_disconnected(struct xdrv_info *drv_info)
{
	bool removed = true;

	if (drv_info->drm_pdev) {
		if (xendrm_is_used(drv_info->drm_pdev)) {
			DRM_WARN("DRM driver still in use, deferring removal\n");
			removed = false;
		} else {
			xdrv_remove_internal(drv_info);
		}
	}
	xdrv_evtchnl_set_state(drv_info, EVTCHNL_STATE_DISCONNECTED);
	if (removed)
		xenbus_switch_state(drv_info->xb_dev, XenbusStateInitialising);
	else
		xenbus_switch_state(drv_info->xb_dev, XenbusStateReconfiguring);
}

static void xdrv_drm_unload(struct xdrv_info *drv_info)
{
	if (drv_info->xb_dev->state != XenbusStateReconfiguring)
		return;
	DRM_INFO("Can try removing driver now\n");
	xenbus_switch_state(drv_info->xb_dev, XenbusStateInitialising);
}

static void xdrv_be_on_changed(struct xenbus_device *xb_dev,
	enum xenbus_state backend_state)
{
	struct xdrv_info *drv_info = dev_get_drvdata(&xb_dev->dev);
	int ret;

	DRM_DEBUG("Backend state is %s, front is %s\n",
		xenbus_strstate(backend_state), xenbus_strstate(xb_dev->state));

	switch (backend_state) {
	case XenbusStateReconfiguring:
		/* fall through */
	case XenbusStateReconfigured:
		/* fall through */
	case XenbusStateInitialised:
		break;

	case XenbusStateInitialising:
		/* recovering after backend unexpected closure */
		mutex_lock(&drv_info->mutex);
		xdrv_be_on_disconnected(drv_info);
		mutex_unlock(&drv_info->mutex);
		break;

	case XenbusStateInitWait:
		/* recovering after backend unexpected closure */
		mutex_lock(&drv_info->mutex);
		xdrv_be_on_disconnected(drv_info);
		if (xb_dev->state != XenbusStateInitialising) {
			mutex_unlock(&drv_info->mutex);
			break;
		}
		ret = xdrv_be_on_initwait(drv_info);
		mutex_unlock(&drv_info->mutex);
		if (ret < 0) {
			xenbus_dev_fatal(xb_dev, ret, "initializing frontend");
			break;
		}
		xenbus_switch_state(xb_dev, XenbusStateInitialised);
		break;

	case XenbusStateConnected:
		if (xb_dev->state != XenbusStateInitialised)
			break;
		mutex_lock(&drv_info->mutex);
		ret = xdrv_be_on_connected(drv_info);
		mutex_unlock(&drv_info->mutex);
		if (ret < 0) {
			xenbus_dev_fatal(xb_dev, ret,
				"initializing DRM driver");
			break;
		}
		xenbus_switch_state(xb_dev, XenbusStateConnected);
		break;

	case XenbusStateClosing:
		/*
		 * in this state backend starts freeing resources,
		 * so let it go into closed state, so we can also
		 * remove ours
		 */
		break;

	case XenbusStateUnknown:
		/* fall through */
	case XenbusStateClosed:
		if (xb_dev->state == XenbusStateClosed)
			break;
		mutex_lock(&drv_info->mutex);
		xdrv_be_on_disconnected(drv_info);
		mutex_unlock(&drv_info->mutex);
		break;
	}
}

static const struct xenbus_device_id xdrv_ids[] = {
	{ XENDISPL_DRIVER_NAME },
	{ "" }
};

static struct xenbus_driver xen_driver = {
	.ids = xdrv_ids,
	.probe = xdrv_probe,
	.remove = xdrv_remove,
	.otherend_changed = xdrv_be_on_changed,
};

static int __init xdrv_init(void)
{
	BUILD_BUG_ON(XEN_PAGE_SIZE > PAGE_SIZE);

	if (!xen_domain())
		return -ENODEV;
	if (xen_initial_domain()) {
		DRM_ERROR(XENDISPL_DRIVER_NAME " cannot run in Dom0\n");
		return -ENODEV;
	}
	if (!xen_has_pv_devices())
		return -ENODEV;
	DRM_INFO("Registering XEN PV " XENDISPL_DRIVER_NAME "\n");
	return xenbus_register_frontend(&xen_driver);
}

static void __exit xdrv_cleanup(void)
{
	DRM_INFO("Unregistering XEN PV " XENDISPL_DRIVER_NAME "\n");
	xenbus_unregister_driver(&xen_driver);
}

module_init(xdrv_init);
module_exit(xdrv_cleanup);

MODULE_DESCRIPTION("Xen virtual display device frontend");
MODULE_LICENSE("GPL");
MODULE_ALIAS("xen:"XENDISPL_DRIVER_NAME);

