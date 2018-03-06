/* Power off driver for i.mx6
 * Copyright (c) 2014, FREESCALE CORPORATION.  All rights reserved.
 *
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

#define I2C_REG 5
#define I2C_VAL 1

struct i2c_client *i2c_client = NULL;

static void wakecpld_poweroff(void)
{
	int err;

	if (!i2c_client) {
		printk(KERN_ERR "wakecpld: Couldn't power off cpld\n");
		return;
	}

	err = i2c_smbus_write_byte_data(i2c_client, I2C_REG, I2C_VAL);
	if (unlikely(err < 0))
		dev_err(&i2c_client->dev, "%d-bit %s failed at 0x%02x\n", 16, "write",
		        I2C_REG);
}

static int wakecpld_poweroff_probe(struct i2c_client *client,
                                   const struct i2c_device_id *id)
{
	i2c_client = client;
	pm_power_off = wakecpld_poweroff;

	return 0;
}

static int wakecpld_poweroff_remove(struct i2c_client *client)
{
	if (pm_power_off == wakecpld_poweroff)
		pm_power_off = NULL;

	return 0;
}

static const struct of_device_id wakecpld_dt_ids[] = {
	{ .compatible = "cetitec,wakecpld" },
	{ },
};
MODULE_DEVICE_TABLE(of, wakecpld_dt_ids);

static const struct i2c_device_id wakecpld_id[] = {
	{ "cetitec,wakecpld", 0 },
	{ },
};
MODULE_DEVICE_TABLE(i2c, wakecpld_id);

static struct i2c_driver wakecpld_poweroff_driver = {
	.driver = {
		.name = "wakecpld",
		.of_match_table = of_match_ptr(wakecpld_dt_ids),
	},
	.probe = wakecpld_poweroff_probe,
	.remove = wakecpld_poweroff_remove,
	.id_table = wakecpld_id,
};
module_i2c_driver(wakecpld_poweroff_driver);
