// SPDX-License-Identifier: GPL-2.0
/* PHY driver for MVQ32xx
 *
 * Copyright (C) 2025 Renesas Electronics Corporation
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/phy.h>
#include <linux/delay.h>
#include <linux/mdio.h>
#include <linux/ethtool.h>
#include <linux/linkmode.h>

#define Q32XX_MMD_PMA			MDIO_MMD_PMAPMD
#define Q32XX_MMD_PCS			MDIO_MMD_PCS
#define Q32XX_MMD_AN			MDIO_MMD_AN

static int mvq32xx_read32(struct phy_device *phydev, u32 regad)
{
	int low, high;

	/* Set 32-bit register address */
	if (phy_write_mmd(phydev, 0x1E, 0x28, regad & 0xFFFF) < 0)
		return -EIO;
	if (phy_write_mmd(phydev, 0x1E, 0x29, regad >> 16) < 0)
		return -EIO;

	/* Trigger read */
	if (phy_write_mmd(phydev, 0x1E, 0x2C, 0) < 0)
		return -EIO;

	/* Read data */
	low = phy_read_mmd(phydev, 0x1E, 0x2D);
	if (low < 0)
		return low;

	high = phy_read_mmd(phydev, 0x1E, 0x2E);
	if (high < 0)
		return high;

	return (high << 16) | (low & 0xFFFF);
}

static int mvq32xx_write32(struct phy_device *phydev, u32 regad, u32 val)
{
	/* Set 32-bit register address */
	if (phy_write_mmd(phydev, 0x1E, 0x28, regad & 0xFFFF) < 0)
		return -EIO;
	if (phy_write_mmd(phydev, 0x1E, 0x29, regad >> 16) < 0)
		return -EIO;

	/* Set 32-bit data */
	if (phy_write_mmd(phydev, 0x1E, 0x2A, val & 0xFFFF) < 0)
		return -EIO;
	if (phy_write_mmd(phydev, 0x1E, 0x2B, val >> 16) < 0)
		return -EIO;

	/* Trigger write */
	if (phy_write_mmd(phydev, 0x1E, 0x2C, 1) < 0)
		return -EIO;

	return 0;
}

static void mvq32xx_update_bits32(struct phy_device *phydev, u32 regad, u32 mask, u32 val)
{
	u32 tmp;

	tmp = mvq32xx_read32(phydev, regad);;
	tmp = (tmp & ~mask) | (val & mask);
	mvq32xx_write32(phydev, regad, tmp);
}

struct q32xx_priv {
	DECLARE_BITMAP(supported, __ETHTOOL_LINK_MODE_MASK_NBITS);
};

static int mvq32xx_config_init(struct phy_device *phydev)
{
	int val;

	phydev_info(phydev, "%s: MV8322 init\n", __func__);

	/* Set master */
	mvq32xx_update_bits32(phydev, 0x40780964, BIT(7) | BIT(15), BIT(7) | BIT(15));
	/* Set T1 speed */
	mvq32xx_update_bits32(phydev, 0x40780964, BIT(4) | BIT(5), BIT(4) | BIT(5));
	/* Set Serdes speed */
	mvq32xx_update_bits32(phydev, 0x40780964, BIT(0) | BIT(1) | BIT(2), BIT(0) | BIT(1) | BIT(2));
	/* Exit from HCS state */
	mvq32xx_update_bits32(phydev, 0x407809C8, BIT(1), BIT(1));

	/* Disable System Interface Loopback */
	val = phy_read_mmd(phydev, 3, 0x0912);
	val &= ~BIT(14);
	phy_write_mmd(phydev, 3, 0x0912, val);

	return 0;
}

