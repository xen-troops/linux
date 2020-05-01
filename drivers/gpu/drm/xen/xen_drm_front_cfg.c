// SPDX-License-Identifier: GPL-2.0 OR MIT

/*
 *  Xen para-virtual DRM device
 *
 * Copyright (C) 2016-2018 EPAM Systems Inc.
 *
 * Author: Oleksandr Andrushchenko <oleksandr_andrushchenko@epam.com>
 */

#include <drm/drmP.h>

#include <linux/device.h>

#include <xen/interface/io/displif.h>
#include <xen/xenbus.h>

#include "xen_drm_front.h"
#include "xen_drm_front_cfg.h"

static int cfg_connector(struct xen_drm_front_info *front_info,
			 struct xen_drm_front_cfg_connector *connector,
			 const char *path, int index)
{
	char *connector_path;

	connector_path = devm_kasprintf(&front_info->xb_dev->dev,
					GFP_KERNEL, "%s/%d", path, index);
	if (!connector_path)
		return -ENOMEM;

	if (xenbus_scanf(XBT_NIL, connector_path, XENDISPL_FIELD_RESOLUTION,
			 "%d" XENDISPL_RESOLUTION_SEPARATOR "%d",
			 &connector->width, &connector->height) < 0) {
		/* either no entry configured or wrong resolution set */
		connector->width = 0;
		connector->height = 0;
		return -EINVAL;
	}

	connector->xenstore_path = connector_path;

	DRM_INFO("Connector %s: resolution %dx%d\n",
		 connector_path, connector->width, connector->height);
	return 0;
}

static void
cfg_connector_free_edid(struct xen_drm_front_cfg_connector *connector)
{
	vfree(connector->edid);
	connector->edid = NULL;
}

static void cfg_connector_edid(struct xen_drm_front_info *front_info,
			       struct xen_drm_front_cfg_connector *connector,
			       int index)
{
	struct page **pages;
	u32 edid_sz;
	int i, npages, ret = -ENOMEM;

	connector->edid = vmalloc(XENDISPL_EDID_MAX_SIZE);
	if (!connector->edid)
		goto fail;

	npages = DIV_ROUND_UP(XENDISPL_EDID_MAX_SIZE, PAGE_SIZE);
	pages = kvmalloc_array(npages, sizeof(struct page *), GFP_KERNEL);
	if (!pages)
		goto fail_free_edid;

	for (i = 0; i < npages; i++)
		pages[i] = vmalloc_to_page((u8 *)connector->edid +
					   i * PAGE_SIZE);

	ret = xen_drm_front_get_edid(front_info, index, pages,
				     XENDISPL_EDID_MAX_SIZE, &edid_sz);

	kvfree(pages);

	if (ret < 0)
		goto fail_free_edid;

	ret = -EINVAL;
	if (!edid_sz || (edid_sz % EDID_LENGTH))
		goto fail_free_edid;

	if (!drm_edid_is_valid(connector->edid))
		goto fail_free_edid;

	DRM_INFO("Connector %s: using EDID for configuration, size %d\n",
		 connector->xenstore_path, edid_sz);
	return;

fail_free_edid:
	cfg_connector_free_edid(connector);
fail:
	/*
	 * If any error this is not critical as we can still read
	 * connector settings from XenStore, so just warn.
	 */
	DRM_WARN("Connector %s: cannot read or wrong EDID: %d\n",
		 connector->xenstore_path, ret);
}

int xen_drm_front_cfg_card(struct xen_drm_front_info *front_info,
			   struct xen_drm_front_cfg *cfg)
{
	struct xenbus_device *xb_dev = front_info->xb_dev;
	int ret, i;

	if (xenbus_read_unsigned(front_info->xb_dev->nodename,
				 XENDISPL_FIELD_BE_ALLOC, 0)) {
		DRM_INFO("Backend can provide display buffers\n");
		cfg->be_alloc = true;
	}

	cfg->num_connectors = 0;
	for (i = 0; i < ARRAY_SIZE(cfg->connectors); i++) {
		ret = cfg_connector(front_info, &cfg->connectors[i],
				    xb_dev->nodename, i);
		if (ret < 0)
			break;
		cfg->num_connectors++;
	}

	if (!cfg->num_connectors) {
		DRM_ERROR("No connector(s) configured at %s\n",
			  xb_dev->nodename);
		return -ENODEV;
	}

	return 0;
}

int xen_drm_front_cfg_tail(struct xen_drm_front_info *front_info,
			   struct xen_drm_front_cfg *cfg)
{
	int i;

	/*
	 * Try reading EDID(s) from the backend: it is not an error
	 * if backend doesn't support or provides no EDID.
	 */
	for (i = 0; i < cfg->num_connectors; i++)
		cfg_connector_edid(front_info, &cfg->connectors[i], i);

	return 0;
}

void xen_drm_front_cfg_free(struct xen_drm_front_info *front_info,
			    struct xen_drm_front_cfg *cfg)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(cfg->connectors); i++)
		cfg_connector_free_edid(&cfg->connectors[i]);
}

