/*
 *  Xen virtual DRM zero copy device
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

#ifndef __XEN_ZCOPY_DRM_H
#define __XEN_ZCOPY_DRM_H

#include "drm.h"

#if defined(__cplusplus)
extern "C" {
#endif

#define XENDRM_ZCOPY_DRIVER_NAME	"xen_drm_zcopy"

/*
 * Xen DRM zero copy specific ioctls.
 *
 * The device specific ioctl range is [DRM_COMMAND_BASE, DRM_COMMAND_END) i.e.
 * [0x40, 0xa0) (a0 is excluded). The numbers below are defined as offset
 * against DRM_COMMAND_BASE and should be between [0x0, 0x60).
 */

/*
 * This will create a DRM dumb buffer from grant references provided
 * by the frontend:
 *  o Frontend
 *    o creates a dumb buffer and allocates memory.
 *    o grants foreign access to the buffer
 *    o passes granted references to the backend
 *  o Backend
 *    o issues DRM_XEN_ZCOPY_DUMB_FROM_REFS ioctl to map
 *      granted references and create a dumb buffer.
 *    o requests handle to fd conversion via DRM_IOCTL_PRIME_HANDLE_TO_FD
 *    o requests real HW driver to import the prime buffer with
 *      DRM_IOCTL_PRIME_FD_TO_HANDLE
 *    o uses handle returned by the real HW driver
 *    o at the end:
 *      o closes real HW driver's handle with DRM_IOCTL_GEM_CLOSE
 *      o closes zero-copy driver's handle with DRM_IOCTL_GEM_CLOSE
 *      o closes file descriptor of the exported buffer
 */
#define DRM_XEN_ZCOPY_DUMB_FROM_REFS	0x00

struct drm_xen_zcopy_dumb_from_refs {
	uint32_t num_grefs;
	/* FIXME: user-space uses uint32_t instead of grant_ref_t
	 * for mapping
	 */
	uint32_t *grefs;
	uint64_t otherend_id;
	struct drm_mode_create_dumb dumb;
};

/*
 * This will grant references to a dumb buffer's memory provided by the
 * backend:
 *  o Frontend
 *    o requests backend to allocate dumb and grant references
 *      to its memory
 *  o Backend
 *    o requests real HW driver to create a dumb with DRM_IOCTL_MODE_CREATE_DUMB
 *    o requests handle to fd conversion via DRM_IOCTL_PRIME_HANDLE_TO_FD
 *    o requests zero-copy driver to import the prime buffer with
 *      DRM_IOCTL_PRIME_FD_TO_HANDLE
 *    o issues DRM_XEN_ZCOPY_DUMB_TO_REFS ioctl to
 *      grant references to the buffer's memory.
 *   o passes grefs to the frontend
 *   o at the end:
 *     o closes zero-copy driver's handle with DRM_IOCTL_GEM_CLOSE
 *     o closes real HW driver's handle with DRM_IOCTL_GEM_CLOSE
 *     o closes file descriptor of the imported buffer
 */
#define DRM_XEN_ZCOPY_DUMB_TO_REFS	0x01

struct drm_xen_zcopy_dumb_to_refs {
	uint32_t num_grefs;
	/* FIXME: user-space uses uint32_t instead of grant_ref_t
	 * for mapping
	 */
	uint32_t *grefs;
	uint64_t otherend_id;
	uint32_t handle;
};

#define DRM_IOCTL_XEN_ZCOPY_DUMB_FROM_REFS DRM_IOWR(DRM_COMMAND_BASE + \
	DRM_XEN_ZCOPY_DUMB_FROM_REFS, struct drm_xen_zcopy_dumb_from_refs)
#define DRM_IOCTL_XEN_ZCOPY_DUMB_TO_REFS DRM_IOWR(DRM_COMMAND_BASE + \
	DRM_XEN_ZCOPY_DUMB_TO_REFS, struct drm_xen_zcopy_dumb_to_refs)

#if defined(__cplusplus)
}
#endif

#endif /* __XEN_ZCOPY_DRM_H*/
