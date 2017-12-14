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
	int ret;

	connector_path = devm_kasprintf(&front_info->xb_dev->dev,
		GFP_KERNEL, "%s/%d", path, index);
	if (!connector_path)
		return -ENOMEM;

	connector->xenstore_path = connector_path;
	if (xenbus_scanf(XBT_NIL, connector_path, XENDISPL_FIELD_RESOLUTION,
			"%d" XENDISPL_RESOLUTION_SEPARATOR "%d",
			&connector->width, &connector->height) < 0) {
		/* either no entry configured or wrong resolution set */
		connector->width = 0;
		connector->height = 0;
		ret = -EINVAL;
		goto fail;
	}

	DRM_INFO("Connector %s: resolution %dx%d\n",
		connector_path, connector->width, connector->height);
	ret = 0;

fail:
	return ret;
}

int xen_drm_front_cfg_card(struct xen_drm_front_info *front_info,
	struct xen_drm_front_cfg_plat_data *plat_data)
{
	struct xenbus_device *xb_dev = front_info->xb_dev;
	int ret, i;

	if (xenbus_read_unsigned(front_info->xb_dev->nodename,
			XENDISPL_FIELD_BE_ALLOC, 0)) {
		DRM_INFO("Backend can provide dumb buffers\n");
		plat_data->be_alloc = true;
	}

	plat_data->num_connectors = 0;
	for (i = 0; i < ARRAY_SIZE(plat_data->connectors); i++) {
		ret = cfg_connector(front_info,
			&plat_data->connectors[i], xb_dev->nodename, i);
		if (ret < 0)
			break;
		plat_data->num_connectors++;
	}

	if (!plat_data->num_connectors) {
		DRM_ERROR("No connector(s) configured at %s\n",
			xb_dev->nodename);
		return -ENODEV;
	}

	return 0;
}

