// SPDX-License-Identifier: GPL-2.0
/*
 * rcar_cluster_drv.c  --  R-Car Cluster driver
 *
 * Copyright (C) 2019-2023 EPAM Systems GmBh Corporation
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include <linux/module.h>
#include <linux/slab.h>
#include <linux/wait.h>
#include <linux/miscdevice.h>
#include <linux/rpmsg.h>
#include <linux/r_taurus_bridge.h>
#include <uapi/linux/r_taurus_cluster_protocol.h>

#define MAX_MISC_RPMSG	1

#define RPMSG_DEV_MAX	(MINORMASK + 1)

#define RCAR_CLUSTER_NAME     "rcar-cluster-drv"

#define CLUSTER_TAURUS_CHANNEL_ID	0x80

#define dev_to_clusterdev(dev) container_of(dev, struct rcar_cluster_device, dev)

static int rpmsg_cluster_probe(struct rpmsg_device *rpdev);
static int rpmsg_cluster_cb(struct rpmsg_device *rpdev, void *data, int len,
			void *priv, u32 src);
static void rpmsg_cluster_remove(struct rpmsg_device *rpdev);

static long misc_ioctl(struct file *p_file, unsigned int cmd, unsigned long arg);
static int misc_open(struct inode *inode, struct file *p_file);
static int misc_release(struct inode *inode, struct file *p_file);

static struct rpmsg_device_id rpmsg_driver_cluster_id_table[] = {
	{ .name	= "taurus-cluster" },
	{ },
};

struct taurus_cluster_res_msg {
	R_TAURUS_ResultMsg_t hdr;
};

struct taurus_event_list {
	uint32_t id;
	struct taurus_cluster_res_msg *result;
	struct list_head list;
	struct completion ack;
	bool ack_received;
	struct completion completed;
};

struct rcar_cluster_device {
	struct miscdevice dev;
	struct rpmsg_device *rpdev;
	struct list_head taurus_event_list_head;
	rwlock_t event_list_lock;
};

static struct rpmsg_driver rpmsg_cluster_drv = {
	.drv.name	= KBUILD_MODNAME,
	.drv.probe_type = PROBE_PREFER_ASYNCHRONOUS,
	.id_table	= rpmsg_driver_cluster_id_table,
	.probe		= rpmsg_cluster_probe,
	.callback	= rpmsg_cluster_cb,
	.remove		= rpmsg_cluster_remove,
};

static const struct file_operations misc_fops = {
	.owner = THIS_MODULE,
	.open = misc_open,
	.release = misc_release,
	.unlocked_ioctl = misc_ioctl,
};

static atomic_t rpmsg_id_counter = ATOMIC_INIT(0);

static int cluster_taurus_get_uniq_id(void)
{
	return atomic_inc_return(&rpmsg_id_counter);
}

static int send_msg(struct rpmsg_device *rpdev,
					uint16_t cmd,
					uint64_t value,
					struct taurus_cluster_res_msg *res_msg)
{
	int ret = 0;
	R_TAURUS_CmdMsg_t msg;
	struct taurus_event_list *event = NULL;
	struct rcar_cluster_device *clusterdrv = NULL;

	event = devm_kzalloc(&rpdev->dev, sizeof(*event), GFP_KERNEL);
	if (!event)
		return -ENOMEM;

	event->result = devm_kzalloc(&rpdev->dev, sizeof(*event->result), GFP_KERNEL);

	if (!event->result) {
		devm_kfree(&rpdev->dev, event);
		return -ENOMEM;
	}

	clusterdrv = (struct rcar_cluster_device *)dev_get_drvdata(&rpdev->dev);

	if (!clusterdrv) {
		dev_err(&rpdev->dev,
				"%s: Can't get data type rcar_cluster_device*\n",
				__func__);
		ret = -ENOMEM;
		goto end;
	}

	msg.Id = cluster_taurus_get_uniq_id();
	msg.Channel = CLUSTER_TAURUS_CHANNEL_ID;
	msg.Cmd = R_TAURUS_CMD_IOCTL;
	msg.Par1 = (uint64_t)cmd;
	msg.Par2 = value;
	msg.Par3 = 0;

	event->id = msg.Id;
	init_completion(&event->ack);
	init_completion(&event->completed);

	write_lock(&clusterdrv->event_list_lock);
	list_add(&event->list, &clusterdrv->taurus_event_list_head);
	write_unlock(&clusterdrv->event_list_lock);

	ret = rpmsg_send(rpdev->ept, &msg, sizeof(msg));

	if (ret) {
		dev_err(&rpdev->dev, "rpmsg_send failed: %d\n", ret);
		goto del_list;
	}
	do {
		ret = wait_for_completion_interruptible(&event->ack);
	} while (ret == -ERESTARTSYS);

	ret = wait_for_completion_interruptible(&event->completed);
	if (ret == -ERESTARTSYS) {
		dev_err(&rpdev->dev,
			"%s: Interrupted while waiting taurus response (%d)\n",
			__func__,
			ret);
		goto del_list;
	} else {
		memcpy(res_msg, event->result, sizeof(struct taurus_cluster_res_msg));
	}
del_list:
	write_lock(&clusterdrv->event_list_lock);
	list_del(&event->list);
	write_unlock(&clusterdrv->event_list_lock);

end:
	devm_kfree(&rpdev->dev, event->result);
	devm_kfree(&rpdev->dev, event);

	return ret;
}

/* -----------------------------------------------------------------------------
 * RPMSG operations
 */

