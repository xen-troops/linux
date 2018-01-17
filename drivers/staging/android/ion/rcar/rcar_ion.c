/*
 * Copyright (C) 2016 GlobalLogic
 */

#include <linux/err.h>
#include "../ion.h"
#include <linux/platform_device.h>
#include <linux/slab.h>
#include "../ion_priv.h"
#include <linux/module.h>
#include <linux/idr.h>
#include <linux/miscdevice.h>
#include <linux/of.h>

static struct ion_heap **g_apsIonHeaps;

struct ion_device *g_psIonDev;
EXPORT_SYMBOL(g_psIonDev);

#include <linux/dma-mapping.h>
#include <linux/vmalloc.h>
#include <linux/uaccess.h>

static struct ion_platform_heap rcar_ion_heaps[] = {
        {
            .id = ION_HEAP_TYPE_SYSTEM,
            .type   = ION_HEAP_TYPE_SYSTEM,
            .name   = "rcar_system",
        },
        {
            .id = ION_HEAP_TYPE_DMA,
            .type   = ION_HEAP_TYPE_DMA,
            .name   = "rcar_cma",
        },
};

static int num_heaps = ARRAY_SIZE(rcar_ion_heaps);

enum {
    RCAR_ION_IOC_CUSTOM_GETPHYADDR = 1,
};

struct rcar_ion_getphys_data {
    int fd;
    uint64_t paddr;
};

static long rcar_ion_get_phys_addr(struct ion_client *client, unsigned long arg)
{
    struct rcar_ion_getphys_data data;
    size_t len;
    struct ion_handle *handle;
    int err;
    ion_phys_addr_t paddr = 0;

    if (copy_from_user(&data, (void __user *)arg,
            sizeof(struct rcar_ion_getphys_data)))
        return -EFAULT;

    handle = ion_import_dma_buf_fd(client, data.fd);
    if (IS_ERR(handle))
        return PTR_ERR(handle);

    err = ion_phys(client, handle, &paddr, &len);
    data.paddr = (uint64_t)paddr;
    ion_free(client, handle);
    if (err)
        return err;

    if (copy_to_user((void __user *)arg, &data,
                sizeof(struct rcar_ion_getphys_data)))
        return -EFAULT;

    return err;
}

static
long rcar_custom_ioctl(struct ion_client *client,
                    unsigned int cmd, unsigned long arg)
{
    switch (cmd) {
        case RCAR_ION_IOC_CUSTOM_GETPHYADDR:
            return rcar_ion_get_phys_addr(client, arg);
        default:
            pr_err("%s: Unknown custom ioctl: cmd=%d, arg=%lu\n", __func__, cmd, arg);
        return -ENOTTY;
    }
    return 0;
}

int rcar_ion_probe(struct platform_device *pdev)
{
	int i, err;

	/* Create the ion devicenode */
	g_psIonDev = ion_device_create(rcar_custom_ioctl);
	if (IS_ERR_OR_NULL(g_psIonDev))
		return -ENOMEM;

	platform_set_drvdata(pdev, g_psIonDev);

	g_apsIonHeaps = devm_kzalloc(&pdev->dev, sizeof(struct ion_heap *) * num_heaps, GFP_KERNEL);
	if (!g_apsIonHeaps) {
		ion_device_destroy(g_psIonDev);
		return -ENOMEM;
	}

	/* Register all the heaps */
	for (i = 0; i < num_heaps; i++) {
		rcar_ion_heaps[i].priv = &pdev->dev;
		g_apsIonHeaps[i] = ion_heap_create(&rcar_ion_heaps[i]);

		if (IS_ERR_OR_NULL(g_apsIonHeaps[i])) {
			err = PTR_ERR(g_apsIonHeaps[i]);
			goto failHeapCreate;
		}
		ion_device_add_heap(g_psIonDev, g_apsIonHeaps[i]);
		dev_info(&pdev->dev, "ion heap: name %s id %d type %d\n",
			rcar_ion_heaps[i].name, rcar_ion_heaps[i].id, rcar_ion_heaps[i].type);
	}

	dev_info(&pdev->dev, "Ion initialized!\n");
 	return 0;

failHeapCreate:
	for (i = 0; i < num_heaps; i++) {
		if (g_apsIonHeaps[i]) {
			ion_heap_destroy(g_apsIonHeaps[i]);
		}
	}
	kfree(g_apsIonHeaps);
	ion_device_destroy(g_psIonDev);

	return -ENOMEM;
}

int rcar_ion_remove(struct platform_device *pdev)
{
	int i;

	for (i = 0; i < num_heaps; i++) {
		if (g_apsIonHeaps[i]) {
			ion_heap_destroy(g_apsIonHeaps[i]);
		}
	}
	ion_device_destroy(g_psIonDev);
	kfree(g_apsIonHeaps);
	return 0;
}

static const struct of_device_id rcar_ion_of_table[] = {
        { .compatible = "renesas,ion-rcar", },
        { },
};
MODULE_DEVICE_TABLE(of, rcar_ion_of_table);

static struct platform_driver ion_driver = {
	.probe = rcar_ion_probe,
	.remove = rcar_ion_remove,
	.driver = {
		.name = "rcar-ion",
		.of_match_table = rcar_ion_of_table,
		.owner = THIS_MODULE
	 },
};

module_platform_driver(ion_driver);
MODULE_ALIAS("platform:ion-rcar");
