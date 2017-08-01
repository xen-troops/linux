/*
 * Xen para-virtual sound device
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
 * Based on: sound/drivers/dummy.c
 *
 * Copyright (C) 2016-2017 EPAM Systems Inc.
 *
 * Author: Oleksandr Andrushchenko <oleksandr_andrushchenko@epam.com>
 */

#include <linux/module.h>
#include <linux/platform_device.h>

#include <sound/core.h>
#include <sound/pcm.h>

#include <xen/events.h>
#include <xen/grant_table.h>
#include <xen/platform_pci.h>
#include <xen/xen.h>
#include <xen/xenbus.h>

#include <xen/interface/io/sndif.h>

/*
 * FIXME: usage of grant reference 0 as invalid grant reference:
 * grant reference 0 is valid, but never exposed to a PV driver,
 * because of the fact it is already in use/reserved by the PV console.
 */
#define GRANT_INVALID_REF	0
/* maximum number of supported streams */
#define VSND_MAX_STREAM		8

enum xdrv_evtchnl_state {
	EVTCHNL_STATE_DISCONNECTED,
	EVTCHNL_STATE_CONNECTED,
};

struct xdrv_evtchnl_info {
	struct xdrv_info *drv_info;
	struct xen_sndif_front_ring ring;
	int ring_ref;
	int port;
	int irq;
	struct completion completion;
	enum xdrv_evtchnl_state state;
	/* latest response status and its corresponding id */
	int resp_status;
	uint16_t resp_id;
};

struct sh_buf_info {
	int num_grefs;
	grant_ref_t *grefs;
	uint8_t *vdirectory;
	uint8_t *vbuffer;
	size_t vbuffer_sz;
};

struct sdev_pcm_stream_info {
	int unique_id;
	struct snd_pcm_hardware pcm_hw;
	struct xdrv_evtchnl_info *evt_chnl;
	bool is_open;
	uint8_t req_next_id;
	struct sh_buf_info sh_buf;
};

struct sdev_pcm_instance_info {
	struct sdev_card_info *card_info;
	struct snd_pcm *pcm;
	struct snd_pcm_hardware pcm_hw;
	int num_pcm_streams_pb;
	struct sdev_pcm_stream_info *streams_pb;
	int num_pcm_streams_cap;
	struct sdev_pcm_stream_info *streams_cap;
};

struct sdev_card_info {
	struct xdrv_info *xdrv_info;
	struct snd_card *card;
	struct snd_pcm_hardware pcm_hw;
	int num_pcm_instances;
	struct sdev_pcm_instance_info *pcm_instances;
};

struct cfg_stream {
	int unique_id;
	char *xenstore_path;
	struct snd_pcm_hardware pcm_hw;
};

struct cfg_pcm_instance {
	char name[80];
	int device_id;
	struct snd_pcm_hardware pcm_hw;
	int  num_streams_pb;
	struct cfg_stream *streams_pb;
	int  num_streams_cap;
	struct cfg_stream *streams_cap;
};

struct cfg_card {
	char name_short[32];
	char name_long[80];
	struct snd_pcm_hardware pcm_hw;
	int num_pcm_instances;
	struct cfg_pcm_instance *pcm_instances;
};

struct sdev_card_plat_data {
	struct xdrv_info *xdrv_info;
	struct cfg_card cfg_card;
};

struct xdrv_info {
	struct xenbus_device *xb_dev;
	spinlock_t io_lock;
	struct mutex mutex;
	bool sdrv_registered;
	struct platform_device *sdrv_pdev;
	int num_evt_channels;
	struct xdrv_evtchnl_info *evt_chnls;
	struct sdev_card_plat_data cfg_plat_data;
};

#define MAX_XEN_BUFFER_SIZE	(64 * 1024)
#define MAX_BUFFER_SIZE		MAX_XEN_BUFFER_SIZE
#define MIN_PERIOD_SIZE		64
#define MAX_PERIOD_SIZE		(MAX_BUFFER_SIZE / 8)
#define USE_FORMATS		(SNDRV_PCM_FMTBIT_U8 | \
				 SNDRV_PCM_FMTBIT_S16_LE)
#define USE_RATE		(SNDRV_PCM_RATE_CONTINUOUS | \
				 SNDRV_PCM_RATE_8000_48000)
#define USE_RATE_MIN		5512
#define USE_RATE_MAX		48000
#define USE_CHANNELS_MIN	1
#define USE_CHANNELS_MAX	2
#define USE_PERIODS_MIN		2
#define USE_PERIODS_MAX		8

static struct snd_pcm_hardware sdrv_pcm_hw_default = {
	.info = (SNDRV_PCM_INFO_MMAP |
		 SNDRV_PCM_INFO_INTERLEAVED |
		 SNDRV_PCM_INFO_RESUME |
		 SNDRV_PCM_INFO_MMAP_VALID),
	.formats = USE_FORMATS,
	.rates = USE_RATE,
	.rate_min = USE_RATE_MIN,
	.rate_max = USE_RATE_MAX,
	.channels_min = USE_CHANNELS_MIN,
	.channels_max = USE_CHANNELS_MAX,
	.buffer_bytes_max = MAX_BUFFER_SIZE,
	.period_bytes_min = MIN_PERIOD_SIZE,
	.period_bytes_max = MAX_PERIOD_SIZE,
	.periods_min = USE_PERIODS_MIN,
	.periods_max = USE_PERIODS_MAX,
	.fifo_size = 0,
};

static int sdrv_new_pcm(struct sdev_card_info *card_info,
	struct cfg_pcm_instance *instance_config,
	struct sdev_pcm_instance_info *pcm_instance_info)
{
	return 0;
}

