// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2014-2018 Xilinx, Inc.
 *
 * Davorin Mista <davorin.mista@aggios.com>
 * Joshua Kuhlmann <joshua.kuhlmann@aggios.com>
 */

#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/suspend.h>

#define DRIVER_NAME	"coord_suspend"

/**
 * struct coord_suspend_struct - Wrapper for struct work_struct
 * @callback_work:	Work structure
 */
struct coord_suspend_struct {
	struct work_struct callback_work;
};

static struct coord_suspend_struct *coord_suspend_work;

/**
 * coord_suspend_work_fn - Trigger suspend
 * @work:	Pointer to work_struct
 *
 * Bottom-half of coordinate suspend IRQ handler.
 */
static void coord_suspend_work_fn(struct work_struct *work)
{
	pm_suspend(PM_SUSPEND_MEM);
}

/**
 * coord_suspend_handler - Handler for coordinated suspend IRQ
 *
 * @irq:	The interrupt number
 * @data:	Private platform device data
 *
 * Return:	Returns IRQ_HANDLED
 *
 * The interrupt handler is non-blocking, triggering a work queue which
 * performs the suspend outside of the interrupt context
 */
static irqreturn_t coord_suspend_handler(int irq, void *data)
{
	if (work_pending(&coord_suspend_work->callback_work) == false)
		queue_work(system_unbound_wq,
			   &coord_suspend_work->callback_work);

	return IRQ_HANDLED;
}

/**
 * coord_suspend_probe - Register IRQ for coordinated suspend
 * 			 and setup work queue
 *
 * @pdev:	Pointer to the platform_device structure
 *
 * Return:	Returns 0 on success
 *		Negative error code otherwise
 */
static int coord_suspend_probe(struct platform_device *pdev)
{
	int ret, irq;

	irq = platform_get_irq(pdev, 0);
	if (irq <= 0) {
		return -ENXIO;
	}

	ret = devm_request_irq(&pdev->dev, irq, coord_suspend_handler, 0,
			       DRIVER_NAME, pdev);
	if (ret) {
		dev_err(&pdev->dev, "request_irq '%d' failed with %d\n",
			irq, ret);
		return ret;
	}

	/* Setup work queue to decouple suspend procedure from IRQ handler */
	coord_suspend_work = devm_kzalloc(&pdev->dev,
					  sizeof(struct coord_suspend_struct),
					  GFP_KERNEL);

	if (!coord_suspend_work)
		return -ENOMEM;

	INIT_WORK(&coord_suspend_work->callback_work, coord_suspend_work_fn);

	return 0;
}

static const struct of_device_id coord_susp_of_match[] = {
	{ .compatible = "xen,coord-suspend", },
	{ /* end of table */ },
};

static struct platform_driver coord_suspend_driver = {
	.probe = coord_suspend_probe,
	.driver = {
		.name = DRIVER_NAME,
		.of_match_table = coord_susp_of_match,
	},
};

builtin_platform_driver(coord_suspend_driver);
