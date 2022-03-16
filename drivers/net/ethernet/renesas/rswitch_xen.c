// SPDX-License-Identifier: GPL-2.0
/* Renesas Ethernet Switch Para-Virtualized driver
 *
 * Copyright (C) 2022 EPAM Systems
 */

#include <linux/etherdevice.h>
#include <linux/platform_device.h>
#include "rswitch.h"


int rswitch_xen_ndev_register(struct rswitch_private *priv, int index)
{
	struct platform_device *pdev = priv->pdev;
	struct net_device *ndev;
	struct rswitch_device *rdev;
	int err;

	ndev = alloc_etherdev_mqs(sizeof(struct rswitch_device), 1, 1);
	if (!ndev)
		return -ENOMEM;

	SET_NETDEV_DEV(ndev, &pdev->dev);
	ether_setup(ndev);

	rdev = netdev_priv(ndev);
	rdev->ndev = ndev;
	rdev->priv = priv;
	/* TODO: Fix index calculation */
	priv->rdev[index + 3] = rdev;
	rdev->port = 4; 	/* TODO: This is supposed to be GWCA0 port */
	rdev->etha = NULL;
	rdev->remote_chain = -1;

	rdev->addr = priv->addr;

	spin_lock_init(&rdev->lock);

	ndev->features = NETIF_F_RXCSUM;
	ndev->hw_features = NETIF_F_RXCSUM;
	ndev->base_addr = (unsigned long)rdev->addr;
	snprintf(ndev->name, IFNAMSIZ, "vmq%d", index);
	ndev->netdev_ops = &rswitch_netdev_ops;
//	ndev->ethtool_ops = &rswitch_ethtool_ops;

	netif_napi_add(ndev, &rdev->napi, rswitch_poll, 64);

	eth_hw_addr_random(ndev);

	/* Network device register */
	err = register_netdev(ndev);
	if (err)
		goto out_reg_netdev;

	err = rswitch_rxdmac_init(ndev, priv);
	if (err < 0)
		goto out_rxdmac;

	err = rswitch_txdmac_init(ndev, priv);
	if (err < 0)
		goto out_txdmac;

	/* Print device information */
	netdev_info(ndev, "MAC address %pMn", ndev->dev_addr);

	return 0;

out_txdmac:
	rswitch_rxdmac_free(ndev, priv);

out_rxdmac:
	unregister_netdev(ndev);

out_reg_netdev:
	netif_napi_del(&rdev->napi);
	free_netdev(ndev);

	return err;
}

int rswitch_xen_connect_devs(struct rswitch_device *rdev1,
			     struct rswitch_device *rdev2)
{
	rdev1->remote_chain = rdev2->rx_chain->index;
	rdev2->remote_chain = rdev1->rx_chain->index;

	return 0;
}