static int sdrv_probe(struct platform_device *pdev)
{
	struct sdev_card_info *card_info;
	struct sdev_card_plat_data *platdata;
	struct snd_card *card;
	int ret, i;

	platdata = dev_get_platdata(&pdev->dev);

	dev_dbg(&pdev->dev, "Creating virtual sound card\n");

	ret = snd_card_new(&pdev->dev, 0, XENSND_DRIVER_NAME, THIS_MODULE,
		sizeof(struct sdev_card_info), &card);
	if (ret < 0)
		return ret;

	card_info = card->private_data;
	card_info->xdrv_info = platdata->xdrv_info;
	card_info->card = card;
	card_info->pcm_instances = devm_kcalloc(&pdev->dev,
			platdata->cfg_card.num_pcm_instances,
			sizeof(struct sdev_pcm_instance_info), GFP_KERNEL);
	if (!card_info->pcm_instances) {
		ret = -ENOMEM;
		goto fail;
	}

	card_info->num_pcm_instances = platdata->cfg_card.num_pcm_instances;
	card_info->pcm_hw = platdata->cfg_card.pcm_hw;

	for (i = 0; i < platdata->cfg_card.num_pcm_instances; i++) {
		ret = sdrv_new_pcm(card_info,
			&platdata->cfg_card.pcm_instances[i],
			&card_info->pcm_instances[i]);
		if (ret < 0)
			goto fail;
	}

	strncpy(card->driver, XENSND_DRIVER_NAME, sizeof(card->driver));
	strncpy(card->shortname, platdata->cfg_card.name_short,
		sizeof(card->shortname));
	strncpy(card->longname, platdata->cfg_card.name_long,
		sizeof(card->longname));

	ret = snd_card_register(card);
	if (ret < 0)
		goto fail;

	platform_set_drvdata(pdev, card);
	return 0;

fail:
	snd_card_free(card);
	return ret;
}

static int sdrv_remove(struct platform_device *pdev)
{
	struct sdev_card_info *info;
	struct snd_card *card = platform_get_drvdata(pdev);

	info = card->private_data;
	dev_dbg(&pdev->dev, "Removing virtual sound card %d\n",
		info->card->number);
	snd_card_free(card);
	return 0;
}

static struct platform_driver sdrv_info = {
	.probe	= sdrv_probe,
	.remove	= sdrv_remove,
	.driver	= {
		.name	= XENSND_DRIVER_NAME,
	},
};

static void sdrv_cleanup(struct xdrv_info *drv_info)
{
	if (!drv_info->sdrv_registered)
		return;

	if (drv_info->sdrv_pdev) {
		struct platform_device *sdrv_pdev;

		sdrv_pdev = drv_info->sdrv_pdev;
		if (sdrv_pdev)
			platform_device_unregister(sdrv_pdev);
	}
	platform_driver_unregister(&sdrv_info);
	drv_info->sdrv_registered = false;
}

static int sdrv_init(struct xdrv_info *drv_info)
{
	struct platform_device *sdrv_pdev;
	int ret;

	ret = platform_driver_register(&sdrv_info);
	if (ret < 0)
		return ret;

	drv_info->sdrv_registered = true;
	/* pass card configuration via platform data */
	sdrv_pdev = platform_device_register_data(NULL,
		XENSND_DRIVER_NAME, 0, &drv_info->cfg_plat_data,
		sizeof(drv_info->cfg_plat_data));
	if (IS_ERR(sdrv_pdev))
		goto fail;

	drv_info->sdrv_pdev = sdrv_pdev;
	return 0;

fail:
	dev_err(&drv_info->xb_dev->dev,
		"failed to register virtual sound driver\n");
	sdrv_cleanup(drv_info);
	return -ENODEV;
}

static irqreturn_t xdrv_evtchnl_interrupt(int irq, void *dev_id)
{
	struct xdrv_evtchnl_info *channel = dev_id;
	struct xdrv_info *drv_info = channel->drv_info;
	struct xensnd_resp *resp;
	RING_IDX i, rp;
	unsigned long flags;

	spin_lock_irqsave(&drv_info->io_lock, flags);
	if (unlikely(channel->state != EVTCHNL_STATE_CONNECTED))
		goto out;

again:
	rp = channel->ring.sring->rsp_prod;
	/* ensure we see queued responses up to rp */
	rmb();

	for (i = channel->ring.rsp_cons; i != rp; i++) {
		resp = RING_GET_RESPONSE(&channel->ring, i);
		if (resp->id != channel->resp_id)
			continue;
		switch (resp->operation) {
		case XENSND_OP_OPEN:
			/* fall through */
		case XENSND_OP_CLOSE:
			/* fall through */
		case XENSND_OP_READ:
			/* fall through */
		case XENSND_OP_WRITE:
			channel->resp_status = resp->status;
			complete(&channel->completion);
			break;

		default:
			dev_err(&drv_info->xb_dev->dev,
				"Operation %d is not supported\n",
				resp->operation);
			break;
		}
	}

	channel->ring.rsp_cons = i;
	if (i != channel->ring.req_prod_pvt) {
		int more_to_do;

		RING_FINAL_CHECK_FOR_RESPONSES(&channel->ring, more_to_do);
		if (more_to_do)
			goto again;
	} else
		channel->ring.sring->rsp_event = i + 1;

out:
	spin_unlock_irqrestore(&drv_info->io_lock, flags);
	return IRQ_HANDLED;
}

static inline void xdrv_evtchnl_flush(
		struct xdrv_evtchnl_info *channel)
{
	int notify;

	channel->ring.req_prod_pvt++;
	RING_PUSH_REQUESTS_AND_CHECK_NOTIFY(&channel->ring, notify);
	if (notify)
		notify_remote_via_irq(channel->irq);
}

