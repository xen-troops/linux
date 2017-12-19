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

#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/module.h>
#include <linux/slab.h>

#include <asm/xen/hypervisor.h>
#include <xen/platform_pci.h>
#include <xen/xen.h>
#include <xen/xenbus.h>

#include "xen_drm_balloon.h"
#include "xen_drm_front.h"

#include "xen_drm_front_drv.h"
#include "xen_drm_front_evtchnl.h"
#include "xen_drm_front_shbuf.h"

static struct xendispl_req *be_prepare_req(
	struct xen_drm_front_evtchnl *evtchnl, uint8_t operation)
{
	struct xendispl_req *req;

	req = RING_GET_REQUEST(&evtchnl->u.req.ring,
		evtchnl->u.req.ring.req_prod_pvt);
	req->operation = operation;
	req->id = evtchnl->evt_next_id++;
	evtchnl->evt_id = req->id;
	return req;
}

static int be_stream_do_io(struct xen_drm_front_evtchnl *evtchnl,
	struct xendispl_req *req)
{
	reinit_completion(&evtchnl->u.req.completion);
	if (unlikely(evtchnl->state != EVTCHNL_STATE_CONNECTED))
		return -EIO;

	xen_drm_front_evtchnl_flush(evtchnl);
	return 0;
}

static int be_stream_wait_io(struct xen_drm_front_evtchnl *evtchnl)
{
	if (wait_for_completion_timeout(
			&evtchnl->u.req.completion,
			msecs_to_jiffies(VDRM_WAIT_BACK_MS)) <= 0)
		return -ETIMEDOUT;

	return evtchnl->u.req.resp_status;
}

static int be_mode_set(struct xen_drm_front_crtc *xen_crtc, uint32_t x,
	uint32_t y, uint32_t width, uint32_t height, uint32_t bpp,
	uint64_t fb_cookie)

{
	struct xen_drm_front_evtchnl *evtchnl;
	struct xen_drm_front_info *front_info;
	struct xendispl_req *req;
	unsigned long flags;
	int ret;

	front_info = xen_crtc->drm_info->front_info;
	evtchnl = &front_info->evt_pairs[xen_crtc->index].req;
	if (unlikely(!evtchnl))
		return -EIO;

	spin_lock_irqsave(&front_info->io_lock, flags);
	req = be_prepare_req(evtchnl, XENDISPL_OP_SET_CONFIG);
	req->op.set_config.x = x;
	req->op.set_config.y = y;
	req->op.set_config.width = width;
	req->op.set_config.height = height;
	req->op.set_config.bpp = bpp;
	req->op.set_config.fb_cookie = fb_cookie;

	ret = be_stream_do_io(evtchnl, req);
	spin_unlock_irqrestore(&front_info->io_lock, flags);

	if (ret < 0)
		return ret;

	return be_stream_wait_io(evtchnl);
}

static int be_dbuf_create_int(struct xen_drm_front_info *front_info,
	uint64_t dbuf_cookie, uint32_t width, uint32_t height,
	uint32_t bpp, uint64_t size, struct page **pages,
	struct sg_table *sgt)
{
	struct xen_drm_front_evtchnl *evtchnl;
	struct xen_drm_front_shbuf *buf;
	struct xendispl_req *req;
	struct xen_drm_front_shbuf_alloc alloc_info;
	unsigned long flags;
	bool be_alloc;
	int ret;

	evtchnl = &front_info->evt_pairs[GENERIC_OP_EVT_CHNL].req;
	if (unlikely(!evtchnl))
		return -EIO;

	be_alloc = front_info->cfg_plat_data.be_alloc;

	memset(&alloc_info, 0, sizeof(alloc_info));
	alloc_info.xb_dev = front_info->xb_dev;
	alloc_info.dbuf_list = &front_info->dbuf_list;
	alloc_info.dbuf_cookie = dbuf_cookie;
	alloc_info.pages = pages;
	alloc_info.size = size;
	alloc_info.sgt = sgt;
	alloc_info.be_alloc = be_alloc;
	buf = xen_drm_front_shbuf_alloc(&alloc_info);
	if (!buf)
		return -ENOMEM;

