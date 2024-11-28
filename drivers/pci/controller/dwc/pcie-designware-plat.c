// SPDX-License-Identifier: GPL-2.0
/*
 * PCIe RC driver for Synopsys DesignWare Core
 *
 * Copyright (C) 2015-2016 Synopsys, Inc. (www.synopsys.com)
 *
 * Authors: Joao Pinto <Joao.Pinto@synopsys.com>
 */
#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/gpio.h>
#include <linux/interrupt.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/of_device.h>
#include <linux/pci.h>
#include <linux/platform_device.h>
#include <linux/resource.h>
#include <linux/types.h>

#include "pcie-designware.h"

/* PCI Express capability */
#define	EXPCAP(x)		(0x0070 + (x))

#define	PCI_EXP_LNKCAP_MLW_X1	0x00000010 /* Maximum Link Width x1 */
#define	PCI_EXP_LNKCAP_MLW_X2	0x00000020 /* Maximum Link Width x2 */
#define	PCI_EXP_LNKCAP_MLW_X4	0x00000040 /* Maximum Link Width x4 */

/* Renesas-specific */
#define	PCIEMSR0		0x0000
#define	BIFUR_MOD_SET_ON	BIT(0)
#define	DEVICE_TYPE_EP		0
#define	DEVICE_TYPE_RC		BIT(4)

#define	PCIEINTSTS0EN		0x0310
#define	MSI_CTRL_INT		BIT(26)

#define to_rcar_gen5_pcie(x)	dev_get_drvdata((x)->dev)

struct dw_plat_pcie {
	struct dw_pcie			*pci;
	enum dw_pcie_device_mode	mode;
	void __iomem			*base;
	void __iomem			*phy_base;
};

struct dw_plat_pcie_of_data {
	enum dw_pcie_device_mode	mode;
};

void rcar_gen5_pcie_set_max_link_width(struct dw_plat_pcie *dw_plat_pcie, int num_lanes)
{
	struct dw_pcie *pci = dw_plat_pcie->pci;
	u32 val;

	val = dw_pcie_readl_dbi(pci, EXPCAP(PCI_EXP_LNKCAP));
	val &= ~PCI_EXP_LNKCAP_MLW;
	switch (num_lanes) {
	case 1:
		val |= PCI_EXP_LNKCAP_MLW_X1;
		break;
	case 2:
		val |= PCI_EXP_LNKCAP_MLW_X2;
		break;
	case 4:
		val |= PCI_EXP_LNKCAP_MLW_X4;
		break;
	default:
		dev_info(pci->dev, "Invalid num-lanes %d\n", num_lanes);
		break;
	}
	dw_pcie_writel_dbi(pci, EXPCAP(PCI_EXP_LNKCAP), val);
}

int rcar_gen5_pcie_set_device_type(struct dw_plat_pcie *dw_plat_pcie, bool rc,
				   int num_lanes)
{
	u32 val;

	/* Note: Assume the reset is asserted here */
	val = readl(dw_plat_pcie->base + PCIEMSR0);
	if (rc)
		val |= DEVICE_TYPE_RC;
	else
		val |= DEVICE_TYPE_EP;
	if (num_lanes < 4)
		val |= BIFUR_MOD_SET_ON;
	writel(val, dw_plat_pcie->base + PCIEMSR0);

	return 0;
}

static int rcar_gen5_pcie_host_init(struct dw_pcie_rp *pp)
{
	struct dw_pcie *pci = to_dw_pcie_from_pp(pp);
	struct dw_plat_pcie *dw_plat_pcie = to_rcar_gen5_pcie(pci);
	int ret;
	u32 val;

	/* Set device type */
	ret = rcar_gen5_pcie_set_device_type(dw_plat_pcie, true, pci->num_lanes);
	if (ret < 0)
		return ret;

	dw_pcie_dbi_ro_wr_en(pci);

	if (IS_ENABLED(CONFIG_PCI_MSI)) {
		/* Enable MSI interrupt signal */
		val = readl(dw_plat_pcie->base + PCIEINTSTS0EN);
		val |= MSI_CTRL_INT;
		writel(val, dw_plat_pcie->base + PCIEINTSTS0EN);
	}

	rcar_gen5_pcie_set_max_link_width(dw_plat_pcie, pci->num_lanes);

	dw_pcie_dbi_ro_wr_dis(pci);

	return 0;
}

static const struct dw_pcie_host_ops dw_plat_pcie_host_ops = {
	.host_init = rcar_gen5_pcie_host_init,
};

static void dw_plat_pcie_ep_init(struct dw_pcie_ep *ep)
{
	struct dw_pcie *pci = to_dw_pcie_from_ep(ep);
	enum pci_barno bar;

	for (bar = 0; bar < PCI_STD_NUM_BARS; bar++)
		dw_pcie_ep_reset_bar(pci, bar);
}

static int dw_plat_pcie_ep_raise_irq(struct dw_pcie_ep *ep, u8 func_no,
				     enum pci_epc_irq_type type,
				     u16 interrupt_num)
{
	struct dw_pcie *pci = to_dw_pcie_from_ep(ep);

	switch (type) {
	case PCI_EPC_IRQ_LEGACY:
		return dw_pcie_ep_raise_legacy_irq(ep, func_no);
	case PCI_EPC_IRQ_MSI:
		return dw_pcie_ep_raise_msi_irq(ep, func_no, interrupt_num);
	case PCI_EPC_IRQ_MSIX:
		return dw_pcie_ep_raise_msix_irq(ep, func_no, interrupt_num);
	default:
		dev_err(pci->dev, "UNKNOWN IRQ type\n");
	}

	return 0;
}