static int rpmsg_cluster_cb(struct rpmsg_device *rpdev, void *data, int len,
			void *priv, u32 src)
{
	int ret = 0;
	struct taurus_event_list *event = NULL;
	struct taurus_cluster_res_msg *res = (struct taurus_cluster_res_msg *)data;
	struct list_head *i = NULL;
	struct rcar_cluster_device *clusterdrv =
			(struct rcar_cluster_device *)dev_get_drvdata(&rpdev->dev);
	uint32_t res_id = res->hdr.Id;

	if (!(res->hdr.Result == R_TAURUS_CMD_NOP && res_id == 0)) {
		/*send ACK message*/
		read_lock(&clusterdrv->event_list_lock);

		list_for_each_prev(i, &clusterdrv->taurus_event_list_head) {
			event = list_entry(i, struct taurus_event_list, list);
			if (event->id == res_id) {
				memcpy(event->result, data, len);
				if (event->ack_received) {
					dev_info(&rpdev->dev,
						"send_msg: Message completed (%d)\n",
						ret);
					complete(&event->completed);
				} else {
					event->ack_received = 1;
					complete(&event->ack);
				}
			}
		}
		read_unlock(&clusterdrv->event_list_lock);
	}
	return 0;
}

static int rpmsg_cluster_probe(struct rpmsg_device *rpdev)
{
	struct rcar_cluster_device *clusterdvc = NULL;
	int ret = 0;
	struct miscdevice *dev = NULL;

	dev_info(&rpdev->dev, "cluster: send_msg: probe\n");

	clusterdvc = devm_kzalloc(&rpdev->dev, sizeof(*clusterdvc), GFP_KERNEL);

	if (clusterdvc == NULL)
		return -ENOMEM;

	clusterdvc->rpdev = rpdev;
	dev = &clusterdvc->dev;
	dev->parent = &rpdev->dev;
	dev->minor = MISC_DYNAMIC_MINOR;
	dev->name = kstrdup("cluster-taurus", GFP_KERNEL);

	if (!dev->name) {
		ret = -ENOMEM;
		goto free_name;
	}

	dev->fops = &misc_fops;
	ret = misc_register(dev);

	dev_set_drvdata(&rpdev->dev, clusterdvc);

	INIT_LIST_HEAD(&clusterdvc->taurus_event_list_head);
	rwlock_init(&clusterdvc->event_list_lock);

	return ret;

free_name:
	kfree(dev->name);
	dev->name = NULL;

	devm_kfree(&rpdev->dev, clusterdvc);

	return ret;
}

static void rpmsg_cluster_remove(struct rpmsg_device *rpdev)
{
	struct rcar_cluster_device *data = dev_get_drvdata(&rpdev->dev);

	misc_deregister(&data->dev);

	kfree(data->dev.name);
	data->dev.name = NULL;

	devm_kfree(&rpdev->dev, data);
}


static int __init cluster_drv_init(void)
{
	int ret = 0;

	ret = register_rpmsg_driver(&rpmsg_cluster_drv);
	if (ret < 0) {
		pr_err("failed to register %s driver\n", __func__);
		return -EAGAIN;
	}

	return ret;
}
late_initcall(cluster_drv_init);

static void __exit cluster_drv_exit(void)
{
	unregister_rpmsg_driver(&rpmsg_cluster_drv);
}

static int misc_open(struct inode *inode, struct file *p_file)
{
	struct miscdevice *misc_device = p_file->private_data;
	struct rcar_cluster_device *cldev =
			container_of(misc_device, struct rcar_cluster_device, dev);

	p_file->private_data = cldev;

	return 0;
}

static int misc_release(struct inode *inode, struct file *p_file)
{
	return 0;
}

static long misc_ioctl(struct file *p_file, unsigned int cmd, unsigned long arg)
{
	static const unsigned int available_commands[] = {
		CLUSTER_SPEED,
		CLUSTER_GEAR,
		CLUSTER_RPM,
		CLUSTER_TURN,
		CLUSTER_DOOR_OPEN,
		CLUSTER_FOG_LIGHTS_BACK,
		CLUSTER_FOG_LIGHTS_FRONT,
		CLUSTER_HIGH_BEAMS_LIGHT,
		CLUSTER_HIGH_ENGINE_TEMPERATURE,
		CLUSTER_LOW_BATTERY,
		CLUSTER_LOW_BEAMS_LIGHTS,
		CLUSTER_LOW_FUEL,
		CLUSTER_LOW_OIL,
		CLUSTER_LOW_TIRE_PRESSURE,
		CLUSTER_SEAT_BELT,
		CLUSTER_SIDE_LIGHTS,
		CLUSTER_BATTERY_ISSUE,
		CLUSTER_AUTO_LIGHTING_ON,
		CLUSTER_ACTIVE
	};
	unsigned int index = 0;
	unsigned int len = ARRAY_SIZE(available_commands);
	struct rcar_cluster_device *cldev = p_file->private_data;
	struct taurus_cluster_res_msg res;

	/*verify of command is supported*/
	for (index = 0; index < len ; ++index) {
		if (available_commands[index] == cmd)
			break;
	}

	if (index == len)
		return -EINVAL;

	send_msg(cldev->rpdev, cmd, arg, &res);

	return (res.hdr.Result != R_TAURUS_RES_COMPLETE) ? -EIO : 0;
}

MODULE_DEVICE_TABLE(rpmsg, rpmsg_driver_cluster_id_table);

module_exit(cluster_drv_exit);
MODULE_ALIAS("rpmsg_cluster:rpmsg_chrdev");
MODULE_DESCRIPTION("Remote processor messaging cluster driver");
MODULE_LICENSE("GPL");