static void xdrv_evtchnl_free(struct xdrv_info *drv_info,
		struct xdrv_evtchnl_info *channel)
{
	if (!channel->ring.sring)
		return;

	channel->state = EVTCHNL_STATE_DISCONNECTED;
	channel->resp_status = -EIO;
	complete_all(&channel->completion);

	if (channel->irq)
		unbind_from_irqhandler(channel->irq, channel);
	channel->irq = 0;

	if (channel->port)
		xenbus_free_evtchn(drv_info->xb_dev, channel->port);
	channel->port = 0;

	if (channel->ring_ref != GRANT_INVALID_REF)
		gnttab_end_foreign_access(channel->ring_ref, 0,
			(unsigned long)channel->ring.sring);
	channel->ring_ref = GRANT_INVALID_REF;
	channel->ring.sring = NULL;
}

static void xdrv_evtchnl_free_all(struct xdrv_info *drv_info)
{
	int i;

	if (!drv_info->evt_chnls)
		return;

	for (i = 0; i < drv_info->num_evt_channels; i++)
		xdrv_evtchnl_free(drv_info, &drv_info->evt_chnls[i]);

	devm_kfree(&drv_info->xb_dev->dev, drv_info->evt_chnls);
	drv_info->evt_chnls = NULL;
}

static int xdrv_evtchnl_alloc(struct xdrv_info *drv_info,
		struct xdrv_evtchnl_info *evt_channel)
{
	struct xenbus_device *xb_dev = drv_info->xb_dev;
	struct xen_sndif_sring *sring;
	grant_ref_t gref;
	int ret;

	evt_channel->drv_info = drv_info;
	init_completion(&evt_channel->completion);
	evt_channel->state = EVTCHNL_STATE_DISCONNECTED;
	evt_channel->ring_ref = GRANT_INVALID_REF;
	evt_channel->ring.sring = NULL;
	evt_channel->port = 0;
	evt_channel->irq = 0;

	sring = (struct xen_sndif_sring *)get_zeroed_page(
		GFP_NOIO | __GFP_HIGH);
	if (!sring) {
		ret = -ENOMEM;
		goto fail;
	}

	SHARED_RING_INIT(sring);
	FRONT_RING_INIT(&evt_channel->ring, sring, XEN_PAGE_SIZE);
	ret = xenbus_grant_ring(xb_dev, sring, 1, &gref);
	if (ret < 0)
		goto fail;
	evt_channel->ring_ref = gref;

	ret = xenbus_alloc_evtchn(xb_dev, &evt_channel->port);
	if (ret < 0)
		goto fail;

	ret = bind_evtchn_to_irqhandler(evt_channel->port,
		xdrv_evtchnl_interrupt, 0, xb_dev->devicetype, evt_channel);
	if (ret < 0)
		goto fail;

	evt_channel->irq = ret;
	return 0;

fail:
	dev_err(&xb_dev->dev, "Failed to allocate ring: %d\n", ret);
	return ret;
}

static int xdrv_evtchnl_create(struct xdrv_info *drv_info,
		struct xdrv_evtchnl_info *evt_channel,
		const char *path)
{
	int ret;

	ret = xdrv_evtchnl_alloc(drv_info, evt_channel);
	if (ret < 0) {
		dev_err(&drv_info->xb_dev->dev,
			"allocating event channel: %d\n", ret);
		return ret;
	}

	/*
	 * write values to Xen store, so backend can find ring reference
	 * and event channel
	 */
	ret = xenbus_printf(XBT_NIL, path, XENSND_FIELD_RING_REF, "%u",
			evt_channel->ring_ref);
	if (ret < 0) {
		dev_err(&drv_info->xb_dev->dev,
			"writing " XENSND_FIELD_RING_REF": %d\n", ret);
		return ret;
	}

	ret = xenbus_printf(XBT_NIL, path, XENSND_FIELD_EVT_CHNL, "%u",
		evt_channel->port);
	if (ret < 0) {
		dev_err(&drv_info->xb_dev->dev,
			"writing " XENSND_FIELD_EVT_CHNL": %d\n", ret);
		return ret;
	}
	return 0;
}

static int xdrv_evtchnl_create_all(struct xdrv_info *drv_info,
		int num_streams)
{
	struct cfg_card *cfg_card;
	int d, ret = 0;

	drv_info->evt_chnls = devm_kcalloc(&drv_info->xb_dev->dev,
		num_streams, sizeof(struct xdrv_evtchnl_info), GFP_KERNEL);
	if (!drv_info->evt_chnls) {
		ret = -ENOMEM;
		goto fail;
	}

	cfg_card = &drv_info->cfg_plat_data.cfg_card;
	/* iterate over devices and their streams and create event channels */
	for (d = 0; d < cfg_card->num_pcm_instances; d++) {
		struct cfg_pcm_instance *pcm_instance;
		int s, stream_idx;

		pcm_instance = &cfg_card->pcm_instances[d];

		for (s = 0; s < pcm_instance->num_streams_pb; s++) {
			stream_idx = pcm_instance->streams_pb[s].unique_id;
			ret = xdrv_evtchnl_create(drv_info,
				&drv_info->evt_chnls[stream_idx],
				pcm_instance->streams_pb[s].xenstore_path);
			if (ret < 0)
				goto fail;
		}

		for (s = 0; s < pcm_instance->num_streams_cap; s++) {
			stream_idx = pcm_instance->streams_cap[s].unique_id;
			ret = xdrv_evtchnl_create(drv_info,
				&drv_info->evt_chnls[stream_idx],
				pcm_instance->streams_cap[s].xenstore_path);
			if (ret < 0)
				goto fail;
		}
	}
	if (ret < 0)
		goto fail;

	drv_info->num_evt_channels = num_streams;
	return 0;

fail:
	xdrv_evtchnl_free_all(drv_info);
	return ret;
}