static const struct pci_epc_features dw_plat_pcie_epc_features = {
	.linkup_notifier = false,
	.msi_capable = true,
	.msix_capable = true,
};

static const struct pci_epc_features*
dw_plat_pcie_get_features(struct dw_pcie_ep *ep)
{
	return &dw_plat_pcie_epc_features;
}

static const struct dw_pcie_ep_ops pcie_ep_ops = {
	.ep_init = dw_plat_pcie_ep_init,
	.raise_irq = dw_plat_pcie_ep_raise_irq,
	.get_features = dw_plat_pcie_get_features,
};

static int dw_plat_add_pcie_port(struct dw_plat_pcie *dw_plat_pcie,
				 struct platform_device *pdev)
{
	struct dw_pcie *pci = dw_plat_pcie->pci;
	struct dw_pcie_rp *pp = &pci->pp;
	struct device *dev = &pdev->dev;
	int ret;

	pp->irq = platform_get_irq(pdev, 1);
	if (pp->irq < 0)
		return pp->irq;

	pp->num_vectors = MAX_MSI_IRQS;
	pp->ops = &dw_plat_pcie_host_ops;

	ret = dw_pcie_host_init(pp);
	if (ret) {
		dev_err(dev, "Failed to initialize host\n");
		return ret;
	}

	return 0;
}

static int rcar_gen5_pcie_get_resources(struct dw_plat_pcie *dw_plat_pcie,
					struct platform_device *pdev)
{
	struct resource *res;
	struct dw_pcie *pci = dw_plat_pcie->pci;
	struct device_node *np = dev_of_node(&pdev->dev);

	of_property_read_u32(np, "num-lanes", &pci->num_lanes);

	/* Renesas-specific registers */
	dw_plat_pcie->base = devm_platform_ioremap_resource_byname(pdev, "app");
	if (IS_ERR(dw_plat_pcie->base))
		return PTR_ERR(dw_plat_pcie->base);

	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "phy");
	if (res) {
		dw_plat_pcie->phy_base = devm_ioremap_resource(&pdev->dev, res);
		if (IS_ERR(dw_plat_pcie->phy_base))
			dw_plat_pcie->phy_base = NULL;
	}

	/* temporarily removed since PCIe on VDK Gen5 has not supported yet */
	//return rcar_gen5_pcie_devm_reset_get(dw_plat_pcie, pci->dev);

	return 0;
}

static int dw_plat_pcie_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct dw_plat_pcie *dw_plat_pcie;
	struct dw_pcie *pci;
	int ret;
	const struct dw_plat_pcie_of_data *data;
	enum dw_pcie_device_mode mode;

	data = of_device_get_match_data(dev);
	if (!data)
		return -EINVAL;

	mode = (enum dw_pcie_device_mode)data->mode;

	dw_plat_pcie = devm_kzalloc(dev, sizeof(*dw_plat_pcie), GFP_KERNEL);
	if (!dw_plat_pcie)
		return -ENOMEM;

	pci = devm_kzalloc(dev, sizeof(*pci), GFP_KERNEL);
	if (!pci)
		return -ENOMEM;

	pci->dev = dev;

	dw_plat_pcie->pci = pci;
	dw_plat_pcie->mode = mode;

	ret = rcar_gen5_pcie_get_resources(dw_plat_pcie, pdev);
	if (ret < 0) {
		dev_err(dev, "Failed to request resource: %d\n", ret);
		return ret;
	}

	platform_set_drvdata(pdev, dw_plat_pcie);

	switch (dw_plat_pcie->mode) {
	case DW_PCIE_RC_TYPE:
		if (!IS_ENABLED(CONFIG_PCIE_DW_PLAT_HOST))
			return -ENODEV;

		ret = dw_plat_add_pcie_port(dw_plat_pcie, pdev);
		break;
	case DW_PCIE_EP_TYPE:
		if (!IS_ENABLED(CONFIG_PCIE_DW_PLAT_EP))
			return -ENODEV;

		pci->ep.ops = &pcie_ep_ops;
		ret = dw_pcie_ep_init(&pci->ep);
		break;
	default:
		dev_err(dev, "INVALID device type %d\n", dw_plat_pcie->mode);
		ret = -EINVAL;
		break;
	}

	return ret;
}

static const struct dw_plat_pcie_of_data dw_plat_pcie_rc_of_data = {
	.mode = DW_PCIE_RC_TYPE,
};

static const struct dw_plat_pcie_of_data dw_plat_pcie_ep_of_data = {
	.mode = DW_PCIE_EP_TYPE,
};

static const struct of_device_id dw_plat_pcie_of_match[] = {
	{
		.compatible = "snps,dw-pcie",
		.data = &dw_plat_pcie_rc_of_data,
	},
	{
		.compatible = "snps,dw-pcie-ep",
		.data = &dw_plat_pcie_ep_of_data,
	},
	{
		.compatible = "renesas,rcar-gen5-pcie",
		.data = &dw_plat_pcie_rc_of_data,
	},
	{
		.compatible = "renesas,rcar-gen5-pcie-ep",
		.data = &dw_plat_pcie_ep_of_data,
	},
	{},
};

static struct platform_driver dw_plat_pcie_driver = {
	.driver = {
		.name	= "pcie4-rcar-gen5",
		.of_match_table = dw_plat_pcie_of_match,
		.suppress_bind_attrs = true,
	},
	.probe = dw_plat_pcie_probe,
};
builtin_platform_driver(dw_plat_pcie_driver);
