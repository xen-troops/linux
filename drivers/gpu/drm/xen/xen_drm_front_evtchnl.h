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

#ifndef __XEN_DRM_FRONT_EVTCHNL_H_
#define __XEN_DRM_FRONT_EVTCHNL_H_

#include <linux/completion.h>
#include <linux/types.h>

#include <xen/interface/io/ring.h>
#include <xen/interface/io/displif.h>

/*
 * all operations which are not connector oriented use this ctrl event channel,
 * e.g. fb_attach/destroy which belong to a DRM device, not to a CRTC
 */
#define GENERIC_OP_EVT_CHNL	0

enum xen_drm_front_evtchnl_state {
	EVTCHNL_STATE_DISCONNECTED,
	EVTCHNL_STATE_CONNECTED,
};

enum xen_drm_front_evtchnl_type {
	EVTCHNL_TYPE_REQ,
	EVTCHNL_TYPE_EVT,
};

struct xen_drm_front_drm_info;

struct xen_drm_front_evtchnl {
	struct xen_drm_front_info *front_info;
	int gref;
	int port;
	int irq;
	int index;
	/* state of the event channel */
	enum xen_drm_front_evtchnl_state state;
	enum xen_drm_front_evtchnl_type type;
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
			struct xen_drm_front_ops *front_ops;
		} evt;
	} u;
};

struct xen_drm_front_evtchnl_pair {
	struct xen_drm_front_evtchnl req;
	struct xen_drm_front_evtchnl evt;
};

int xen_drm_front_evtchnl_create_all(struct xen_drm_front_info *front_info,
	struct xen_drm_front_ops *front_ops);
int xen_drm_front_evtchnl_publish_all(struct xen_drm_front_info *front_info);
void xen_drm_front_evtchnl_flush(struct xen_drm_front_evtchnl *channel);
void xen_drm_front_evtchnl_set_state(struct xen_drm_front_info *front_info,
	enum xen_drm_front_evtchnl_state state);
void xen_drm_front_evtchnl_free_all(struct xen_drm_front_info *front_info);

#endif /* __XEN_DRM_FRONT_EVTCHNL_H_ */
