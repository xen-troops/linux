/*
 * Remote processor machine-specific module for R-Car Gen4 - Cortex-R52
 *
 * Copyright (C) 2019 Renesas Electronics
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * version 2 as published by the Free Software Foundation.
 */

#include <linux/bitops.h>
#include <linux/clk.h>
#include <linux/err.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/notifier.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/of_address.h>
#include <linux/of_device.h>
#include <linux/platform_device.h>
#include <linux/remoteproc.h>
#include <linux/delay.h>

#include "remoteproc_internal.h"

#include <misc/rcar-mfis/rcar_mfis_public.h>
#define MFIS_CHANNEL 0 //use this mfis channel to trigger interrupts

static char *rcar_cr52_fw_name;
module_param(rcar_cr52_fw_name, charp, S_IRUGO);
MODULE_PARM_DESC(rcar_cr52_fw_name,
		 "Name of CR52 firmware file in /lib/firmware (if not specified defaults to 'rproc-cr52-fw')");

#define RST_BASE                0xE6160000
#define RST_CR52BAR_OFFSET       0x00000070

#define SYSC_BASE               0xE6180000
#define SYSC_PWRSR7_OFFSET      0x00000240
#define SYSC_PWRONCR52_OFFSET    0x0000024C

#define APMU_CR52PSTR            0XE6153040

#define CPG_BASE                0xE6150000
#define CPG_WPCR_OFFSET         0x00000904
#define CPG_WPR_OFFSET          0x00000900

#define MSSR_BASE               0xE6150000 //same as CPG
#define MSSR_SRCR2_OFFSET       0x000000B0
#define MSSR_SRSTCLR2_OFFSET    0x00000948

#define CR52_BASE                0xF0100000
#define CR52_WBPWRCTLR_OFFSET    0x00000F80
#define CR52_WBCTLR_OFFSET       0x00000000

#define CR52_RST_ADDRESS         0x7FF00000
#define CR52_RST_SIZE            1024

/**
 * struct rcar_cr52_rproc - rcar_cr52 remote processor instance state
 * @rproc: rproc handle
 * @workqueue: work queue list
 * @cr52_already_running: indicate Cortex-R7 core is already running or not
 * @mem_va: virtual memory address
 * @mem_da: device memory address
 * @mem_len: length of internal memory regions data
 */
struct rcar_cr52_rproc {
	struct rproc *rproc;
	struct work_struct workqueue;
	void __iomem *mem_va;
	phys_addr_t mem_da;
	u64 mem_len;
};

/**
 * handle_event() - inbound virtqueue message workqueue function
 * @work: work queue list
 *
 * This callback is registered with the R-Car MFIS atomic notifier
 * chain and is called every time the remote processor (Cortex-R52)
 * wants to notify us of pending messages available.
 */
static void handle_event(struct work_struct *work)
{
        struct rcar_cr52_rproc *rrproc =
                container_of(work, struct rcar_cr52_rproc, workqueue);

	/* Process incoming buffers on all our vrings */
        rproc_vq_interrupt(rrproc->rproc, 0);
        rproc_vq_interrupt(rrproc->rproc, 1);
}

/**
 * cr52_interrupt_cb()
 * @self: R-Car Cortex CR52 notifer block
 * @action: type of interrupt request
 * @data: message data
 *
 * This callback is registered with the R-Car MFIS atomic notifier
 * chain and is called every time the remote processor (Cortex-R7)
 * wants to notify us of pending messages available.
 */
static int cr52_interrupt_cb(struct notifier_block *self, unsigned long action, void *data)
{
	struct rcar_cr52_rproc *rrproc = (struct rcar_cr52_rproc *)data;
	struct device *dev = rrproc->rproc->dev.parent;

	dev_dbg(dev, "%s\n", __FUNCTION__);

	schedule_work(&rrproc->workqueue);

	return NOTIFY_DONE;
}

static struct notifier_block rcar_cr52_notifier_block = {
	.notifier_call = cr52_interrupt_cb,
};

static int rcar_cr52_rproc_start(struct rproc *rproc)
{
	return 0;
}

static int rcar_cr52_rproc_stop(struct rproc *rproc)
{
	return 0;
}

/* kick a virtqueue */
static void rcar_cr52_rproc_kick(struct rproc *rproc, int vqid)
{
	int ret;
	struct device *dev = rproc->dev.parent;
	struct rcar_mfis_msg msg;
	// struct rcar_cr52_rproc *rrproc = (struct rcar_cr52_rproc *)rproc->priv;
	unsigned int n_tries = 3;

	dev_dbg(dev, "%s\n", __FUNCTION__);

	msg.icr = vqid;
	msg.mbr = 0;

	do {
	    ret = rcar_mfis_trigger_interrupt(MFIS_CHANNEL, msg);
	    if (ret)
		udelay(500);

	} while (ret && n_tries--);

	if (ret) {
		dev_dbg(dev, "%s failed\n", __FUNCTION__);
	}
}

static void *rcar_cr52_da_to_va(struct rproc *rproc, u64 da, size_t len)
{
	struct rcar_cr52_rproc *rrproc = rproc->priv;
	int offset;

	offset = da - rrproc->mem_da;
	if (offset < 0 || offset + len > rrproc->mem_len)
		return NULL;

	return (void __force *)rrproc->mem_va + offset;
}

