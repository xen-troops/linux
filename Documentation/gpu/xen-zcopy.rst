==============================================
 drm/xen-zcopy Xen zero-copy helper DRM driver
==============================================

This helper driver allows implementing zero-copying use-cases
when using Xen para-virtualized frontend display driver:

 - a dumb buffer created on backend's side can be shared
   with the Xen PV frontend driver, so it directly writes
   into backend's domain memory (into the buffer exported from
   DRM/KMS driver of a physical display device)
 - a dumb buffer allocated by the frontend can be imported
   into physical device DRM/KMS driver, thus allowing to
   achieve no copying as well

DRM_XEN_ZCOPY_DUMB_FROM_REFS IOCTL
==================================

.. kernel-doc:: include/uapi/drm/xen_zcopy_drm.h
   :doc: DRM_XEN_ZCOPY_DUMB_FROM_REFS

DRM_XEN_ZCOPY_DUMB_TO_REFS IOCTL
================================

.. kernel-doc:: include/uapi/drm/xen_zcopy_drm.h
   :doc: DRM_XEN_ZCOPY_DUMB_TO_REFS

DRM_XEN_ZCOPY_DUMB_WAIT_FREE IOCTL
==================================

.. kernel-doc:: include/uapi/drm/xen_zcopy_drm.h
   :doc: DRM_XEN_ZCOPY_DUMB_WAIT_FREE
