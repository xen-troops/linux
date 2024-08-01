// SPDX-License-Identifier: GPL-2.0
/*
 * Renesas R-Car Gen4/Gen5 WCRC Driver
 *
 * Copyright (C) 2024 Renesas Electronics Inc.
 *
 * Author: huybui2 <huy.bui.wm@renesas.com>
 */

#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/fs.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/of_device.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/types.h>
#include <linux/uaccess.h>
#include <linux/clk.h>
#include <linux/dmaengine.h>
#include <linux/dma-mapping.h>
#include <linux/wait.h>
#include <linux/delay.h>
#include <linux/interrupt.h>

#include "usr_wcrc.h"
#include "crc-wrapper.h"
#include "crc-drv.h"
#include "kcrc-drv.h"

#define DEVNAME "crc-wrapper"
#define CLASS_NAME "wcrc"

/* Define global variable */
DEFINE_MUTEX(lock);

struct wcrc_device {
	// WCRC part
	void __iomem *base[WCRC_DEVICES];
	struct device *dev[WCRC_DEVICES];
	struct clk *clk;
	struct cdev cdev;
	dev_t devt;
	int irq;
	int module;

	// DMA part
	// void __iomem *fifo_base;
	struct resource *fifo_res[WCRC_DEVICES];
	enum dma_data_direction dma_data_dir;
	unsigned int num_desc_tx;
	unsigned int num_desc_rx;
	unsigned int num_desc_rx_in;
	wait_queue_head_t in_wait_wcrc_irq;
	wait_queue_head_t dma_in_wait;
	wait_queue_head_t dma_tx_in_wait;
	bool ongoing;
	bool ongoing_dma;
	bool ongoing_dma_tx;
	bool ongoing_dma_rx_in;
	// struct completion done_dma;
	// TX
	struct scatterlist *sg_tx;
	struct dma_chan *dma_tx;
	enum dma_slave_buswidth tx_bus_width;
	void *buf_tx;
	unsigned int len_tx;
	dma_addr_t tx_dma_addr;
	// RX
	struct scatterlist *sg_rx;
	struct dma_chan *dma_rx;
	enum dma_slave_buswidth rx_bus_width;
	void *buf_rx;
	unsigned int len_rx;
	dma_addr_t rx_dma_addr;
	// RX in
	struct scatterlist *sg_rx_in;
	struct dma_chan *dma_rx_in;
	enum dma_slave_buswidth rx_in_bus_width;
	void *buf_rx_in;
	unsigned int len_rx_in;
	dma_addr_t rx_in_dma_addr;
};

static struct wcrc_device *wcrc;
static bool device_created;
static int dev_chan;
static int dev_unit;
static dev_t wcrc_devt;
static struct class *wcrc_class;

static void rcar_wcrc_dma_tx_callback(void *data);
static void rcar_wcrc_dma_rx_callback(void *data);
static void rcar_wcrc_dma_rx_in_callback(void *data);

static u32 wcrc_read(void __iomem *base, unsigned int offset)
{
	return ioread32(base + offset);
}

static void wcrc_write(void __iomem *base, unsigned int offset, u32 data)
{
	iowrite32(data, base + offset);
}

/* DMA API */
static struct dma_chan *wcrc_request_dma_chan(struct device *dev,
					enum dma_transfer_direction dir,
					dma_addr_t port_addr, char *chan_name,
					enum dma_slave_buswidth bus_width)
{
	struct dma_chan *chan;
	struct dma_slave_config cfg;
	int ret;

	chan = dma_request_chan(dev, chan_name);
	if (IS_ERR(chan)) {
		dev_dbg(dev, "request_channel failed for %s (%ld)\n",
			chan_name, PTR_ERR(chan));
		pr_info("dma_request_chan: %s FAILED\n", chan_name);
		return chan;
	}

	memset(&cfg, 0, sizeof(cfg));
	cfg.direction = dir;
	if (dir == DMA_MEM_TO_DEV) {
		cfg.dst_addr = port_addr;
		cfg.dst_addr_width = bus_width;
	} else {
		cfg.src_addr = port_addr;
		cfg.src_addr_width = bus_width;
	}

	ret = dmaengine_slave_config(chan, &cfg);
	if (ret) {
		dev_dbg(dev, "slave_config failed for %s (%d)\n",
			chan_name, ret);
		dma_release_channel(chan);
		pr_info("dmaengine_slave_config: FAILED\n");
		return ERR_PTR(ret);
	}

	dev_dbg(dev, "got DMA channel for %s\n", chan_name);
	return chan;
}

static struct dma_chan *wcrc_request_dma(struct wcrc_device *priv,
					uint8_t unit,
					enum dma_transfer_direction dir,
					dma_addr_t offs_port_addr, char *chan_name,
					enum dma_slave_buswidth bus_width)
{
	struct device *dev = priv->dev[unit];
	struct dma_chan *chan;

