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

#include "xen_drm.h"
#include "xen_drm_front.h"

#define GRANT_INVALID_REF	0
/* timeout in ms to wait for backend to respond */
#define VDRM_WAIT_BACK_MS	5000

/* all operations which are not connector oriented use this ctrl event channel,
 * e.g. fb_attach/destroy which belong to a DRM device, not to a CRTC
 */
#define GENERIC_OP_EVT_CHNL	0

enum xdrv_evtchnl_state {
	EVTCHNL_STATE_DISCONNECTED,
	EVTCHNL_STATE_CONNECTED,
};

enum xdrv_evtchnl_type {
	EVTCHNL_TYPE_CTRL,
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
		} ctrl;
		struct {
			struct xendispl_event_page *page;
		} evt;
	} u;
};

struct xdrv_shared_buffer_info {
	struct list_head list;
	uint64_t dumb_cookie;
	uint64_t fb_cookie;
	int num_grefs;
	grant_ref_t *grefs;
	unsigned char *vdirectory;
	struct sg_table *sgt;
	size_t vbuffer_sz;
};

struct xdrv_evtchnl_pair_info {
	struct xdrv_evtchnl_info ctrl;
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

	struct dma_map_ops dma_map_ops;
};

struct DRMIF_TO_KERN_ERROR {
	int drmif;
	int kern;
};

static struct DRMIF_TO_KERN_ERROR drmif_kern_error_codes[] = {
	{ .drmif = XENDISPL_RSP_OKAY,     .kern = 0 },
	{ .drmif = XENDISPL_RSP_ERROR,    .kern = EIO },
	{ .drmif = XENDISPL_RSP_NOTSUPP,  .kern = EOPNOTSUPP },
	{ .drmif = XENDISPL_RSP_NOMEM,    .kern = ENOMEM },
	{ .drmif = XENDISPL_RSP_INVAL,    .kern = EINVAL },
};

static int drmif_to_kern_error(int drmif_err)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(drmif_kern_error_codes); i++)
		if (drmif_kern_error_codes[i].drmif == drmif_err)
			return -drmif_kern_error_codes[i].kern;
	return -EIO;
}

static inline void xdrv_evtchnl_flush(
		struct xdrv_evtchnl_info *channel);
static struct xdrv_shared_buffer_info *xdrv_sh_buf_alloc(
	struct xdrv_info *drv_info, uint64_t dumb_cookie,
	struct sg_table *sgt, unsigned int buffer_size);
static grant_ref_t xdrv_sh_buf_get_dir_start(
	struct xdrv_shared_buffer_info *buf);
static void xdrv_sh_buf_free_by_dumb_cookie(struct xdrv_info *drv_info,
	uint64_t dumb_cookie);
static struct xdrv_shared_buffer_info *xdrv_sh_buf_get_by_dumb_cookie(
	struct xdrv_info *drv_info, uint64_t dumb_cookie);
static void xdrv_sh_buf_flush_fb(struct xdrv_info *drv_info,
	uint64_t fb_cookie);
static void xdrv_drm_unload(struct xdrv_info *drv_info);

static inline struct xendispl_req *ddrv_be_prepare_req(
	struct xdrv_evtchnl_info *evtchnl, uint8_t operation)
{
	struct xendispl_req *req;

	req = RING_GET_REQUEST(&evtchnl->u.ctrl.ring,
		evtchnl->u.ctrl.ring.req_prod_pvt);
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

	reinit_completion(&evtchnl->u.ctrl.completion);
	if (unlikely(evtchnl->state != EVTCHNL_STATE_CONNECTED)) {
		spin_unlock_irqrestore(&evtchnl->drv_info->io_lock, flags);
		return -EIO;
	}
	xdrv_evtchnl_flush(evtchnl);
	spin_unlock_irqrestore(&evtchnl->drv_info->io_lock, flags);
	ret = 0;
	if (wait_for_completion_interruptible_timeout(
			&evtchnl->u.ctrl.completion,
			msecs_to_jiffies(VDRM_WAIT_BACK_MS)) <= 0)
		ret = -ETIMEDOUT;
	if (ret < 0)
		return ret;
	return drmif_to_kern_error(evtchnl->u.ctrl.resp_status);
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
	evtchnl = &drv_info->evt_pairs[xen_crtc->index].ctrl;
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

int xendispl_front_dbuf_create(struct xdrv_info *drv_info, uint64_t dumb_cookie,
	uint32_t width, uint32_t height, uint32_t bpp, uint64_t size,
	struct sg_table *sgt)
{
	struct xdrv_evtchnl_info *evtchnl;
	struct xdrv_shared_buffer_info *buf;
	struct xendispl_req *req;
	unsigned long flags;
	int ret;