struct CFG_HW_SAMPLE_RATE {
	const char *name;
	unsigned int mask;
	unsigned int value;
};

static struct CFG_HW_SAMPLE_RATE cfg_hw_supported_rates[] = {
	{ .name = "5512",   .mask = SNDRV_PCM_RATE_5512,   .value = 5512 },
	{ .name = "8000",   .mask = SNDRV_PCM_RATE_8000,   .value = 8000 },
	{ .name = "11025",  .mask = SNDRV_PCM_RATE_11025,  .value = 11025 },
	{ .name = "16000",  .mask = SNDRV_PCM_RATE_16000,  .value = 16000 },
	{ .name = "22050",  .mask = SNDRV_PCM_RATE_22050,  .value = 22050 },
	{ .name = "32000",  .mask = SNDRV_PCM_RATE_32000,  .value = 32000 },
	{ .name = "44100",  .mask = SNDRV_PCM_RATE_44100,  .value = 44100 },
	{ .name = "48000",  .mask = SNDRV_PCM_RATE_48000,  .value = 48000 },
	{ .name = "64000",  .mask = SNDRV_PCM_RATE_64000,  .value = 64000 },
	{ .name = "96000",  .mask = SNDRV_PCM_RATE_96000,  .value = 96000 },
	{ .name = "176400", .mask = SNDRV_PCM_RATE_176400, .value = 176400 },
	{ .name = "192000", .mask = SNDRV_PCM_RATE_192000, .value = 192000 },
};

struct CFG_HW_SAMPLE_FORMAT {
	const char *name;
	u64 mask;
};

static struct CFG_HW_SAMPLE_FORMAT cfg_hw_supported_formats[] = {
	{
		.name = XENSND_PCM_FORMAT_U8_STR,
		.mask = SNDRV_PCM_FMTBIT_U8
	},
	{
		.name = XENSND_PCM_FORMAT_S8_STR,
		.mask = SNDRV_PCM_FMTBIT_S8
	},
	{
		.name = XENSND_PCM_FORMAT_U16_LE_STR,
		.mask = SNDRV_PCM_FMTBIT_U16_LE
	},
	{
		.name = XENSND_PCM_FORMAT_U16_BE_STR,
		.mask = SNDRV_PCM_FMTBIT_U16_BE
	},
	{
		.name = XENSND_PCM_FORMAT_S16_LE_STR,
		.mask = SNDRV_PCM_FMTBIT_S16_LE
	},
	{
		.name = XENSND_PCM_FORMAT_S16_BE_STR,
		.mask = SNDRV_PCM_FMTBIT_S16_BE
	},
	{
		.name = XENSND_PCM_FORMAT_U24_LE_STR,
		.mask = SNDRV_PCM_FMTBIT_U24_LE
	},
	{
		.name = XENSND_PCM_FORMAT_U24_BE_STR,
		.mask = SNDRV_PCM_FMTBIT_U24_BE
	},
	{
		.name = XENSND_PCM_FORMAT_S24_LE_STR,
		.mask = SNDRV_PCM_FMTBIT_S24_LE
	},
	{
		.name = XENSND_PCM_FORMAT_S24_BE_STR,
		.mask = SNDRV_PCM_FMTBIT_S24_BE
	},
	{
		.name = XENSND_PCM_FORMAT_U32_LE_STR,
		.mask = SNDRV_PCM_FMTBIT_U32_LE
	},
	{
		.name = XENSND_PCM_FORMAT_U32_BE_STR,
		.mask = SNDRV_PCM_FMTBIT_U32_BE
	},
	{
		.name = XENSND_PCM_FORMAT_S32_LE_STR,
		.mask = SNDRV_PCM_FMTBIT_S32_LE
	},
	{
		.name = XENSND_PCM_FORMAT_S32_BE_STR,
		.mask = SNDRV_PCM_FMTBIT_S32_BE
	},
	{
		.name = XENSND_PCM_FORMAT_A_LAW_STR,
		.mask = SNDRV_PCM_FMTBIT_A_LAW
	},
	{
		.name = XENSND_PCM_FORMAT_MU_LAW_STR,
		.mask = SNDRV_PCM_FMTBIT_MU_LAW
	},
	{
		.name = XENSND_PCM_FORMAT_F32_LE_STR,
		.mask = SNDRV_PCM_FMTBIT_FLOAT_LE
	},
	{
		.name = XENSND_PCM_FORMAT_F32_BE_STR,
		.mask = SNDRV_PCM_FMTBIT_FLOAT_BE
	},
	{
		.name = XENSND_PCM_FORMAT_F64_LE_STR,
		.mask = SNDRV_PCM_FMTBIT_FLOAT64_LE
	},
	{
		.name = XENSND_PCM_FORMAT_F64_BE_STR,
		.mask = SNDRV_PCM_FMTBIT_FLOAT64_BE
	},
	{
		.name = XENSND_PCM_FORMAT_IEC958_SUBFRAME_LE_STR,
		.mask = SNDRV_PCM_FMTBIT_IEC958_SUBFRAME_LE
	},
	{
		.name = XENSND_PCM_FORMAT_IEC958_SUBFRAME_BE_STR,
		.mask = SNDRV_PCM_FMTBIT_IEC958_SUBFRAME_BE
	},
	{
		.name = XENSND_PCM_FORMAT_IMA_ADPCM_STR,
		.mask = SNDRV_PCM_FMTBIT_IMA_ADPCM
	},
	{
		.name = XENSND_PCM_FORMAT_MPEG_STR,
		.mask = SNDRV_PCM_FMTBIT_MPEG
	},
	{
		.name = XENSND_PCM_FORMAT_GSM_STR,
		.mask = SNDRV_PCM_FMTBIT_GSM
	},
};