	if (dir == DMA_DEV_TO_MEM) {
		if ((offs_port_addr == PORT_RES(CRCm)) | (offs_port_addr == PORT_RES(KCRCm)))
			priv->rx_bus_width = bus_width;
		if ((offs_port_addr == PORT_DATA(CRCm)) | (offs_port_addr == PORT_DATA(KCRCm)))
			priv->rx_in_bus_width = bus_width;
	}

	if (dir == DMA_MEM_TO_DEV)
		priv->tx_bus_width = bus_width;

	if ((dir == DMA_DEV_TO_MEM) || (dir == DMA_MEM_TO_DEV)) {
		chan = wcrc_request_dma_chan(dev, dir, (priv->fifo_res[unit]->start + offs_port_addr),
					chan_name, bus_width);
		//pr_info(">>>%s: dma_addr=0x%llx\n", chan_name, priv->fifo_res[unit]->start + offs_port_addr);
	} else {
		dev_dbg(dev, "%s: FAILED for dir=%d\n, chan %s", __func__, dir, chan_name);
		return ERR_PTR(-EPROBE_DEFER);
	}

	return chan;
}

static void rcar_wcrc_cleanup_dma(struct wcrc_device *priv)
{
	if (priv->dma_data_dir == DMA_NONE)
		return;
	else if (priv->dma_data_dir == DMA_FROM_DEVICE)
		dmaengine_terminate_async(priv->dma_rx);
	else if (priv->dma_data_dir == DMA_TO_DEVICE)
		dmaengine_terminate_async(priv->dma_tx);
}

static bool rcar_wcrc_dma_tx(struct wcrc_device *priv,
			uint8_t unit, void *data,
			unsigned int len)
{
	struct device *dev = priv->dev[unit];
	struct dma_async_tx_descriptor *txdesc;
	struct dma_chan *chan;
	dma_cookie_t cookie;
	int num_desc;
	enum dma_data_direction data_dir;
	enum dma_transfer_direction trans_dir;
	void *buf;
	int i;

	/* DMA_MEM_TO_DEV */
	priv->buf_tx = data;
	// pr_info("priv->buf_tx points to block mem %zu bytes\n", ksize(priv->buf_tx));
	buf = priv->buf_tx;
	num_desc = len / priv->tx_bus_width;
	priv->num_desc_tx = num_desc;
	priv->dma_data_dir = DMA_TO_DEVICE;
	data_dir = priv->dma_data_dir;
	trans_dir = DMA_MEM_TO_DEV;
	chan = priv->dma_tx;

	priv->sg_tx = kmalloc_array(num_desc, sizeof(struct scatterlist), GFP_KERNEL);
	if (!priv->sg_tx)
		return false;

	sg_init_table(priv->sg_tx, num_desc);

	for (i = 0; i < num_desc; i++) {
		sg_dma_len(&priv->sg_tx[i]) = len/num_desc;
		sg_dma_address(&priv->sg_tx[i]) = dma_map_single(chan->device->dev,
								buf + i*(priv->tx_bus_width),
								len/num_desc, data_dir);
	}

	txdesc = dmaengine_prep_slave_sg(chan, priv->sg_tx, num_desc,
					trans_dir, DMA_PREP_INTERRUPT | DMA_CTRL_ACK);

	txdesc->callback = rcar_wcrc_dma_tx_callback;
	txdesc->callback_param = priv;

	cookie = dmaengine_submit(txdesc);
	if (dma_submit_error(cookie)) {
		dev_dbg(dev, "%s: submit TX dma failed, using PIO\n", __func__);
		rcar_wcrc_cleanup_dma(priv);
		return false;
	}

	dma_async_issue_pending(chan);

	return true;
}

static bool rcar_wcrc_dma_rx(struct wcrc_device *priv,
			uint8_t unit, void *data,
			unsigned int len)
{
	struct device *dev = priv->dev[unit];
	struct dma_async_tx_descriptor *rxdesc;
	struct dma_chan *chan;
	dma_cookie_t cookie;
	int num_desc;
	enum dma_data_direction data_dir;
	enum dma_transfer_direction trans_dir;
	void *buf;
	int i;

	/* DMA_DEV_TO_MEM */
	data = kzalloc(len, GFP_KERNEL);
	priv->buf_rx = data;
	// pr_info("priv->buf_rx points to block mem %zu bytes\n", ksize(priv->buf_rx));
	buf = priv->buf_rx;
	priv->len_rx = len;
	num_desc = len / priv->rx_bus_width;
	priv->num_desc_rx = num_desc;
	data_dir = DMA_FROM_DEVICE;
	trans_dir = DMA_DEV_TO_MEM;
	chan = priv->dma_rx;

	priv->sg_rx = kmalloc_array(num_desc, sizeof(struct scatterlist), GFP_KERNEL);
	if (!priv->sg_rx)
		return false;

	sg_init_table(priv->sg_rx, num_desc);

	for (i = 0; i < num_desc; i++) {
		sg_dma_len(&priv->sg_rx[i]) = len/num_desc;
		sg_dma_address(&priv->sg_rx[i]) = dma_map_single(chan->device->dev,
								buf + i*(priv->rx_bus_width),
								len/num_desc, data_dir);
	}

	rxdesc = dmaengine_prep_slave_sg(chan, priv->sg_rx, num_desc,
					trans_dir, DMA_PREP_INTERRUPT | DMA_CTRL_ACK);


	rxdesc->callback = rcar_wcrc_dma_rx_callback;
	rxdesc->callback_param = priv;

	cookie = dmaengine_submit(rxdesc);
	if (dma_submit_error(cookie)) {
		pr_info("dmaengine_submit_error\n");
		dev_dbg(dev, "%s: submit RX dma failed, using PIO\n", __func__);
		rcar_wcrc_cleanup_dma(priv);
		return false;
	}

	priv->ongoing_dma = 1;

	dma_async_issue_pending(chan);

	return true;

}

