/*
 *  Xen para-virtual sound device
 *
 *  Based on sound/drivers/dummy.c
 *  Based on drivers/net/xen-netfront.c
 *  Based on drivers/block/xen-blkfront.c
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
 */

#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/platform_device.h>
#include <linux/irq.h>
#include <linux/vmalloc.h>

#include <sound/core.h>
#include <sound/pcm.h>

#include <asm/xen/hypervisor.h>
#include <xen/xen.h>
#include <xen/platform_pci.h>
#include <xen/xenbus.h>
#include <xen/events.h>
#include <xen/grant_table.h>
#include <xen/interface/io/ring.h>
#include <xen/interface/io/sndif_linux.h>

#define GRANT_INVALID_REF	0
/* timeout in ms to wait for backend to respond */
#define VSND_WAIT_BACK_MS	5000

enum xdrv_evtchnl_state {
	EVTCHNL_STATE_DISCONNECTED,
	EVTCHNL_STATE_CONNECTED,
	EVTCHNL_STATE_SUSPENDED,
};

struct xdrv_evtchnl_info {
	struct xdrv_info *drv_info;
	struct xen_sndif_front_ring ring;
	int ring_ref;
	int port;
	int irq;
	struct completion completion;
	/* state of the event channel */
	enum xdrv_evtchnl_state state;
	/* latest response status and id */
	int resp_status;
	uint16_t resp_id;
};

struct xdrv_shared_buffer_info {
	int num_grefs;
	grant_ref_t *grefs;
	unsigned char *vdirectory;
	unsigned char *vbuffer;
	size_t vbuffer_sz;
};

struct sdev_alsa_timer_info {
	spinlock_t lock;
	struct timer_list timer;
	unsigned long base_time;
	unsigned int frac_pos;	/* fractional sample position (based HZ) */
	unsigned int frac_period_rest;
	unsigned int frac_buffer_size;	/* buffer_size * HZ */
	unsigned int frac_period_size;	/* period_size * HZ */
	unsigned int rate;
	int elapsed;
	struct snd_pcm_substream *substream;
};

struct sdev_pcm_stream_info {
	int index;
	struct snd_pcm_hardware pcm_hw;
	struct xdrv_evtchnl_info *evtchnl;
	bool is_open;
	uint8_t req_next_id;
	struct sdev_alsa_timer_info dpcm;
	struct xdrv_shared_buffer_info sh_buf;
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
	/* array of PCM instances of this card */
	int num_pcm_instances;
	struct sdev_pcm_instance_info *pcm_instances;
};

struct xdrv_info {
	struct xenbus_device *xb_dev;
	spinlock_t io_lock;
	struct mutex mutex;
	bool sdrv_registered;
	/* array of virtual sound platform devices */
	struct platform_device **sdrv_devs;

	int num_evt_channels;
	struct xdrv_evtchnl_info *evtchnls;

	/* number of virtual cards */
	int cfg_num_cards;
	struct sdev_card_plat_data *cfg_plat_data;
};

struct cfg_stream {
	int index;
	char *xenstore_path;
	struct snd_pcm_hardware pcm_hw;
};

struct cfg_pcm_instance {
	char name[80];
	/* device number */
	int device_id;
	/* Device's PCM hardware descriptor */
	struct snd_pcm_hardware pcm_hw;
	int  num_streams_pb;
	struct cfg_stream *streams_pb;
	int  num_streams_cap;
	struct cfg_stream *streams_cap;
};

struct cfg_card {
	/* card configuration */
	char shortname[32];
	char longname[80];
	/* number of PCM instances in this configuration */
	int num_devices;
	/* Card's PCM hardware descriptor */
	struct snd_pcm_hardware pcm_hw;

	/* pcm instance configurations */
	struct cfg_pcm_instance *pcm_instances;
};

struct sdev_card_plat_data {
	int index;
	struct xdrv_info *xdrv_info;
	struct cfg_card cfg_card;
};

static inline void xdrv_evtchnl_flush(
		struct xdrv_evtchnl_info *channel);
static void sdrv_copy_pcm_hw(struct snd_pcm_hardware *dst,
	struct snd_pcm_hardware *src,
	struct snd_pcm_hardware *ref_pcm_hw);
static void xdrv_sh_buf_clear(struct xdrv_shared_buffer_info *buf);
static void xdrv_sh_buf_free(struct xdrv_shared_buffer_info *buf);
static int xdrv_sh_buf_alloc(struct xenbus_device *xb_dev,
	struct xdrv_shared_buffer_info *buf,
	unsigned int buffer_size);
static grant_ref_t xdrv_sh_buf_get_dir_start(
	struct xdrv_shared_buffer_info *buf);

struct SNDIF_TO_KERN_ERROR {
	int sndif;
	int kern;
};
static struct SNDIF_TO_KERN_ERROR sndif_kern_error_codes[] = {
	{ .sndif = XENSND_RSP_OKAY,     .kern = 0 },
	{ .sndif = XENSND_RSP_ERROR,    .kern = EIO },
};

static int sndif_to_kern_error(int sndif_err)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(sndif_kern_error_codes); i++)
		if (sndif_kern_error_codes[i].sndif == sndif_err)
			return -sndif_kern_error_codes[i].kern;
	return -EIO;
}

struct ALSA_SNDIF_SAMPLE_FORMAT {
	uint8_t sndif;
	snd_pcm_format_t alsa;
};
static struct ALSA_SNDIF_SAMPLE_FORMAT alsa_sndif_formats[] = {
	{
		.sndif = XENSND_PCM_FORMAT_U8,
		.alsa = SNDRV_PCM_FORMAT_S8
	},
	{
		.sndif = XENSND_PCM_FORMAT_S8,
		.alsa = SNDRV_PCM_FORMAT_S8
	},
	{
		.sndif = XENSND_PCM_FORMAT_U16_LE,
		.alsa = SNDRV_PCM_FORMAT_U16_LE
	},
	{
		.sndif = XENSND_PCM_FORMAT_U16_BE,
		.alsa = SNDRV_PCM_FORMAT_U16_BE
	},
	{
		.sndif = XENSND_PCM_FORMAT_S16_LE,
		.alsa = SNDRV_PCM_FORMAT_S16_LE
	},
	{
		.sndif = XENSND_PCM_FORMAT_S16_BE,
		.alsa = SNDRV_PCM_FORMAT_S16_BE
	},
	{
		.sndif = XENSND_PCM_FORMAT_U24_LE,
		.alsa = SNDRV_PCM_FORMAT_U24_LE
	},
	{
		.sndif = XENSND_PCM_FORMAT_U24_BE,
		.alsa = SNDRV_PCM_FORMAT_U24_BE
	},
	{
		.sndif = XENSND_PCM_FORMAT_S24_LE,
		.alsa = SNDRV_PCM_FORMAT_S24_LE
	},
	{
		.sndif = XENSND_PCM_FORMAT_S24_BE,
		.alsa = SNDRV_PCM_FORMAT_S24_BE
	},
	{
		.sndif = XENSND_PCM_FORMAT_U32_LE,
		.alsa = SNDRV_PCM_FORMAT_U32_LE
	},
	{
		.sndif = XENSND_PCM_FORMAT_U32_BE,
		.alsa = SNDRV_PCM_FORMAT_U32_BE
	},
	{
		.sndif = XENSND_PCM_FORMAT_S32_LE,
		.alsa = SNDRV_PCM_FORMAT_S32_LE
	},
	{
		.sndif = XENSND_PCM_FORMAT_S32_BE,
		.alsa = SNDRV_PCM_FORMAT_S32_BE
	},
	{
		.sndif = XENSND_PCM_FORMAT_A_LAW,
		.alsa = SNDRV_PCM_FORMAT_A_LAW
	},
	{
		.sndif = XENSND_PCM_FORMAT_MU_LAW,
		.alsa = SNDRV_PCM_FORMAT_MU_LAW
	},
	{
		.sndif = XENSND_PCM_FORMAT_F32_LE,
		.alsa = SNDRV_PCM_FORMAT_FLOAT_LE
	},
	{
		.sndif = XENSND_PCM_FORMAT_F32_BE,
		.alsa = SNDRV_PCM_FORMAT_FLOAT_BE
	},
	{
		.sndif = XENSND_PCM_FORMAT_F64_LE,
		.alsa = SNDRV_PCM_FORMAT_FLOAT64_LE
	},
	{
		.sndif = XENSND_PCM_FORMAT_F64_BE,
		.alsa = SNDRV_PCM_FORMAT_FLOAT64_BE
	},
	{
		.sndif = XENSND_PCM_FORMAT_IEC958_SUBFRAME_LE,
		.alsa = SNDRV_PCM_FORMAT_IEC958_SUBFRAME_LE
	},
	{
		.sndif = XENSND_PCM_FORMAT_IEC958_SUBFRAME_BE,
		.alsa = SNDRV_PCM_FORMAT_IEC958_SUBFRAME_BE
	},
	{
		.sndif = XENSND_PCM_FORMAT_IMA_ADPCM,
		.alsa = SNDRV_PCM_FORMAT_IMA_ADPCM
	},
	{
		.sndif = XENSND_PCM_FORMAT_MPEG,
		.alsa = SNDRV_PCM_FORMAT_MPEG
	},
	{
		.sndif = XENSND_PCM_FORMAT_GSM,
		.alsa = SNDRV_PCM_FORMAT_GSM
	},
	{
		.sndif = XENSND_PCM_FORMAT_SPECIAL,
		.alsa = SNDRV_PCM_FORMAT_SPECIAL
	},
};

