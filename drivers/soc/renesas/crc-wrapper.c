/*************************************************************************/ /*
 CRC Wrapper (kernel module)
*/ /*************************************************************************/

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/string.h>
#include <linux/fs.h>
#include <linux/uaccess.h>
#include <linux/dma-mapping.h>
#include <linux/slab.h>
#include <linux/device.h>
#include <linux/of_device.h>
#include <linux/types.h>
#include <linux/of_address.h>
#include <linux/of_reserved_mem.h>

#include "crc-wrapper.h"
#include "crc-drv.h"
#include "kcrc-drv.h"

/* Define global variable */
DEFINE_MUTEX(lock);

struct wcrc_device {
    struct device *dev;
    void __iomem *base;
};

#define DEVNAME "crc-wrapper"

static unsigned int wcrc_major;
static struct class *wcrc_class = NULL;

static int wcrc_independent_crc(struct wcrc_info *info)
{
    int ret;

    mutex_lock(&lock);

    if (info->wcrc_indp_opt == INDEPENDENT_CRC) {
        ret = crc_calculate(info);
    } else { //INDEPENDENT_KCRC mode
        ret = kcrc_calculate(info);
    }

    mutex_unlock(&lock);

    if (ret)
        pr_err("Calculation Aborted!, ERR: %d", ret);

    return 0;
};

static int wcrc_open(struct inode *inode, struct file *filep)
{
    struct wcrc_info *p_access;

    p_access = kzalloc(sizeof(*p_access), GFP_KERNEL);
    if (!p_access)
        return -ENOMEM;

    pr_debug("Device Open\n");
    filep->private_data = p_access;

	return 0;
}

static int wcrc_release(struct inode *inode, struct file *filep)
{
    struct wcrc_info *p_access;

    p_access = filep->private_data;
    kfree(p_access);
    pr_debug("Device Release\n");

	return 0;
}

static long dev_ioctl(struct file *filep, unsigned int cmd, unsigned long arg)
{
    struct wcrc_info u_features;
    int ret = 0;

    switch (cmd) {
    case INDEPENDENT_CRC_MODE :
        ret = copy_from_user(&u_features, (struct wcrc_info *)arg, sizeof(u_features));
        if (ret)
            return ret;

        wcrc_independent_crc(&u_features);

        ret = copy_to_user((struct wcrc_info *)arg, &u_features, sizeof(u_features));
        break;
    case E2E_CRC_MODE :
        break;
    case DATA_THROUGH_MODE :
        break;
    case E2E_CRC_DATA_THROUGH_MODE :
        break;
    default:
        pr_warn("## wcrc: unknown ioctl command; %d\n", cmd);
        return -EINVAL;
    }
    return ret;
};

static const struct of_device_id wcrc_of_ids[] = {
    {
        .compatible = "renesas,crc-wrapper",
    }, {
        .compatible = "renesas,wcrc-r8a78000",
    }, {
        /* Terminator */
    },
};

static struct file_operations fops = {
	.owner = THIS_MODULE,
	.open = wcrc_open,
	.release = wcrc_release,
	.unlocked_ioctl = dev_ioctl,
};

static int wcrc_probe(struct platform_device *pdev)
{
    struct wcrc_device *wcrc;
    struct resource *res;

    wcrc = devm_kzalloc(&pdev->dev, sizeof(*wcrc), GFP_KERNEL);
    if (!wcrc) {
        dev_err(&pdev->dev, "cannot allocate device data\n");
        return -ENOMEM;
    }

    wcrc->dev = &pdev->dev;

    /* Map I/O memory and request IRQ. */
	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	wcrc->base = devm_ioremap_resource(&pdev->dev, res);
	if (IS_ERR(wcrc->base))
		return PTR_ERR(wcrc->base);

	wcrc_major = register_chrdev(0, "CRC-Wrapper", &fops);
	if (wcrc_major < 0)
		pr_err("wcrc: Failed to register device\n");

	wcrc_class = class_create(THIS_MODULE, "wcrc");

	device_create(wcrc_class, NULL, MKDEV(wcrc_major, 6), NULL, DEVNAME);

    return 0;
};

static int wcrc_remove(struct platform_device *pdev)
{
	device_destroy(wcrc_class, wcrc_major);
	class_destroy(wcrc_class);
	unregister_chrdev(wcrc_major, "CRC-Wrapper");

    return 0;
}

static struct platform_driver wcrc_driver = {
    .driver = {
        .name = DEVNAME,
        .of_match_table = of_match_ptr(wcrc_of_ids),
        .owner = THIS_MODULE,
    },
    .probe = wcrc_probe,
    .remove = wcrc_remove,
};

static int __init wcrc_init(void)
{
    struct device_node *np;
    int ret;

    np = of_find_matching_node(NULL, wcrc_of_ids);
    if (!np)
        return 0;

    of_node_put(np);

    ret = platform_driver_register(&wcrc_driver);
    if (ret < 0)
        return ret;

    return 0;
};
subsys_initcall(wcrc_init);