static bool rcar_wcrc_dma_rx_in(struct wcrc_device *priv,
				uint8_t unit, void *data,
				unsigned int len)
{
	struct device *dev = priv->dev[unit];
	struct dma_async_tx_descriptor *rxdesc;
	struct dma_chan *chan;
	dma_cookie_t cookie;
	int num_desc;
	enum dma_data_direction data_dir;
	enum dma_transfer_direction trans_dir;
	void *buf;
	int i;

	/* DMA_DEV_TO_MEM */
	data = kzalloc(len, GFP_KERNEL);
	priv->buf_rx_in = data;
	//pr_info("priv->buf_rx_in points to block mem %zu bytes\n", ksize(priv->buf_rx_in));
	buf = priv->buf_rx_in;
	priv->len_rx_in = len;
	//pr_info("priv->len_rx_in = %d\n", priv->len_rx_in);
	num_desc = len / priv->rx_in_bus_width;
	//pr_info("num_desc = %d\n", num_desc);
	priv->num_desc_rx_in = num_desc;
	data_dir = DMA_FROM_DEVICE;
	trans_dir = DMA_DEV_TO_MEM;
	chan = priv->dma_rx_in;

	priv->sg_rx_in = kmalloc_array(num_desc, sizeof(struct scatterlist), GFP_KERNEL);
	if (!priv->sg_rx_in)
		return false;

	sg_init_table(priv->sg_rx_in, num_desc);

	for (i = 0; i < num_desc; i++) {
		sg_dma_len(&priv->sg_rx_in[i]) = len/num_desc;
		sg_dma_address(&priv->sg_rx_in[i]) = dma_map_single(chan->device->dev,
								buf + i*(priv->rx_in_bus_width),
								len/num_desc, data_dir);
	}

	rxdesc = dmaengine_prep_slave_sg(chan, priv->sg_rx_in, num_desc,
					trans_dir, DMA_PREP_INTERRUPT | DMA_CTRL_ACK);


	rxdesc->callback = rcar_wcrc_dma_rx_in_callback;
	rxdesc->callback_param = priv;

	cookie = dmaengine_submit(rxdesc);
	if (dma_submit_error(cookie)) {
		pr_info("dmaengine_submit_error\n");
		dev_dbg(dev, "%s: submit RX dma failed, using PIO\n", __func__);
		rcar_wcrc_cleanup_dma(priv);
		return false;
	}

	priv->ongoing_dma_rx_in = 1;

	dma_async_issue_pending(chan);

	return true;

}

static void rcar_wcrc_dma_tx_callback(void *data)
{
	//struct wcrc_device *priv = data;
	//struct dma_chan *chan = priv->dma_tx;

	//priv->ongoing_dma_tx = 0;
	//wake_up_interruptible(&priv->dma_tx_in_wait);

	// Use when device accesses the buffer through dma_map_single()
	//dma_sync_sg_for_device(chan->device->dev, priv->sg_tx, priv->num_desc_tx, DMA_TO_DEVICE);
	pr_info("======>>>>>>%s: %d\n", __func__, __LINE__);
}

static void rcar_wcrc_dma_rx_callback(void *data)
{
	struct wcrc_device *priv = data;

	priv->ongoing_dma = 0;
	wake_up_interruptible(&priv->dma_in_wait);
	pr_info("<<<<<<======%s: %d\n", __func__, __LINE__);
}

static void rcar_wcrc_dma_rx_in_callback(void *data)
{
	struct wcrc_device *priv = data;

	priv->ongoing_dma_rx_in = 0;
	wake_up_interruptible(&priv->dma_in_wait);
	//pr_info("<<<<<<======%s: %d\n", __func__, __LINE__);
}

