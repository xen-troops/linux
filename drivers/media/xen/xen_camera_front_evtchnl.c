// SPDX-License-Identifier: GPL-2.0 OR MIT

/*
 * Xen para-virtual camera device
 *
 * Copyright (C) 2018 EPAM Systems Inc.
 *
 * Author: Oleksandr Andrushchenko <oleksandr_andrushchenko@epam.com>
 */

#include <xen/events.h>
#include <xen/grant_table.h>
#include <xen/xen.h>
#include <xen/xenbus.h>

#include "xen_camera_front.h"
#include "xen_camera_front_evtchnl.h"
#include "xen_camera_front_v4l2.h"

static irqreturn_t evtchnl_interrupt_req(int irq, void *dev_id)
{
	struct xen_camera_front_evtchnl *channel = dev_id;
	struct xen_camera_front_info *front_info = channel->front_info;
	struct xencamera_resp *resp;
	RING_IDX i, rp;

	if (unlikely(channel->state != EVTCHNL_STATE_CONNECTED))
		return IRQ_HANDLED;

	mutex_lock(&channel->ring_io_lock);

again:
	rp = channel->u.req.ring.sring->rsp_prod;
	/* Ensure we see queued responses up to rp. */
	rmb();

	/*
	 * Assume that the backend is trusted to always write sane values
	 * to the ring counters, so no overflow checks on frontend side
	 * are required.
	 */
	for (i = channel->u.req.ring.rsp_cons; i != rp; i++) {
		resp = RING_GET_RESPONSE(&channel->u.req.ring, i);
		if (resp->id != channel->evt_id)
			continue;
		switch (resp->operation) {
		case XENCAMERA_OP_CONFIG_SET:
			/* fall through */
		case XENCAMERA_OP_CONFIG_GET:
			/* fall through */
		case XENCAMERA_OP_CONFIG_VALIDATE:
			/* fall through */
		case XENCAMERA_OP_BUF_GET_LAYOUT:
			/* fall through */
		case XENCAMERA_OP_BUF_REQUEST:
			/* fall through */
		case XENCAMERA_OP_CTRL_ENUM:
			/* fall through */
		case XENCAMERA_OP_CTRL_GET:
			/*
			 * The requests above all expect data in the response,
			 * so we need to make a copy and then proceeed as
			 * usually.
			 */
			channel->u.req.resp = *resp;
			/* fall through */
		case XENCAMERA_OP_FRAME_RATE_SET:
			/* fall through */
		case XENCAMERA_OP_BUF_CREATE:
			/* fall through */
		case XENCAMERA_OP_BUF_DESTROY:
			/* fall through */
		case XENCAMERA_OP_BUF_QUEUE:
			/* fall through */
		case XENCAMERA_OP_BUF_DEQUEUE:
			/* fall through */
		case XENCAMERA_OP_CTRL_SET:
			/* fall through */
		case XENCAMERA_OP_STREAM_START:
			/* fall through */
		case XENCAMERA_OP_STREAM_STOP:
			channel->u.req.resp_status = resp->status;
			complete(&channel->u.req.completion);
			break;
		default:
			dev_err(&front_info->xb_dev->dev,
				"Operation %d is not supported\n",
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
	} else {
		channel->u.req.ring.sring->rsp_event = i + 1;
	}

	mutex_unlock(&channel->ring_io_lock);
	return IRQ_HANDLED;
}

static irqreturn_t evtchnl_interrupt_evt(int irq, void *dev_id)
{
	struct xen_camera_front_evtchnl *channel = dev_id;
	struct xen_camera_front_info *front_info = channel->front_info;
	struct xencamera_event_page *page = channel->u.evt.page;
	u32 cons, prod;

	if (unlikely(channel->state != EVTCHNL_STATE_CONNECTED))
		return IRQ_HANDLED;

	mutex_lock(&channel->ring_io_lock);

	prod = page->in_prod;
	/* Ensure we see ring contents up to prod. */
	virt_rmb();
	if (prod == page->in_cons)
		goto out;

	/*
	 * Assume that the backend is trusted to always write sane values
	 * to the ring counters, so no overflow checks on frontend side
	 * are required.
	 */
	for (cons = page->in_cons; cons != prod; cons++) {
		struct xencamera_evt *event;

		event = &XENCAMERA_IN_RING_REF(page, cons);
		if (unlikely(event->id != channel->evt_id++))
			continue;

		switch (event->type) {
		case XENCAMERA_EVT_FRAME_AVAIL:
			xen_camera_front_v4l2_on_frame(channel->front_info,
						       &event->evt.frame_avail);
			break;
		case XENCAMERA_EVT_CTRL_CHANGE:
			xen_camera_front_v4l2_on_ctrl(channel->front_info,
						      &event->evt.ctrl_value);
			break;
		default:
			dev_err(&front_info->xb_dev->dev,
				"Event %d is not supported\n",
				event->type);
			break;
		}
	}

	page->in_cons = cons;
	/* Ensure ring contents. */
	virt_wmb();

out:
	mutex_unlock(&channel->ring_io_lock);
	return IRQ_HANDLED;
}

void xen_camera_front_evtchnl_flush(struct xen_camera_front_evtchnl *channel)
{
	int notify;

	channel->u.req.ring.req_prod_pvt++;
	RING_PUSH_REQUESTS_AND_CHECK_NOTIFY(&channel->u.req.ring, notify);
	if (notify)
		notify_remote_via_irq(channel->irq);
}

static void evtchnl_free(struct xen_camera_front_info *front_info,
			 struct xen_camera_front_evtchnl *channel)
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
		/* Release all who still waits for response if any. */
		channel->u.req.resp_status = -EIO;
		complete_all(&channel->u.req.completion);
	}

	if (channel->irq)
		unbind_from_irqhandler(channel->irq, channel);

	if (channel->port)
		xenbus_free_evtchn(front_info->xb_dev, channel->port);

	/* End access and free the page. */
	if (channel->gref != GRANT_INVALID_REF)
		gnttab_end_foreign_access(channel->gref, 0, page);
	else
		free_page(page);

	memset(channel, 0, sizeof(*channel));
}