static int rcar_cr52_rproc_elf_load_segments(struct rproc *rproc,
					    const struct firmware *fw)
{
	return 0;
}

static int rcar_cr52_rproc_parse_fw(struct rproc *rproc, const struct firmware *fw)
{
	struct resource_table *table = ioremap(CR52_RST_ADDRESS, CR52_RST_SIZE);
	size_t tablesz = CR52_RST_SIZE;
	rproc->cached_table = kmemdup(table, tablesz, GFP_KERNEL);
	rproc->table_ptr = rproc->cached_table;
	rproc->table_sz = tablesz;
	return 0;
}

static struct resource_table *
rcar_cr52_rproc_elf_find_loaded_rsc_table(struct rproc *rproc,
				         const struct firmware *fw)
{
	// Use fixed address
	u64 sh_addr, sh_size;
	sh_addr = CR52_RST_ADDRESS;
	sh_size = CR52_RST_SIZE;
	return rproc_da_to_va(rproc, sh_addr, sh_size);
}

static const struct rproc_ops rcar_cr52_rproc_ops = {
	.start = rcar_cr52_rproc_start,
	.stop = rcar_cr52_rproc_stop,
	.kick = rcar_cr52_rproc_kick,
	.da_to_va = rcar_cr52_da_to_va,
	.load = rcar_cr52_rproc_elf_load_segments,
	.parse_fw = rcar_cr52_rproc_parse_fw,
	.find_loaded_rsc_table = rcar_cr52_rproc_elf_find_loaded_rsc_table,
};

static int rcar_cr52_rproc_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct rcar_cr52_rproc *rrproc;
	struct device_node *np = dev->of_node;
	struct device_node *node;
	struct resource res;
	struct rproc *rproc;
	int ret;

	rproc = rproc_alloc(dev, "cr52", &rcar_cr52_rproc_ops, rcar_cr52_fw_name, sizeof(*rrproc));
	if (!rproc) {
		return -ENOMEM;
	}

	rrproc = rproc->priv;
	rrproc->rproc = rproc;
	rproc->has_iommu = false;

	node = of_parse_phandle(np, "memory-region", 0);
	if (!node) {
		dev_err(dev, "no memory-region specified\n");
		ret = -EINVAL;
		goto free_rproc;
	}

	ret = of_address_to_resource(node, 0, &res);
	if (ret) {
		dev_err(dev, "unable to resolve memory region\n");
		goto free_rproc;
	}

	rrproc->mem_da = res.start;
	rrproc->mem_len = resource_size(&res);
	rrproc->mem_va = devm_ioremap_wc(dev, rrproc->mem_da, rrproc->mem_len);
	if (IS_ERR(rrproc->mem_va)) {
		dev_err(dev, "unable to map memory region: %pa+%llx\n",
			&res.start, rrproc->mem_len);
		ret = PTR_ERR(rrproc->mem_va);
		goto free_rproc;
	}

	INIT_WORK(&rrproc->workqueue, handle_event);

	platform_set_drvdata(pdev, rrproc);

	ret = rcar_mfis_register_notifier(MFIS_CHANNEL, &rcar_cr52_notifier_block, rrproc);
	if (ret) {
		dev_err(dev, "cannot register notifier on mfis channel %d\n", MFIS_CHANNEL);
		goto free_rproc;
	}

	rproc->skip_fw_load = 1;
        rproc->auto_boot =0;

	ret = rproc_add(rproc);
	if (ret) {
		dev_err(dev, "rproc_add failed: %d\n", ret);
		goto unregister_notifier;
	}

	return 0;

unregister_notifier:
	rcar_mfis_unregister_notifier(MFIS_CHANNEL, &rcar_cr52_notifier_block);
	flush_work(&rrproc->workqueue);
free_rproc:
	rproc_free(rproc);
	return ret;
}

static int rcar_cr52_rproc_remove(struct platform_device *pdev)
{
	struct rcar_cr52_rproc *rrproc = platform_get_drvdata(pdev);
	struct rproc *rproc = rrproc->rproc;

	rcar_mfis_unregister_notifier(MFIS_CHANNEL, &rcar_cr52_notifier_block);
	flush_work(&rrproc->workqueue);
	rproc_del(rproc);
	rproc_free(rproc);

	return 0;
}

static const struct of_device_id rcar_cr52_rproc_of_match[] = {
	{ .compatible = "renesas,rcar-cr52", },
	{ },
};
MODULE_DEVICE_TABLE(of, rcar_cr52_rproc_of_match);

static struct platform_driver rcar_cr52_rproc_driver = {
	.probe = rcar_cr52_rproc_probe,
	.remove = rcar_cr52_rproc_remove,
	.driver = {
		.name = "rcar-cr52-rproc",
		.of_match_table = of_match_ptr(rcar_cr52_rproc_of_match),
	},
};

module_platform_driver(rcar_cr52_rproc_driver);

MODULE_LICENSE("GPL v2");
MODULE_DESCRIPTION("RCAR_CR52 Remote Processor control driver");