	evtchnl = &drv_info->evt_pairs[GENERIC_OP_EVT_CHNL].ctrl;
	if (unlikely(!evtchnl))
		return -EIO;
	buf = xdrv_sh_buf_alloc(drv_info, dumb_cookie, sgt, size);
	if (!buf)
		return -ENOMEM;
	spin_lock_irqsave(&drv_info->io_lock, flags);
	req = ddrv_be_prepare_req(evtchnl, XENDISPL_OP_DBUF_CREATE);
	req->op.dbuf_create.gref_directory_start =
		xdrv_sh_buf_get_dir_start(buf);
	req->op.dbuf_create.buffer_sz = size;
	req->op.dbuf_create.dbuf_cookie = dumb_cookie;
	req->op.dbuf_create.width = width;
	req->op.dbuf_create.height = height;
	req->op.dbuf_create.bpp = bpp;
	ret = ddrv_be_stream_do_io(evtchnl, req, flags);
	if (ret < 0)
		xdrv_sh_buf_free_by_dumb_cookie(drv_info, dumb_cookie);
	return ret;
}

int xendispl_front_dbuf_destroy(struct xdrv_info *drv_info,
	uint64_t dumb_cookie)
{
	struct xdrv_evtchnl_info *evtchnl;
	struct xendispl_req *req;
	unsigned long flags;
	int ret;

	evtchnl = &drv_info->evt_pairs[GENERIC_OP_EVT_CHNL].ctrl;
	if (unlikely(!evtchnl))
		return -EIO;
	spin_lock_irqsave(&drv_info->io_lock, flags);
	req = ddrv_be_prepare_req(evtchnl, XENDISPL_OP_DBUF_DESTROY);
	req->op.dbuf_destroy.dbuf_cookie = dumb_cookie;
	ret = ddrv_be_stream_do_io(evtchnl, req, flags);
	xdrv_sh_buf_free_by_dumb_cookie(drv_info, dumb_cookie);
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

	evtchnl = &drv_info->evt_pairs[GENERIC_OP_EVT_CHNL].ctrl;
	if (unlikely(!evtchnl))
		return -EIO;
	buf = xdrv_sh_buf_get_by_dumb_cookie(drv_info, dumb_cookie);
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

	evtchnl = &drv_info->evt_pairs[GENERIC_OP_EVT_CHNL].ctrl;
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
	xdrv_sh_buf_flush_fb(drv_info, fb_cookie);
	evtchnl = &drv_info->evt_pairs[conn_idx].ctrl;
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

#ifdef CONFIG_DRM_GEM_CMA_HELPER
/* Unprivileged guests (i.e. ones without hardware) are not permitted to
 * make mappings with anything other than a writeback memory type
 * So, we need to override mmap used by the kernel and make changes
 * to vma->vm_page_prot
 * N.B. this is almost the original dma_common_mmap altered
 */
static int xdrv_mmap(struct device *dev, struct vm_area_struct *vma,
	void *cpu_addr, dma_addr_t dma_addr, size_t size, unsigned long attrs)
{
	int ret = -ENXIO;
#if defined(CONFIG_MMU) && !defined(CONFIG_ARCH_NO_COHERENT_DMA_MMAP)
	unsigned long user_count = vma_pages(vma);
	unsigned long count = PAGE_ALIGN(size) >> PAGE_SHIFT;
	unsigned long pfn = page_to_pfn(virt_to_page(cpu_addr));
	unsigned long off = vma->vm_pgoff;

	vma->vm_page_prot = PAGE_SHARED;

	if (dma_mmap_from_coherent(dev, vma, cpu_addr, size, &ret))
		return ret;
	if (off < count && user_count <= (count - off)) {
		ret = remap_pfn_range(vma, vma->vm_start,
			pfn + off, user_count << PAGE_SHIFT,
			vma->vm_page_prot);
	}
#endif	/* CONFIG_MMU && !CONFIG_ARCH_NO_COHERENT_DMA_MMAP */
	return ret;
}

static void xdrv_setup_dma_map_ops(struct xdrv_info *xdrv_info,
	struct device *dev)
{
	if (xdrv_info->dma_map_ops.mmap != xdrv_mmap) {
		xdrv_info->dma_map_ops = *(get_dma_ops(dev));
		xdrv_info->dma_map_ops.mmap = xdrv_mmap;
	}
	dev->archdata.dma_ops = &xdrv_info->dma_map_ops;
}
#endif

static int ddrv_probe(struct platform_device *pdev)
{
#ifdef CONFIG_DRM_GEM_CMA_HELPER
	struct xendrm_plat_data *platdata = dev_get_platdata(&pdev->dev);

	xdrv_setup_dma_map_ops(platdata->xdrv_info, &pdev->dev);
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
	rp = channel->u.ctrl.ring.sring->rsp_prod;
	/* Ensure we see queued responses up to 'rp'. */
	virt_rmb();
	for (i = channel->u.ctrl.ring.rsp_cons; i != rp; i++) {
		resp = RING_GET_RESPONSE(&channel->u.ctrl.ring, i);
		if (unlikely(resp->id != channel->evt_id))
			continue;
		switch (resp->operation) {
		case XENDISPL_OP_PG_FLIP:
		case XENDISPL_OP_FB_ATTACH:
		case XENDISPL_OP_FB_DETACH:
		case XENDISPL_OP_DBUF_CREATE:
		case XENDISPL_OP_DBUF_DESTROY:
		case XENDISPL_OP_SET_CONFIG:
			channel->u.ctrl.resp_status = resp->status;
			complete(&channel->u.ctrl.completion);
			break;
		default:
			DRM_ERROR("Operation %d is not supported\n",
				resp->operation);
			break;
		}
	}
	channel->u.ctrl.ring.rsp_cons = i;
	if (i != channel->u.ctrl.ring.req_prod_pvt) {
		int more_to_do;

		RING_FINAL_CHECK_FOR_RESPONSES(&channel->u.ctrl.ring,
			more_to_do);
		if (more_to_do)
			goto again;
	} else
		channel->u.ctrl.ring.sring->rsp_event = i + 1;
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

	if (channel->type == EVTCHNL_TYPE_CTRL)
		page = (unsigned long)channel->u.ctrl.ring.sring;
	else if (channel->type == EVTCHNL_TYPE_EVT)
		page = (unsigned long)channel->u.evt.page;
	if (!page)
		return;
	channel->state = EVTCHNL_STATE_DISCONNECTED;
	if (channel->type == EVTCHNL_TYPE_CTRL) {
		/* release all who still waits for response if any */
		channel->u.ctrl.resp_status = -XENDISPL_RSP_ERROR;
		complete_all(&channel->u.ctrl.completion);
	}
	if (channel->irq)
		unbind_from_irqhandler(channel->irq, channel);
	if (channel->port)
		xenbus_free_evtchn(drv_info->xb_dev, channel->port);
	/* End access and free the pages */
	if (channel->gref != GRANT_INVALID_REF)
		gnttab_end_foreign_access(channel->gref, 0, page);
	if (channel->type == EVTCHNL_TYPE_CTRL)
		channel->u.ctrl.ring.sring = NULL;
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
			&drv_info->evt_pairs[i].ctrl);
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
	if (type == EVTCHNL_TYPE_CTRL) {
		struct xen_displif_sring *sring;

		init_completion(&evt_channel->u.ctrl.completion);
		sring = (struct xen_displif_sring *)page;
		SHARED_RING_INIT(sring);
		FRONT_RING_INIT(&evt_channel->u.ctrl.ring,
			sring, XEN_PAGE_SIZE);

		ret = xenbus_grant_ring(xb_dev, sring, 1, &gref);
		if (ret < 0)
			goto fail;
		handler = xdrv_evtchnl_interrupt_ctrl;
	} else {
		evt_channel->u.evt.page = (struct xendispl_event_page *)page;
		ret = gnttab_grant_foreign_access(xb_dev->otherend_id,
			virt_to_gfn(page), 0);
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

	channel->u.ctrl.ring.req_prod_pvt++;
	RING_PUSH_REQUESTS_AND_CHECK_NOTIFY(&channel->u.ctrl.ring, notify);
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
			&drv_info->evt_pairs[conn].ctrl, EVTCHNL_TYPE_CTRL);
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
			&drv_info->evt_pairs[conn].ctrl,
			plat_data->connectors[conn].xenstore_path,
			XENDISPL_FIELD_CTRL_RING_REF,
			XENDISPL_FIELD_CTRL_CHANNEL);
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
		drv_info->evt_pairs[i].ctrl.state = state;
		drv_info->evt_pairs[i].evt.state = state;
	}
	spin_unlock_irqrestore(&drv_info->io_lock, flags);

}
/* get number of nodes under the path to get number of
 * connectors
 */
