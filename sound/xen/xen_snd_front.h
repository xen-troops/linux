/* SPDX-License-Identifier: GPL-2.0 OR MIT */

/*
 * Xen para-virtual sound device
 *
 * Copyright (C) 2016-2018 EPAM Systems Inc.
 *
 * Author: Oleksandr Andrushchenko <oleksandr_andrushchenko@epam.com>
 */

#ifndef __XEN_SND_FRONT_H
#define __XEN_SND_FRONT_H

#include "xen_snd_front_cfg.h"

struct card_info;
struct xen_snd_front_evtchnl;
struct xen_snd_front_evtchnl_pair;
struct xen_snd_front_shbuf;
struct xensnd_query_hw_param;

struct xen_snd_front_info {
	struct xenbus_device *xb_dev;

	struct card_info *card_info;

	spinlock_t io_lock;

	int num_evt_pairs;
	struct xen_snd_front_evtchnl_pair *evt_pairs;

	struct xen_front_cfg_card cfg;
};

int xen_snd_front_stream_query_hw_param(struct xen_snd_front_evtchnl *evtchnl,
		struct xensnd_query_hw_param *hw_param_req,
		struct xensnd_query_hw_param *hw_param_resp);

int xen_snd_front_stream_prepare(struct xen_snd_front_evtchnl *evtchnl,
		struct xen_snd_front_shbuf *sh_buf,
		uint8_t format, unsigned int channels,
		unsigned int rate, uint32_t buffer_sz,
		uint32_t period_sz);

int xen_snd_front_stream_close(struct xen_snd_front_evtchnl *evtchnl);

int xen_snd_front_stream_write(struct xen_snd_front_evtchnl *evtchnl,
		unsigned long pos, unsigned long count);

int xen_snd_front_stream_read(struct xen_snd_front_evtchnl *evtchnl,
		unsigned long pos, unsigned long count);

int xen_snd_front_stream_trigger(struct xen_snd_front_evtchnl *evtchnl,
		int type);

#endif /* __XEN_SND_FRONT_H */
