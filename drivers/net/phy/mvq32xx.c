// SPDX-License-Identifier: GPL-2.0+
/* PHY driver for MVQ32xx
 *
 * Copyright (C) 2025 Renesas Electronics Corporation
 */

#include <linux/bitfield.h>
#include <linux/ctype.h>
#include <linux/delay.h>
#include <linux/hwmon.h>
#include <linux/marvell_phy.h>
#include <linux/phy.h>
#include <linux/sfp.h>
#include <linux/netdevice.h>

static int mvq32xx_read32(struct phy_device *phydev, u32 regad)
{
	int low, high;

	/* Set 32-bit register address */
	if (phy_write_mmd(phydev, 0x1e, 0x28, regad & 0xffff) < 0)
		return -EIO;
	if (phy_write_mmd(phydev, 0x1e, 0x29, regad >> 16) < 0)
		return -EIO;

	/* Trigger read */
	if (phy_write_mmd(phydev, 0x1e, 0x2c, 0) < 0)
		return -EIO;

	/* Read data */
	low = phy_read_mmd(phydev, 0x1e, 0x2d);
	if (low < 0)
		return low;

	high = phy_read_mmd(phydev, 0x1e, 0x2e);
	if (high < 0)
		return high;

	return (high << 16) | (low & 0xffff);
}

static int mvq32xx_write32(struct phy_device *phydev, u32 regad, u32 val)
{
	/* Set 32-bit register address */
	if (phy_write_mmd(phydev, 0x1e, 0x28, regad & 0xffff) < 0)
		return -EIO;
	if (phy_write_mmd(phydev, 0x1e, 0x29, regad >> 16) < 0)
		return -EIO;

	/* Set 32-bit data */
	if (phy_write_mmd(phydev, 0x1e, 0x2A, val & 0xffff) < 0)
		return -EIO;
	if (phy_write_mmd(phydev, 0x1e, 0x2b, val >> 16) < 0)
		return -EIO;

	/* Trigger write */
	if (phy_write_mmd(phydev, 0x1e, 0x2c, 1) < 0)
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

static int mvq32xx_config_init(struct phy_device *phydev)
{
	u32 val;

	phydev_info(phydev, "%s: MVQ32XX init\n", __func__);

	/* AN + USXGMII AN + advertise all + master + 10G + USXGMII */
	val = BIT(15)              /* Line Auto-Negotiation Enable */
	    | BIT(14)              /* USXGMII Auto-Negotiation Enable */
	    | BIT(10) | BIT(9) | BIT(8)    /* Advertise 10G/5G/2.5G */
	    | BIT(7)               /* Prefer Master */
	    | (3 << 4)             /* Line Rate = 10G */
	    | (7 << 0);            /* System PCS = 10G USXGMII */

	mvq32xx_write32(phydev, 0x40780964, val);

	/* Exit from HCS state */
	mvq32xx_update_bits32(phydev, 0x407809C8, BIT(1), BIT(1));

	/* Disable System PCS loopback */
	val = phy_read_mmd(phydev, MDIO_MMD_PCS, 0x0912);
	val &= ~BIT(14);
	phy_write_mmd(phydev, MDIO_MMD_PCS, 0x0912, val);

	return 0;
}

static int mvq32xx_read_status(struct phy_device *phydev)
{
	int val;
	bool linkStatus = false;

	/* PMA link check (Line-side) */
	for (int i = 0; i < 300; i++) {
		val = phy_read_mmd(phydev, MDIO_MMD_PMAPMD, 0x0906);
		if ((val & BIT(0)) != 0) {
			phydev->link = 1;
			phydev_dbg(phydev, "%s: PMA (Line) Linkup OK\n", __func__);
			linkStatus = true;
			break;
		}
		usleep_range(1000, 2000);
	}
	if (!linkStatus)
		phydev_dbg(phydev, "%s: PMA (Line) Linkup NG\n", __func__);

	/* Line Device Mode: Master/Slave */
	val = mvq32xx_read32(phydev, 0x40780964);
	if (val & BIT(7))
		phydev_dbg(phydev, "%s: Line Device Mode: Master\n", __func__);
	else
		phydev_dbg(phydev, "%s: Line Device Mode: Slave\n", __func__);

	/* Line Rate (forced mode) */
	switch ((val >> 4) & 0x3) {
	case 1:
		phydev->speed = SPEED_2500;
		phydev_dbg(phydev, "%s: Line Rate = 2.5G\n", __func__);
		break;
	case 2:
		phydev->speed = SPEED_5000;
		phydev_dbg(phydev, "%s: Line Rate = 5G\n", __func__);
		break;
	case 3:
		phydev->speed = SPEED_10000;
		phydev_dbg(phydev, "%s: Line Rate = 10G\n", __func__);
		break;
	default:
		phydev->speed = SPEED_UNKNOWN;
		phydev_dbg(phydev, "%s: Line Rate = Unknown\n", __func__);
		break;
	}

	/* PCS link check (System-side) */
	linkStatus = false;
	for (int i = 0; i < 300; i++) {
		val = phy_read_mmd(phydev, MDIO_MMD_PCS, 0x3C7F);
		if (val & BIT(12)) {
			phydev_dbg(phydev, "%s: PCS (System) Linkup OK\n", __func__);
			linkStatus = true;
			break;
		}
		usleep_range(1000, 2000);
	}
	if (!linkStatus)
		phydev_dbg(phydev, "%s: PCS (System) Linkup NG\n", __func__);

	/* System Interface Mode */
	val = mvq32xx_read32(phydev, 0x40780964);
	switch (val & 0x7) {
	case 1:
		phydev_dbg(phydev, "%s: Sys Mode: 2500 BaseX\n", __func__);
		break;
	case 2:
		phydev_dbg(phydev, "%s: Sys Mode: 2.5G BaseX\n", __func__);
		break;
	case 3:
		phydev_dbg(phydev, "%s: Sys Mode: 5G BaseR\n", __func__);
		break;
	case 4:
		phydev_dbg(phydev, "%s: Sys Mode: 5G BaseR\n", __func__);
		break;
	case 5:
		phydev_dbg(phydev, "%s: Sys Mode: 10G BaseR\n", __func__);
		break;
	case 6:
		phydev_dbg(phydev, "%s: Sys Mode: 5G USXGMII\n", __func__);
		break;
	case 7:
		phydev_dbg(phydev, "%s: Sys Mode: 10G USXGMII\n", __func__);
		break;
	default:
		phydev_dbg(phydev, "%s: Sys Mode: Unknown\n", __func__);
		break;
	}

	/* Duplex/Autoneg default assumption */
	phydev->duplex = DUPLEX_FULL;
	phydev->autoneg = AUTONEG_ENABLE;

	return 0;
}

static int mvq32xx_probe(struct phy_device *phydev)
{
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