static char **xdrv_cfg_get_num_nodes(const char *path, const char *node,
		int *num_entries)
{
	char **result;

	result = xenbus_directory(XBT_NIL, path, node, num_entries);
	if (IS_ERR(result)) {
		*num_entries = 0;
		return NULL;
	}
	return result;
}

static int xdrv_cfg_connector(struct xdrv_info *drv_info,
	struct xendrm_cfg_connector *connector,
	const char *path, int index)
{
	char *connector_path;
	int ret;

	connector_path = devm_kasprintf(&drv_info->xb_dev->dev,
		GFP_KERNEL, "%s/%s/%d", path, XENDISPL_PATH_CONNECTOR, index);
	if (!connector_path)
		return -ENOMEM;
	connector->xenstore_path = connector_path;
	if (xenbus_scanf(XBT_NIL, connector_path, XENDISPL_FIELD_RESOLUTION,
			"%d" XENDISPL_RESOLUTION_SEPARATOR "%d",
			&connector->width, &connector->height) < 0) {
		connector->width = 0;
		connector->height = 0;
		ret = -EINVAL;
		DRM_ERROR("Wrong connector resolution\n");
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
	const char *path;
	char **connector_nodes = NULL;
	int ret, num_conn, i;

	path = xb_dev->nodename;
	plat_data->num_connectors = 0;
	connector_nodes = xdrv_cfg_get_num_nodes(path, XENDISPL_PATH_CONNECTOR,
		&num_conn);
	kfree(connector_nodes);
	if (!num_conn) {
		DRM_ERROR("No connectors configured at %s/%s\n",
			path, XENDISPL_PATH_CONNECTOR);
		return -ENODEV;
	}
	if (num_conn > XENDRM_MAX_CRTCS) {
		DRM_WARN("Only %d connectors supported, skipping the rest\n",
			XENDRM_MAX_CRTCS);
		num_conn = XENDRM_MAX_CRTCS;
	}
	for (i = 0; i < num_conn; i++) {
		ret = xdrv_cfg_connector(drv_info,
			&plat_data->connectors[i], path, i);
		if (ret < 0)
			return ret;
	}
	plat_data->num_connectors = num_conn;
	return 0;
}

static grant_ref_t xdrv_sh_buf_get_dir_start(
	struct xdrv_shared_buffer_info *buf)
{
	if (!buf->grefs)
		return GRANT_INVALID_REF;
	return buf->grefs[0];
}

static struct xdrv_shared_buffer_info *xdrv_sh_buf_get_by_dumb_cookie(
	struct xdrv_info *drv_info, uint64_t dumb_cookie)
{
	struct xdrv_shared_buffer_info *buf, *q;

	list_for_each_entry_safe(buf, q, &drv_info->dumb_buf_list, list) {
		if (buf->dumb_cookie == dumb_cookie)
			return buf;
	}
	return NULL;
}

#ifdef CONFIG_X86
static void xdrv_sh_buf_flush_fb(struct xdrv_info *drv_info, uint64_t fb_cookie)
{
	struct xdrv_shared_buffer_info *buf, *q;

	list_for_each_entry_safe(buf, q, &drv_info->dumb_buf_list, list) {
		if (buf->fb_cookie == fb_cookie) {
			struct scatterlist *sg;
			unsigned int count;

			for_each_sg(buf->sgt->sgl, sg, buf->sgt->nents, count)
				clflush_cache_range(sg_virt(sg), sg->length);
			break;
		}
	}
}
#else
#define xdrv_sh_buf_flush_fb {}
#endif

static void xdrv_sh_buf_free(struct xdrv_shared_buffer_info *buf)
{
	int i;

	if (buf->grefs) {
		/* [0] entry is used for page directory, so skip it and use
		 * the one which is used for the buffer which is expected
		 * to be released at this time
		 */
		if (unlikely(gnttab_query_foreign_access(buf->grefs[1]) &&
				buf->num_grefs)) {
			int try = 5;

			/* reference is not yet updated by the Xen, we can
			 * end access but it will make the removal deferred,
			 * so give it a chance
			 */
			do {
				DRM_WARN("Grant refs are not released yet\n");
				msleep(20);
				if (!gnttab_query_foreign_access(buf->grefs[1]))
					break;
			} while (--try);
		}
		for (i = 0; i < buf->num_grefs; i++)
			if (buf->grefs[i] != GRANT_INVALID_REF)
				gnttab_end_foreign_access(buf->grefs[i],
					0, 0UL);
		kfree(buf->grefs);
	}
	kfree(buf->vdirectory);
	sg_free_table(buf->sgt);
	kfree(buf);
}

static void xdrv_sh_buf_free_by_dumb_cookie(struct xdrv_info *drv_info,
	uint64_t dumb_cookie)
{
	struct xdrv_shared_buffer_info *buf, *q;

	list_for_each_entry_safe(buf, q, &drv_info->dumb_buf_list, list) {
		if (buf->dumb_cookie == dumb_cookie) {
			list_del(&buf->list);
			xdrv_sh_buf_free(buf);
			break;
		}
	}
}

static void xdrv_sh_buf_free_all(struct xdrv_info *drv_info)
{
	struct xdrv_shared_buffer_info *buf, *q;

	list_for_each_entry_safe(buf, q, &drv_info->dumb_buf_list, list) {
		list_del(&buf->list);
		xdrv_sh_buf_free(buf);
	}
}

/* number of grefs a page can hold with respect to the
 * xendispl_page_directory header
 */
#define XENDRM_NUM_GREFS_PER_PAGE ((XEN_PAGE_SIZE - \
	offsetof(struct xendispl_page_directory, gref)) / \
	sizeof(grant_ref_t))

