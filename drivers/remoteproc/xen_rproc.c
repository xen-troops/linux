/*
 * XEN Remoteproc paravirtual driver
 *
 * Copyright (C) 2021 EPAM Systems - All Rights Reserved
 *
 * Author: Volodymyr Babchuk <volodymyr_babchuk@epam.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <linux/arm-smccc.h>
#include <linux/interrupt.h>
#include <linux/err.h>
#include <linux/firmware.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/of_reserved_mem.h>
#include <linux/platform_device.h>
#include <linux/remoteproc.h>
#include <linux/delay.h>
#include "remoteproc_internal.h"

#define MFIS_SMC_TRIG  ARM_SMCCC_CALL_VAL(ARM_SMCCC_FAST_CALL,         \
                                          ARM_SMCCC_SMC_32,	       \
                                          ARM_SMCCC_OWNER_SIP,         \
                                          0x100)
#define MFIS_SMC_ERR_BUSY               0x01
#define MFIS_SMC_ERR_NOT_AVAILABLE      0x02

struct xen_rproc_data {
	void __iomem *rtable;
	struct rproc *rproc;
	struct work_struct workqueue;
};

static void handle_event(struct work_struct *work)
{
        struct xen_rproc_data *data =
                container_of(work, struct xen_rproc_data, workqueue);

	/* Process incoming buffers on all our vrings */
        rproc_vq_interrupt(data->rproc, 0);
        rproc_vq_interrupt(data->rproc, 1);
}

static irqreturn_t xen_rproc_irq_handler(int irq, void *arg)
{
	struct xen_rproc_data *data = (struct xen_rproc_data *)arg;

	schedule_work(&data->workqueue);

	return IRQ_HANDLED;
}

static void xen_rproc_kick(struct rproc *rproc, int vqid)
{
	unsigned int n_tries = 3;
	struct arm_smccc_res res;
	struct device *dev = rproc->dev.parent;

	do {
		arm_smccc_1_1_hvc(MFIS_SMC_TRIG, &res);
		if (res.a0 == MFIS_SMC_ERR_BUSY)
			udelay(500);

	} while (res.a0 != SMCCC_RET_SUCCESS && n_tries--);

	if (res.a0 != SMCCC_RET_SUCCESS)
		dev_dbg(dev, "%s failed: %lx\n", __FUNCTION__, res.a0);
}

static int xen_rproc_start(struct rproc *rproc)
{
	/* Xen Will boot it for us */

	return 0;
}

static int xen_rproc_stop(struct rproc *rproc)
{
	/* We can't stop it */

	return 0;
}

static const struct rproc_ops xen_rproc_ops = {
	.kick		= xen_rproc_kick,
	.start		= xen_rproc_start,
	.stop		= xen_rproc_stop,
};

static const uint8_t xen_rtable[]  __aligned(__alignof__(uint64_t)) = {
	0x01, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x50, 0x00, 0x00, 0x00, 0x88, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x70,
	0x00, 0x00, 0x00, 0x70, 0x00, 0x00, 0x00, 0x07,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x63, 0x61, 0x72, 0x76, 0x65, 0x6f, 0x75, 0x74,
	0x31, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x03, 0x00, 0x00, 0x00, 0x07, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x10, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00,
	0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x00,
	0x00, 0x02, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00,
};

struct resource_table *xen_rproc_find_rsc_table(struct rproc *rproc,
						const struct firmware *fw,
						int *tablesz)
{
	*tablesz = sizeof(xen_rtable);
	return (void*)xen_rtable;
}

static struct resource_table *
xen_rproc_find_loaded_rsc_table(struct rproc *rproc, const struct firmware *fw)
{
	struct xen_rproc_data *data = rproc->priv;

	return data->rtable;
}

static int xen_rproc_load_fw(struct rproc *rproc, const struct firmware *fw)
{
	/* It is already loaded by some other means */

	return 0;
}

static const struct rproc_fw_ops xen_rproc_fw_ops = {
	.load = xen_rproc_load_fw,
	.find_rsc_table = xen_rproc_find_rsc_table,
	.find_loaded_rsc_table = xen_rproc_find_loaded_rsc_table,
};

static int xen_rproc_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct device_node *np = dev->of_node;
	struct rproc *rproc;
	struct resource *resource;
	struct xen_rproc_data *data;
	int ret;

	resource = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!resource) {
		dev_err(dev, "Can't get iomem resource\n");
		return -EINVAL;
	}

	rproc = rproc_alloc(dev, np->name, &xen_rproc_ops, NULL,
			    sizeof(struct xen_rproc_data));
	if (!rproc)
		return -ENOMEM;

	data = rproc->priv;
	data->rtable = devm_ioremap_resource(dev, resource);
	if (IS_ERR(data->rtable)) {
		ret = PTR_ERR(data->rtable);
		dev_err(dev, "devm_ioremap_resource failed: %d\n", ret);
		goto free_rproc;
	}

	/* Get IRQ resource */
	resource = platform_get_resource(pdev, IORESOURCE_IRQ, 0);
	if (!resource) {
		dev_err(dev, "Missing IRQ entry\n");
		ret = -EINVAL;
		goto free_rproc;
	}

	ret = devm_request_irq(dev, resource->start, xen_rproc_irq_handler,
			       IRQF_SHARED, dev_name(dev), data);
	if (ret < 0) {
		dev_err(dev, "Failed to request IRQ\n");
		goto free_rproc;
	}

	data->rproc = rproc;

	INIT_WORK(&data->workqueue, handle_event);

	rproc->has_iommu = false;
	rproc->fw_ops = &xen_rproc_fw_ops;
	rproc->auto_boot = false;

	platform_set_drvdata(pdev, rproc);

	ret = rproc_add(rproc);
	if (ret)
		goto free_rproc;

	rproc_fw_boot(rproc, NULL);

	return 0;

free_rproc:
	rproc_free(rproc);
	return ret;
}

static int xen_rproc_remove(struct platform_device *pdev)
{
	struct rproc *rproc = platform_get_drvdata(pdev);

	rproc_del(rproc);

	rproc_free(rproc);

	return 0;
}

static const struct of_device_id xen_rproc_match[] = {
	{ .compatible = "xen-rproc", .data = NULL },
	{},
};
MODULE_DEVICE_TABLE(of, xen_rproc_match);

static struct platform_driver xen_rproc_driver = {
	.probe = xen_rproc_probe,
	.remove = xen_rproc_remove,
	.driver = {
		.name = "xen-rproc",
		.of_match_table = of_match_ptr(xen_rproc_match),
	},
};
module_platform_driver(xen_rproc_driver);

MODULE_DESCRIPTION("XEN Remoteproc paravirtual driver");
MODULE_AUTHOR("Volodymyr Babchuk <volodymyr_babchuk@epam.com>");
MODULE_LICENSE("GPL v2");