static void cfg_hw_rates(char *list, unsigned int len,
	const char *path, struct snd_pcm_hardware *pcm_hw)
{
	char *cur_rate;
	unsigned int cur_mask;
	unsigned int cur_value;
	unsigned int rates;
	unsigned int rate_min;
	unsigned int rate_max;
	int i;

	rates = 0;
	rate_min = -1;
	rate_max = 0;
	while ((cur_rate = strsep(&list, XENSND_LIST_SEPARATOR))) {
		for (i = 0; i < ARRAY_SIZE(cfg_hw_supported_rates); i++)
			if (!strncasecmp(cur_rate,
					cfg_hw_supported_rates[i].name,
					XENSND_SAMPLE_RATE_MAX_LEN)) {
				cur_mask = cfg_hw_supported_rates[i].mask;
				cur_value = cfg_hw_supported_rates[i].value;
				rates |= cur_mask;
				if (rate_min > cur_value)
					rate_min = cur_value;
				if (rate_max < cur_value)
					rate_max = cur_value;
			}
	}

	if (rates) {
		pcm_hw->rates = rates;
		pcm_hw->rate_min = rate_min;
		pcm_hw->rate_max = rate_max;
	}
}

static void cfg_formats(char *list, unsigned int len,
	const char *path, struct snd_pcm_hardware *pcm_hw)
{
	u64 formats;
	char *cur_format;
	int i;

	formats = 0;
	while ((cur_format = strsep(&list, XENSND_LIST_SEPARATOR))) {
		for (i = 0; i < ARRAY_SIZE(cfg_hw_supported_formats); i++)
			if (!strncasecmp(cur_format,
					cfg_hw_supported_formats[i].name,
					XENSND_SAMPLE_FORMAT_MAX_LEN))
				formats |= cfg_hw_supported_formats[i].mask;
	}

	if (formats)
		pcm_hw->formats = formats;
}

static void cfg_pcm_hw(const char *path,
	struct snd_pcm_hardware *parent_pcm_hw,
	struct snd_pcm_hardware *pcm_hw)
{
	char *list;
	int val;
	size_t buf_sz;
	unsigned int len;

	/* inherit parent's PCM HW and read overrides if any */
	*pcm_hw = *parent_pcm_hw;

	val = xenbus_read_unsigned(path, XENSND_FIELD_CHANNELS_MIN, 0);
	if (val)
		pcm_hw->channels_min = val;

	val = xenbus_read_unsigned(path, XENSND_FIELD_CHANNELS_MAX, 0);
	if (val)
		pcm_hw->channels_max = val;

	list = xenbus_read(XBT_NIL, path, XENSND_FIELD_SAMPLE_RATES, &len);
	if (!IS_ERR(list)) {
		cfg_hw_rates(list, len, path, pcm_hw);
		kfree(list);
	}

	list = xenbus_read(XBT_NIL, path, XENSND_FIELD_SAMPLE_FORMATS, &len);
	if (!IS_ERR(list)) {
		cfg_formats(list, len, path, pcm_hw);
		kfree(list);
	}

	buf_sz = xenbus_read_unsigned(path, XENSND_FIELD_BUFFER_SIZE, 0);
	if (buf_sz)
		pcm_hw->buffer_bytes_max = buf_sz;
}

static int cfg_get_stream_type(const char *path, int index,
	int *num_pb, int *num_cap)
{
	char *str = NULL;
	char *stream_path;
	int ret;

	*num_pb = 0;
	*num_cap = 0;
	stream_path = kasprintf(GFP_KERNEL, "%s/%d", path, index);
	if (!stream_path) {
		ret = -ENOMEM;
		goto fail;
	}

	str = xenbus_read(XBT_NIL, stream_path, XENSND_FIELD_TYPE, NULL);
	if (IS_ERR(str)) {
		ret = -EINVAL;
		goto fail;
	}

	if (!strncasecmp(str, XENSND_STREAM_TYPE_PLAYBACK,
		sizeof(XENSND_STREAM_TYPE_PLAYBACK)))
		(*num_pb)++;
	else if (!strncasecmp(str, XENSND_STREAM_TYPE_CAPTURE,
		sizeof(XENSND_STREAM_TYPE_CAPTURE)))
		(*num_cap)++;
	else {
		ret = -EINVAL;
		goto fail;
	}
	ret = 0;

fail:
	kfree(stream_path);
	kfree(str);
	return ret;
}

static int cfg_stream(struct xdrv_info *drv_info,
	struct cfg_pcm_instance *pcm_instance,
	const char *path, int index, int *cur_pb, int *cur_cap,
	int *stream_idx)
{
	char *str = NULL;
	char *stream_path;
	struct cfg_stream *stream;
	int ret;

	stream_path = devm_kasprintf(&drv_info->xb_dev->dev,
		GFP_KERNEL, "%s/%d", path, index);
	if (!stream_path) {
		ret = -ENOMEM;
		goto fail;
	}

	str = xenbus_read(XBT_NIL, stream_path, XENSND_FIELD_TYPE, NULL);
	if (IS_ERR(str)) {
		ret = -EINVAL;
		goto fail;
	}

	if (!strncasecmp(str, XENSND_STREAM_TYPE_PLAYBACK,
		sizeof(XENSND_STREAM_TYPE_PLAYBACK))) {
		stream = &pcm_instance->streams_pb[(*cur_pb)++];
	} else if (!strncasecmp(str, XENSND_STREAM_TYPE_CAPTURE,
		sizeof(XENSND_STREAM_TYPE_CAPTURE))) {
		stream = &pcm_instance->streams_cap[(*cur_cap)++];
	} else {
		ret = -EINVAL;
		goto fail;
	}

	/* get next stream index */
	stream->unique_id = (*stream_idx)++;
	stream->xenstore_path = stream_path;
	/*
	 * check in Xen store if PCM HW configuration exists for this stream
	 * and update if so, e.g. we inherit all values from device's PCM HW,
	 * but can still override some of the values for the stream
	 */
	cfg_pcm_hw(stream->xenstore_path,
		&pcm_instance->pcm_hw, &stream->pcm_hw);
	ret = 0;

fail:
	kfree(str);
	return ret;
}

