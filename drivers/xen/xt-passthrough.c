// SPDX-License-Identifier: GPL-2.0

/* Copyright 2021 EPAM Systems Inc. */

#include <linux/clk.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/of_device.h>
#include <linux/reset.h>

struct xt_passthrough_priv {
	struct clk **clk;
	struct reset_control **rst;
};

static unsigned int of_reset_get_parent_count(struct device_node *np)
{
	int count;

	count = of_count_phandle_with_args(np, "resets", "#reset-cells");
	if (count < 0)
		return 0;

	return count;
}

static unsigned int of_clk_get_parent_count(struct device_node *np)
{
	int count;

	count = of_count_phandle_with_args(np, "clocks", "#clock-cells");
	if (count < 0)
		return 0;

	return count;
}

static int enable_clocks(struct device *dev, struct xt_passthrough_priv *priv)
{
	struct device_node *np = dev_of_node(dev);
	int i, cnt, ret;

	cnt = of_clk_get_parent_count(dev->of_node);
	if (!cnt)
		return 0;

	priv->clk = devm_kcalloc(dev, cnt, sizeof(struct clk *), GFP_KERNEL);
	if (!priv->clk)
		return -ENOMEM;

	for (i = 0; i < cnt; i++) {
		priv->clk[i] = of_clk_get(np, i);
		if (IS_ERR(priv->clk[i])) {
			ret = PTR_ERR(priv->clk[i]);
			dev_err(dev, "failed to get clk index: %d ret: %d\n",
				i, ret);
			priv->clk[i] = NULL;
			goto err_put_clks;
		}
	}

	for (i = 0; i < cnt; i++)
	{
		ret = clk_prepare_enable(priv->clk[i]);
		if (ret) {
			dev_err(dev, "failed to prepare clock, ret %d\n", ret);
			goto err_disable_clks;
		}
	}

	dev_info(dev, "enabled %d clock(s)\n", cnt);
	return 0;

err_disable_clks:
	while (--i >= 0)
		clk_disable_unprepare(priv->clk[i]);
err_put_clks:
	while (--cnt >= 0)
		if (priv->clk[i])
			clk_put(priv->clk[i]);
	return ret;
}

static int enable_resets(struct device *dev, struct xt_passthrough_priv *priv)
{
	int i, cnt, ret;

	cnt = of_reset_get_parent_count(dev->of_node);
	if (!cnt)
		return 0;

	priv->rst = devm_kcalloc(dev, cnt, sizeof(struct reset_control *),
				 GFP_KERNEL);
	if (!priv->rst)
		return -ENOMEM;

	for (i = 0; i < cnt; i++) {
		priv->rst[i] = devm_reset_control_get_shared_by_index(dev, i);
		if (IS_ERR(priv->rst[i])) {
			ret = PTR_ERR(priv->rst[i]);
			if (ret == -EPROBE_DEFER)
				goto err_reset;
			priv->rst[i] = NULL;
			break;
		}
		ret = reset_control_deassert(priv->rst[i]);
		if (ret)
			goto err_reset;
	}

	dev_info(dev, "de-asserted %d reset(s)\n", cnt);
	return 0;

err_reset:
	dev_err(dev, "failed to de-assert resets, ret %d\n", ret);
	while (--priv->rst >= 0)
		reset_control_assert(priv->rst[i]);
	return ret;
}

static int xt_passthrough_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct xt_passthrough_priv *priv;
	int ret;

	dev_info(dev, "Xen-troops passtrough helper driver\n");

	priv = devm_kzalloc(&pdev->dev, sizeof(struct xt_passthrough_priv),
			    GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	ret = enable_clocks(dev, priv);
	if (ret)
		return ret;
	ret = enable_resets(dev, priv);
	if (ret)
		return ret;

	platform_set_drvdata(pdev, priv);
	return 0;
}

static const struct of_device_id xt_passthrough_match_table[] = {
	{ .compatible = "xen-troops,passthrough", },
	{ /* sentinel */ }
};

MODULE_DEVICE_TABLE(of, xt_passthrough_match_table);

static struct platform_driver xt_passthrough_driver = {
	.driver = {
		.name		= "xt-passthrough",
		.of_match_table	= xt_passthrough_match_table,
	},
	.probe = xt_passthrough_probe,
};

module_platform_driver(xt_passthrough_driver);

MODULE_DESCRIPTION("Xen-troops passthrough helper driver");
MODULE_LICENSE("GPL v2");
