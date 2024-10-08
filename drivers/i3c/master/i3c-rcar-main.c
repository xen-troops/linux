// SPDX-License-Identifier: GPL-2.0

#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>
#include <linux/io.h>

#include "i3c-rcar.h"

inline void i3c_reg_update(u32 mask, u32 val, u32 __iomem *reg)
{
	u32 data = readl(reg);

	data &= ~mask;
	data |= (val & mask);
	writel(data, reg);
}

inline u32 i3c_reg_read(void __iomem *base, u32 offset)
{
	return readl(base + (offset));
}

inline void i3c_reg_write(void __iomem *base, u32 offset, u32 val)
{
	writel(val, base + (offset));
}

void i3c_reg_set_bit(void __iomem *base, u32 reg, u32 val)
{
	i3c_reg_update(val, val, base + (reg));
}

void i3c_reg_clear_bit(void __iomem *base, u32 reg, u32 val)
{
	i3c_reg_update(val, 0, base + (reg));
}

void i3c_reg_update_bit(void __iomem *base, u32 reg,
				  u32 mask, u32 val)
{
	i3c_reg_update(mask, val, base + (reg));
}

static bool rcar_i3c_is_master(struct device *dev)
{
	const char *mode = NULL;

	device_property_read_string(dev, "mode", &mode);

	if (!mode)
		return true;

	if (strncmp(mode, "target", 6) == 0)
		return false;

	return true;
}

static int rcar_i3c_probe(struct platform_device *pdev)
{
	if (rcar_i3c_is_master(&pdev->dev))
		return rcar_i3c_master_probe(pdev);

	return rcar_i3c_target_probe(pdev);
}

static int rcar_i3c_remove(struct platform_device *pdev)
{
	if (rcar_i3c_is_master(&pdev->dev))
		return rcar_i3c_master_remove(pdev);

	return rcar_i3c_target_remove(pdev);
}

static const struct of_device_id rcar_i3c_master_of_ids[] = {
	{ .compatible = "renesas,rcar-i3c-master"},
	{ /* sentinel */ },
};

MODULE_DEVICE_TABLE(of, rcar_i3c_master_of_match);

static struct platform_driver rcar_i3c_master_driver = {
	.probe = rcar_i3c_probe,
	.remove = rcar_i3c_remove,
	.driver = {
		.name = "rcar-i3c-master",
		.of_match_table = rcar_i3c_master_of_ids,
	},
};
module_platform_driver(rcar_i3c_master_driver);
