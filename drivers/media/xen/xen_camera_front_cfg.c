// SPDX-License-Identifier: GPL-2.0 OR MIT

/*
 * Xen para-virtual camera device
 *
 * Copyright (C) 2018 EPAM Systems Inc.
 *
 * Author: Oleksandr Andrushchenko <oleksandr_andrushchenko@epam.com>
 */

#include <xen/xenbus.h>

#include <xen/interface/io/cameraif.h>

#include "xen_camera_front.h"

static int cfg_read_framerates(struct xenbus_device *xb_dev,
			       struct xen_camera_front_cfg_resolution *res,
			       const char *xenstore_base_path,
			       const char *name)
{
	struct device *dev = &xb_dev->dev;
	char *list, *tmp;
	char *cur_frame_rate;
	char *xs_frame_rate_base_path;
	int num_frame_rates, i, ret, len;

	xs_frame_rate_base_path = kasprintf(GFP_KERNEL, "%s/%s",
					    xenstore_base_path, name);

	list = xenbus_read(XBT_NIL, xs_frame_rate_base_path,
			   XENCAMERA_FIELD_FRAME_RATES, &len);
	if (IS_ERR(list)) {
		dev_err(dev, "No frame rates configured at %s/%s\n",
			xs_frame_rate_base_path, XENCAMERA_FIELD_FRAME_RATES);
		kfree(xs_frame_rate_base_path);
		return PTR_ERR(list);
	}

	/*
	 * Empty list just means that frame rates are not configured for
	 * the given guest. Return -ENOTTY to the upper layer for treating
	 * this error accordingly and continue initialization sequence.
	 */
	if (!list[0]) {
		ret = -ENOTTY;
		goto fail;
	}

	/*
	 * At the first pass find out how many frame rates are there.
	 * At the second pass read frame rates, validate and store.
	 * Start from 1 as a single entry frame rate configuration will
	 * have no separators at all.
	 */
	num_frame_rates = 1;
	for (i = 0; list[i]; i++)
		if (list[i] == XENCAMERA_LIST_SEPARATOR[0])
			num_frame_rates++;

	res->frame_rate = devm_kcalloc(dev, num_frame_rates,
				       sizeof(*res->frame_rate), GFP_KERNEL);
	if (!res->frame_rate) {
		ret = -ENOMEM;
		goto fail;
	}

	i = 0;
	/* Remember the original pointer, so we can kfree it after strsep. */
	tmp = list;
	while ((cur_frame_rate = strsep(&tmp, XENCAMERA_LIST_SEPARATOR))) {
		int cnt, num, denom;

		cnt = sscanf(cur_frame_rate,
			     "%d" XENCAMERA_FRACTION_SEPARATOR "%d",
			     &num, &denom);
		if (cnt != 2) {
			dev_err(dev, "Wrong frame rate %s\n", cur_frame_rate);
			ret = -EINVAL;
			goto fail;
		}

		res->frame_rate[i].numerator = num;
		res->frame_rate[i++].denominator = denom;
	}

	res->num_frame_rates = num_frame_rates;

	ret = 0;

fail:
	kfree(xs_frame_rate_base_path);
	kfree(list);
	return ret;
}

static int cfg_read_format(struct xenbus_device *xb_dev,
			   struct xen_camera_front_cfg_format *fmt,
			   const char *xenstore_base_path,
			   const char *name)
{
	struct device *dev = &xb_dev->dev;
	char **dir_nodes;
	char *xs_res_base_path;
	int num_resolutions, i, ret, cnt, width, height;

	if (strlen(name) != 4) {
		dev_info(dev, "%s isn't a FOURCC code\n", name);
		return -EINVAL;
	}

	fmt->pixel_format =  v4l2_fourcc(name[0], name[1], name[2], name[3]);

	/* Find out how many resolutions are configured. */
	dir_nodes = xenbus_directory(XBT_NIL, xenstore_base_path, name,
				     &num_resolutions);
	if (IS_ERR(dir_nodes)) {
		dev_err(dev, "No resolutions configured for format %s\n", name);
		return -EINVAL;
	}

	xs_res_base_path = kasprintf(GFP_KERNEL, "%s/%s",
				     xenstore_base_path, name);
	if (!xs_res_base_path) {
		ret = -ENOMEM;
		goto fail;
	}

	fmt->resolution = devm_kcalloc(dev, num_resolutions,
				       sizeof(*fmt->resolution), GFP_KERNEL);
	if (!fmt->resolution) {
		ret = -ENOMEM;
		goto fail;
	}

	for (i = 0; i < num_resolutions; i++) {
		bool no_framerate = false;

		cnt = sscanf(dir_nodes[i],
			     "%d" XENCAMERA_RESOLUTION_SEPARATOR "%d",
			     &width, &height);
		if (cnt != 2) {
			dev_err(dev, "Wrong resolution %s\n", dir_nodes[i]);
			ret = -EINVAL;
			goto fail;
		}

		if (!no_framerate) {
			ret = cfg_read_framerates(xb_dev, &fmt->resolution[i],
						  xs_res_base_path, dir_nodes[i]);
			if (ret < 0) {
				if (ret != -ENOTTY)
					goto fail;

				/*
				 * There is no need to try to read frame rates for other
				 * resolutions if they are not configured for the first one.
				 * The frame rates must be either configured for all resolutions
				 * or for none of them.
				 */
				no_framerate = true;
			}
		}

		fmt->resolution[i].width = width;
		fmt->resolution[i].height = height;
	}

	fmt->num_resolutions = num_resolutions;

	ret = 0;

fail:
	kfree(xs_res_base_path);
	kfree(dir_nodes);
	return ret;
}

