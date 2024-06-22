// SPDX-License-Identifier: GPL-2.0-only
/*
 * UCIe Endpoint driver for Renesas R-Car Gen5 Series SoCs
 * Copyright (C) 2024 Renesas Electronics Corporation
 */

#include <linux/interrupt.h>
#include <linux/module.h>
#include <linux/of_device.h>
#include <linux/pci.h>
#include <linux/pci-epc.h>
#include <linux/pm_runtime.h>
#include <linux/platform_device.h>

#include "ucie-rcar.h"

struct rcar_ucie_ep {
	struct rcar_ucie ucie;
	phys_addr_t *ob_mapped_addr;
	struct pci_epc_mem_window *ob_window;
	u8 max_functions;
	unsigned long *ib_window_map;
	u32 num_ib_windows;
	u32 num_ob_windows;
};

static int rcar_ucie_ep_get_resources(struct rcar_ucie_ep *ep, struct platform_device *pdev)
{
	struct rcar_ucie *ucie = &ep->ucie;
	struct device *dev = ucie->dev;
	int ret;

	ucie->base = devm_platform_ioremap_resource_byname(pdev, "base");
	if (IS_ERR(ucie->base))
		return PTR_ERR(ucie->base);

	/* FIXME: Correct these values */
	ep->num_ib_windows = MAX_NR_INBOUND_MAPS;
	ep->num_ob_windows = MAX_NR_OUTBOUND_MAPS;

	ret = of_property_read_u8(dev->of_node, "max-functions", &ep->max_functions);
	if (ret)
		ep->max_functions = 1;

	return 0;
}

static void rcar_ucie_ep_hw_enable(struct rcar_ucie_ep *ep)
{
	struct rcar_ucie *ucie = &ep->ucie;

	/* Configure as Endpoint */
	/* FIXME: Confirm the used of this register */
	rcar_ucie_mem_write32(ucie, IMP_CORECONFIG_CONFIG0, UCIECTL_DEF_EP_EN);

	rcar_ucie_controller_enable(ucie);
	rcar_ucie_phy_enable(ucie);
	rcar_ucie_link_up(ucie);
}

static int rcar_ucie_ep_write_header(struct pci_epc *epc,  u8 fn, struct pci_epf_header *hdr)
{
	return 0;
}

static int rcar_ucie_ep_set_bar(struct pci_epc *epc, u8 fn, struct pci_epf_bar *epf_bar)
{
	return 0;
}

static void rcar_ucie_ep_clear_bar(struct pci_epc *epc, u8 fn, struct pci_epf_bar *epf_bar)
{
}

static int rcar_ucie_ep_set_msi(struct pci_epc *epc, u8 fn, u8 interrupts)
{
	return 0;
}

static int rcar_ucie_ep_get_msi(struct pci_epc *epc, u8 fn)
{
	return 0;
}

static int rcar_ucie_ep_map_addr(struct pci_epc *epc, u8 fn, phys_addr_t addr,
				 u64 pci_addr, size_t size)
{
	return 0;
}

static void rcar_ucie_ep_unmap_addr(struct pci_epc *epc, u8 fn, phys_addr_t addr)
{
}

static int rcar_ucie_ep_raise_irq(struct pci_epc *epc, u8 fn, unsigned int type, u16 interrupt_num)
{
	return 0;
}

static int rcar_ucie_ep_start(struct pci_epc *epc)
{
	return 0;
}

static void rcar_ucie_ep_stop(struct pci_epc *epc)
{
}

static const struct pci_epc_features rcar_ucie_epc_features = {
	.linkup_notifier = false,
};

static const struct pci_epc_features *rcar_ucie_ep_get_features(struct pci_epc *epc, u8 fn)
{
	return &rcar_ucie_epc_features;
}

static const struct pci_epc_ops rcar_ucie_epc_ops = {
	.write_header	= rcar_ucie_ep_write_header,
	.set_bar	= rcar_ucie_ep_set_bar,
	.clear_bar	= rcar_ucie_ep_clear_bar,
	.set_msi	= rcar_ucie_ep_set_msi,
	.get_msi	= rcar_ucie_ep_get_msi,
	.map_addr	= rcar_ucie_ep_map_addr,
	.unmap_addr	= rcar_ucie_ep_unmap_addr,
	.raise_irq	= rcar_ucie_ep_raise_irq,
	.start		= rcar_ucie_ep_start,
	.stop		= rcar_ucie_ep_stop,
	.get_features	= rcar_ucie_ep_get_features,
};

static int rcar_ucie_ep_init(struct rcar_ucie_ep *ep)
{
	struct rcar_ucie *ucie = &ep->ucie;
	struct device *dev = ucie->dev;
	struct pci_epc *epc;
	int ret;

	ep->ib_window_map = devm_kcalloc(dev, BITS_TO_LONGS(ep->num_ib_windows),
					 sizeof(long), GFP_KERNEL);
	if (!ep->ib_window_map)
		return ret;

	ep->ob_mapped_addr = devm_kcalloc(dev, ep->num_ob_windows, sizeof(*ep->ob_mapped_addr),
					  GFP_KERNEL);

	if (!ep->ob_mapped_addr)
		return ret;

	epc = devm_pci_epc_create(dev, &rcar_ucie_epc_ops);
	if (IS_ERR(epc)) {
		dev_err(dev, "failed to create epc device\n");
		return PTR_ERR(epc);
	}

	/* FIXME: Get outbound ranges */
	/*
	epc->max_functions = ep->max_functions;
	epc_set_drvdata(epc, ep);

	ret = pci_epc_multi_mem_init(epc, ep->ob_window, ep->num_ob_windows);
	if (ret) {
		dev_err(dev, "failed to initialize the epc memory space\n");
		return ret;
	}
	*/

	rcar_ucie_ep_hw_enable(ep);

	return 0;
}

static int rcar_ucie_ep_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct rcar_ucie_ep *ep;
	struct rcar_ucie *ucie;
	int ret;

	ep = devm_kzalloc(dev, sizeof(*ep), GFP_KERNEL);
	if (!ep)
		return -ENOMEM;

	ucie = &ep->ucie;
	ucie->dev = dev;

	pm_runtime_enable(dev);
	ret = pm_runtime_get_sync(dev);
	if (ret)
		goto err_pm_disable;

	ret = rcar_ucie_ep_get_resources(ep, pdev);
	if (ret)
		goto err_pm_put;

	ret = rcar_ucie_ep_init(ep);
	if (ret)
		goto err_pm_put;

	return 0;

err_pm_put:
	pm_runtime_put(dev);

err_pm_disable:
	pm_runtime_disable(dev);

	return ret;
}

static int rcar_ucie_ep_remove(struct platform_device *pdev)
{
	pm_runtime_put(&pdev->dev);
	pm_runtime_disable(&pdev->dev);

	return 0;
}

static const struct of_device_id rcar_ucie_ep_of_match[] = {
	{ .compatible = "renesas,r8a78000-ucie-ep", },
	{},
};

static struct platform_driver rcar_ucie_ep_driver = {
	.driver = {
		.name = "ucie-ep-rcar",
		.of_match_table = rcar_ucie_ep_of_match,
	},
	.probe = rcar_ucie_ep_probe,
	.remove = rcar_ucie_ep_remove,
};
module_platform_driver(rcar_ucie_ep_driver);

MODULE_DESCRIPTION("Renesas R-Car UCIe Endpoint driver");
MODULE_LICENSE("GPL");