void xdrv_sh_buf_fill_page_dir(struct xdrv_shared_buffer_info *buf,
		int num_pages_dir)
{
	struct xendispl_page_directory *page_dir;
	unsigned char *ptr;
	int i, cur_gref, grefs_left, to_copy;

	ptr = buf->vdirectory;
	grefs_left = buf->num_grefs - num_pages_dir;
	/* skip grefs at start, they are for pages granted for the directory */
	cur_gref = num_pages_dir;
	for (i = 0; i < num_pages_dir; i++) {
		page_dir = (struct xendispl_page_directory *)ptr;
		if (grefs_left <= XENDRM_NUM_GREFS_PER_PAGE) {
			to_copy = grefs_left;
			page_dir->gref_dir_next_page = GRANT_INVALID_REF;
		} else {
			to_copy = XENDRM_NUM_GREFS_PER_PAGE;
			page_dir->gref_dir_next_page = buf->grefs[i + 1];
		}
		memcpy(&page_dir->gref, &buf->grefs[cur_gref],
			to_copy * sizeof(grant_ref_t));
		ptr += XEN_PAGE_SIZE;
		grefs_left -= to_copy;
		cur_gref += to_copy;
	}
}

int xdrv_sh_buf_grant_refs(struct xenbus_device *xb_dev,
	struct xdrv_shared_buffer_info *buf,
	int num_pages_dir, int num_pages_vbuffer, int num_grefs)
{
	grant_ref_t priv_gref_head;
	int ret, i, j, cur_ref;
	int otherend_id;
	int count;
	struct scatterlist *sg;

	ret = gnttab_alloc_grant_references(num_grefs, &priv_gref_head);
	if (ret < 0) {
		DRM_ERROR("Cannot allocate grant references\n");
		return ret;
	}
	buf->num_grefs = num_grefs;
	otherend_id = xb_dev->otherend_id;
	j = 0;
	for (i = 0; i < num_pages_dir; i++) {
		cur_ref = gnttab_claim_grant_reference(&priv_gref_head);
		if (cur_ref < 0)
			return cur_ref;
		gnttab_grant_foreign_access_ref(cur_ref, otherend_id,
			xen_page_to_gfn(virt_to_page(buf->vdirectory +
				XEN_PAGE_SIZE * i)), 0);
		buf->grefs[j++] = cur_ref;
	}
	for_each_sg(buf->sgt->sgl, sg, buf->sgt->nents, count) {
		struct page *page;
		int len;

		len = sg->length;
		page = sg_page(sg);
		while (len > 0) {
			cur_ref = gnttab_claim_grant_reference(&priv_gref_head);
			if (cur_ref < 0)
				return cur_ref;
			gnttab_grant_foreign_access_ref(cur_ref, otherend_id,
				xen_page_to_gfn(page), 0);
			buf->grefs[j++] = cur_ref;
			len -= PAGE_SIZE;
			page++;
			num_pages_vbuffer--;
		}
	}
	WARN_ON(num_pages_vbuffer != 0);
	gnttab_free_grant_references(priv_gref_head);
	return 0;
}