static uint8_t alsa_to_sndif_format(snd_pcm_format_t format)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(alsa_sndif_formats); i++)
		if (alsa_sndif_formats[i].alsa == format)
			return alsa_sndif_formats[i].sndif;
	return XENSND_PCM_FORMAT_SPECIAL;
}

/*
 * Sound driver start
 */

struct sdev_pcm_stream_info *sdrv_stream_get(
		struct snd_pcm_substream *substream)
{
	struct sdev_pcm_instance_info *pcm_instance =
		snd_pcm_substream_chip(substream);
	struct sdev_pcm_stream_info *stream;

	if (substream->stream == SNDRV_PCM_STREAM_PLAYBACK)
		stream = &pcm_instance->streams_pb[substream->number];
	else
		stream = &pcm_instance->streams_cap[substream->number];
	return stream;
}

static void sdrv_stream_clear(struct sdev_pcm_stream_info *stream)
{
	stream->is_open = false;
	stream->req_next_id = 0;
	xdrv_sh_buf_clear(&stream->sh_buf);
}

static inline struct xensnd_req *sdrv_be_stream_prepare_req(
		struct sdev_pcm_stream_info *stream,
		uint8_t operation)
{
	struct xensnd_req *req;

	req = RING_GET_REQUEST(&stream->evtchnl->ring,
		stream->evtchnl->ring.req_prod_pvt);
	req->u.data.operation = operation;
	req->u.data.stream_idx = stream->index;
	req->u.data.id = stream->req_next_id++;
	stream->evtchnl->resp_id = req->u.data.id;
	return req;
}

void sdrv_be_stream_free(struct sdev_pcm_stream_info *stream)
{
	xdrv_sh_buf_free(&stream->sh_buf);
	sdrv_stream_clear(stream);
}

/* CAUTION!!! Call this with the spin lock held.
 * This function will release it
 */
int sdrv_be_stream_do_io(struct xdrv_evtchnl_info *evtchnl,
	struct xensnd_req *req, unsigned long flags)
{
	int ret;

	reinit_completion(&evtchnl->completion);
	if (unlikely(evtchnl->state != EVTCHNL_STATE_CONNECTED)) {
		spin_unlock_irqrestore(&evtchnl->drv_info->io_lock, flags);
		return -EIO;
	}
	xdrv_evtchnl_flush(evtchnl);
	spin_unlock_irqrestore(&evtchnl->drv_info->io_lock, flags);
	ret = 0;
	if (wait_for_completion_interruptible_timeout(
			&evtchnl->completion,
			msecs_to_jiffies(VSND_WAIT_BACK_MS)) <= 0)
		ret = -ETIMEDOUT;
	if (ret < 0)
		return ret;
	return sndif_to_kern_error(evtchnl->resp_status);
}

int sdrv_be_stream_open(struct snd_pcm_substream *substream,
		struct sdev_pcm_stream_info *stream)
{
	struct sdev_pcm_instance_info *pcm_instance =
		snd_pcm_substream_chip(substream);
	struct snd_pcm_runtime *runtime = substream->runtime;
	struct xdrv_info *xdrv_info;
	struct xensnd_req *req;
	int ret;
	unsigned long flags;

	xdrv_info = pcm_instance->card_info->xdrv_info;
	spin_lock_irqsave(&xdrv_info->io_lock, flags);

	req = sdrv_be_stream_prepare_req(stream, XENSND_OP_OPEN);
	req->u.data.op.open.pcm_format = alsa_to_sndif_format(runtime->format);
	req->u.data.op.open.pcm_channels = runtime->channels;
	req->u.data.op.open.pcm_rate = runtime->rate;
	req->u.data.op.open.gref_directory_start =
		xdrv_sh_buf_get_dir_start(&stream->sh_buf);
	ret = sdrv_be_stream_do_io(stream->evtchnl, req, flags);
	stream->is_open = ret < 0 ? false : true;
	return ret;
}

int sdrv_be_stream_close(struct snd_pcm_substream *substream,
	struct sdev_pcm_stream_info *stream)
{
	struct sdev_pcm_instance_info *pcm_instance =
		snd_pcm_substream_chip(substream);
	struct xdrv_info *xdrv_info;
	struct xensnd_req *req;
	int ret;
	unsigned long flags;

	xdrv_info = pcm_instance->card_info->xdrv_info;
	spin_lock_irqsave(&xdrv_info->io_lock, flags);

	req = sdrv_be_stream_prepare_req(stream, XENSND_OP_CLOSE);
	ret = sdrv_be_stream_do_io(stream->evtchnl, req, flags);
	stream->is_open = false;
	return ret;
}

static void sdrv_alsa_timer_rearm(struct sdev_alsa_timer_info *dpcm)
{
	mod_timer(&dpcm->timer, jiffies +
		(dpcm->frac_period_rest + dpcm->rate - 1) / dpcm->rate);
}

static void sdrv_alsa_timer_update(struct sdev_alsa_timer_info *dpcm)
{
	unsigned long delta;

	delta = jiffies - dpcm->base_time;
	if (!delta)
		return;
	dpcm->base_time += delta;
	delta *= dpcm->rate;
	dpcm->frac_pos += delta;
	while (dpcm->frac_pos >= dpcm->frac_buffer_size)
		dpcm->frac_pos -= dpcm->frac_buffer_size;
	while (dpcm->frac_period_rest <= delta) {
		dpcm->elapsed++;
		dpcm->frac_period_rest += dpcm->frac_period_size;
	}
	dpcm->frac_period_rest -= delta;
}

static int sdrv_alsa_timer_start(struct snd_pcm_substream *substream)
{
	struct sdev_pcm_stream_info *stream = sdrv_stream_get(substream);
	struct sdev_alsa_timer_info *dpcm = &stream->dpcm;

	spin_lock(&dpcm->lock);
	dpcm->base_time = jiffies;
	sdrv_alsa_timer_rearm(dpcm);
	spin_unlock(&dpcm->lock);
	return 0;
}

static int sdrv_alsa_timer_stop(struct snd_pcm_substream *substream)
{
	struct sdev_pcm_stream_info *stream = sdrv_stream_get(substream);
	struct sdev_alsa_timer_info *dpcm = &stream->dpcm;

	spin_lock(&dpcm->lock);
	del_timer(&dpcm->timer);
	spin_unlock(&dpcm->lock);
	return 0;
}

static int sdrv_alsa_timer_prepare(struct snd_pcm_substream *substream)
{
	struct snd_pcm_runtime *runtime = substream->runtime;
	struct sdev_pcm_stream_info *stream = sdrv_stream_get(substream);
	struct sdev_alsa_timer_info *dpcm = &stream->dpcm;

	dpcm->frac_pos = 0;
	dpcm->rate = runtime->rate;
	dpcm->frac_buffer_size = runtime->buffer_size * HZ;
	dpcm->frac_period_size = runtime->period_size * HZ;
	dpcm->frac_period_rest = dpcm->frac_period_size;
	dpcm->elapsed = 0;
	return 0;
}

