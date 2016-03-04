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

static int num_heaps;
static struct ion_heap **g_apsIonHeaps;
static struct ion_platform_heap **rcar_ion_heaps_data;

struct ion_device *g_psIonDev;
EXPORT_SYMBOL(g_psIonDev);

#include <linux/dma-mapping.h>
#include <linux/vmalloc.h>
#include <linux/uaccess.h>


static int rcar_ion_set_platform_data(struct platform_device *pdev)
{
	const char *heap_name;
	int ret;
	struct device_node *np;
	struct ion_platform_heap *p_data;
	const struct device_node *dt_node = pdev->dev.of_node;
	int index = 0;

	for_each_child_of_node(dt_node, np)
		num_heaps++;

	rcar_ion_heaps_data = devm_kzalloc(&pdev->dev,
				  sizeof(struct ion_platform_heap *) *
				  num_heaps,
				  GFP_KERNEL);
	if (!rcar_ion_heaps_data)
		return -ENOMEM;

	for_each_child_of_node(dt_node, np) {
		ret = of_property_read_string(np, "heap-name", &heap_name);
		if (ret < 0) {
			pr_err("check the name of node %s\n", np->name);
			continue;
		}

		p_data = devm_kzalloc(&pdev->dev,
				      sizeof(struct ion_platform_heap),
				      GFP_KERNEL);
		if (!p_data)
			return -ENOMEM;

		p_data->priv = &pdev->dev;
		p_data->name = heap_name;
		p_data->base = 0;
		p_data->size = 0;

		p_data->id = ION_HEAP_TYPE_DMA;
		p_data->type = ION_HEAP_TYPE_DMA;

		printk("ion: heap index %d : name %s base 0x%lx size 0x%lx id %d type %d\n",
			index, p_data->name, p_data->base, p_data->size, p_data->id, p_data->type);

		rcar_ion_heaps_data[index] = p_data;
		index++;
	}
	return 0;
}

int rcar_ion_probe(struct platform_device *pdev)
{
	int i, err;

	/* Create the ion devicenode */
	g_psIonDev = ion_device_create(NULL);
	if (IS_ERR_OR_NULL(g_psIonDev))
		return -ENOMEM;

	platform_set_drvdata(pdev, g_psIonDev);

	err = rcar_ion_set_platform_data(pdev);
	if (err) {
		pr_err("ion set platform data error!\n");
		ion_device_destroy(g_psIonDev);
		return err;
	}

	g_apsIonHeaps = devm_kzalloc(&pdev->dev, sizeof(struct ion_heap *) * num_heaps, GFP_KERNEL);
	if (!g_apsIonHeaps) {
		ion_device_destroy(g_psIonDev);
		return -ENOMEM;
	}

	/* Register all the heaps */
	for (i = 0; i < num_heaps; i++) {
		struct ion_platform_heap *psPlatHeapData = rcar_ion_heaps_data[i];

		g_apsIonHeaps[i] = ion_heap_create(psPlatHeapData);

		if (IS_ERR_OR_NULL(g_apsIonHeaps[i])) {
			err = PTR_ERR(g_apsIonHeaps[i]);
			goto failHeapCreate;
		}
		ion_device_add_heap(g_psIonDev, g_apsIonHeaps[i]);
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