int xdrv_sh_buf_alloc_buffers(struct xdrv_shared_buffer_info *buf,
		int num_pages_dir, int num_pages_vbuffer,
		int num_grefs)
{
	buf->grefs = kcalloc(num_grefs, sizeof(*buf->grefs), GFP_KERNEL);
	if (!buf->grefs)
		return -ENOMEM;
	buf->vdirectory = kcalloc(num_pages_dir, XEN_PAGE_SIZE, GFP_KERNEL);
	if (!buf->vdirectory) {
		kfree(buf->grefs);
		buf->grefs = NULL;
		return -ENOMEM;
	}
	buf->vbuffer_sz = num_pages_vbuffer * XEN_PAGE_SIZE;
	return 0;
}

static struct xdrv_shared_buffer_info *
xdrv_sh_buf_alloc(struct xdrv_info *drv_info, uint64_t dumb_cookie,
	struct sg_table *sgt, unsigned int buffer_size)
{
	struct xdrv_shared_buffer_info *buf;
	int num_pages_vbuffer, num_pages_dir, num_grefs;

	if (!sgt)
		return NULL;
	buf = kzalloc(sizeof(*buf), GFP_KERNEL);
	if (!buf)
		return NULL;
	buf->sgt = sgt;
	buf->dumb_cookie = dumb_cookie;
	num_pages_vbuffer = DIV_ROUND_UP(buffer_size, XEN_PAGE_SIZE);
	/* number of pages the directory itself consumes */
	num_pages_dir = DIV_ROUND_UP(num_pages_vbuffer,
		XENDRM_NUM_GREFS_PER_PAGE);
	num_grefs = num_pages_vbuffer + num_pages_dir;