static irqreturn_t rcar_wcrc_irq(int irq_num, void *ptr)
{
	struct wcrc_device *priv = ptr;
	uint32_t reg_val;

	reg_val = wcrc_read(priv->base[dev_unit], WCRCm_XXXX_STS(priv->module));
	//Clear trans_done in WCRCm_XXXX_STS.
	if ((TRANS_DONE & reg_val)) {
		wcrc_write(priv->base[dev_unit], WCRCm_XXXX_STS(priv->module), TRANS_DONE);
		//pr_info("<<<<<<======%s: TRANS_DONE %d\n", __func__, __LINE__);
		goto end_irq;
	}

	//Clear res_done in WCRCm_XXXX_STS.
	if ((RES_DONE & reg_val)) {
		wcrc_write(priv->base[dev_unit], WCRCm_XXXX_STS(priv->module), RES_DONE);
		priv->ongoing = 0;
		//pr_info("<<<<<<======%s: RES_DONE %d\n", __func__, __LINE__);
		goto return_irq;
	}

	//Clear cmd_done in WCRCm_XXXX_STS.
	if ((CMD_DONE & reg_val)) {
		wcrc_write(priv->base[dev_unit], WCRCm_XXXX_STS(priv->module), CMD_DONE);
		//pr_info("<<<<<<======%s: CMD_DONE %d\n", __func__, __LINE__);
		goto end_irq;
	}

	//Clear stop_done in WCRCm_XXXX_STS.
	if ((STOP_DONE & reg_val)) {
		wcrc_write(priv->base[dev_unit], WCRCm_XXXX_STS(priv->module), STOP_DONE);
		//pr_info("<<<<<<======%s: STOP_DONE %d\n", __func__, __LINE__);
	}

end_irq:
	priv->ongoing = 0;
	wake_up_interruptible(&priv->in_wait_wcrc_irq);

return_irq:
	return IRQ_HANDLED;
}

static void rcar_wcrc_release_dma_tx(struct wcrc_device *priv)
{
	dma_unmap_sg(priv->dma_tx->device->dev, priv->sg_tx, priv->num_desc_tx, DMA_TO_DEVICE);
	kfree(priv->buf_tx);

	if (!IS_ERR(priv->dma_tx)) {
		dma_release_channel(priv->dma_tx);
		priv->dma_tx = ERR_PTR(-EPROBE_DEFER);
	}
}

static void rcar_wcrc_release_dma_rx(struct wcrc_device *priv)
{
	dma_unmap_sg(priv->dma_rx->device->dev, priv->sg_rx, priv->num_desc_rx, DMA_FROM_DEVICE);
	kfree(priv->buf_rx);

	if (!IS_ERR(priv->dma_rx)) {
		dma_release_channel(priv->dma_rx);
		priv->dma_rx = ERR_PTR(-EPROBE_DEFER);
	}
}

static void rcar_wcrc_release_dma_rx_in(struct wcrc_device *priv)
{
	dma_unmap_sg(priv->dma_rx_in->device->dev, priv->sg_rx_in, priv->num_desc_rx_in, DMA_FROM_DEVICE);
	kfree(priv->buf_rx_in);

	if (!IS_ERR(priv->dma_rx_in)) {
		dma_release_channel(priv->dma_rx_in);
		priv->dma_rx_in = ERR_PTR(-EPROBE_DEFER);
	}
}

static void rcar_wcrc_release_dma(struct wcrc_device *priv)
{
	rcar_wcrc_release_dma_tx(priv);
	rcar_wcrc_release_dma_rx(priv);
}

static int wcrc_independent_crc(struct wcrc_info *info)
{
	int ret;

	mutex_lock(&lock);

	if (info->crc_opt == 0) // INDEPENDENT_CRC mode
		ret = crc_calculate(info);
	else // INDEPENDENT_KCRC mode
		ret = kcrc_calculate(info);

	mutex_unlock(&lock);

	if (ret)
		pr_err("Calculation Aborted!, ERR: %d", ret);

	return 0;
}