static void sdrv_alsa_timer_callback(unsigned long data)
{
	struct sdev_alsa_timer_info *dpcm = (struct sdev_alsa_timer_info *)data;
	unsigned long flags;
	int elapsed = 0;

	spin_lock_irqsave(&dpcm->lock, flags);
	sdrv_alsa_timer_update(dpcm);
	sdrv_alsa_timer_rearm(dpcm);
	elapsed = dpcm->elapsed;
	dpcm->elapsed = 0;
	spin_unlock_irqrestore(&dpcm->lock, flags);
	if (elapsed)
		snd_pcm_period_elapsed(dpcm->substream);
}

static snd_pcm_uframes_t sdrv_alsa_timer_pointer(
		struct snd_pcm_substream *substream)
{
	struct sdev_pcm_stream_info *stream = sdrv_stream_get(substream);
	struct sdev_alsa_timer_info *dpcm = &stream->dpcm;
	snd_pcm_uframes_t pos;

	spin_lock(&dpcm->lock);
	sdrv_alsa_timer_update(dpcm);
	pos = dpcm->frac_pos / HZ;
	spin_unlock(&dpcm->lock);
	return pos;
}

static int sdrv_alsa_timer_create(struct snd_pcm_substream *substream)
{
	struct sdev_pcm_stream_info *stream = sdrv_stream_get(substream);
	struct sdev_alsa_timer_info *dpcm = &stream->dpcm;

	setup_timer(&dpcm->timer, sdrv_alsa_timer_callback,
		(unsigned long) dpcm);
	spin_lock_init(&dpcm->lock);
	dpcm->substream = substream;
	return 0;
}

int sdrv_alsa_open(struct snd_pcm_substream *substream)
{
	struct sdev_pcm_instance_info *pcm_instance =
		snd_pcm_substream_chip(substream);
	struct sdev_pcm_stream_info *stream = sdrv_stream_get(substream);
	struct snd_pcm_runtime *runtime = substream->runtime;
	struct xdrv_info *xdrv_info;
	int ret;
	unsigned long flags;

	sdrv_copy_pcm_hw(&runtime->hw,
		&stream->pcm_hw, &pcm_instance->pcm_hw);

	runtime->hw.info &= ~(SNDRV_PCM_INFO_MMAP |
			      SNDRV_PCM_INFO_MMAP_VALID |
			      SNDRV_PCM_INFO_DOUBLE |
			      SNDRV_PCM_INFO_BATCH |
			      SNDRV_PCM_INFO_NONINTERLEAVED |
			      SNDRV_PCM_INFO_RESUME |
			      SNDRV_PCM_INFO_PAUSE);
	runtime->hw.info |= SNDRV_PCM_INFO_INTERLEAVED;

	xdrv_info = pcm_instance->card_info->xdrv_info;
	ret = sdrv_alsa_timer_create(substream);
	spin_lock_irqsave(&xdrv_info->io_lock, flags);
	xdrv_sh_buf_clear(&stream->sh_buf);
	sdrv_stream_clear(stream);
	stream->evtchnl = &xdrv_info->evtchnls[stream->index];
	if (ret < 0)
		stream->evtchnl->state = EVTCHNL_STATE_DISCONNECTED;
	else
		stream->evtchnl->state = EVTCHNL_STATE_CONNECTED;
	spin_unlock_irqrestore(&xdrv_info->io_lock, flags);
	return ret;
}

int sdrv_alsa_close(struct snd_pcm_substream *substream)
{
	struct sdev_pcm_instance_info *pcm_instance =
		snd_pcm_substream_chip(substream);
	struct sdev_pcm_stream_info *stream = sdrv_stream_get(substream);
	struct xdrv_info *xdrv_info;
	unsigned long flags;

	xdrv_info = pcm_instance->card_info->xdrv_info;
	sdrv_alsa_timer_stop(substream);
	spin_lock_irqsave(&xdrv_info->io_lock, flags);
	stream->evtchnl->state = EVTCHNL_STATE_DISCONNECTED;
	spin_unlock_irqrestore(&xdrv_info->io_lock, flags);
	return 0;
}

int sdrv_alsa_hw_params(struct snd_pcm_substream *substream,
		 struct snd_pcm_hw_params *params)
{
	struct sdev_pcm_instance_info *pcm_instance =
		snd_pcm_substream_chip(substream);
	struct sdev_pcm_stream_info *stream = sdrv_stream_get(substream);
	struct xdrv_info *xdrv_info;
	int ret;
	unsigned int buffer_size;

	buffer_size = params_buffer_bytes(params);
	xdrv_sh_buf_clear(&stream->sh_buf);
	sdrv_stream_clear(stream);
	/* number of pages needed for the runtime buffer */
	xdrv_info = pcm_instance->card_info->xdrv_info;
	ret = xdrv_sh_buf_alloc(xdrv_info->xb_dev,
		&stream->sh_buf, buffer_size);
	if (ret < 0)
		goto fail;
	return 0;

fail:
	dev_err(&xdrv_info->xb_dev->dev,
		"Failed to allocate buffers for stream idx %d",
		stream->index);
	sdrv_be_stream_free(stream);
	return ret;
}

int sdrv_alsa_hw_free(struct snd_pcm_substream *substream)
{
	struct sdev_pcm_stream_info *stream = sdrv_stream_get(substream);
	int ret;

	ret = sdrv_be_stream_close(substream, stream);
	sdrv_be_stream_free(stream);
	return ret;
}

int sdrv_alsa_prepare(struct snd_pcm_substream *substream)
{
	struct sdev_pcm_stream_info *stream = sdrv_stream_get(substream);
	int ret = 0;

	if (!stream->is_open) {
		ret = sdrv_be_stream_open(substream, stream);
		if (ret < 0)
			return ret;
		ret = sdrv_alsa_timer_prepare(substream);
	}
	return ret;
}

int sdrv_alsa_trigger(struct snd_pcm_substream *substream, int cmd)
{
	switch (cmd) {
	case SNDRV_PCM_TRIGGER_START:
		/* fall through */
	case SNDRV_PCM_TRIGGER_RESUME:
		return sdrv_alsa_timer_start(substream);
	case SNDRV_PCM_TRIGGER_STOP:
		/* fall through */
	case SNDRV_PCM_TRIGGER_SUSPEND:
		return sdrv_alsa_timer_stop(substream);
	default:
		break;
	}
	return 0;
}

snd_pcm_uframes_t sdrv_alsa_pointer(struct snd_pcm_substream *substream)
{
	return sdrv_alsa_timer_pointer(substream);
}

int sdrv_alsa_playback_do_write(struct snd_pcm_substream *substream,
		snd_pcm_uframes_t len)
{
	struct sdev_pcm_stream_info *stream = sdrv_stream_get(substream);
	struct sdev_pcm_instance_info *pcm_instance =
		snd_pcm_substream_chip(substream);
	struct xdrv_info *xdrv_info;
	struct xensnd_req *req;
	unsigned long flags;

	xdrv_info = pcm_instance->card_info->xdrv_info;
	spin_lock_irqsave(&xdrv_info->io_lock, flags);
	req = sdrv_be_stream_prepare_req(stream, XENSND_OP_WRITE);
	req->u.data.op.write.len = len;
	req->u.data.op.write.offset = 0;
	return sdrv_be_stream_do_io(stream->evtchnl, req, flags);
}

int sdrv_alsa_playback_copy(struct snd_pcm_substream *substream, int channel,
	snd_pcm_uframes_t pos, void __user *buf, snd_pcm_uframes_t count)
{
	struct sdev_pcm_stream_info *stream = sdrv_stream_get(substream);
	ssize_t len;

	len = frames_to_bytes(substream->runtime, count);
	/* TODO: use XC_PAGE_SIZE */
	if (len > stream->sh_buf.vbuffer_sz)
		return -EFAULT;
	if (copy_from_user(stream->sh_buf.vbuffer, buf, len))
		return -EFAULT;
	return sdrv_alsa_playback_do_write(substream, len);
}

int sdrv_alsa_capture_copy(struct snd_pcm_substream *substream, int channel,
	snd_pcm_uframes_t pos, void __user *buf, snd_pcm_uframes_t count)
{
	struct sdev_pcm_stream_info *stream = sdrv_stream_get(substream);
	struct sdev_pcm_instance_info *pcm_instance =
		snd_pcm_substream_chip(substream);
	struct xdrv_info *xdrv_info;
	struct xensnd_req *req;
	unsigned long flags;
	int ret;
	ssize_t len;

	len = frames_to_bytes(substream->runtime, count);
	/* TODO: use XC_PAGE_SIZE */
	if (len > stream->sh_buf.vbuffer_sz)
		return -EFAULT;
	xdrv_info = pcm_instance->card_info->xdrv_info;
	spin_lock_irqsave(&xdrv_info->io_lock, flags);
	req = sdrv_be_stream_prepare_req(stream, XENSND_OP_READ);
	req->u.data.op.read.len = len;
	req->u.data.op.read.offset = 0;
	ret = sdrv_be_stream_do_io(stream->evtchnl, req, flags);
	if (ret < 0)
		return ret;
	return copy_to_user(buf, stream->sh_buf.vbuffer, len);
}

