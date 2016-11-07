/*
 *  Unified sound-device I/O interface for Xen guest OSes
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to
 * deal in the Software without restriction, including without limitation the
 * rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
 * sell copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 *
 * Copyright (C) 2016 EPAM Systems Inc.
 */

#ifndef __XEN_PUBLIC_IO_XENSND_LINUX_H__
#define __XEN_PUBLIC_IO_XENSND_LINUX_H__

#include <xen/interface/io/ring.h>
#include <xen/interface/io/sndif.h>
#include <xen/interface/grant_table.h>

struct xensnd_open_req {
	uint32_t pcm_rate;
	uint8_t pcm_format;
	uint8_t pcm_channels;
	/* in Hz */
	uint16_t __reserved0;
	grant_ref_t gref_directory_start;
} __packed;

struct xensnd_page_directory {
	grant_ref_t gref_dir_next_page;
	uint32_t num_grefs;
	grant_ref_t gref[0];
} __packed;

struct xensnd_close_req {
} __packed;

struct xensnd_write_req {
	uint32_t offset;
	uint32_t len;
} __packed;

struct xensnd_read_req {
	uint32_t offset;
	uint32_t len;
} __packed;

struct xensnd_get_vol_req {
} __packed;

struct xensnd_set_vol_req {
} __packed;

struct xensnd_mute_req {
} __packed;

struct xensnd_unmute_req {
} __packed;

struct xensnd_req {
	union {
		struct xensnd_request raw;
		struct {
			uint16_t id;
			uint8_t operation;
			uint8_t stream_idx;
			union {
				struct xensnd_open_req open;
				struct xensnd_close_req close;
				struct xensnd_write_req write;
				struct xensnd_read_req read;
				struct xensnd_get_vol_req get_vol;
				struct xensnd_set_vol_req set_vol;
				struct xensnd_mute_req mute;
				struct xensnd_unmute_req unmute;
			} op;
		} data;
	} u;
};

struct xensnd_resp {
	union {
		struct xensnd_response raw;
		struct {
			uint16_t id;
			uint8_t operation;
			uint8_t stream_idx;
			int8_t status;
		} data;
	} u;
};

DEFINE_RING_TYPES(xen_sndif, struct xensnd_req,
		struct xensnd_resp);

#endif /* __XEN_PUBLIC_IO_XENSND_LINUX_H__ */
