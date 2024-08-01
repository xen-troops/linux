// SPDX-License-Identifier: GPL-2.0
/*
 * Renesas R-Car Gen4/Gen5 KCRC Driver
 *
 * Copyright (C) 2024 Renesas Electronics Inc.
 *
 * Author: huybui2 <huy.bui.wm@renesas.com>
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/io.h>
#include <linux/of_device.h>
#include <linux/platform_device.h>

#include "usr_wcrc.h"
#include "crc-wrapper.h"
#include "kcrc-drv.h"

struct kcrc_device {
	void __iomem *base[WCRC_DEVICES];
	struct device *dev[WCRC_DEVICES];
};

static struct kcrc_device *kcrc;

static int dev_chan;

static u32 kcrc_read(void __iomem *base, unsigned int offset)
{
	return ioread32(base + offset);
}

static void kcrc_write(void __iomem *base, unsigned int offset, u32 data)
{
	iowrite32(data, base + offset);
}

void kcrc_setting(struct wcrc_info *info)
{
	unsigned int poly_set;
	unsigned int p_size;
	unsigned int kcrc_cmd;
	unsigned int input_dw;
	u32 reg;

	/* Checking the Polynomial mode */
	switch (info->poly_mode) {
	case POLY_32_ETHERNET:
		p_size = KCRCmCTL_PSIZE_32;
		poly_set = KCRCmPOLY_32_ETHERNET;
		break;
	case POLY_16_CCITT_FALSE_CRC16:
		p_size = KCRCmCTL_PSIZE_16;
		poly_set = KCRCmPOLY_16_CCITT;
		break;
	case POLY_8_SAE_J1850:
		p_size = KCRCmCTL_PSIZE_8;
		poly_set = KCRCmPOLY_8_SAE_J1850;
		break;
	case POLY_8_0x2F:
		p_size = KCRCmCTL_PSIZE_8;
		poly_set = KCRCmPOLY_8_0x2F;
		break;
	case POLY_32_0x1EDC6F41:
		p_size = KCRCmCTL_PSIZE_32;
		poly_set = KCRCmPOLY_32_CRC32C;
		break;
	default:
		pr_err("ERROR: Polynomial mode NOT found\n");
		return;
	}

	/* Checking KCRC Calculate Mode 0/1/2 */
	kcrc_cmd = (info->kcrc_cmd0 ? KCRCmCTL_CMD0 : 0) |
			(info->kcrc_cmd1 ? KCRCmCTL_CMD1 : 0) |
			(info->kcrc_cmd2 ? KCRCmCTL_CMD2 : 0);

	/* Checking KCRCmDIN and KCRCmDOUT data size setting */
	if (info->d_in_sz == 8)
		input_dw = KCRCmCTL_DW_8;
	else if (info->d_in_sz == 16)
		input_dw = KCRCmCTL_DW_16;
	else  //default 32-bit
		input_dw = KCRCmCTL_DW_32;

	/* Set KCRCmCTL registers. */
	reg = p_size | kcrc_cmd | input_dw;
	kcrc_write(kcrc->base[info->wcrc_unit], KCRCmCTL, reg);

	/* Set KCRCmPOLY registers. */
	kcrc_write(kcrc->base[info->wcrc_unit], KCRCmPOLY, poly_set);

	/* Set KCRCmXOR register. */
	kcrc_write(kcrc->base[info->wcrc_unit], KCRCmXOR, KCRCmXOR_XOR);

	/* Set initial value to KCRCmDOUT register. */
	kcrc_write(kcrc->base[info->wcrc_unit], KCRCmDOUT, KCRCmDOUT_INITIAL);
}
EXPORT_SYMBOL(kcrc_setting);

int kcrc_calculate(struct wcrc_info *info)
{
	/* Skiping data input to*/
	if (info->skip_data_in)
		goto geting_output;

	/* Calculating data larger than 4bytes*/
	if (info->conti_cal)
		goto bypass_setting;

	/* Setting KCRC registers */
	kcrc_setting(info);

bypass_setting:
	/* Set input value to KCRCmDIN register. */
	kcrc_write(kcrc->base[info->wcrc_unit], KCRCmDIN, info->data_input);

geting_output:
	/* Read out the operated data from KCRCmDOUT register. */
	if (!info->during_conti_cal)
		info->kcrc_data_out = kcrc_read(kcrc->base[info->wcrc_unit], KCRCmDOUT);

	return 0;
}
EXPORT_SYMBOL(kcrc_calculate);

void kcrc_return_reg(struct wcrc_info *info)
{
	u32 reg_addr;

	reg_addr = 0xe7020090;
	// info->crc_data_out_8 |= kcrc_read(kcrc->base[info->wcrc_unit], KCRCmCTL);
	// info->crc_data_out_8 = (info->crc_data_out_8 << 32) | reg_addr;
}
EXPORT_SYMBOL(kcrc_return_reg);

static const struct of_device_id kcrc_of_ids[] = {
	{
		.compatible = "renesas,kcrc-drv",
	},
	{
		.compatible = "renesas,kcrc-r8a78000",
	},
	{
		.compatible = "renesas,rcar-gen5-kcrc",
	},
	{
		/* Terminator */
	}
};

static int kcrc_probe(struct platform_device *pdev)
{
	struct resource *res;


	if (dev_chan == 0) {
		kcrc = devm_kzalloc(&pdev->dev, sizeof(*kcrc), GFP_KERNEL);
		if (!kcrc)
			return -ENOMEM;
	}
	kcrc->dev[dev_chan] = &pdev->dev;

	/* Map I/O memory */
	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	kcrc->base[dev_chan] = devm_ioremap_resource(&pdev->dev, res);
	if (IS_ERR(kcrc->base[dev_chan]))
		return PTR_ERR(kcrc->base[dev_chan]);

	dev_chan++;

	return 0;
}

static int kcrc_remove(struct platform_device *pdev)
{
	return 0;
}

static struct platform_driver kcrc_driver = {
	.driver = {
		.name = "kcrc-driver",
		.of_match_table = of_match_ptr(kcrc_of_ids),
		.owner = THIS_MODULE,
	},
	.probe = kcrc_probe,
	.remove = kcrc_remove,
};

static int __init kcrc_drv_init(void)
{
	struct device_node *np;
	int ret;

	np = of_find_matching_node(NULL, kcrc_of_ids);
	if (!np)
		return 0;

	of_node_put(np);

	ret = platform_driver_register(&kcrc_driver);
	if (ret < 0)
		return ret;

	return 0;
}

static void __exit kcrc_drv_exit(void)
{
	platform_driver_unregister(&kcrc_driver);
}
module_init(kcrc_drv_init);
module_exit(kcrc_drv_exit);