static int wcrc_e2e_crc(struct wcrc_info *info, struct wcrc_device *priv,
					void *p_u_data, void *p_drv_data, unsigned int data_len)
{
	int ret = 0;
	int module = 0;
	unsigned int reg_val = 0;
	char *dma_name[2];

	mutex_lock(&lock);

	dev_unit = info->wcrc_unit;

	if (info->crc_opt == 0) {
		module = CRCm;
		dma_name[0] = "crc_tx";
		dma_name[1] = "crc_rx";
	} else if (info->crc_opt == 1) {
		module = KCRCm;
		dma_name[0] = "kcrc_tx";
		dma_name[1] = "kcrc_rx";
	} else {
		ret = -EINVAL;
		goto end_mode;
	}
	priv->module = module;

	//Enable interrupt for the complete of stop operation, command function and input data transfer.
	reg_val = STOP_DONE_IE | RES_DONE_IE | TRANS_DONE_IE;
	wcrc_write(priv->base[dev_unit], WCRCm_XXXX_INTEN(module), reg_val);
	//pr_info("WCRCm_XXXX_INTEN = 0x%x\n", wcrc_read(priv->base[dev_unit], WCRCm_XXXX_INTEN(module)));

	//1. Set CRC conversion size to once in WCRCm_XXXX_CONV register.
	reg_val = info->conv_size;
	wcrc_write(priv->base[dev_unit], WCRCm_XXXX_CONV(module), reg_val);
	//pr_info("WCRCm_XXXX_CONV = 0x%x\n", wcrc_read(priv->base[dev_unit], WCRCm_XXXX_CONV(module)));

	//2. Set initial CRC code value in WCRCm_XXXX_INIT_CRC register.
	reg_val = 0xFFFFFFFF;
	wcrc_write(priv->base[dev_unit], WCRCm_XXXX_INIT_CRC(module), reg_val);
	//pr_info("WCRCm_XXXX_INIT_CRC=0x%x\n", wcrc_read(priv->base[dev_unit], WCRCm_XXXX_INIT_CRC(module)));

	//3. (For CRC) Set DCRAmCTL, DCRAmCTL2, DCRAmCOUT registers.
	//	(For KCRC) Set KCRCmCTL, KCRCmPOLY, KCRCmXOR, KCRCmDOUT registers.
	if (CRCm == module) {
		crc_setting(info);
	} else if (KCRCm == module) {
		kcrc_setting(info);
	} else {
		ret = -EINVAL;
		goto end_mode;
	}

	//4. Set in_en=1, trans_en=1, res_en=1 in WCRCm_XXXX_EN register.
	reg_val = wcrc_read(priv->base[dev_unit], WCRCm_XXXX_EN(module));
	reg_val = IN_EN | TRANS_EN | RES_EN;
	wcrc_write(priv->base[dev_unit], WCRCm_XXXX_EN(module), reg_val);
	//pr_info("WCRCm_XXXX_EN= 0x%x\n", wcrc_read(priv->base[dev_unit], WCRCm_XXXX_EN(module)));

	//5. Set cmd_en=1 in WCRCm_XXXX_CMDEN register
	reg_val = wcrc_read(priv->base[dev_unit], WCRCm_XXXX_CMDEN(module));
	reg_val = CMD_EN;
	wcrc_write(priv->base[dev_unit], WCRCm_XXXX_CMDEN(module), reg_val);
	//pr_info("WCRCm_XXXX_CMDEN= 0x%x\n", wcrc_read(priv->base[dev_unit], WCRCm_XXXX_CMDEN(module)));

	priv->ongoing = 1;

	//6. Transfer input data to data port of FIFO by DMAC.
	priv->dma_tx = wcrc_request_dma(priv, dev_unit,
					DMA_MEM_TO_DEV, PORT_DATA(module),
					dma_name[0], DMA_SLAVE_BUSWIDTH_4_BYTES);
	(void)rcar_wcrc_dma_tx(priv, dev_unit, p_u_data, data_len);

	//7. Read out result data from result port of FIFO by DMAC.
	priv->dma_rx = wcrc_request_dma(priv, dev_unit,
					DMA_DEV_TO_MEM, PORT_RES(module),
					dma_name[1], DMA_SLAVE_BUSWIDTH_4_BYTES);
	(void)rcar_wcrc_dma_rx(priv, dev_unit, p_drv_data, 4);

	//Wait for the complete of DMA rx.
	//priv->ongoing_dma = 1;
	ret = wait_event_interruptible(priv->dma_in_wait, (!(priv->ongoing | priv->ongoing_dma)));
	if (ret < 0) {
		pr_info("%s: wait_event_interruptible FAILED\n", __func__);
		ret = -ERESTARTSYS;
		goto end_mode;
	}

	//8. Set stop=1 in WCRCm_XXXX_STOP by command function.
	reg_val = wcrc_read(priv->base[dev_unit], WCRCm_XXXX_STOP(module));
	reg_val |= STOP;
	wcrc_write(priv->base[dev_unit], WCRCm_XXXX_STOP(module), reg_val);

	//Wait for the complete of stop operation.
	priv->ongoing = 1;
	ret = wait_event_interruptible(priv->in_wait_wcrc_irq, (!priv->ongoing));
	if (ret < 0) {
		ret = -ERESTARTSYS;
		goto end_mode;
	}

	//9. Clear stop_done in WCRCm_XXXX_STS.
	//The func rcar_wcrc_irq() will handle.

	//Give CPU permission to access DMA buffer.
	dma_sync_sg_for_cpu(priv->dma_rx->device->dev, priv->sg_rx,
						priv->num_desc_rx, DMA_FROM_DEVICE);

end_mode:
	mutex_unlock(&lock);

	return ret;
}