void xen_camera_front_evtchnl_free_all(struct xen_camera_front_info *front_info)
{
	evtchnl_free(front_info, &front_info->evt_pair.req);
	evtchnl_free(front_info, &front_info->evt_pair.evt);

	memset(&front_info->evt_pair, 0, sizeof(front_info->evt_pair));
}

static int evtchnl_alloc(struct xen_camera_front_info *front_info,
			 struct xen_camera_front_evtchnl *channel,
			 enum xen_camera_front_evtchnl_type type)
{
	struct xenbus_device *xb_dev = front_info->xb_dev;
	unsigned long page;
	grant_ref_t gref;
	irq_handler_t handler;
	char *handler_name = NULL;
	int ret;

	memset(channel, 0, sizeof(*channel));
	channel->type = type;
	channel->front_info = front_info;
	channel->state = EVTCHNL_STATE_DISCONNECTED;
	channel->gref = GRANT_INVALID_REF;
	page = get_zeroed_page(GFP_KERNEL);
	if (!page) {
		ret = -ENOMEM;
		goto fail;
	}

	handler_name = kasprintf(GFP_KERNEL, "%s-%s", XENCAMERA_DRIVER_NAME,
				 type == EVTCHNL_TYPE_REQ ?
				 XENCAMERA_FIELD_REQ_RING_REF :
				 XENCAMERA_FIELD_EVT_RING_REF);
	if (!handler_name) {
		ret = -ENOMEM;
		goto fail;
	}

	mutex_init(&channel->ring_io_lock);

	if (type == EVTCHNL_TYPE_REQ) {
		struct xen_cameraif_sring *sring =
			(struct xen_cameraif_sring *)page;

		init_completion(&channel->u.req.completion);
		mutex_init(&channel->u.req.req_io_lock);
		SHARED_RING_INIT(sring);
		FRONT_RING_INIT(&channel->u.req.ring, sring, XEN_PAGE_SIZE);

		ret = xenbus_grant_ring(xb_dev, sring, 1, &gref);
		if (ret < 0) {
			channel->u.req.ring.sring = NULL;
			goto fail;
		}

		handler = evtchnl_interrupt_req;
	} else {
		ret = gnttab_grant_foreign_access(xb_dev->otherend_id,
						  virt_to_gfn((void *)page), 0);
		if (ret < 0)
			goto fail;

		channel->u.evt.page = (struct xencamera_event_page *)page;
		gref = ret;
		handler = evtchnl_interrupt_evt;
	}

	channel->gref = gref;

	ret = xenbus_alloc_evtchn(xb_dev, &channel->port);
	if (ret < 0)
		goto fail;

	ret = bind_evtchn_to_irq(channel->port);
	if (ret < 0) {
		dev_err(&xb_dev->dev,
			"Failed to bind IRQ for domid %d port %d: %d\n",
			front_info->xb_dev->otherend_id, channel->port, ret);
		goto fail;
	}

	channel->irq = ret;