	spin_lock_irqsave(&front_info->io_lock, flags);
	req = be_prepare_req(evtchnl, XENDISPL_OP_DBUF_CREATE);
	req->op.dbuf_create.gref_directory = xen_drm_front_shbuf_get_dir_start(buf);
	req->op.dbuf_create.buffer_sz = size;
	req->op.dbuf_create.dbuf_cookie = dbuf_cookie;
	req->op.dbuf_create.width = width;
	req->op.dbuf_create.height = height;
	req->op.dbuf_create.bpp = bpp;
	if (be_alloc)
		req->op.dbuf_create.flags |= XENDISPL_DBUF_FLG_REQ_ALLOC;

	ret = be_stream_do_io(evtchnl, req);
	spin_unlock_irqrestore(&front_info->io_lock, flags);

	if (ret < 0)
		goto fail;

	ret = be_stream_wait_io(evtchnl);
	if (ret < 0)
		goto fail;

	if (be_alloc) {
		ret = xen_drm_front_shbuf_be_alloc_map(buf);
		if (ret < 0)
			goto fail;
	}

	return 0;

fail:
	xen_drm_front_shbuf_free_by_dbuf_cookie(&front_info->dbuf_list, dbuf_cookie);
	return ret;
}

static int be_dbuf_create_from_sgt(struct xen_drm_front_info *front_info,
	uint64_t dbuf_cookie, uint32_t width, uint32_t height,
	uint32_t bpp, uint64_t size, struct sg_table *sgt)
{
	return be_dbuf_create_int(front_info,
		dbuf_cookie, width, height, bpp, size, NULL, sgt);
}

static int be_dbuf_create(struct xen_drm_front_info *front_info,
	uint64_t dbuf_cookie, uint32_t width, uint32_t height,
	uint32_t bpp, uint64_t size, struct page **pages)
{
	return be_dbuf_create_int(front_info,
		dbuf_cookie, width, height, bpp, size, pages, NULL);
}

static int be_dbuf_destroy(struct xen_drm_front_info *front_info,
	uint64_t dbuf_cookie)
{
	struct xen_drm_front_evtchnl *evtchnl;
	struct xendispl_req *req;
	unsigned long flags;
	bool be_alloc;
	int ret;

	evtchnl = &front_info->evt_pairs[GENERIC_OP_EVT_CHNL].req;
	if (unlikely(!evtchnl))
		return -EIO;

	be_alloc = front_info->cfg_plat_data.be_alloc;

	if (be_alloc)
		xen_drm_front_shbuf_free_by_dbuf_cookie(&front_info->dbuf_list,
			dbuf_cookie);

	spin_lock_irqsave(&front_info->io_lock, flags);
	req = be_prepare_req(evtchnl, XENDISPL_OP_DBUF_DESTROY);
	req->op.dbuf_destroy.dbuf_cookie = dbuf_cookie;

	ret = be_stream_do_io(evtchnl, req);
	spin_unlock_irqrestore(&front_info->io_lock, flags);

	if (ret == 0)
		ret = be_stream_wait_io(evtchnl);

	/*
	 * do this regardless of communication status with the backend:
	 * if we cannot remove remote resources remove what we can locally
	 */
	if (!be_alloc)
		xen_drm_front_shbuf_free_by_dbuf_cookie(&front_info->dbuf_list,
			dbuf_cookie);
	return ret;
}

