// SPDX-License-Identifier: GPL-2.0-only
/*
 * UCIe host controller driver for Renesas R-Car Gen5 Series SoCs
 * Copyright (C) 2024 Renesas Electronics Corporation
 */

#include <linux/interrupt.h>
#include <linux/module.h>
#include <linux/of_device.h>
#include <linux/pci.h>
#include <linux/pm_runtime.h>
#include <linux/platform_device.h>

#include "ucie-rcar.h"

struct rcar_ucie_host {
	struct rcar_ucie ucie;
};

static int rcar_ucie_read_conf(struct pci_bus *bus, unsigned int devfn,
			       int where, int size, u32 *val)
{
	struct rcar_ucie_host *host = bus->sysdata;
	struct rcar_ucie *ucie = &host->ucie;

	*val = rcar_ucie_conf_read32(ucie, where);

	if (size == 1)
		*val = (*val >> (BITS_PER_BYTE * (where & 3))) & 0xff;
	else if (size == 2)
		*val = (*val >> (BITS_PER_BYTE * (where & 2))) & 0xffff;

	return PCIBIOS_SUCCESSFUL;
}

static int rcar_ucie_write_conf(struct pci_bus *bus, unsigned int devfn,
				int where, int size, u32 val)
{
	struct rcar_ucie_host *host = bus->sysdata;
	struct rcar_ucie *ucie = &host->ucie;
	unsigned int shift;
	u32 data;

	if (size == 1) {
		shift = BITS_PER_BYTE * (where & 3);
		data &= ~(0xff << shift);
		data |= ((val & 0xff) << shift);
	} else if (size == 2) {
		shift = BITS_PER_BYTE * (where & 2);
		data &= ~(0xffff << shift);
		data |= ((val & 0xffff) << shift);
	} else {
		data = val;
	}

	rcar_ucie_conf_write32(ucie, where, data);

	return PCIBIOS_SUCCESSFUL;
}

static struct pci_ops rcar_ucie_ops = {
	.read	= rcar_ucie_read_conf,
	.write	= rcar_ucie_write_conf,
};

static void __iomem *rcar_ucie_other_conf_map_bus(struct pci_bus *bus, unsigned int devfn,
						  int where)
{
	return NULL;
}

static int rcar_ucie_read_other_conf(struct pci_bus *bus, unsigned int devfn,
				     int where, int size, u32 *val)
{
	return 0;
}

static int rcar_ucie_write_other_conf(struct pci_bus *bus, unsigned int devfn,
				      int where, int size, u32 val)
{
	return 0;
}

static struct pci_ops rcar_ucie_child_ops = {
	.map_bus = rcar_ucie_other_conf_map_bus,
	.read = rcar_ucie_read_other_conf,
	.write = rcar_ucie_write_other_conf,
};

static int rcar_ucie_get_resources(struct rcar_ucie_host *host, struct platform_device *pdev)
{
	struct rcar_ucie *ucie = &host->ucie;

	ucie->base = devm_platform_ioremap_resource_byname(pdev, "base");
	if (IS_ERR(ucie->base))
		return PTR_ERR(ucie->base);

	return 0;
}

static void rcar_ucie_hw_enable(struct rcar_ucie_host *host)
{
	struct rcar_ucie *ucie = &host->ucie;

	/* Configure as Root Port */
	/* FIXME: Confirm the used of this register */
	rcar_ucie_mem_write32(ucie, IMP_CORECONFIG_CONFIG0, UCIECTL_DEF_RP_EN);

	rcar_ucie_controller_enable(ucie);
	rcar_ucie_phy_enable(ucie);
	rcar_ucie_link_up(ucie);
}

static int rcar_ucie_host_enable(struct rcar_ucie_host *host)
{
	struct pci_host_bridge *bridge = pci_host_bridge_from_priv(host);

	rcar_ucie_hw_enable(host);

	bridge->sysdata = host;
	bridge->ops = &rcar_ucie_ops;
	bridge->child_ops = &rcar_ucie_child_ops;

	return pci_host_probe(bridge);
}

static int rcar_ucie_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct pci_host_bridge *bridge;
	struct rcar_ucie_host *host;
	struct rcar_ucie *ucie;
	int ret;

	bridge = devm_pci_alloc_host_bridge(dev, sizeof(*host));
	if (!bridge)
		return -ENOMEM;

	host = pci_host_bridge_priv(bridge);
	ucie = &host->ucie;
	ucie->dev = dev;
	platform_set_drvdata(pdev, host);

	pm_runtime_enable(dev);
	ret = pm_runtime_get_sync(dev);
	if (ret)
		goto err_pm_disable;

	ret = rcar_ucie_get_resources(host, pdev);
	if (ret)
		goto err_pm_put;

	ret = rcar_ucie_host_enable(host);
	if (ret)
		goto err_pm_put;

	/* Ignore errors, the link may come up later */
	if (rcar_ucie_is_link_up(ucie))
		dev_info(dev, "UCIe link down\n");

	return 0;

err_pm_put:
	pm_runtime_put(dev);

err_pm_disable:
	pm_runtime_disable(dev);

	return ret;
}

static int rcar_ucie_remove(struct platform_device *pdev)
{
	pm_runtime_put(&pdev->dev);
	pm_runtime_disable(&pdev->dev);

	return 0;
}

static const struct of_device_id rcar_ucie_of_match[] = {
	{ .compatible = "renesas,r8a78000-ucie", },
	{},
};

static struct platform_driver rcar_ucie_driver = {
	.driver = {
		.name = "ucie-rcar",
		.of_match_table = rcar_ucie_of_match,
	},
	.probe = rcar_ucie_probe,
	.remove = rcar_ucie_remove,
};
module_platform_driver(rcar_ucie_driver);

MODULE_DESCRIPTION("Renesas R-Car UCIe host controller driver");
MODULE_LICENSE("GPL");