	ret = request_threaded_irq(channel->irq, NULL, handler,
				   IRQF_ONESHOT, handler_name, channel);
	if (ret < 0) {
		dev_err(&xb_dev->dev, "Failed to request IRQ %d: %d\n",
			channel->irq, ret);
		goto fail;
	}

	kfree(handler_name);
	return 0;

fail:
	if (page)
		free_page(page);
	kfree(handler_name);
	dev_err(&xb_dev->dev, "Failed to allocate ring: %d\n", ret);
	return ret;
}

int xen_camera_front_evtchnl_create_all(struct xen_camera_front_info *front_info)
{
	struct device *dev = &front_info->xb_dev->dev;
	int ret;

	ret = evtchnl_alloc(front_info, &front_info->evt_pair.req,
			    EVTCHNL_TYPE_REQ);
	if (ret < 0) {
		dev_err(dev, "Error allocating control channel\n");
		goto fail;
	}

	ret = evtchnl_alloc(front_info, &front_info->evt_pair.evt,
			    EVTCHNL_TYPE_EVT);
	if (ret < 0) {
		dev_err(dev, "Error allocating in-event channel\n");
		goto fail;
	}

	return 0;

fail:
	xen_camera_front_evtchnl_free_all(front_info);
	return ret;
}

static int evtchnl_publish(struct xenbus_transaction xbt,
			   struct xen_camera_front_evtchnl *channel,
			   const char *path, const char *node_ring,
			   const char *node_chnl)
{
	struct xenbus_device *xb_dev = channel->front_info->xb_dev;
	int ret;

	/* Write control channel ring reference. */
	ret = xenbus_printf(xbt, path, node_ring, "%u", channel->gref);
	if (ret < 0) {
		dev_err(&xb_dev->dev, "Error writing ring-ref: %d\n", ret);
		return ret;
	}

	/* Write event channel ring reference. */
	ret = xenbus_printf(xbt, path, node_chnl, "%u", channel->port);
	if (ret < 0) {
		dev_err(&xb_dev->dev, "Error writing event channel: %d\n", ret);
		return ret;
	}

	return 0;
}

int xen_camera_front_evtchnl_publish_all(struct xen_camera_front_info *front_info)
{
	struct xenbus_transaction xbt;
	int ret;

again:
	ret = xenbus_transaction_start(&xbt);
	if (ret < 0) {
		xenbus_dev_fatal(front_info->xb_dev, ret,
				 "starting transaction");

		return ret;
	}

	ret = evtchnl_publish(xbt,
			      &front_info->evt_pair.req,
			      front_info->xb_dev->nodename,
			      XENCAMERA_FIELD_REQ_RING_REF,
			      XENCAMERA_FIELD_REQ_CHANNEL);
	if (ret < 0)
		goto fail;

	ret = evtchnl_publish(xbt,
			      &front_info->evt_pair.evt,
			      front_info->xb_dev->nodename,
			      XENCAMERA_FIELD_EVT_RING_REF,
			      XENCAMERA_FIELD_EVT_CHANNEL);
	if (ret < 0)
		goto fail;

	ret = xenbus_transaction_end(xbt, 0);
	if (ret < 0) {
		if (ret == -EAGAIN)
			goto again;

		xenbus_dev_fatal(front_info->xb_dev, ret,
				 "completing transaction");
		goto fail_to_end;
	}
	return 0;
fail:
	xenbus_transaction_end(xbt, 1);
fail_to_end:
	xenbus_dev_fatal(front_info->xb_dev, ret, "writing XenStore");
	return ret;
}

void xen_camera_front_evtchnl_pair_set_connected(struct xen_camera_front_evtchnl_pair *evt_pair,
						 bool is_connected)
{
	enum xen_camera_front_evtchnl_state state;

	if (is_connected)
		state = EVTCHNL_STATE_CONNECTED;
	else
		state = EVTCHNL_STATE_DISCONNECTED;

	mutex_lock(&evt_pair->req.ring_io_lock);
	evt_pair->req.state = state;
	mutex_unlock(&evt_pair->req.ring_io_lock);

	mutex_lock(&evt_pair->evt.ring_io_lock);
	evt_pair->evt.state = state;
	mutex_unlock(&evt_pair->evt.ring_io_lock);
}

void xen_camera_front_evtchnl_pair_clear(struct xen_camera_front_evtchnl_pair *evt_pair)
{
	mutex_lock(&evt_pair->req.ring_io_lock);
	evt_pair->req.evt_next_id = 0;
	mutex_unlock(&evt_pair->req.ring_io_lock);

	mutex_lock(&evt_pair->evt.ring_io_lock);
	evt_pair->evt.evt_next_id = 0;
	mutex_unlock(&evt_pair->evt.ring_io_lock);
}

