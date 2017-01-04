#
# Makefile for the drm device driver.  This driver provides support for the
# Direct Rendering Infrastructure (DRI) in XFree86 4.1.0 and higher.

drm_xen_front-objs := xen_drm_front.o xen_drm.o xen_drm_kms.o xen_drm_crtc.o \
		  xen_drm_timer.o

ifneq ($(CONFIG_DRM_XENFRONT_CMA),y)
	drm_xen_front-objs += xen_drm_gem.o
endif

obj-$(CONFIG_DRM_XENFRONT) += drm_xen_front.o

drm_xen_zcopy-objs := xen_drm_zcopy.o

obj-$(CONFIG_DRM_XENZCOPY) += drm_xen_zcopy.o