	if (xdrv_sh_buf_alloc_buffers(buf, num_pages_dir,
			num_pages_vbuffer, num_grefs) < 0)
		goto fail;
	if (xdrv_sh_buf_grant_refs(drv_info->xb_dev, buf,
			num_pages_dir, num_pages_vbuffer, num_grefs) < 0)
		goto fail;
	xdrv_sh_buf_fill_page_dir(buf, num_pages_dir);
	list_add(&buf->list, &drv_info->dumb_buf_list);
	return buf;
fail:
	xdrv_sh_buf_free(buf);
	return NULL;
}

static void xdrv_remove_internal(struct xdrv_info *drv_info)
{
	ddrv_cleanup(drv_info);
	xdrv_evtchnl_free_all(drv_info);
	xdrv_sh_buf_free_all(drv_info);
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
	DRM_INFO("Have %d conectors\n", cfg_plat_data->num_connectors);
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

	case XenbusStateUnknown:
		/* fall through */
	case XenbusStateClosed:
		if (xb_dev->state == XenbusStateClosed)
			break;
		if (xb_dev->state == XenbusStateInitialising)
			break;
		/* Missed the backend's CLOSING state -- fall-through */
	case XenbusStateClosing:
		/* FIXME: is this check needed? */
		if (xb_dev->state == XenbusStateClosing)
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

