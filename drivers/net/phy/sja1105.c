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
#include <linux/of.h>

MODULE_DESCRIPTION("SJA1105 dummy PHY driver");
MODULE_AUTHOR("Michael Wegner");
MODULE_LICENSE("GPL");

struct sja1105_phy_priv {
	u32 speed;
};

static int sja1105_config_aneg(struct phy_device *phydev)
{
	struct sja1105_phy_priv *priv = phydev->priv;

	phydev->supported = PHY_DEFAULT_FEATURES;
	if (priv->speed == 1000) {
		phydev->supported |= PHY_1000BT_FEATURES;
	} else if (priv->speed == 100) {
		phydev->supported |= PHY_100BT_FEATURES;
	}
	phydev->advertising = phydev->supported;

	return 0;
}

static int sja1105_read_status(struct phy_device *phydev)
{
	struct sja1105_phy_priv *priv = phydev->priv;

	phydev->duplex = DUPLEX_FULL;
	phydev->speed = priv->speed;
	phydev->pause = 0;
	phydev->asym_pause = 0;

	return 0;
}

static int sja1105_phy_probe(struct phy_device *phydev)
{
	struct device *dev = &phydev->mdio.dev;
	struct device_node *of_node = dev->of_node;
	struct sja1105_phy_priv *priv;

	priv = devm_kzalloc(dev, sizeof(struct sja1105_phy_priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	if (of_property_read_u32(of_node, "speed", &priv->speed) != 0) {
		dev_info(dev, "Could not read speed property, using default 100");
		priv->speed = 100;
	}

	if (priv->speed != 1000 && priv->speed != 100) {
		dev_err(dev, "Invalid link speed %u, must be 100 or 1000", priv->speed);
		kfree(priv);
		return -EINVAL;
	}

	phydev->priv = priv;

	return 0;
}

static void sja1105_phy_remove(struct phy_device *phydev)
{
	if (ZERO_OR_NULL_PTR(phydev)) {
		return;
	}

	kfree(phydev->priv);
}

static struct phy_driver sja1105_driver[] = { {
	.phy_id		= 0xfffffffe,
	.name		= "SJA1105 Dummy PHY driver",
	.phy_id_mask	= 0x0ffffff0,
	.features	= (PHY_DEFAULT_FEATURES | PHY_1000BT_FEATURES |
				   PHY_1000BT_FEATURES),
	.flags		= PHY_POLL,
	.probe      = sja1105_phy_probe,
	.remove     = sja1105_phy_remove,
	.config_aneg	= sja1105_config_aneg,
	.read_status	= sja1105_read_status,
} };

module_phy_driver(sja1105_driver);

static struct mdio_device_id __maybe_unused sja1105_tbl[] = {
	{ 0xfffffffe, 0x0ffffff0 },
	{ }
};

MODULE_DEVICE_TABLE(mdio, sja1105_tbl);