static int wcrc_data_through_mode(struct wcrc_info *info, struct wcrc_device *priv,
							void *p_u_data, void *p_drv_data, unsigned int data_len)
{
	int ret = 0;
	int module = 0;
	unsigned int reg_val = 0;
	char *dma_name[2];
	enum dma_slave_buswidth bus_width;

	mutex_lock(&lock);

	dev_unit = info->wcrc_unit;

	//Select which module to run: CRC or KCRC
	if (info->crc_opt == 0) {
		module = CRCm;
		dma_name[0] = "crc_tx";
		dma_name[1] = "crc_rx_in";
	} else if (info->crc_opt == 1) {
		module = KCRCm;
		dma_name[0] = "kcrc_tx";
		dma_name[1] = "kcrc_rx_in";
	} else {
		//pr_info("Not support\n");
		ret = -EINVAL;
		goto end_mode;
	}
	priv->module = module;

	//In HW_UM V4H, sec 135.1.3 / (4) Data through mode:
	//Note: 2. The DMA transfer size (TS[3:0]) of read should be same as one of write.
	// ==> DMA Rx read size = DMA Tx transfer size = bus_width
	bus_width = DMA_SLAVE_BUSWIDTH_4_BYTES;

	//Enable interrupt for the complete of stop operation by WCRCm_CRCm_STOP register.
	reg_val = STOP_DONE_IE;
	wcrc_write(priv->base[dev_unit], WCRCm_XXXX_INTEN(module), reg_val);

	//1. Set in_en=1, out_en=1 in WCRCm_XXXX_EN register.
	reg_val = wcrc_read(priv->base[dev_unit], WCRCm_XXXX_EN(module));
	reg_val = IN_EN | OUT_EN;
	wcrc_write(priv->base[dev_unit], WCRCm_XXXX_EN(module), reg_val);

	//2. Transfer input data to data port of FIFO by DMAC.
	priv->dma_tx = wcrc_request_dma(priv, dev_unit,
					DMA_MEM_TO_DEV, PORT_DATA(module),
					dma_name[0], bus_width);
	(void)rcar_wcrc_dma_tx(priv, dev_unit, p_u_data, data_len);

	//3. Read out input data from data port of FIFO by DMAC.
	priv->dma_rx_in = wcrc_request_dma(priv, dev_unit,
					DMA_DEV_TO_MEM, PORT_DATA(module),
					dma_name[1], bus_width);
	(void)rcar_wcrc_dma_rx_in(priv, dev_unit, p_drv_data, data_len);

	//Wait for the complete of DMA RX.
	//priv->ongoing_dma_rx_in = 1;
	ret = wait_event_interruptible(priv->dma_in_wait, (!priv->ongoing_dma_rx_in));
	if (ret < 0) {
		ret = -ERESTARTSYS;
		goto end_mode;
	}

	//4. Set stop=1 in WCRCm_XXXX_STOP by command function.
	reg_val = wcrc_read(priv->base[dev_unit], WCRCm_XXXX_STOP(module));
	reg_val |= STOP;
	wcrc_write(priv->base[dev_unit], WCRCm_XXXX_STOP(module), reg_val);

	//Wait for the complete of stop operation.
	priv->ongoing = 1;
	ret = wait_event_interruptible(priv->in_wait_wcrc_irq, (!priv->ongoing));
	if (ret < 0) {
		ret = -ERESTARTSYS;
		goto end_mode;
	}

	//5. Clear stop_done in WCRCm_XXXX_STS.
	//The func rcar_wcrc_irq() will handle.

	//Give CPU permission to access DMA buffer.
	dma_sync_sg_for_cpu(priv->dma_rx_in->device->dev, priv->sg_rx_in,
						priv->num_desc_rx_in, DMA_FROM_DEVICE);

end_mode:
	mutex_unlock(&lock);

	return ret;
}



static int wcrc_open(struct inode *inode, struct file *filep)
{
	struct wcrc_info *p_access;

	p_access = kzalloc(sizeof(*p_access), GFP_KERNEL);
	if (!p_access)
		return -ENOMEM;

	pr_debug("Device Open\n");
	filep->private_data = p_access;

	return 0;
}

static int wcrc_release(struct inode *inode, struct file *filep)
{
	struct wcrc_info *p_access;

	p_access = filep->private_data;
	kfree(p_access);
	pr_debug("Device Release\n");

	return 0;
}