static int be_fb_attach(struct xen_drm_front_info *front_info,
	uint64_t dbuf_cookie, uint64_t fb_cookie, uint32_t width,
	uint32_t height, uint32_t pixel_format)
{
	struct xen_drm_front_evtchnl *evtchnl;
	struct xen_drm_front_shbuf *buf;
	struct xendispl_req *req;
	unsigned long flags;
	int ret;

	evtchnl = &front_info->evt_pairs[GENERIC_OP_EVT_CHNL].req;
	if (unlikely(!evtchnl))
		return -EIO;

	buf = xen_drm_front_shbuf_get_by_dbuf_cookie(&front_info->dbuf_list,
		dbuf_cookie);
	if (!buf)
		return -EINVAL;

	buf->fb_cookie = fb_cookie;

	spin_lock_irqsave(&front_info->io_lock, flags);
	req = be_prepare_req(evtchnl, XENDISPL_OP_FB_ATTACH);
	req->op.fb_attach.dbuf_cookie = dbuf_cookie;
	req->op.fb_attach.fb_cookie = fb_cookie;
	req->op.fb_attach.width = width;
	req->op.fb_attach.height = height;
	req->op.fb_attach.pixel_format = pixel_format;

	ret = be_stream_do_io(evtchnl, req);
	spin_unlock_irqrestore(&front_info->io_lock, flags);

	if (ret < 0)
		return ret;

	return be_stream_wait_io(evtchnl);
}

static int be_fb_detach(struct xen_drm_front_info *front_info,
	uint64_t fb_cookie)
{
	struct xen_drm_front_evtchnl *evtchnl;
	struct xendispl_req *req;
	unsigned long flags;
	int ret;

	evtchnl = &front_info->evt_pairs[GENERIC_OP_EVT_CHNL].req;
	if (unlikely(!evtchnl))
		return -EIO;

	spin_lock_irqsave(&front_info->io_lock, flags);
	req = be_prepare_req(evtchnl, XENDISPL_OP_FB_DETACH);
	req->op.fb_detach.fb_cookie = fb_cookie;

	ret = be_stream_do_io(evtchnl, req);
	spin_unlock_irqrestore(&front_info->io_lock, flags);

	if (ret < 0)
		return ret;

	return be_stream_wait_io(evtchnl);
}

static int be_page_flip(struct xen_drm_front_info *front_info, int conn_idx,
	uint64_t fb_cookie)
{
	struct xen_drm_front_evtchnl *evtchnl;
	struct xendispl_req *req;
	unsigned long flags;
	int ret;

	if (unlikely(conn_idx >= front_info->num_evt_pairs))
		return -EINVAL;

	xen_drm_front_shbuf_flush_fb(&front_info->dbuf_list, fb_cookie);
	evtchnl = &front_info->evt_pairs[conn_idx].req;
	spin_lock_irqsave(&front_info->io_lock, flags);
	req = be_prepare_req(evtchnl, XENDISPL_OP_PG_FLIP);
	req->op.pg_flip.fb_cookie = fb_cookie;

	ret = be_stream_do_io(evtchnl, req);
	spin_unlock_irqrestore(&front_info->io_lock, flags);

	if (ret < 0)
		return ret;

	return be_stream_wait_io(evtchnl);
}

static void drm_drv_unload(struct xen_drm_front_info *front_info)
{
	if (front_info->xb_dev->state != XenbusStateReconfiguring)
		return;

	DRM_INFO("Can try removing driver now\n");
	xenbus_switch_state(front_info->xb_dev, XenbusStateInitialising);
}

static struct xen_drm_front_ops xen_drm_backend_ops = {
	.mode_set = be_mode_set,
	.dbuf_create = be_dbuf_create,
	.dbuf_create_from_sgt = be_dbuf_create_from_sgt,
	.dbuf_destroy = be_dbuf_destroy,
	.fb_attach = be_fb_attach,
	.fb_detach = be_fb_detach,
	.page_flip = be_page_flip,
	.drm_last_close = drm_drv_unload,
};