static int cfg_device(struct xdrv_info *drv_info,
	struct cfg_pcm_instance *pcm_instance,
	struct snd_pcm_hardware *parent_pcm_hw,
	const char *path, int node_index, int *stream_idx)
{
	char *str;
	char *device_path;
	int ret, i, num_streams;
	int num_pb, num_cap;
	int cur_pb, cur_cap;
	char node[3];

	device_path = kasprintf(GFP_KERNEL, "%s/%d", path, node_index);
	if (!device_path)
		return -ENOMEM;

	str = xenbus_read(XBT_NIL, device_path, XENSND_FIELD_DEVICE_NAME, NULL);
	if (!IS_ERR(str)) {
		strncpy(pcm_instance->name, str, sizeof(pcm_instance->name));
		kfree(str);
	}

	pcm_instance->device_id = node_index;

	/*
	 * check in Xen store if PCM HW configuration exists for this device
	 * and update if so, e.g. we inherit all values from card's PCM HW,
	 * but can still override some of the values for the device
	 */
	cfg_pcm_hw(device_path, parent_pcm_hw, &pcm_instance->pcm_hw);

	/*
	 * find out how many streams were configured in Xen store:
	 * streams must have sequential unique IDs, so stop when one
	 * does not exist
	 */
	num_streams = 0;
	do {
		snprintf(node, sizeof(node), "%d", num_streams);
		if (!xenbus_exists(XBT_NIL, device_path, node))
			break;

		num_streams++;
	} while (num_streams < VSND_MAX_STREAM);

	pcm_instance->num_streams_pb = 0;
	pcm_instance->num_streams_cap = 0;
	/* get number of playback and capture streams */
	for (i = 0; i < num_streams; i++) {
		ret = cfg_get_stream_type(device_path, i, &num_pb, &num_cap);
		if (ret < 0)
			goto fail;

		pcm_instance->num_streams_pb += num_pb;
		pcm_instance->num_streams_cap += num_cap;
	}

	if (pcm_instance->num_streams_pb) {
		pcm_instance->streams_pb = devm_kcalloc(
			&drv_info->xb_dev->dev,
			pcm_instance->num_streams_pb,
			sizeof(struct cfg_stream), GFP_KERNEL);
		if (!pcm_instance->streams_pb) {
			ret = -ENOMEM;
			goto fail;
		}
	}

	if (pcm_instance->num_streams_cap) {
		pcm_instance->streams_cap = devm_kcalloc(
			&drv_info->xb_dev->dev,
			pcm_instance->num_streams_cap,
			sizeof(struct cfg_stream), GFP_KERNEL);
		if (!pcm_instance->streams_cap) {
			ret = -ENOMEM;
			goto fail;
		}
	}

	cur_pb = 0;
	cur_cap = 0;
	for (i = 0; i < num_streams; i++) {
		ret = cfg_stream(drv_info,
			pcm_instance, device_path, i, &cur_pb, &cur_cap,
			stream_idx);
		if (ret < 0)
			goto fail;
	}
	ret = 0;

fail:
	kfree(device_path);
	return ret;
}

static int cfg_card(struct xdrv_info *drv_info,
	struct sdev_card_plat_data *plat_data, int *stream_idx)
{
	struct xenbus_device *xb_dev = drv_info->xb_dev;
	int ret, num_devices, i;
	char node[3];

	num_devices = 0;
	do {
		snprintf(node, sizeof(node), "%d", num_devices);
		if (!xenbus_exists(XBT_NIL, xb_dev->nodename, node))
			break;

		num_devices++;
	} while (num_devices < SNDRV_PCM_DEVICES);

	if (!num_devices) {
		dev_warn(&xb_dev->dev,
			"No devices configured for sound card at %s\n",
			xb_dev->nodename);
		return -ENODEV;
	}

	/* start from default PCM HW configuration for the card */
	cfg_pcm_hw(xb_dev->nodename, &sdrv_pcm_hw_default,
		&plat_data->cfg_card.pcm_hw);

	plat_data->cfg_card.pcm_instances = devm_kcalloc(
		&drv_info->xb_dev->dev, num_devices,
		sizeof(struct cfg_pcm_instance), GFP_KERNEL);
	if (!plat_data->cfg_card.pcm_instances)
		return -ENOMEM;

	for (i = 0; i < num_devices; i++) {
		ret = cfg_device(drv_info,
			&plat_data->cfg_card.pcm_instances[i],
			&plat_data->cfg_card.pcm_hw,
			xb_dev->nodename, i, stream_idx);
		if (ret < 0)
			return ret;
	}
	plat_data->cfg_card.num_pcm_instances = num_devices;
	return 0;
}

static void xdrv_remove_internal(struct xdrv_info *drv_info)
{
	sdrv_cleanup(drv_info);
	xdrv_evtchnl_free_all(drv_info);
}

