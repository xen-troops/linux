/*
 * drivers/net/phy/sja1105.c
 *
 * Dummy driver for PHY for SJA1105 ethernet switch
 *
 */
#include <linux/kernel.h>
#include <linux/string.h>
#include <linux/errno.h>
#include <linux/unistd.h>
#include <linux/interrupt.h>
#include <linux/init.h>
#include <linux/delay.h>
#include <linux/netdevice.h>
#include <linux/etherdevice.h>
#include <linux/skbuff.h>
#include <linux/spinlock.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/mii.h>
#include <linux/ethtool.h>
#include <linux/phy.h>

MODULE_DESCRIPTION("SJA1105 dummy PHY driver");
MODULE_AUTHOR("Michael Wegner");
MODULE_LICENSE("GPL");

static int sja1105_config_aneg(struct phy_device *phydev)
{
	phydev->supported = (PHY_DEFAULT_FEATURES | PHY_1000BT_FEATURES);
	phydev->advertising = phydev->supported;

	return 0;
}

static int sja1105_read_status(struct phy_device *phydev)
{
	phydev->duplex = DUPLEX_FULL;
	phydev->speed = SPEED_1000;
	phydev->pause = 0;
	phydev->asym_pause = 0;

	return 0;
}

static struct phy_driver sja1105_driver[] = { {
	.phy_id		= 0xfffffffe,
	.name		= "SJA1105 Dummy PHY driver",
	.phy_id_mask	= 0x0ffffff0,
	.features	= (PHY_DEFAULT_FEATURES | PHY_1000BT_FEATURES),
	.flags		= PHY_POLL,
	.config_aneg	= sja1105_config_aneg,
	.read_status	= sja1105_read_status,
} };

module_phy_driver(sja1105_driver);

static struct mdio_device_id __maybe_unused sja1105_tbl[] = {
	{ 0xfffffffe, 0x0ffffff0 },
	{ }
};

MODULE_DEVICE_TABLE(mdio, sja1105_tbl);