static int drm_drv_probe(struct platform_device *pdev)
{
#ifdef CONFIG_DRM_XEN_FRONTEND_CMA
	struct device *dev = &pdev->dev;

	/* make sure we have DMA ops set up, so no dummy ops are in use */
	arch_setup_dma_ops(dev, 0, *dev->dma_mask, NULL, false);
#endif
	return xen_drm_front_drv_probe(pdev, &xen_drm_backend_ops);
}

static int drm_drv_remove(struct platform_device *pdev)
{
	return xen_drm_front_drv_remove(pdev);
}

struct platform_device_info xen_drm_front_platform_info = {
	.name = XENDISPL_DRIVER_NAME,
	.id = 0,
	.num_res = 0,
	.dma_mask = DMA_BIT_MASK(32),
};

static struct platform_driver xen_drm_front_front_info = {
	.probe		= drm_drv_probe,
	.remove		= drm_drv_remove,
	.driver		= {
		.name	= XENDISPL_DRIVER_NAME,
	},
};

static void drm_drv_deinit(struct xen_drm_front_info *front_info)
{
	if (!front_info->drm_pdrv_registered)
		return;

	if (front_info->drm_pdev)
		platform_device_unregister(front_info->drm_pdev);

	platform_driver_unregister(&xen_drm_front_front_info);
	front_info->drm_pdrv_registered = false;
	front_info->drm_pdev = NULL;
}

static int drm_drv_init(struct xen_drm_front_info *front_info)
{
	struct xen_drm_front_cfg_plat_data *platdata;
	int ret;

	ret = platform_driver_register(&xen_drm_front_front_info);
	if (ret < 0)
		return ret;

	front_info->drm_pdrv_registered = true;
	platdata = &front_info->cfg_plat_data;
	/* pass card configuration via platform data */
	xen_drm_front_platform_info.data = platdata;
	xen_drm_front_platform_info.size_data = sizeof(*platdata);
	front_info->drm_pdev = platform_device_register_full(
		&xen_drm_front_platform_info);
	if (IS_ERR(front_info->drm_pdev)) {
		front_info->drm_pdev = NULL;
		goto fail;
	}

	return 0;

fail:
	DRM_ERROR("Failed to register DRM driver\n");
	drm_drv_deinit(front_info);
	return -ENODEV;
}

static void remove_internal(struct xen_drm_front_info *front_info)
{
	drm_drv_deinit(front_info);
	xen_drm_front_evtchnl_free_all(front_info);
	xen_drm_front_shbuf_free_all(&front_info->dbuf_list);
}

static int be_on_initwait(struct xen_drm_front_info *front_info)
{
	struct xen_drm_front_cfg_plat_data *cfg_plat_data;
	int ret;

	cfg_plat_data = &front_info->cfg_plat_data;
	cfg_plat_data->front_info = front_info;
	ret = xen_drm_front_cfg_card(front_info, cfg_plat_data);
	if (ret < 0)
		return ret;

	DRM_INFO("Have %d conector(s)\n", cfg_plat_data->num_connectors);
	/* create event channels for all streams and publish */
	ret = xen_drm_front_evtchnl_create_all(front_info, &xen_drm_backend_ops);
	if (ret < 0)
		return ret;

	return xen_drm_front_evtchnl_publish_all(front_info);
}

static int be_on_connected(struct xen_drm_front_info *front_info)
{
	xen_drm_front_evtchnl_set_state(front_info, EVTCHNL_STATE_CONNECTED);
	return drm_drv_init(front_info);
}

static void be_on_disconnected(struct xen_drm_front_info *front_info)
{
	bool removed = true;

	if (front_info->drm_pdev) {
		if (xen_drm_front_drv_is_used(front_info->drm_pdev)) {
			DRM_WARN("DRM driver still in use, deferring removal\n");
			removed = false;
		} else {
			remove_internal(front_info);
		}
	}

	xen_drm_front_evtchnl_set_state(front_info, EVTCHNL_STATE_DISCONNECTED);

	if (removed)
		xenbus_switch_state(front_info->xb_dev, XenbusStateInitialising);
	else
		xenbus_switch_state(front_info->xb_dev, XenbusStateReconfiguring);
}