static inline grant_ref_t sh_buf_get_dir_start(struct sh_buf_info *buf)
{
	if (!buf->grefs)
		return GRANT_INVALID_REF;
	return buf->grefs[0];
}

static inline void sh_buf_clear(struct sh_buf_info *buf)
{
	memset(buf, 0, sizeof(*buf));
}

static void sh_buf_free(struct sh_buf_info *buf)
{
	int i;

	if (buf->grefs) {
		for (i = 0; i < buf->num_grefs; i++)
			if (buf->grefs[i] != GRANT_INVALID_REF)
				gnttab_end_foreign_access(buf->grefs[i],
						0, 0UL);
		kfree(buf->grefs);
	}
	kfree(buf->vdirectory);
	free_pages_exact(buf->vbuffer, buf->vbuffer_sz);
	sh_buf_clear(buf);
}

/*
 * number of grant references a page can hold with respect to the
 * xendispl_page_directory header
 */
#define XENSND_NUM_GREFS_PER_PAGE ((XEN_PAGE_SIZE - \
	offsetof(struct xensnd_page_directory, gref)) / \
	sizeof(grant_ref_t))

static void sh_buf_fill_page_dir(struct sh_buf_info *buf, int num_pages_dir)
{
	struct xensnd_page_directory *page_dir;
	unsigned char *ptr;
	int i, cur_gref, grefs_left, to_copy;

	ptr = buf->vdirectory;
	grefs_left = buf->num_grefs - num_pages_dir;
	/*
	 * skip grant references at the beginning, they are for pages granted
	 * for the page directory itself
	 */
	cur_gref = num_pages_dir;
	for (i = 0; i < num_pages_dir; i++) {
		page_dir = (struct xensnd_page_directory *)ptr;
		if (grefs_left <= XENSND_NUM_GREFS_PER_PAGE) {
			to_copy = grefs_left;
			page_dir->gref_dir_next_page = GRANT_INVALID_REF;
		} else {
			to_copy = XENSND_NUM_GREFS_PER_PAGE;
			page_dir->gref_dir_next_page = buf->grefs[i + 1];
		}
		memcpy(&page_dir->gref, &buf->grefs[cur_gref],
			to_copy * sizeof(grant_ref_t));
		ptr += XEN_PAGE_SIZE;
		grefs_left -= to_copy;
		cur_gref += to_copy;
	}
}

static int sh_buf_grant_refs(struct xenbus_device *xb_dev,
	struct sh_buf_info *buf,
	int num_pages_dir, int num_pages_vbuffer, int num_grefs)
{
	grant_ref_t priv_gref_head;
	int ret, i, j, cur_ref;
	int otherend_id;

	ret = gnttab_alloc_grant_references(num_grefs, &priv_gref_head);
	if (ret)
		return ret;

	buf->num_grefs = num_grefs;
	otherend_id = xb_dev->otherend_id;
	j = 0;

	for (i = 0; i < num_pages_dir; i++) {
		cur_ref = gnttab_claim_grant_reference(&priv_gref_head);
		if (cur_ref < 0) {
			ret = cur_ref;
			goto fail;
		}

		gnttab_grant_foreign_access_ref(cur_ref, otherend_id,
			xen_page_to_gfn(virt_to_page(buf->vdirectory +
				XEN_PAGE_SIZE * i)), 0);
		buf->grefs[j++] = cur_ref;
	}

	for (i = 0; i < num_pages_vbuffer; i++) {
		cur_ref = gnttab_claim_grant_reference(&priv_gref_head);
		if (cur_ref < 0) {
			ret = cur_ref;
			goto fail;
		}

		gnttab_grant_foreign_access_ref(cur_ref, otherend_id,
			xen_page_to_gfn(virt_to_page(buf->vbuffer +
				XEN_PAGE_SIZE * i)), 0);
		buf->grefs[j++] = cur_ref;
	}

	gnttab_free_grant_references(priv_gref_head);
	sh_buf_fill_page_dir(buf, num_pages_dir);
	return 0;

fail:
	gnttab_free_grant_references(priv_gref_head);
	return ret;
}

static int sh_buf_alloc_int_buffers(struct sh_buf_info *buf,
		int num_pages_dir, int num_pages_vbuffer, int num_grefs)
{
	buf->grefs = kcalloc(num_grefs, sizeof(*buf->grefs), GFP_KERNEL);
	if (!buf->grefs)
		return -ENOMEM;

	buf->vdirectory = kcalloc(num_pages_dir, XEN_PAGE_SIZE, GFP_KERNEL);
	if (!buf->vdirectory)
		goto fail;

	buf->vbuffer_sz = num_pages_vbuffer * XEN_PAGE_SIZE;
	buf->vbuffer = alloc_pages_exact(buf->vbuffer_sz, GFP_KERNEL);
	if (!buf->vbuffer)
		goto fail;
	return 0;

fail:
	kfree(buf->grefs);
	buf->grefs = NULL;
	kfree(buf->vdirectory);
	buf->vdirectory = NULL;
	return -ENOMEM;
}

static int sh_buf_alloc(struct xenbus_device *xb_dev,
	struct sh_buf_info *buf, unsigned int buffer_size)
{
	int num_pages_vbuffer, num_pages_dir, num_grefs;
	int ret;

	sh_buf_clear(buf);

	num_pages_vbuffer = DIV_ROUND_UP(buffer_size, XEN_PAGE_SIZE);
	/* number of pages the page directory consumes itself */
	num_pages_dir = DIV_ROUND_UP(num_pages_vbuffer,
		XENSND_NUM_GREFS_PER_PAGE);
	num_grefs = num_pages_vbuffer + num_pages_dir;

