#
# Makefile for the drm device driver.  This driver provides support for the
# Direct Rendering Infrastructure (DRI) in XFree86 4.1.0 and higher.

drm_xen_front-objs := xen_drm_front.o xen_drm_front_drv.o xen_drm_front_kms.o \
		      xen_drm_front_crtc.o xen_drm_front_shbuf.o xen_drm_balloon.o \
		      xen_drm_front_evtchnl.o xen_drm_front_cfg.o

ifeq ($(CONFIG_DRM_XEN_FRONTEND_CMA),y)
       drm_xen_front-objs += xen_drm_front_gem_cma.o
else
       drm_xen_front-objs += xen_drm_front_gem.o
endif

obj-$(CONFIG_DRM_XEN_FRONTEND) += drm_xen_front.o

drm_xen_zcopy-objs := xen_drm_zcopy_drv.o xen_drm_balloon.o

obj-$(CONFIG_DRM_XEN_ZCOPY) += drm_xen_zcopy.o