static void cfg_dump(struct xen_camera_front_info *front_info)
{
	struct device *dev = &front_info->xb_dev->dev;
	struct xen_camera_front_cfg_card *cfg = &front_info->cfg;
	int fmt, res, rate;

	for (fmt = 0; fmt < cfg->num_formats; fmt++) {
		struct xen_camera_front_cfg_format *format = &cfg->format[fmt];
		char *pixel_format = (char *)&format->pixel_format;

		dev_info(dev, "Format[%d] %c%c%c%c\n", fmt,
			 pixel_format[0], pixel_format[1],
			 pixel_format[2], pixel_format[3]);

		for (res = 0; res < format->num_resolutions; res++) {
			struct xen_camera_front_cfg_resolution *resolution =
				&format->resolution[res];

			dev_info(dev, "\tResolution [%d] %dx%d\n", res,
				 resolution->width, resolution->height);

			for (rate = 0; rate < resolution->num_frame_rates;
			     rate++) {
				struct xen_camera_front_cfg_fract *fr =
					&resolution->frame_rate[rate];

				dev_info(dev, "\t\tFrame rate [%d] %d/%d\n",
					 rate, fr->numerator, fr->denominator);
			}
		}
	}
}

int xen_camera_front_cfg_init(struct xen_camera_front_info *front_info)
{
	struct xenbus_device *xb_dev = front_info->xb_dev;
	struct device *dev = &xb_dev->dev;
	struct xen_camera_front_cfg_card *cfg = &front_info->cfg;
	char **dir_nodes;
	char *xs_fmt_base_path;
	int num_formats;
	int i, ret;

	if (xenbus_read_unsigned(xb_dev->nodename,
				 XENCAMERA_FIELD_BE_ALLOC, 0)) {
		dev_info(dev, "Backend can provide camera buffers\n");
		cfg->be_alloc = true;
	}

	cfg->max_buffers = xenbus_read_unsigned(xb_dev->nodename,
						XENCAMERA_FIELD_MAX_BUFFERS,
						2);
	dev_info(dev, "Maximum allowed buffers: %d\n", cfg->max_buffers);

	cfg->num_formats = 0;
	/* Find out how many formats are configured. */
	dir_nodes = xenbus_directory(XBT_NIL, xb_dev->nodename,
				     XENCAMERA_FIELD_FORMATS,
				     &num_formats);
	if (IS_ERR(dir_nodes)) {
		dev_err(dev, "No formats configured\n");
		return -EINVAL;
	}

	xs_fmt_base_path = kasprintf(GFP_KERNEL, "%s/%s",
				     xb_dev->nodename, XENCAMERA_FIELD_FORMATS);
	if (!xs_fmt_base_path) {
		ret = -ENOMEM;
		goto fail;
	}

	cfg->format = devm_kcalloc(dev, num_formats, sizeof(*cfg->format),
				   GFP_KERNEL);
	if (!cfg->format) {
		ret = -ENOMEM;
		goto fail;
	}

	for (i = 0; i < num_formats; i++) {
		ret = cfg_read_format(xb_dev, &cfg->format[i],
				      xs_fmt_base_path, dir_nodes[i]);
		if (ret < 0)
			goto fail;
	}

	cfg->num_formats = num_formats;

	cfg_dump(front_info);

	ret = 0;

fail:
	kfree(xs_fmt_base_path);
	kfree(dir_nodes);
	return ret;
}

