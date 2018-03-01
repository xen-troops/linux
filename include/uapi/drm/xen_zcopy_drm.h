/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */

/*
 *  Xen zero-copy helper DRM device
 *
 * Copyright (C) 2016-2018 EPAM Systems Inc.
 *
 * Author: Oleksandr Andrushchenko <oleksandr_andrushchenko@epam.com>
 */
#ifndef __XEN_ZCOPY_DRM_H
#define __XEN_ZCOPY_DRM_H

#include "drm.h"

#if defined(__cplusplus)
extern "C" {
#endif

#define XENDRM_ZCOPY_DRIVER_NAME	"xen_drm_zcopy"

/**
 * DOC: DRM_XEN_ZCOPY_DUMB_FROM_REFS
 *
 * This will create a DRM dumb buffer from grant references provided
 * by the frontend:
 *
 * - Frontend
 *
 *  - creates a dumb/display buffer and allocates memory.
 *  - grants foreign access to the buffer pages
 *  - passes granted references to the backend
 *
 * - Backend
 *
 *  - issues DRM_XEN_ZCOPY_DUMB_FROM_REFS ioctl to map
 *    granted references and create a dumb buffer.
 *  - requests handle to fd conversion via DRM_IOCTL_PRIME_HANDLE_TO_FD
 *  - requests real HW driver to import the PRIME buffer with
 *    DRM_IOCTL_PRIME_FD_TO_HANDLE
 *  - uses handle returned by the real HW driver
 *
 *  At the end:
 *
 *   - closes real HW driver's handle with DRM_IOCTL_GEM_CLOSE
 *   - closes zero-copy driver's handle with DRM_IOCTL_GEM_CLOSE
 *   - closes file descriptor of the exported buffer
 *   - may wait for the object to be actually freed via wait_handle
 *     and DRM_XEN_ZCOPY_DUMB_WAIT_FREE
 */
#define DRM_XEN_ZCOPY_DUMB_FROM_REFS	0x00

struct drm_xen_zcopy_dumb_from_refs {
	__u32 num_grefs;
	/* user-space uses __u32 instead of grant_ref_t for mapping */
	__u32 *grefs;
	__u64 otherend_id;
	struct drm_mode_create_dumb dumb;
	__u32 wait_handle;
};

/**
 * DOC: DRM_XEN_ZCOPY_DUMB_TO_REFS
 *
 * This will grant references to a dumb/display buffer's memory provided by the
 * backend:
 *
 * - Frontend
 *
 *  - requests backend to allocate dumb/display buffer and grant references
 *    to its pages
 *
 * - Backend
 *
 *  - requests real HW driver to create a dumb with DRM_IOCTL_MODE_CREATE_DUMB
 *  - requests handle to fd conversion via DRM_IOCTL_PRIME_HANDLE_TO_FD
 *  - requests zero-copy driver to import the PRIME buffer with
 *    DRM_IOCTL_PRIME_FD_TO_HANDLE
 *  - issues DRM_XEN_ZCOPY_DUMB_TO_REFS ioctl to grant references to the
 *    buffer's memory.
 *  - passes grant references to the frontend
 *
 *  At the end:
 *
 *   - closes zero-copy driver's handle with DRM_IOCTL_GEM_CLOSE
 *   - closes real HW driver's handle with DRM_IOCTL_GEM_CLOSE
 *   - closes file descriptor of the imported buffer
 */
#define DRM_XEN_ZCOPY_DUMB_TO_REFS	0x01

struct drm_xen_zcopy_dumb_to_refs {
	__u32 num_grefs;
	/* user-space uses __u32 instead of grant_ref_t for mapping */
	__u32 *grefs;
	__u64 otherend_id;
	__u32 handle;
};

/**
 * DOC: DRM_XEN_ZCOPY_DUMB_WAIT_FREE
 *
 * This will block until the dumb buffer with the wait handle provided be freed:
 * this is needed for synchronization between frontend and backend in case
 * frontend provides grant references of the buffer via
 * DRM_XEN_ZCOPY_DUMB_FROM_REFS IOCTL and which must be released before
 * backend replies with XENDISPL_OP_DBUF_DESTROY response.
 * wait_handle must be the same value returned while calling
 * DRM_XEN_ZCOPY_DUMB_FROM_REFS IOCTL.
 */
#define DRM_XEN_ZCOPY_DUMB_WAIT_FREE	0x02

struct drm_xen_zcopy_dumb_wait_free {
	__u32 wait_handle;
	__u32 wait_to_ms;
};

#define DRM_IOCTL_XEN_ZCOPY_DUMB_FROM_REFS DRM_IOWR(DRM_COMMAND_BASE + \
	DRM_XEN_ZCOPY_DUMB_FROM_REFS, struct drm_xen_zcopy_dumb_from_refs)

#define DRM_IOCTL_XEN_ZCOPY_DUMB_TO_REFS DRM_IOWR(DRM_COMMAND_BASE + \
	DRM_XEN_ZCOPY_DUMB_TO_REFS, struct drm_xen_zcopy_dumb_to_refs)

#define DRM_IOCTL_XEN_ZCOPY_DUMB_WAIT_FREE DRM_IOWR(DRM_COMMAND_BASE + \
	DRM_XEN_ZCOPY_DUMB_WAIT_FREE, struct drm_xen_zcopy_dumb_wait_free)

#if defined(__cplusplus)
}
#endif

#endif /* __XEN_ZCOPY_DRM_H*/
