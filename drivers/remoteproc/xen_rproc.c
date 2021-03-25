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

#define RPMSG_SMC_GET_VDEV_INFO  ARM_SMCCC_CALL_VAL(ARM_SMCCC_FAST_CALL, \
                                                    ARM_SMCCC_SMC_32,  \
                                                    ARM_SMCCC_OWNER_SIP, \
                                                    0x200)
#define RPMSG_SMC_GET_VRING_INFO  ARM_SMCCC_CALL_VAL(ARM_SMCCC_FAST_CALL, \
                                                     ARM_SMCCC_SMC_32, \
                                                     ARM_SMCCC_OWNER_SIP, \
                                                     0x201)
#define RPMSG_SMC_SET_VRING_DATA  ARM_SMCCC_CALL_VAL(ARM_SMCCC_FAST_CALL, \
                                                     ARM_SMCCC_SMC_32, \
                                                     ARM_SMCCC_OWNER_SIP, \
                                                     0x202)

/* We define minimal table with one resource: virtqueue device, which has 2 rings */
struct xen_rproc_rtable {
	struct resource_table tbl_header;
	u32 offset;
	struct fw_rsc_hdr r_hdr;
	struct fw_rsc_vdev vdev;
	struct fw_rsc_vdev_vring vrings[2];
};

/* Fill the table */
/* TODO: Make table part of struct xen_rproc_data */
static struct xen_rproc_rtable xen_rtable = {
	.tbl_header.ver = 1,
	.tbl_header.num = 1,
	.offset = offsetof(struct xen_rproc_rtable, r_hdr),
	.r_hdr.type = RSC_VDEV,
	.vdev.num_of_vrings = 2,
};

struct xen_rproc_data {
	struct rproc *rproc;
	struct work_struct workqueue;
	struct xen_rproc_rtable *rtable;
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
	struct xen_rproc_data *data = rproc->priv;
	struct arm_smccc_res res;

	pr_info("boot: vring: %x %d", data->rtable->vrings[0].da,
		data->rtable->vrings[0].notifyid);
	pr_info("boot: vring: %x %d", data->rtable->vrings[1].da,
		data->rtable->vrings[1].notifyid);
	/* Set ring 1 first, because ring 2 will unlock remote end */
	arm_smccc_1_1_hvc(RPMSG_SMC_SET_VRING_DATA, 1, data->rtable->vrings[1].da,
			  data->rtable->vrings[1].notifyid, &res);
	arm_smccc_1_1_hvc(RPMSG_SMC_SET_VRING_DATA, 0, data->rtable->vrings[0].da,
			  data->rtable->vrings[0].notifyid, &res);
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

struct resource_table *xen_rproc_find_rsc_table(struct rproc *rproc,
						const struct firmware *fw,
						int *tablesz)
{
	*tablesz = sizeof(xen_rtable);
	return (void*)&xen_rtable;
}

static struct resource_table *
xen_rproc_find_loaded_rsc_table(struct rproc *rproc, const struct firmware *fw)
{
	struct xen_rproc_data *data = rproc->priv;

	return (void*)data->rtable;
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
	struct arm_smccc_res res;
	int ret;
	int i;

	rproc = rproc_alloc(dev, np->name, &xen_rproc_ops, NULL,
			    sizeof(struct xen_rproc_data));
	if (!rproc)
		return -ENOMEM;

	data = rproc->priv;

	/* Fill resource table */
	arm_smccc_1_1_hvc(RPMSG_SMC_GET_VDEV_INFO, &res);
	if (res.a0 != 0) {
		ret = -ENODEV;
		goto free_rproc;
	}

	xen_rtable.vdev.id = res.a1;
	xen_rtable.vdev.dfeatures = res.a2;

	for (i = 0; i < 2; i ++) {
		arm_smccc_1_1_hvc(RPMSG_SMC_GET_VRING_INFO, i, &res);
		if (res.a0 != 0) {
			ret = -ENODEV;
			goto free_rproc;
		}
		xen_rtable.vrings[i].align = res.a1;
		xen_rtable.vrings[i].num = res.a2;
		xen_rtable.vrings[i].notifyid = res.a3;
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

	data->rtable = kmemdup(&xen_rtable, sizeof(struct xen_rproc_rtable), GFP_KERNEL);
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