static int mvq32xx_read_status(struct phy_device *phydev)
{
	int val;
	bool linkStatus = false;

	/* 2) check T1 status */
	for (int i = 0; i < 300; i++) {
		val = phy_read_mmd(phydev, 1, 0x0906);
		if ((val & BIT(0)) !=0) {
			//printk("%s %d: MV3244 PMA (Line interface) Linkup OK\n",__func__,__LINE__);
			phydev->link = 1;
			phydev_dbg(phydev, "%s: MV3244 PMA (Line interface) Linkup OK\n",__func__);
			linkStatus = true;
			break;
		}
		usleep_range(1000, 2000);
	}
	if (linkStatus == false)
		phydev_dbg(phydev, "%s: MV3244 PMA (Line interface) Linkup NG\n",__func__);

	val = mvq32xx_read32(phydev, 0x40780964);
	if (((val >> 7) &0x1) == 0x1) {
		phydev_dbg(phydev, "%s: MV3244 Line Device Mode Master\n",__func__);
	} else {
		phydev_dbg(phydev, "%s: MV3244 Line Device Mode Slave\n",__func__);
	}

	val = mvq32xx_read32(phydev, 0x40780964);
	switch (((val >> 4) & 0x3U)) {
	case 1:
		phydev_dbg(phydev, "%s: MV3244 Line Rate speed 2.5G\n", __func__);
		break;
	case 2:
		phydev_dbg(phydev, "%s: MV3244 Line Rate speed 5G\n", __func__);
		break;
	case 3:
		phydev_dbg(phydev, "%s: MV3244 Line Rate speed 10G\n", __func__);
		break;
	default:
		phydev_dbg(phydev, "%s: MV3244 Line Rate speed Unknown\n", __func__);
		break;
	}

	linkStatus = false;
	for (int i = 0; i < 300; i++)
	{
		val = phy_read_mmd(phydev, 4, 0x3C7F);
		if ((val & BIT(12)) !=0) {
			phydev_dbg(phydev, "%s:MV3244 PCS (System Line) Linkup OK\n",__func__);
			linkStatus = true;
			break;
		}
	}
	if (linkStatus == false) {
		phydev_dbg(phydev, "%s: MV3244 PCS (System Line) Linkup NG\n",__func__);
		phydev->speed = SPEED_UNKNOWN;
		phydev->duplex = DUPLEX_UNKNOWN;
	}

	val = mvq32xx_read32(phydev, 0x40780964);
	switch (((val >> 0) & 0x7U)) {
	case 1:
		phydev_dbg(phydev, "%s: MV3244 System Interface Mode speed 2500 BaseX\n", __func__);
		break;
	case 2:
		phydev_dbg(phydev, "%s: MV3244 System Interface Mode speed 2.5G BaseX\n", __func__);
		break;
	case 3:
		phydev_dbg(phydev, "%s: MV3244 System Interface Mode speed 5000 BaseR\n", __func__);
		break;
	case 4:
		phydev_dbg(phydev, "%s: MV3244 System Interface Mode speed 5G BaseR\n", __func__);
		break;
	case 6:
		phydev_dbg(phydev, "%s: MV3244 System Interface Mode speed 5G USXGMII\n", __func__);
		break;
	case 5:
		phydev_dbg(phydev, "%s: MV3244 System Interface Mode speed 10G BaseR\n", __func__);
		break;
	case 7:
		phydev->speed = SPEED_10000;
		phydev_dbg(phydev, "%s: MV3244 System Interface Mode speed 10G USXGMII\n", __func__);
		break;
	default:
		phydev_dbg(phydev, "%s: MV3244 System Interface Mode speed Unknow\n", __func__);
		break;
	}

	phydev->duplex = DUPLEX_FULL;
	phydev->autoneg = AUTONEG_ENABLE;

	val = phy_read_mmd(phydev, 3, 0x0000);
	if (((val >>14) & 0x1) == 0x1)
		phydev_dbg(phydev, "%s: MV3244 TUNIT PCS System Loopback ENABLE\n", __func__);
	else {
		phydev_dbg(phydev, "%s: MV3244 TUNIT PCS System Loopback DISABLE\n", __func__);
	}
	return 0;
}

static int mvq32xx_probe(struct phy_device *phydev)
{
	struct q32xx_priv *priv;

	priv = devm_kzalloc(&phydev->mdio.dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	bitmap_copy(priv->supported, phydev->supported, __ETHTOOL_LINK_MODE_MASK_NBITS);
	phydev->priv = priv;
	return 0;
}

static void mvq32xx_remove(struct phy_device *phydev)
{
	phydev->priv = NULL;
}

static struct phy_driver mvq32xx_driver[] = {
	{
		PHY_ID_MATCH_EXACT(0x002b0b20),
		.name		= "MVQ32xx",
		.probe		= mvq32xx_probe,
		.remove		= mvq32xx_remove,
		.config_init	= mvq32xx_config_init,
		.read_status	= mvq32xx_read_status,
		.features	= PHY_GBIT_FEATURES,
	},
};

module_phy_driver(mvq32xx_driver);

MODULE_DESCRIPTION("MVQ32xx PHY driver");
MODULE_LICENSE("GPL");