int sdrv_alsa_playback_silence(struct snd_pcm_substream *substream, int channel,
	snd_pcm_uframes_t pos, snd_pcm_uframes_t count)
{
	struct sdev_pcm_stream_info *stream = sdrv_stream_get(substream);
	ssize_t len;

	len = frames_to_bytes(substream->runtime, count);
	/* TODO: use XC_PAGE_SIZE */
	if (len > stream->sh_buf.vbuffer_sz)
		return -EFAULT;
	if (memset(stream->sh_buf.vbuffer, 0, len))
		return -EFAULT;
	return sdrv_alsa_playback_do_write(substream, len);
}

/* defaults */
/* TODO: use XC_PAGE_SIZE */
#define MAX_XEN_BUFFER_SIZE	(64 * 1024)
#define MAX_BUFFER_SIZE		MAX_XEN_BUFFER_SIZE
#define MIN_PERIOD_SIZE		64
#define MAX_PERIOD_SIZE		(MAX_BUFFER_SIZE / 8)
#define USE_FORMATS		(SNDRV_PCM_FMTBIT_U8 | \
				 SNDRV_PCM_FMTBIT_S16_LE)
#define USE_RATE		(SNDRV_PCM_RATE_CONTINUOUS | \
				 SNDRV_PCM_RATE_8000_48000)
#define USE_RATE_MIN		5500
#define USE_RATE_MAX		48000
#define USE_CHANNELS_MIN	1
#define USE_CHANNELS_MAX	2
#define USE_PERIODS_MIN		2
#define USE_PERIODS_MAX		8

static struct snd_pcm_hardware sdrv_pcm_hardware_def = {
		.info =			(SNDRV_PCM_INFO_MMAP |
					 SNDRV_PCM_INFO_INTERLEAVED |
					 SNDRV_PCM_INFO_RESUME |
					 SNDRV_PCM_INFO_MMAP_VALID),
		.formats =		USE_FORMATS,
		.rates =		USE_RATE,
		.rate_min =		USE_RATE_MIN,
		.rate_max =		USE_RATE_MAX,
		.channels_min =		USE_CHANNELS_MIN,
		.channels_max =		USE_CHANNELS_MAX,
		.buffer_bytes_max =	MAX_BUFFER_SIZE,
		.period_bytes_min =	MIN_PERIOD_SIZE,
		.period_bytes_max =	MAX_PERIOD_SIZE,
		.periods_min =		USE_PERIODS_MIN,
		.periods_max =		USE_PERIODS_MAX,
		.fifo_size =		0,
};

static struct snd_pcm_ops sdrv_alsa_playback_ops = {
		.open =		sdrv_alsa_open,
		.close =	sdrv_alsa_close,
		.ioctl =	snd_pcm_lib_ioctl,
		.hw_params =	sdrv_alsa_hw_params,
		.hw_free =	sdrv_alsa_hw_free,
		.prepare =	sdrv_alsa_prepare,
		.trigger =	sdrv_alsa_trigger,
		.pointer =	sdrv_alsa_pointer,
		.copy =		sdrv_alsa_playback_copy,
		.silence =	sdrv_alsa_playback_silence,
};

static struct snd_pcm_ops sdrv_alsa_capture_ops = {
		.open =		sdrv_alsa_open,
		.close =	sdrv_alsa_close,
		.ioctl =	snd_pcm_lib_ioctl,
		.hw_params =	sdrv_alsa_hw_params,
		.hw_free =	sdrv_alsa_hw_free,
		.prepare =	sdrv_alsa_prepare,
		.trigger =	sdrv_alsa_trigger,
		.pointer =	sdrv_alsa_pointer,
		.copy =		sdrv_alsa_capture_copy,
};

static int sdrv_new_pcm(struct sdev_card_info *card_info,
		struct cfg_pcm_instance *instance_config,
		struct sdev_pcm_instance_info *pcm_instance_info)
{
	struct snd_pcm *pcm;
	int ret, i;

	dev_dbg(&card_info->xdrv_info->xb_dev->dev,
		"New PCM device \"%s\" with id %d playback %d capture %d",
		instance_config->name,
		instance_config->device_id,
		instance_config->num_streams_pb,
		instance_config->num_streams_cap);
	pcm_instance_info->card_info = card_info;
	sdrv_copy_pcm_hw(&pcm_instance_info->pcm_hw,
		&instance_config->pcm_hw, &card_info->pcm_hw);
	/* allocate info for playback streams if any */
	if (instance_config->num_streams_pb) {
		pcm_instance_info->streams_pb = devm_kzalloc(
			&card_info->card->card_dev,
			instance_config->num_streams_pb *
			sizeof(struct sdev_pcm_stream_info),
			GFP_KERNEL);
		if (!pcm_instance_info->streams_pb)
			return -ENOMEM;
	}
	/* allocate info for capture streams if any */
	if (instance_config->num_streams_cap) {
		pcm_instance_info->streams_cap = devm_kzalloc(
			&card_info->card->card_dev,
			instance_config->num_streams_cap *
			sizeof(struct sdev_pcm_stream_info),
			GFP_KERNEL);
		if (!pcm_instance_info->streams_cap)
			return -ENOMEM;
	}
	pcm_instance_info->num_pcm_streams_pb =
			instance_config->num_streams_pb;
	pcm_instance_info->num_pcm_streams_cap =
			instance_config->num_streams_cap;
	for (i = 0; i < pcm_instance_info->num_pcm_streams_pb; i++) {
		pcm_instance_info->streams_pb[i].pcm_hw =
			instance_config->streams_pb[i].pcm_hw;
		pcm_instance_info->streams_pb[i].index =
			instance_config->streams_pb[i].index;
	}
	for (i = 0; i < pcm_instance_info->num_pcm_streams_cap; i++) {
		pcm_instance_info->streams_cap[i].pcm_hw =
			instance_config->streams_cap[i].pcm_hw;
		pcm_instance_info->streams_cap[i].index =
			instance_config->streams_cap[i].index;
	}

	ret = snd_pcm_new(card_info->card, instance_config->name,
			instance_config->device_id,
			instance_config->num_streams_pb,
			instance_config->num_streams_cap,
			&pcm);
	if (ret < 0)
		return ret;
	if (instance_config->num_streams_pb)
		snd_pcm_set_ops(pcm, SNDRV_PCM_STREAM_PLAYBACK,
				&sdrv_alsa_playback_ops);
	if (instance_config->num_streams_cap)
		snd_pcm_set_ops(pcm, SNDRV_PCM_STREAM_CAPTURE,
				&sdrv_alsa_capture_ops);
	pcm->private_data = pcm_instance_info;
	pcm->info_flags = 0;
	strcpy(pcm->name, "Virtual card PCM");
	pcm_instance_info->pcm = pcm;
	return 0;
}

static void sdrv_copy_pcm_hw(struct snd_pcm_hardware *dst,
	struct snd_pcm_hardware *src,
	struct snd_pcm_hardware *ref_pcm_hw)
{
	*dst = *ref_pcm_hw;
	if (src->formats)
		dst->formats = src->formats;
	if (src->buffer_bytes_max)
		dst->buffer_bytes_max =
				src->buffer_bytes_max;
	if (src->period_bytes_min)
		dst->period_bytes_min =
				src->period_bytes_min;
	if (src->period_bytes_max)
		dst->period_bytes_max =
				src->period_bytes_max;
	if (src->periods_min)
		dst->periods_min = src->periods_min;
	if (src->periods_max)
		dst->periods_max = src->periods_max;
	if (src->rates)
		dst->rates = src->rates;
	if (src->rate_min)
		dst->rate_min = src->rate_min;
	if (src->rate_max)
		dst->rate_max = src->rate_max;
	if (src->channels_min)
		dst->channels_min = src->channels_min;
	if (src->channels_max)
		dst->channels_max = src->channels_max;
	if (src->buffer_bytes_max) {
		dst->buffer_bytes_max = src->buffer_bytes_max;
		dst->period_bytes_max = src->buffer_bytes_max /
				src->periods_max;
	}
}