static void be_on_changed(struct xenbus_device *xb_dev,
	enum xenbus_state backend_state)
{
	struct xen_drm_front_info *front_info = dev_get_drvdata(&xb_dev->dev);
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
		mutex_lock(&front_info->mutex);
		be_on_disconnected(front_info);
		mutex_unlock(&front_info->mutex);
		break;

	case XenbusStateInitWait:
		/* recovering after backend unexpected closure */
		mutex_lock(&front_info->mutex);
		be_on_disconnected(front_info);
		if (xb_dev->state != XenbusStateInitialising) {
			mutex_unlock(&front_info->mutex);
			break;
		}

		ret = be_on_initwait(front_info);
		mutex_unlock(&front_info->mutex);
		if (ret < 0) {
			xenbus_dev_fatal(xb_dev, ret, "initializing frontend");
			break;
		}

		xenbus_switch_state(xb_dev, XenbusStateInitialised);
		break;

	case XenbusStateConnected:
		if (xb_dev->state != XenbusStateInitialised)
			break;

		mutex_lock(&front_info->mutex);
		ret = be_on_connected(front_info);
		mutex_unlock(&front_info->mutex);
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

		mutex_lock(&front_info->mutex);
		be_on_disconnected(front_info);
		mutex_unlock(&front_info->mutex);
		break;
	}
}

static int xen_drv_probe(struct xenbus_device *xb_dev,
	const struct xenbus_device_id *id)
{
	struct xen_drm_front_info *front_info;
	int ret;

	front_info = devm_kzalloc(&xb_dev->dev, sizeof(*front_info), GFP_KERNEL);
	if (!front_info) {
		ret = -ENOMEM;
		goto fail;
	}

	xenbus_switch_state(xb_dev, XenbusStateInitialising);
	front_info->xb_dev = xb_dev;
	spin_lock_init(&front_info->io_lock);
	INIT_LIST_HEAD(&front_info->dbuf_list);
	mutex_init(&front_info->mutex);
	front_info->drm_pdrv_registered = false;
	dev_set_drvdata(&xb_dev->dev, front_info);
	return 0;

fail:
	xenbus_dev_fatal(xb_dev, ret, "allocating device memory");
	return ret;
}

static int xen_drv_remove(struct xenbus_device *dev)
{
	struct xen_drm_front_info *front_info = dev_get_drvdata(&dev->dev);
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

	while ((xenbus_read_unsigned(front_info->xb_dev->otherend,
		"state", XenbusStateUnknown) != XenbusStateInitWait) && to--)
		msleep(10);

	if (!to)
		DRM_ERROR("Backend state is %s while removing driver\n",
			xenbus_strstate(xenbus_read_unsigned(
					front_info->xb_dev->otherend,
					"state", XenbusStateUnknown)));

	mutex_lock(&front_info->mutex);
	remove_internal(front_info);
	mutex_unlock(&front_info->mutex);
	xenbus_frontend_closed(dev);
	return 0;
}

static const struct xenbus_device_id xen_drv_ids[] = {
	{ XENDISPL_DRIVER_NAME },
	{ "" }
};

static struct xenbus_driver xen_driver = {
	.ids = xen_drv_ids,
	.probe = xen_drv_probe,
	.remove = xen_drv_remove,
	.otherend_changed = be_on_changed,
};

static int __init xen_drv_init(void)
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

static void __exit xen_drv_cleanup(void)
{
	DRM_INFO("Unregistering XEN PV " XENDISPL_DRIVER_NAME "\n");
	xenbus_unregister_driver(&xen_driver);
}

module_init(xen_drv_init);
module_exit(xen_drv_cleanup);

MODULE_DESCRIPTION("Xen virtual display device frontend");
MODULE_LICENSE("GPL");
MODULE_ALIAS("xen:"XENDISPL_DRIVER_NAME);

