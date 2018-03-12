/* Power off driver for CETiBOX
 * Copyright (c) 2018, CETiTEC GmbH.  All rights reserved.
 *
 * based on imx-snvs-poweroff.c
 * based on msm-poweroff.c
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */

#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/reboot.h>
#include <linux/pm_runtime.h>

#define I2C_REG 5
#define I2C_VAL 1

#define I2C_CONF_REFRESH 0x79

extern int rcar_i2c_xfer_atomic(struct i2c_adapter *adap, struct i2c_msg *msgs,
								int num);

static struct i2c_adapter *i2c_adapt_cplds = NULL, *i2c_adapt_config = NULL;
static unsigned short wakecpld_addr, wakecpld_config_addr;
static struct platform_device *cetibox_poweroff_pdev = NULL;

extern void (*arm_pm_restart)(enum reboot_mode reboot_mode, const char *cmd);
static void (*orig_arm_pm_restart)(enum reboot_mode reboot_mode, const char *cmd) = NULL;

static void wakecpld_poweroff(void)
{
	int err;
	struct i2c_msg msgs[1];
	char data[2];

	if (!i2c_adapt_cplds) {
		dev_err(&cetibox_poweroff_pdev->dev, "wakecpld: Couldn't power off cpld\n");
		return;
	}

	dev_info(&cetibox_poweroff_pdev->dev, "Sending poweroff on bus %s addr %hu\n",
			 i2c_adapt_cplds->name, wakecpld_addr);
	data[0] = I2C_REG;
	data[1] = I2C_VAL;
	msgs[0].addr = wakecpld_addr;
	msgs[0].flags = 0;
	msgs[0].len = 2;
	msgs[0].buf = data;

	err = rcar_i2c_xfer_atomic(i2c_adapt_cplds, msgs, 1);

	if (unlikely(err < 0)) {
		dev_err(&cetibox_poweroff_pdev->dev, "%lu-bit %s failed at 0x%02x: %d\n", sizeof(data)*8,
				"write", I2C_REG, err);
	}
}

static void wakecpld_reconfigure(enum reboot_mode reboot_mode, const char *arg)
{
	int err;
	struct i2c_msg msgs[1];
	char data[4];

	if (!arg || strcmp(arg, "reload_cpld") != 0) {
		dev_info(&cetibox_poweroff_pdev->dev,
				 "Not reconfiguring CPLD because arg not set (%s)\n",
				 arg);
		goto reboot;
	}
	if (!i2c_adapt_config) {
		printk(KERN_ERR "wakecpld: Couldn't reconfigure cpld\n");
		goto reboot;
	}

	dev_info(&cetibox_poweroff_pdev->dev, "Sending refresh on bus %s addr %hu\n",
			 i2c_adapt_config->name, wakecpld_config_addr);
	data[0] = I2C_CONF_REFRESH;
	data[1] = 0;
	data[2] = 0;
	data[3] = 0;
	msgs[0].addr = wakecpld_config_addr;
	msgs[0].flags = 0;
	msgs[0].len = 4;
	msgs[0].buf = data;

	err = rcar_i2c_xfer_atomic(i2c_adapt_config, msgs, 1);

	if (unlikely(err < 0)) {
		dev_err(&cetibox_poweroff_pdev->dev, "%lu-bit %s failed at 0x%02x\n", sizeof(data)*8,
				"write", I2C_REG);
	}

  reboot:
	if (orig_arm_pm_restart)
		orig_arm_pm_restart(reboot_mode, arg);
}

static int cetibox_poweroff_probe(struct platform_device *pdev)
{
	int err;
	struct of_phandle_args args;
	struct device_node *np = pdev->dev.of_node;

	if (!i2c_adapt_cplds) {
		err = of_parse_phandle_with_fixed_args(np, "wakecpld", 1, 0, &args);
		i2c_adapt_cplds = of_get_i2c_adapter_by_node(args.np);
		of_node_put(args.np);
		if (!i2c_adapt_cplds) {
			return -EPROBE_DEFER;
		}
		wakecpld_addr = args.args[0];
		// FIXME: We can't wake up i2c controller in atomic context when
		// wakecpld_poweroff is called, so keep them awake always.
		pm_runtime_get_sync(&i2c_adapt_cplds->dev);
	}

	if (!i2c_adapt_config) {
		err = of_parse_phandle_with_fixed_args(np, "wakecpld-config", 1, 0, &args);
		i2c_adapt_config = of_get_i2c_adapter_by_node(args.np);
		of_node_put(args.np);
		if (!i2c_adapt_config) {
			return -EPROBE_DEFER;
		}
		wakecpld_config_addr = args.args[0];
		// FIXME: We can't wake up i2c controller in atomic context when
		// wakecpld_restart is called, so keep them awake always.
		pm_runtime_get_sync(&i2c_adapt_config->dev);
	}

	orig_arm_pm_restart = arm_pm_restart;
	cetibox_poweroff_pdev = pdev;
	arm_pm_restart = wakecpld_reconfigure;
	pm_power_off = wakecpld_poweroff;

	return 0;
}

static int cetibox_poweroff_remove(struct platform_device *pdev)
{
	if (pm_power_off == wakecpld_poweroff)
		pm_power_off = NULL;
	if (arm_pm_restart == wakecpld_reconfigure)
		arm_pm_restart = orig_arm_pm_restart;

	if (i2c_adapt_config) {
		pm_runtime_put(&i2c_adapt_config->dev);
		i2c_put_adapter(i2c_adapt_config);
	}
	if (i2c_adapt_cplds) {
		pm_runtime_put(&i2c_adapt_cplds->dev);
		i2c_put_adapter(i2c_adapt_cplds);
	}

	return 0;
}

static const struct of_device_id cetibox_poweroff_dt_ids[] = {
	{ .compatible = "cetitec,cetibox-poweroff" },
	{ },
};
MODULE_DEVICE_TABLE(of, cetibox_poweroff_dt_ids);

static struct platform_driver cetibox_poweroff_driver = {
	.driver = {
		.name = "cetibox-poweroff",
		.of_match_table = of_match_ptr(cetibox_poweroff_dt_ids),
	},
	.probe = cetibox_poweroff_probe,
	.remove = cetibox_poweroff_remove,
};
module_platform_driver(cetibox_poweroff_driver);