static int sdrv_probe(struct platform_device *pdev)
{
	struct sdev_card_info *card_info;
	struct sdev_card_plat_data *platdata;
	struct snd_card *card;
	char card_id[sizeof(card->id)];
	int ret, i;

	platdata = dev_get_platdata(&pdev->dev);
	dev_dbg(&pdev->dev, "Creating virtual sound card %d", platdata->index);
	snprintf(card_id, sizeof(card->id), XENSND_DRIVER_NAME "%d",
		platdata->index);
	ret = snd_card_new(&pdev->dev, platdata->index, card_id, THIS_MODULE,
		sizeof(struct sdev_card_info), &card);
	if (ret < 0)
		return ret;
	/* card_info is allocated and maintained by snd_card_new */
	card_info = card->private_data;
	card_info->xdrv_info = platdata->xdrv_info;
	card_info->card = card;
	card_info->pcm_instances = devm_kzalloc(&pdev->dev,
			platdata->cfg_card.num_devices *
			sizeof(struct sdev_pcm_instance_info), GFP_KERNEL);
	if (!card_info->pcm_instances)
		goto fail;
	card_info->num_pcm_instances = platdata->cfg_card.num_devices;

	card_info->pcm_hw = platdata->cfg_card.pcm_hw;

	for (i = 0; i < platdata->cfg_card.num_devices; i++) {
		ret = sdrv_new_pcm(card_info,
			&platdata->cfg_card.pcm_instances[i],
			&card_info->pcm_instances[i]);
		if (ret < 0)
			goto fail;
	}
	strncpy(card->driver, XENSND_DRIVER_NAME, sizeof(card->driver));
	strncpy(card->shortname, platdata->cfg_card.shortname,
		sizeof(card->shortname));
	strncpy(card->longname, platdata->cfg_card.longname,
		sizeof(card->longname));
	ret = snd_card_register(card);
	if (ret == 0) {
		platform_set_drvdata(pdev, card);
		return 0;
	}
fail:
	snd_card_free(card);
	return ret;
}

static int sdrv_remove(struct platform_device *pdev)
{
	struct sdev_card_info *info;
	struct snd_card *card = platform_get_drvdata(pdev);

	info = card->private_data;
	dev_dbg(&pdev->dev, "Removing card %d", info->card->number);
	snd_card_free(card);
	return 0;
}

static struct platform_driver sdrv_info = {
	.probe		= sdrv_probe,
	.remove		= sdrv_remove,
	.driver		= {
		.name	= XENSND_DRIVER_NAME,
	},
};

static void sdrv_cleanup(struct xdrv_info *drv_info)
{
	int i;

	if (!drv_info->sdrv_registered)
		return;
	if (drv_info->sdrv_devs) {
		for (i = 0; i < drv_info->cfg_num_cards; i++) {
			struct platform_device *sdrv_dev;

			sdrv_dev = drv_info->sdrv_devs[i];
			if (sdrv_dev)
				platform_device_unregister(sdrv_dev);
		}
	}
	platform_driver_unregister(&sdrv_info);
	drv_info->sdrv_registered = false;
}

static int sdrv_init(struct xdrv_info *drv_info)
{
	int i, num_cards, ret;

	ret = platform_driver_register(&sdrv_info);
	if (ret < 0)
		return ret;
	drv_info->sdrv_registered = true;

	num_cards = drv_info->cfg_num_cards;
	drv_info->sdrv_devs = devm_kzalloc(&drv_info->xb_dev->dev,
			sizeof(drv_info->sdrv_devs[0]) * num_cards,
			GFP_KERNEL);
	if (!drv_info->sdrv_devs)
		goto fail;
	for (i = 0; i < num_cards; i++) {
		struct platform_device *sdrv_dev;
		struct sdev_card_plat_data *snd_dev_platdata;

		snd_dev_platdata = &drv_info->cfg_plat_data[i];
		/* pass card configuration via platform data */
		sdrv_dev = platform_device_register_data(NULL,
			XENSND_DRIVER_NAME,
			snd_dev_platdata->index, snd_dev_platdata,
			sizeof(*snd_dev_platdata));
		drv_info->sdrv_devs[i] = sdrv_dev;
		if (IS_ERR(sdrv_dev)) {
			drv_info->sdrv_devs[i] = NULL;
			goto fail;
		}
	}
	return 0;

fail:
	dev_err(&drv_info->xb_dev->dev, "Failed to register sound driver");
	sdrv_cleanup(drv_info);
	return -ENODEV;
}

