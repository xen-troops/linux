// SPDX-License-Identifier: GPL-2.0
/*
 * Window watchdog driver for Renesas WWDT Watchdog timer
 *
 */

#include <linux/bitops.h>
#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>
#include <linux/reset.h>
#include <linux/units.h>
#include <linux/watchdog.h>
#include <linux/interrupt.h>

#define WWDTE		0x00
#define WDTA0MD		0xC
#define WSIZE_25P	0x0
#define WSIZE_50P	0x1
#define WSIZE_75P	0x2
#define WSIZE_100P	0x3
#define WDTA0ERM	BIT(2)
#define WDTA0WIE	BIT(3)
#define WDTA0OVF(x)	(((x) << 4) & GENMASK(6, 4))
#define WWDTE_KEY	0xAC

static bool nowayout = WATCHDOG_NOWAYOUT;
module_param(nowayout, bool, 0);
MODULE_PARM_DESC(nowayout, "Watchdog cannot be stopped once started (default="
					__MODULE_STRING(WATCHDOG_NOWAYOUT) ")");

static struct wwdt_priv {
	void __iomem *base;
	struct watchdog_device wdev;
	int interval_time;
	int error_mode;
};

static void wwdt_write(struct wwdt_priv *priv, u8 val, unsigned int reg)
{
	writeb(val, priv->base + reg);
}

static u8 wwdt_read(struct wwdt_priv *priv, int reg)
{
	return readb_relaxed(priv->base + reg);
}

static void wwdt_refresh_counter(struct watchdog_device *wdev)
{
	struct wwdt_priv *priv = watchdog_get_drvdata(wdev);

	wwdt_write(priv, 0xAC, WWDTE);
}

static void wwdt_setup(struct watchdog_device *wdev)
{
	struct wwdt_priv *priv = watchdog_get_drvdata(wdev);
	u8 val;

	/* Setup for WDTA0MD
	 * - Interval time mode
	 * - Window size selection: 100%
	 * - Reset mode
	 */
	val = wwdt_read(priv, WDTA0MD);
	if (!priv->error_mode)
		val &= ~WDTA0ERM;
	val |= (WDTA0OVF(priv->interval_time)) | WSIZE_100P;

	wwdt_write(priv, val, WDTA0MD);
}

static int wwdt_start(struct watchdog_device *wdev)
{
	struct wwdt_priv *priv = watchdog_get_drvdata(wdev);

	pm_runtime_get_sync(wdev->parent);

	wwdt_setup(wdev);
	wwdt_write(priv, 0xAC, WWDTE);
	/* Delay 1 RCLK */
	udelay(30);

	return 0;
}

static int wwdt_stop(struct watchdog_device *wdev)
{
	pm_runtime_put(wdev->parent);

	return 0;
}

static int wwdt_ping(struct watchdog_device *wdev)
{
	wwdt_refresh_counter(wdev);

	return 0;
}

static const struct watchdog_info wwdt_ident = {
	.options = WDIOF_MAGICCLOSE | WDIOF_KEEPALIVEPING,
	.identity = "Renesas Window WWDT Watchdog",
};

static const struct watchdog_ops wwdt_ops = {
	.owner = THIS_MODULE,
	.start = wwdt_start,
	.stop = wwdt_stop,
	.ping = wwdt_ping,
};

static int wwdt_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct wwdt_priv *priv;
	int ret;

	priv = devm_kzalloc(dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	priv->base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(priv->base))
		return PTR_ERR(priv->base);

	pm_runtime_enable(dev);
	priv->wdev.info = &wwdt_ident;
	priv->wdev.ops = &wwdt_ops;
	priv->wdev.parent = dev;

	platform_set_drvdata(pdev, priv);
	watchdog_set_drvdata(&priv->wdev, priv);

	ret = device_property_read_u32(dev, "interval-time", &priv->interval_time);
	if (ret)
		dev_warn(dev, "interval-time not found, defaulting to 0\n");

	ret = device_property_read_u32(dev, "error-mode", &priv->error_mode);
	if (ret) {
		dev_warn(dev, "error-mode not found, defaulting to 1\n");
		priv->error_mode = 1;
	}

	watchdog_set_nowayout(&priv->wdev, nowayout);
	watchdog_set_restart_priority(&priv->wdev, 0);
	watchdog_stop_on_unregister(&priv->wdev);

	ret = watchdog_register_device(&priv->wdev);
	if (ret < 0)
		pr_info("Fail to register device\n");

	dev_info(dev, "probed\n");
	return 0;
}

static int wwdt_remove(struct platform_device *pdev)
{
	struct wwdt_priv *priv = platform_get_drvdata(pdev);

	watchdog_unregister_device(&priv->wdev);
	pm_runtime_disable(&pdev->dev);
	return 0;
}

static const struct of_device_id renesas_wwdt_ids[] = {
	{ .compatible = "renesas, rcar-gen5-wwdt", },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, renesas_wwdt_ids);

static struct platform_driver renesas_wwdt_driver = {
	.driver = {
		.name = "renesas_wwdt",
		.of_match_table = renesas_wwdt_ids,
	},
	.probe = wwdt_probe,
	.remove = wwdt_remove,
};
module_platform_driver(renesas_wwdt_driver);

MODULE_DESCRIPTION("Renesas WWDT Window Watchdog Driver");
MODULE_LICENSE("GPL v2");
MODULE_AUTHOR("<Minh Le <minh.le.aj@renesas.com>");
