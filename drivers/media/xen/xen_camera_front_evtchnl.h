/* SPDX-License-Identifier: GPL-2.0 OR MIT */

/*
 * Xen para-virtual camera device
 *
 * Copyright (C) 2018 EPAM Systems Inc.
 *
 * Author: Oleksandr Andrushchenko <oleksandr_andrushchenko@epam.com>
 */

#ifndef __XEN_CAMERA_FRONT_EVTCHNL_H
#define __XEN_CAMERA_FRONT_EVTCHNL_H

#include <xen/interface/io/cameraif.h>

struct xen_camera_front_info;

#ifndef GRANT_INVALID_REF
/*
 * FIXME: usage of grant reference 0 as invalid grant reference:
 * grant reference 0 is valid, but never exposed to a PV driver,
 * because of the fact it is already in use/reserved by the PV console.
 */
#define GRANT_INVALID_REF	0
#endif

/* Timeout in ms to wait for backend to respond. */
#define XEN_CAMERA_FRONT_WAIT_BACK_MS	3000

enum xen_camera_front_evtchnl_state {
	EVTCHNL_STATE_DISCONNECTED,
	EVTCHNL_STATE_CONNECTED,
};

enum xen_camera_front_evtchnl_type {
	EVTCHNL_TYPE_REQ,
	EVTCHNL_TYPE_EVT,
};

struct xen_camera_front_evtchnl {
	struct xen_camera_front_info *front_info;
	int gref;
	int port;
	int irq;
	/* State of the event channel. */
	enum xen_camera_front_evtchnl_state state;
	enum xen_camera_front_evtchnl_type type;
	/* Either response id or incoming event id. */
	u16 evt_id;
	/* Next request id or next expected event id. */
	u16 evt_next_id;
	/* Shared ring access lock. */
	struct mutex ring_io_lock;
	union {
		struct {
			struct xen_cameraif_front_ring ring;
			struct completion completion;
			/* Serializer for backend IO: request/response. */
			struct mutex req_io_lock;

			/* Latest response status. */
			int resp_status;

			/*
			 * This will hold a copy of the response for those
			 * requests expecting data to be sent back.
			 */
			struct xencamera_resp resp;
		} req;
		struct {
			struct xencamera_event_page *page;
		} evt;
	} u;
};

struct xen_camera_front_evtchnl_pair {
	struct xen_camera_front_evtchnl req;
	struct xen_camera_front_evtchnl evt;
};

int xen_camera_front_evtchnl_create_all(struct xen_camera_front_info *front_info);

void xen_camera_front_evtchnl_free_all(struct xen_camera_front_info *front_info);

int xen_camera_front_evtchnl_publish_all(struct xen_camera_front_info *front_info);

void xen_camera_front_evtchnl_flush(struct xen_camera_front_evtchnl *evtchnl);

void xen_camera_front_evtchnl_pair_set_connected(struct xen_camera_front_evtchnl_pair *evt_pair,
						 bool is_connected);

void xen_camera_front_evtchnl_pair_clear(struct xen_camera_front_evtchnl_pair *evt_pair);

#endif /* __XEN_CAMERA_FRONT_EVTCHNL_H */
