# SPDX-License-Identifier: GPL-2.0 OR MIT

drm_xen_front-objs := xen_drm_front.o \
		      xen_drm_front_kms.o \
		      xen_drm_front_conn.o \
		      xen_drm_front_evtchnl.o \
		      xen_drm_front_shbuf.o \
		      xen_drm_front_cfg.o \
		      xen_drm_front_gem.o

obj-$(CONFIG_DRM_XEN_FRONTEND) += drm_xen_front.o

drm_xen_zcopy-objs := xen_drm_zcopy.o \
		      xen_drm_zcopy_balloon.o

obj-$(CONFIG_DRM_XEN_ZCOPY) += drm_xen_zcopy.o