static long dev_ioctl(struct file *filep, unsigned int cmd, unsigned long arg)
{
	struct wcrc_info u_features;
	struct wcrc_device *priv;
	int ret;
	void *u_data;
	void *drv_data[2];
	unsigned int u_data_len;

	priv = wcrc;
	ret = 0;
	u_data_len = 0;
	u_data = NULL;
	drv_data[0] = NULL;
	drv_data[1] = NULL;

	switch (cmd) {
	case INDEPENDENT_CRC_MODE:
		ret = copy_from_user(&u_features, (struct wcrc_info *)arg, sizeof(u_features));
		if (ret) {
			pr_err("INDEPENDENT_CRC_MODE: Error taking data from user\n");
			ret = -EFAULT;
			goto exit_func;
		}

		wcrc_independent_crc(&u_features);

		ret = copy_to_user((struct wcrc_info *)arg, &u_features, sizeof(u_features));
		if (ret) {
			pr_err("INDEPENDENT_CRC_MODE: Error sending data to user\n");
			ret = -EFAULT;
			goto exit_func;
		}

		break;

	case E2E_CRC_MODE:
		//Read setting from user
		ret = copy_from_user(&u_features, (struct wcrc_info *)arg, sizeof(u_features));
		if (ret) {
			//pr_err("E2E_CRC_MODE: Error taking data from user\n");
			ret = -EFAULT;
			goto exit_func;
		}

		// In sec135.1.3/(8) FIFO, DATA PORT FIFO can only store 64 data (each takes 4 bytes).
		// => Max byte: 64*4 = 256, Min byte: 1*4 = 4.
		// User should send max 256 bytes/packet.
		u_data_len = u_features.data_input_len;
		if ((u_data_len >= 4) && (u_data_len <= 256)) {
			u_data = kzalloc(u_data_len, GFP_KERNEL);
			if (u_data != NULL) {
				ret = copy_from_user(u_data, u_features.pdata_input, u_data_len);
				if (ret) {
					//pr_err("E2E_CRC_MODE: Error taking data from user\n");
					ret = -EFAULT;
					goto exit_func;
				}
			} else {
				//pr_err("E2E_CRC_MODE: Allocate memory for u_data FAILED\n");
				ret = -ENOMEM;
				goto exit_func;
			}

			//Setting and run e2e crc mode
			ret = wcrc_e2e_crc(&u_features, priv, u_data, drv_data, u_data_len);
			if (ret) {
				//pr_err("E2E_CRC_MODE: setting FAILED\n");
				//ret = -ESRCH;
				goto exit_func;
			}

			//Return data to user
			ret = copy_to_user(u_features.pdata_output, priv->buf_rx, 4);
			if (ret) {
				pr_err("E2E_CRC_MODE: Error sending data to user\n");
				ret = -EFAULT;
				goto exit_func;
			}

			//Release DMA
			if (priv != NULL)
				rcar_wcrc_release_dma(priv);

		} else {
			//pr_err("E2E_CRC_MODE: Error data_input_len out of range 4 to 256 (bytes)\n");
			ret = -EINVAL;
			goto exit_func;
		}

		break;

	case DATA_THROUGH_MODE:
		//Read setting from user
		ret = copy_from_user(&u_features, (struct wcrc_info *)arg, sizeof(u_features));
		if (ret) {
			//pr_err("DATA_THROUGH_MODE: Error taking data from user\n");
			ret = -EFAULT;
			goto exit_func;
		}

		// In sec135.1.3/(8) FIFO, DATA PORT FIFO can only store 64 data (each takes 4 bytes).
		// => Max byte: 64*4 = 256, Min byte: 1*4 = 4.
		// User should send max 256 bytes/packet.
		u_data_len = u_features.data_input_len;
		if ((u_data_len >= 4) && (u_data_len <= 256)) {
			u_data = kzalloc(u_data_len, GFP_KERNEL);
			if (u_data != NULL) {
				ret = copy_from_user(u_data, u_features.pdata_input, u_data_len);
				if (ret) {
					//pr_err("DATA_THROUGH_MODE: Error taking data from user\n");
					ret = -EFAULT;
					goto exit_func;
				}
			} else {
				//pr_err("DATA_THROUGH_MODE: Allocate memory for u_data FAILED\n");
				ret = -ENOMEM;
				goto exit_func;
			}

			//Setting and run data through mode
			ret = wcrc_data_through_mode(&u_features, priv, u_data, drv_data, u_data_len);
			if (ret) {
				//pr_err("DATA_THROUGH_MODE: setting FAILED\n");
				//ret = -ESRCH;
				goto exit_func;
			}

			//Return data to user
			ret = copy_to_user(u_features.pdata_output, priv->buf_rx_in, u_data_len);
			if (ret) {
				//pr_err("DATA_THROUGH_MODE: Error sending data to user\n");
				ret = -EFAULT;
				goto exit_func;
			}

			//Release DMA
			if (priv != NULL) {
				rcar_wcrc_release_dma_tx(priv);
				rcar_wcrc_release_dma_rx_in(priv);
			}

		} else {
			//pr_err("DATA_THROUGH_MODE: Error data_input_len out of range 4 to 256 (bytes)\n");
			ret = -EINVAL;
			goto exit_func;
		}

		break;

	default:
		ret = -EINVAL;
	}

exit_func:
	return ret;
}

static const struct of_device_id wcrc_of_ids[] = {
	{
		.compatible = "renesas,crc-wrapper",
	}, {
		.compatible = "renesas,wcrc-r8a78000",
	}, {
		.compatible = "renesas,rcar-gen5-wcrc",
	}, {
		/* Terminator */
	},
};

static const struct file_operations fops = {
	.owner = THIS_MODULE,
	.open = wcrc_open,
	.release = wcrc_release,
	.unlocked_ioctl = dev_ioctl,
};