/*
 * Sound driver stop
 */

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
	rmb(); /* Ensure we see queued responses up to 'rp'. */

	for (i = channel->ring.rsp_cons; i != rp; i++) {
		resp = RING_GET_RESPONSE(&channel->ring, i);
		if (resp->u.data.id != channel->resp_id)
			continue;
		switch (resp->u.data.operation) {
		case XENSND_OP_OPEN:
		case XENSND_OP_CLOSE:
		case XENSND_OP_READ:
		case XENSND_OP_WRITE:
			channel->resp_status = resp->u.data.status;
			complete(&channel->completion);
			break;
		case XENSND_OP_SET_VOLUME:
		case XENSND_OP_GET_VOLUME:
			channel->resp_status = XENSND_RSP_OKAY;
			complete(&channel->completion);
			break;
		default:
			dev_err(&drv_info->xb_dev->dev,
				"Operation %d is not supported",
				resp->u.data.operation);
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

static void xdrv_evtchnl_free(struct xdrv_info *drv_info,
		struct xdrv_evtchnl_info *channel)
{
	if (!channel->ring.sring)
		return;
	channel->state = EVTCHNL_STATE_DISCONNECTED;
	/* release all who still waits for response if any */
	channel->resp_status = -XENSND_RSP_ERROR;
	complete_all(&channel->completion);
	if (channel->irq)
		unbind_from_irqhandler(channel->irq, channel);
	channel->irq = 0;
	if (channel->port)
		xenbus_free_evtchn(drv_info->xb_dev, channel->port);
	channel->port = 0;
	/* End access and free the pages */
	if (channel->ring_ref != GRANT_INVALID_REF)
		gnttab_end_foreign_access(channel->ring_ref, 0,
			(unsigned long)channel->ring.sring);
	channel->ring_ref = GRANT_INVALID_REF;
	channel->ring.sring = NULL;
}

static void xdrv_evtchnl_free_all(struct xdrv_info *drv_info)
{
	int i;

	if (!drv_info->evtchnls)
		return;
	for (i = 0; i < drv_info->num_evt_channels; i++)
		xdrv_evtchnl_free(drv_info,
			&drv_info->evtchnls[i]);
	devm_kfree(&drv_info->xb_dev->dev, drv_info->evtchnls);
	drv_info->evtchnls = NULL;
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
	/* TODO: use XC_PAGE_SIZE */
	FRONT_RING_INIT(&evt_channel->ring, sring, PAGE_SIZE);

	ret = xenbus_grant_ring(xb_dev, sring, 1, &gref);
	if (ret < 0)
		goto fail;
	evt_channel->ring_ref = gref;

	ret = xenbus_alloc_evtchn(xb_dev, &evt_channel->port);
	if (ret < 0)
		goto fail;

	ret = bind_evtchn_to_irqhandler(evt_channel->port,
		xdrv_evtchnl_interrupt,
		0, xb_dev->devicetype, evt_channel);

	if (ret < 0)
		goto fail;
	evt_channel->irq = ret;
	return 0;

fail:
	dev_err(&xb_dev->dev, "Failed to allocate ring with err %d", ret);
	return ret;
}

static int xdrv_evtchnl_create(struct xdrv_info *drv_info,
		struct xdrv_evtchnl_info *evt_channel,
		const char *path)
{
	const char *message;
	int ret;

	/* allocate and open control channel */
	ret = xdrv_evtchnl_alloc(drv_info, evt_channel);
	if (ret < 0) {
		message = "allocating event channel";
		goto fail;
	}
	/* Write control channel ring reference */
	ret = xenbus_printf(XBT_NIL, path, XENSND_FIELD_RING_REF, "%u",
			evt_channel->ring_ref);
	if (ret < 0) {
		message = "writing " XENSND_FIELD_RING_REF;
		goto fail;
	}

	ret = xenbus_printf(XBT_NIL, path, XENSND_FIELD_EVT_CHNL, "%u",
		evt_channel->port);
	if (ret < 0) {
		message = "writing " XENSND_FIELD_EVT_CHNL;
		goto fail;
	}
	return 0;

fail:
	dev_err(&drv_info->xb_dev->dev, "Error %s with err %d", message, ret);
	return ret;
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

static int xdrv_evtchnl_create_all(struct xdrv_info *drv_info,
		int num_streams)
{
	int ret, c, d, s, stream_idx;

	drv_info->evtchnls = devm_kcalloc(&drv_info->xb_dev->dev,
		num_streams, sizeof(struct xdrv_evtchnl_info),
		GFP_KERNEL);
	if (!drv_info->evtchnls) {
		ret = -ENOMEM;
		goto fail;
	}
	for (c = 0; c < drv_info->cfg_num_cards; c++) {
		struct sdev_card_plat_data *plat_data;

		plat_data = &drv_info->cfg_plat_data[c];
		for (d = 0; d < plat_data->cfg_card.num_devices; d++) {
			struct cfg_pcm_instance *pcm_instance;

			pcm_instance = &plat_data->cfg_card.pcm_instances[d];
			for (s = 0; s < pcm_instance->num_streams_pb; s++) {
				stream_idx = pcm_instance->streams_pb[s].index;
				ret = xdrv_evtchnl_create(drv_info,
					&drv_info->evtchnls[stream_idx],
					pcm_instance->streams_pb[s].xenstore_path);
				if (ret < 0)
					goto fail;
			}
			for (s = 0; s < pcm_instance->num_streams_cap; s++) {
				stream_idx = pcm_instance->streams_cap[s].index;
				ret = xdrv_evtchnl_create(drv_info,
					&drv_info->evtchnls[stream_idx],
					pcm_instance->streams_cap[s].xenstore_path);
				if (ret < 0)
					goto fail;
			}
		}
		if (ret < 0)
			goto fail;
	}
	drv_info->num_evt_channels = num_streams;
	return 0;
fail:
	xdrv_evtchnl_free_all(drv_info);
	return ret;
}

/* get number of nodes under the path to get number of
 * cards configured or number of devices within the card
 */
static char **xdrv_cfg_get_num_nodes(const char *path, const char *node,
		int *num_entries)
{
	char **result;

	result = xenbus_directory(XBT_NIL, path, node, num_entries);
	if (IS_ERR(result)) {
		*num_entries = 0;
		return NULL;
	}
	return result;
}

struct CFG_HW_SAMPLE_RATE {
	const char *name;
	unsigned int mask;
	unsigned int value;
};
static struct CFG_HW_SAMPLE_RATE xdrv_cfg_hw_supported_rates[] = {
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
static struct CFG_HW_SAMPLE_FORMAT xdrv_cfg_hw_supported_formats[] = {
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

static void xdrv_cfg_hw_rates(char *list, unsigned int len,
		const char *path, struct snd_pcm_hardware *pcm_hw)
{
	char *cur_rate;
	unsigned int cur_mask;
	unsigned int cur_value;
	unsigned int rates;
	unsigned int rate_min;
	unsigned int rate_max;
	int i;

	cur_rate = NULL;
	rates = 0;
	rate_min = -1;
	rate_max = 0;
	while ((cur_rate = strsep(&list, XENSND_LIST_SEPARATOR))) {
		for (i = 0; i < ARRAY_SIZE(xdrv_cfg_hw_supported_rates); i++)
			if (!strncasecmp(cur_rate,
					xdrv_cfg_hw_supported_rates[i].name,
					XENSND_SAMPLE_RATE_MAX_LEN)) {
				cur_mask =
					xdrv_cfg_hw_supported_rates[i].mask;
				cur_value =
					xdrv_cfg_hw_supported_rates[i].value;
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

static void xdrv_cfg_formats(char *list, unsigned int len,
		const char *path, struct snd_pcm_hardware *pcm_hw)
{
	u64 formats;
	char *cur_format;
	int i;

	cur_format = NULL;
	formats = 0;
	while ((cur_format = strsep(&list, XENSND_LIST_SEPARATOR))) {
		for (i = 0; i < ARRAY_SIZE(xdrv_cfg_hw_supported_formats); i++)
			if (!strncasecmp(cur_format,
					xdrv_cfg_hw_supported_formats[i].name,
					XENSND_SAMPLE_FORMAT_MAX_LEN))
				formats |= xdrv_cfg_hw_supported_formats[i].mask;
	}
	if (formats)
		pcm_hw->formats = formats;
}

static void xdrv_cfg_pcm_hw(const char *path,
		struct snd_pcm_hardware *parent_pcm_hw,
		struct snd_pcm_hardware *pcm_hw)
{
	char *list;
	int val;
	size_t buf_sz;
	unsigned int len;

	*pcm_hw = *parent_pcm_hw;
	if (xenbus_scanf(XBT_NIL, path, XENSND_FIELD_CHANNELS_MIN,
			"%d", &val) < 0)
		val = 0;
	if (val)
		pcm_hw->channels_min = val;
	if (xenbus_scanf(XBT_NIL, path, XENSND_FIELD_CHANNELS_MAX,
			"%d", &val) < 0)
		val = 0;
	if (val)
		pcm_hw->channels_max = val;
	list = xenbus_read(XBT_NIL, path, XENSND_FIELD_SAMPLE_RATES, &len);
	if (!IS_ERR(list)) {
		xdrv_cfg_hw_rates(list, len, path, pcm_hw);
		kfree(list);
	}
	list = xenbus_read(XBT_NIL, path, XENSND_FIELD_SAMPLE_FORMATS, &len);
	if (!IS_ERR(list)) {
		xdrv_cfg_formats(list, len, path, pcm_hw);
		kfree(list);
	}
	if (xenbus_scanf(XBT_NIL, path, XENSND_FIELD_BUFFER_SIZE,
			"%zu", &buf_sz) < 0)
		buf_sz = 0;
	if (buf_sz)
		pcm_hw->buffer_bytes_max = buf_sz;
}

static int xdrv_cfg_get_stream_type(const char *path, int index,
		int *num_pb, int *num_cap)
{
	int ret;
	char *str = NULL;
	char *stream_path;

	*num_pb = 0;
	*num_cap = 0;
	stream_path = kasprintf(GFP_KERNEL, "%s/%s/%d", path,
		XENSND_PATH_STREAM, index);
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
		ret = EINVAL;
		goto fail;
	}
	ret = 0;
fail:
	kfree(stream_path);
	kfree(str);
	return -ret;
}

static int xdrv_cfg_stream(struct xdrv_info *drv_info,
		struct cfg_pcm_instance *pcm_instance,
		const char *path, int index, int *cur_pb, int *cur_cap,
		int *stream_idx)
{
	int ret;
	char *str = NULL;
	char *stream_path;
	struct cfg_stream *stream;

	stream_path = devm_kasprintf(&drv_info->xb_dev->dev,
		GFP_KERNEL, "%s/%s/%d", path, XENSND_PATH_STREAM, index);
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
	/* assign and publish next unique stream index */
	stream->index = (*stream_idx)++;
	stream->xenstore_path = stream_path;
	ret = xenbus_printf(XBT_NIL, stream->xenstore_path,
		XENSND_FIELD_STREAM_INDEX, "%d", stream->index);
	if (ret < 0)
		goto fail;
	xdrv_cfg_pcm_hw(stream->xenstore_path,
		&pcm_instance->pcm_hw, &stream->pcm_hw);
	ret = 0;
fail:
	kfree(str);
	return -ret;
}

static int xdrv_cfg_device(struct xdrv_info *drv_info,
		struct cfg_pcm_instance *pcm_instance,
		struct snd_pcm_hardware *parent_pcm_hw,
		const char *path, const char *device_node,
		int *stream_idx)
{
	char **stream_nodes;
	char *str;
	char *device_path;
	int ret, i, num_streams;
	int num_pb, num_cap;
	int cur_pb, cur_cap;

	device_path = kasprintf(GFP_KERNEL, "%s/%s", path, device_node);
	if (!device_path) {
		ret = -ENOMEM;
		goto fail;
	}
	str = xenbus_read(XBT_NIL, device_path,
		XENSND_FIELD_DEVICE_NAME, NULL);
	if (!IS_ERR(str)) {
		strncpy(pcm_instance->name, str, sizeof(pcm_instance->name));
		kfree(str);
	}
	if (kstrtoint(device_node, 10, &pcm_instance->device_id)) {
		dev_err(&drv_info->xb_dev->dev,
			"Wrong device id at %s", device_path);
		ret = -EINVAL;
		goto fail;
	}
	/* check if PCM HW configuration exists for this device
	 * and update if so
	 */
	xdrv_cfg_pcm_hw(device_path, parent_pcm_hw, &pcm_instance->pcm_hw);
	/* read streams */
	stream_nodes = xdrv_cfg_get_num_nodes(device_path, XENSND_PATH_STREAM,
		&num_streams);
	kfree(stream_nodes);
	pcm_instance->num_streams_pb = 0;
	pcm_instance->num_streams_cap = 0;
	/* get number of playback and capture streams */
	for (i = 0; i < num_streams; i++) {
		ret = xdrv_cfg_get_stream_type(device_path, i,
			&num_pb, &num_cap);
		if (ret < 0)
			goto fail;
		pcm_instance->num_streams_pb += num_pb;
		pcm_instance->num_streams_cap += num_cap;
	}
	if (pcm_instance->num_streams_pb) {
		pcm_instance->streams_pb = devm_kzalloc(
			&drv_info->xb_dev->dev,
			pcm_instance->num_streams_pb *
			sizeof(struct cfg_stream), GFP_KERNEL);
		if (!pcm_instance->streams_pb) {
			ret = -ENOMEM;
			goto fail;
		}
	}
	if (pcm_instance->num_streams_cap) {
		pcm_instance->streams_cap = devm_kzalloc(
			&drv_info->xb_dev->dev,
			pcm_instance->num_streams_cap *
			sizeof(struct cfg_stream), GFP_KERNEL);
		if (!pcm_instance->streams_cap) {
			ret = -ENOMEM;
			goto fail;
		}
	}
	cur_pb = 0;
	cur_cap = 0;
	for (i = 0; i < num_streams; i++) {
		ret = xdrv_cfg_stream(drv_info,
			pcm_instance, device_path, i, &cur_pb, &cur_cap,
			stream_idx);
		if (ret < 0)
			goto fail;
	}
	ret = 0;
fail:
	kfree(device_path);
	return -ret;
}

static void xdrv_cfg_card_common(const char *path,
		struct cfg_card *card_config)
{
	char *str;

	str = xenbus_read(XBT_NIL, path, XENSND_FIELD_CARD_SHORT_NAME, NULL);
	if (!IS_ERR(str)) {
		strncpy(card_config->shortname, str,
			sizeof(card_config->shortname));
		kfree(str);
	}
	str = xenbus_read(XBT_NIL, path, XENSND_FIELD_CARD_LONG_NAME, NULL);
	if (!IS_ERR(str)) {
		strncpy(card_config->longname, str,
			sizeof(card_config->longname));
		kfree(str);
	}
	xdrv_cfg_pcm_hw(path, &sdrv_pcm_hardware_def,
		&card_config->pcm_hw);
}

static int xdrv_cfg_card(struct xdrv_info *drv_info,
		struct sdev_card_plat_data *plat_data,
		int *stream_idx)
{
	struct xenbus_device *xb_dev = drv_info->xb_dev;
	char *path;
	char **device_nodes = NULL;
	int ret, num_devices, i;

	path = kasprintf(GFP_KERNEL, "%s/" XENSND_PATH_CARD "/%d",
		xb_dev->nodename, plat_data->index);
	if (!path) {
		ret = -ENOMEM;
		goto fail;
	}
	device_nodes = xdrv_cfg_get_num_nodes(path, XENSND_PATH_DEVICE,
		&num_devices);
	if (!num_devices) {
		dev_warn(&xb_dev->dev,
			"No devices configured for sound card %d at %s/%s",
			plat_data->index, path, XENSND_PATH_DEVICE);
		ret = -ENODEV;
		goto fail;
	}
	xdrv_cfg_card_common(path, &plat_data->cfg_card);
	/* read configuration for devices of this card */
	plat_data->cfg_card.pcm_instances = devm_kzalloc(
		&drv_info->xb_dev->dev,
		num_devices * sizeof(struct cfg_pcm_instance),
		GFP_KERNEL);
	if (!plat_data->cfg_card.pcm_instances) {
		ret = -ENOMEM;
		goto fail;
	}
	kfree(path);
	path = kasprintf(GFP_KERNEL,
		"%s/" XENSND_PATH_CARD "/%d/" XENSND_PATH_DEVICE,
		xb_dev->nodename, plat_data->index);
	if (!path) {
		ret = -ENOMEM;
		goto fail;
	}
	for (i = 0; i < num_devices; i++) {
		ret = xdrv_cfg_device(drv_info,
			&plat_data->cfg_card.pcm_instances[i],
			&plat_data->cfg_card.pcm_hw,
			path, device_nodes[i], stream_idx);
		if (ret < 0)
			goto fail;
	}
	plat_data->cfg_card.num_devices = num_devices;
	ret = 0;
fail:
	kfree(device_nodes);
	kfree(path);
	return ret;
}

static void xdrv_remove_internal(struct xdrv_info *drv_info)
{
	sdrv_cleanup(drv_info);
	xdrv_evtchnl_free_all(drv_info);
}

static int xdrv_probe(struct xenbus_device *xb_dev,
				const struct xenbus_device_id *id)
{
	struct xdrv_info *drv_info;
	int ret;

	drv_info = devm_kzalloc(&xb_dev->dev, sizeof(*drv_info), GFP_KERNEL);
	if (!drv_info) {
		ret = -ENOMEM;
		goto fail;
	}

	xenbus_switch_state(xb_dev, XenbusStateInitialising);

	drv_info->xb_dev = xb_dev;
	spin_lock_init(&drv_info->io_lock);
	mutex_init(&drv_info->mutex);
	drv_info->sdrv_registered = false;
	dev_set_drvdata(&xb_dev->dev, drv_info);
	return 0;
fail:
	xenbus_dev_fatal(xb_dev, ret, "allocating device memory");
	return ret;
}

static int xdrv_remove(struct xenbus_device *dev)
{
	struct xdrv_info *drv_info = dev_get_drvdata(&dev->dev);

	mutex_lock(&drv_info->mutex);
	xdrv_remove_internal(drv_info);
	mutex_unlock(&drv_info->mutex);
	xenbus_switch_state(dev, XenbusStateClosed);
	return 0;
}

static int xdrv_resume(struct xenbus_device *dev)
{
	return 0;
}

static grant_ref_t xdrv_sh_buf_get_dir_start(
	struct xdrv_shared_buffer_info *buf)
{
	if (!buf->grefs)
		return GRANT_INVALID_REF;
	return buf->grefs[0];
}

static void xdrv_sh_buf_clear(struct xdrv_shared_buffer_info *buf)
{
	buf->num_grefs = 0;
	buf->grefs = NULL;
	buf->vdirectory = NULL;
	buf->vbuffer = NULL;
	buf->vbuffer_sz = 0;
}

static void xdrv_sh_buf_free(struct xdrv_shared_buffer_info *buf)
{
	int i;

	if (buf->grefs) {
		for (i = 0; i < buf->num_grefs; i++)
			if (buf->grefs[i] != GRANT_INVALID_REF)
				gnttab_end_foreign_access(buf->grefs[i],
						0, 0UL);
		kfree(buf->grefs);
	}
	if (buf->vdirectory)
		vfree(buf->vdirectory);
	if (buf->vbuffer)
		vfree(buf->vbuffer);
	xdrv_sh_buf_clear(buf);
}

void xdrv_sh_buf_fill_page_dir(struct xdrv_shared_buffer_info *buf,
		int num_pages_dir)
{
	struct xensnd_page_directory *page_dir;
	unsigned char *ptr;
	int i, cur_gref, grefs_left, num_grefs_per_page, to_copy;

	ptr = buf->vdirectory;
	grefs_left = buf->num_grefs - num_pages_dir;
	num_grefs_per_page = (PAGE_SIZE - sizeof(
		struct xensnd_page_directory)) / sizeof(grant_ref_t);
	/* skip grefs at start, they are for pages granted for the directory */
	cur_gref = num_pages_dir;
	for (i = 0; i < num_pages_dir; i++) {
		page_dir = (struct xensnd_page_directory *)ptr;
		if (grefs_left <= num_grefs_per_page) {
			to_copy = grefs_left;
			page_dir->num_grefs = to_copy;
			page_dir->gref_dir_next_page = GRANT_INVALID_REF;
		} else {
			to_copy = num_grefs_per_page;
			page_dir->num_grefs = to_copy;
			page_dir->gref_dir_next_page = buf->grefs[i + 1];
		}
		memcpy(&page_dir->gref, &buf->grefs[cur_gref],
			to_copy * sizeof(grant_ref_t));
		ptr += PAGE_SIZE;
		grefs_left -= to_copy;
		cur_gref += to_copy;
	}
}

int xdrv_sh_buf_grant_refs(struct xenbus_device *xb_dev,
	struct xdrv_shared_buffer_info *buf,
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
		if (cur_ref < 0)
			return cur_ref;
		gnttab_grant_foreign_access_ref(cur_ref, otherend_id,
			xen_page_to_gfn(vmalloc_to_page(buf->vdirectory +
				PAGE_SIZE * i)), 0);
		buf->grefs[j++] = cur_ref;
	}
	for (i = 0; i < num_pages_vbuffer; i++) {
		cur_ref = gnttab_claim_grant_reference(&priv_gref_head);
		if (cur_ref < 0)
			return cur_ref;
		gnttab_grant_foreign_access_ref(cur_ref, otherend_id,
			xen_page_to_gfn(vmalloc_to_page(buf->vbuffer +
				PAGE_SIZE * i)), 0);
		buf->grefs[j++] = cur_ref;
	}
	gnttab_free_grant_references(priv_gref_head);
	xdrv_sh_buf_fill_page_dir(buf, num_pages_dir);
	return 0;
}

int xdrv_sh_buf_alloc_buffers(struct xdrv_shared_buffer_info *buf,
		int num_pages_dir, int num_pages_vbuffer,
		int num_grefs)
{
	/* TODO: use XC_PAGE_SIZE */
	buf->grefs = kcalloc(num_grefs, sizeof(*buf->grefs), GFP_KERNEL);
	if (!buf->grefs)
		return -ENOMEM;
	buf->vdirectory = vmalloc(num_pages_dir * PAGE_SIZE);
	if (!buf->vdirectory)
		return -ENOMEM;
	buf->vbuffer_sz = num_pages_vbuffer * PAGE_SIZE;
	buf->vbuffer = vmalloc(buf->vbuffer_sz);
	if (!buf->vbuffer)
		return -ENOMEM;
	return 0;
}

static int xdrv_sh_buf_alloc(struct xenbus_device *xb_dev,
	struct xdrv_shared_buffer_info *buf,
	unsigned int buffer_size)
{
	int num_pages_vbuffer, num_grefs_per_page, num_pages_dir, num_grefs;
	int ret;

	xdrv_sh_buf_clear(buf);
	/* TODO: use XC_PAGE_SIZE */
	num_pages_vbuffer = DIV_ROUND_UP(buffer_size, PAGE_SIZE);
	/* number of grefs a page can hold with respect to the
	 * xensnd_page_directory header
	 */
	num_grefs_per_page = (PAGE_SIZE - sizeof(
		struct xensnd_page_directory)) / sizeof(grant_ref_t);
	/* number of pages the directory itself consumes */
	num_pages_dir = DIV_ROUND_UP(num_pages_vbuffer, num_grefs_per_page);
	num_grefs = num_pages_vbuffer + num_pages_dir;

	ret = xdrv_sh_buf_alloc_buffers(buf, num_pages_dir,
		num_pages_vbuffer, num_grefs);
	if (ret < 0)
		return ret;
	ret = xdrv_sh_buf_grant_refs(xb_dev, buf,
		num_pages_dir, num_pages_vbuffer, num_grefs);
	if (ret < 0)
		return ret;
	xdrv_sh_buf_fill_page_dir(buf, num_pages_dir);
	return 0;
}

static int xdrv_be_on_initwait(struct xdrv_info *drv_info)
{
	struct xenbus_device *xb_dev = drv_info->xb_dev;
	char **card_nodes;
	int stream_idx;
	int i, ret;

	card_nodes = xdrv_cfg_get_num_nodes(xb_dev->nodename,
		XENSND_PATH_CARD, &drv_info->cfg_num_cards);
	kfree(card_nodes);
	if (!drv_info->cfg_num_cards) {
		dev_err(&drv_info->xb_dev->dev, "No sound cards configured");
		return 0;
	}
	drv_info->cfg_plat_data = devm_kzalloc(&drv_info->xb_dev->dev,
		drv_info->cfg_num_cards *
		sizeof(struct sdev_card_plat_data), GFP_KERNEL);
	if (!drv_info->cfg_plat_data)
		return -ENOMEM;
	/* stream index must be unique through all cards: pass it in to be
	 * incremented when creating streams
	 */
	stream_idx = 0;
	for (i = 0; i < drv_info->cfg_num_cards; i++) {
		/* read card configuration from the store and
		 * set platform data structure
		 */
		drv_info->cfg_plat_data[i].index = i;
		drv_info->cfg_plat_data[i].xdrv_info = drv_info;
		ret = xdrv_cfg_card(drv_info,
			&drv_info->cfg_plat_data[i], &stream_idx);
		if (ret < 0)
			return ret;
	}
	/* create event channels for all streams and publish */
	return xdrv_evtchnl_create_all(drv_info, stream_idx);
}

static int xdrv_be_on_connected(struct xdrv_info *drv_info)
{
	return sdrv_init(drv_info);
}

static void xdrv_be_on_disconnected(struct xdrv_info *drv_info)
{
	xdrv_remove_internal(drv_info);
}

static void xdrv_be_on_changed(struct xenbus_device *xb_dev,
	enum xenbus_state backend_state)
{
	struct xdrv_info *drv_info = dev_get_drvdata(&xb_dev->dev);
	int ret;

	dev_dbg(&xb_dev->dev,
		"Backend state is %s, front is %s",
		xenbus_strstate(backend_state),
		xenbus_strstate(xb_dev->state));
	switch (backend_state) {
	case XenbusStateReconfiguring:
		/* fall through */
	case XenbusStateReconfigured:
		/* fall through */
	case XenbusStateInitialised:
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
			xenbus_dev_fatal(xb_dev, ret, "initializing frontend");
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
				"initializing sound driver");
			break;
		}
		xenbus_switch_state(xb_dev, XenbusStateConnected);
		break;

	case XenbusStateUnknown:
		/* fall through */
	case XenbusStateClosed:
		if (xb_dev->state == XenbusStateClosed)
			break;
		if (xb_dev->state == XenbusStateInitialising)
			break;
		/* Missed the backend's CLOSING state -- fallthrough */
	case XenbusStateClosing:
		/* FIXME: is this check needed? */
		if (xb_dev->state == XenbusStateClosing)
			break;
		mutex_lock(&drv_info->mutex);
		xdrv_be_on_disconnected(drv_info);
		mutex_unlock(&drv_info->mutex);
		xenbus_switch_state(xb_dev, XenbusStateInitialising);
		break;
	}
}

static const struct xenbus_device_id xdrv_ids[] = {
	{ XENSND_DRIVER_NAME },
	{ "" }
};

static struct xenbus_driver xen_driver = {
	.ids = xdrv_ids,
	.probe = xdrv_probe,
	.remove = xdrv_remove,
	.resume = xdrv_resume,
	.otherend_changed = xdrv_be_on_changed,
};

static int __init xdrv_init(void)
{
	if (!xen_domain())
		return -ENODEV;
	if (xen_initial_domain()) {
		pr_err(XENSND_DRIVER_NAME " cannot run in Dom0\n");
		return -ENODEV;
	}
	if (!xen_has_pv_devices())
		return -ENODEV;
	pr_info("Registering XEN PV " XENSND_DRIVER_NAME "\n");
	return xenbus_register_frontend(&xen_driver);
}

static void __exit xdrv_cleanup(void)
{
	pr_info("Unregistering XEN PV " XENSND_DRIVER_NAME "\n");
	xenbus_unregister_driver(&xen_driver);
}

module_init(xdrv_init);
module_exit(xdrv_cleanup);

MODULE_DESCRIPTION("Xen virtual sound device frontend");
MODULE_LICENSE("GPL");
MODULE_ALIAS("xen:"XENSND_DRIVER_NAME);
MODULE_SUPPORTED_DEVICE("{{ALSA,Virtual soundcard}}");