	ret = sh_buf_alloc_int_buffers(buf, num_pages_dir,
		num_pages_vbuffer, num_grefs);
	if (ret < 0)
		return ret;

	ret = sh_buf_grant_refs(xb_dev, buf,
		num_pages_dir, num_pages_vbuffer, num_grefs);
	if (ret < 0)
		return ret;

	sh_buf_fill_page_dir(buf, num_pages_dir);
	return 0;
}

static int xdrv_be_on_initwait(struct xdrv_info *drv_info)
{
	int stream_idx;
	int ret;

	drv_info->cfg_plat_data.xdrv_info = drv_info;
	stream_idx = 0;
	ret = cfg_card(drv_info, &drv_info->cfg_plat_data, &stream_idx);
	if (ret < 0)
		return ret;
	return xdrv_evtchnl_create_all(drv_info, stream_idx);
}

static inline int xdrv_be_on_connected(struct xdrv_info *drv_info)
{
	return sdrv_init(drv_info);
}

static inline void xdrv_be_on_disconnected(struct xdrv_info *drv_info)
{
	xdrv_remove_internal(drv_info);
}

static void xdrv_be_on_changed(struct xenbus_device *xb_dev,
	enum xenbus_state backend_state)
{
	struct xdrv_info *drv_info = dev_get_drvdata(&xb_dev->dev);
	int ret;

	switch (backend_state) {
	case XenbusStateReconfiguring:
		/* fall through */
	case XenbusStateReconfigured:
		/* fall through */
	case XenbusStateInitialised:
		/* fall through */
		break;

	case XenbusStateInitialising:
		if (xb_dev->state == XenbusStateInitialising)
			break;

		/* recovering after backend unexpected closure */
		mutex_lock(&drv_info->mutex);
		xdrv_be_on_disconnected(drv_info);
		mutex_unlock(&drv_info->mutex);
		xenbus_switch_state(xb_dev, XenbusStateInitialising);
		break;

	case XenbusStateInitWait:
		if (xb_dev->state != XenbusStateInitialising)
			break;

		mutex_lock(&drv_info->mutex);
		ret = xdrv_be_on_initwait(drv_info);
		mutex_unlock(&drv_info->mutex);
		if (ret < 0) {
			xenbus_dev_fatal(xb_dev, ret,
				"initializing " XENSND_DRIVER_NAME);
			break;
		}

		xenbus_switch_state(xb_dev, XenbusStateInitialised);
		break;

	case XenbusStateConnected:
		if (xb_dev->state != XenbusStateInitialised)
			break;

		mutex_lock(&drv_info->mutex);
		ret = xdrv_be_on_connected(drv_info);
		mutex_unlock(&drv_info->mutex);
		if (ret < 0) {
			xenbus_dev_fatal(xb_dev, ret,
				"connecting " XENSND_DRIVER_NAME);
			break;
		}

		xenbus_switch_state(xb_dev, XenbusStateConnected);
		break;

	case XenbusStateClosing:
		/*
		 * in this state backend starts freeing resources,
		 * so let it go into closed state first, so we can also
		 * remove ours
		 */
		break;

	case XenbusStateUnknown:
		/* fall through */
	case XenbusStateClosed:
		if (xb_dev->state == XenbusStateClosed)
			break;

		mutex_lock(&drv_info->mutex);
		xdrv_be_on_disconnected(drv_info);
		mutex_unlock(&drv_info->mutex);
		xenbus_switch_state(xb_dev, XenbusStateInitialising);
		break;
	}
}

static int xdrv_probe(struct xenbus_device *xb_dev,
	const struct xenbus_device_id *id)
{
	struct xdrv_info *drv_info;

	drv_info = devm_kzalloc(&xb_dev->dev, sizeof(*drv_info), GFP_KERNEL);
	if (!drv_info) {
		xenbus_dev_fatal(xb_dev, -ENOMEM, "allocating device memory");
		return -ENOMEM;
	}

	drv_info->xb_dev = xb_dev;
	spin_lock_init(&drv_info->io_lock);
	mutex_init(&drv_info->mutex);
	dev_set_drvdata(&xb_dev->dev, drv_info);
	xenbus_switch_state(xb_dev, XenbusStateInitialising);
	return 0;
}

static int xdrv_remove(struct xenbus_device *dev)
{
	struct xdrv_info *drv_info = dev_get_drvdata(&dev->dev);

	xenbus_switch_state(dev, XenbusStateClosed);
	mutex_lock(&drv_info->mutex);
	xdrv_remove_internal(drv_info);
	mutex_unlock(&drv_info->mutex);
	return 0;
}

static const struct xenbus_device_id xdrv_ids[] = {
	{ XENSND_DRIVER_NAME },
	{ "" }
};

static struct xenbus_driver xen_driver = {
	.ids = xdrv_ids,
	.probe = xdrv_probe,
	.remove = xdrv_remove,
	.otherend_changed = xdrv_be_on_changed,
};

static int __init xdrv_init(void)
{
	if (!xen_domain())
		return -ENODEV;

	pr_info("Initialising Xen " XENSND_DRIVER_NAME " frontend driver\n");
	return xenbus_register_frontend(&xen_driver);
}

static void __exit xdrv_cleanup(void)
{
	pr_info("Unregistering Xen " XENSND_DRIVER_NAME " frontend driver\n");
	xenbus_unregister_driver(&xen_driver);
}

module_init(xdrv_init);
module_exit(xdrv_cleanup);

MODULE_DESCRIPTION("Xen virtual sound device frontend");
MODULE_LICENSE("GPL");
MODULE_ALIAS("xen:"XENSND_DRIVER_NAME);
MODULE_SUPPORTED_DEVICE("{{ALSA,Virtual soundcard}}");