static int wcrc_probe(struct platform_device *pdev)
{
	struct device *dev;
	struct resource *res;
	int ret;
	unsigned long irqflags = 0;
	irqreturn_t (*irqhandler)(int irq_num, void *ptr) = rcar_wcrc_irq;

	if (dev_chan == 0) {
		wcrc = devm_kzalloc(&pdev->dev, sizeof(*wcrc), GFP_KERNEL);
		if (!wcrc)
			return -ENOMEM;
	}

	wcrc->dev[dev_chan] = &pdev->dev;

	/* Map I/O memory */
	//res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "wcrc");
	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	pr_info("Instance %d: wcrc_res=0x%llx", dev_chan, res->start);
	wcrc->base[dev_chan] = devm_ioremap_resource(&pdev->dev, res);
	if (IS_ERR(wcrc->base[dev_chan])) {
		dev_err(&pdev->dev, "Unable to map I/O for device\n");
		return PTR_ERR(wcrc->base[dev_chan]);
	}

	//wcrc->fifo_res[dev_chan] = platform_get_resource_byname(pdev, IORESOURCE_MEM, "fifo");
	wcrc->fifo_res[dev_chan] = platform_get_resource(pdev, IORESOURCE_MEM, 1);
	pr_info("Instance %d: fifo_res=0x%llx", dev_chan, wcrc->fifo_res[dev_chan]->start);

	if (dev_chan == 0) {
		/* Init DMA */
		wcrc->dma_rx = wcrc->dma_tx = ERR_PTR(-EPROBE_DEFER);
		wcrc->buf_tx = wcrc->buf_rx = ERR_PTR(-EPROBE_DEFER);
		init_waitqueue_head(&wcrc->in_wait_wcrc_irq);
		init_waitqueue_head(&wcrc->dma_in_wait);
		init_waitqueue_head(&wcrc->dma_tx_in_wait);
		wcrc->ongoing = 0;
		wcrc->ongoing_dma = 0;
		wcrc->ongoing_dma_tx = 0;
		wcrc->ongoing_dma_rx_in = 0;
		//init_completion(&wcrc->done_dma);
	}

	if (!device_created) {
		/* Creating WCRC device */
		wcrc->devt = MKDEV(MAJOR(wcrc_devt), 0);
		cdev_init(&wcrc->cdev, &fops);
		wcrc->cdev.owner = THIS_MODULE;
		ret = cdev_add(&wcrc->cdev, wcrc->devt, 1);
		if (ret < 0) {
			dev_err(&pdev->dev, "Unable to add char device\n");
			return ret;
		}

		dev = device_create(wcrc_class,
				NULL,
				wcrc->devt,
				NULL,
				DEVNAME);
		if (IS_ERR(dev)) {
			dev_err(&pdev->dev, "Unable to create device\n");
			cdev_del(&wcrc->cdev);
			return PTR_ERR(dev);
		}
		device_created = 1;
	}

	ret = platform_get_irq(pdev, 0);
	if (ret < 0)
		return ret;
	wcrc->irq = ret;
	ret = devm_request_irq(wcrc->dev[dev_chan], wcrc->irq, irqhandler, irqflags, DEVNAME, wcrc);
	if (ret < 0) {
		dev_err(&pdev->dev, "cannot get irq %d\n", wcrc->irq);
		return ret;
	}

	dev_chan++;

	platform_set_drvdata(pdev, wcrc);

	return 0;
}

static int wcrc_remove(struct platform_device *pdev)
{
	struct wcrc_device *priv = platform_get_drvdata(pdev);

	rcar_wcrc_release_dma(priv);
	device_destroy(wcrc_class, MKDEV(MAJOR(wcrc_devt), 0));
	cdev_del(&priv->cdev);

	return 0;
}

static struct platform_driver wcrc_driver = {
	.driver = {
		.name = DEVNAME,
		.of_match_table = of_match_ptr(wcrc_of_ids),
		.owner = THIS_MODULE,
	},
	.probe = wcrc_probe,
	.remove = wcrc_remove,
};

static int __init wcrc_init(void)
{
	struct device_node *np;
	int ret;

	np = of_find_matching_node(NULL, wcrc_of_ids);
	if (!np)
		return 0;

	of_node_put(np);

	ret = alloc_chrdev_region(&wcrc_devt, 0, WCRC_DEVICES, DEVNAME);
	if (ret) {
		pr_err("wcrc: Failed to register device\n");
		return ret;
	}

	wcrc_class = class_create(THIS_MODULE, CLASS_NAME);
	if (IS_ERR(wcrc_class)) {
		pr_err("wcrc: Failed to create class\n");
		ret = (PTR_ERR(wcrc_class));
		goto class_err;
	}

	ret = platform_driver_register(&wcrc_driver);
	if (ret < 0)
		goto drv_reg_err;

	return 0;

drv_reg_err:
	class_destroy(wcrc_class);

class_err:
	unregister_chrdev_region(wcrc_devt, WCRC_DEVICES);

	return ret;
}

static void __exit wcrc_exit(void)
{
	unregister_chrdev_region(wcrc_devt, WCRC_DEVICES);
	class_destroy(wcrc_class);
	platform_driver_unregister(&wcrc_driver);
}

module_init(wcrc_init);
module_exit(wcrc_exit);

MODULE_LICENSE("GPL v2");
MODULE_AUTHOR("Renesas Electronics Corporation");
MODULE_DESCRIPTION("R-Car Cyclic Redundancy Check Wrapper");

