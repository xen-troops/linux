/*
 * drivers/gpu/rcar/rcar_ion.c
 *
 * Copyright (C) 2011 Google, Inc.
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
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

#define CONST_HEAP_ID

static int num_heaps;
static struct ion_heap **g_apsIonHeaps;
static struct platform_device *ion_pdev = NULL;
static struct ion_platform_heap **rcar_ion_heaps_data;
struct ion_device *g_psIonDev;
EXPORT_SYMBOL(g_psIonDev);

#include <linux/dma-mapping.h>
#include <linux/vmalloc.h>
#include <linux/uaccess.h>

enum {
	RCAR_ION_IOC_CUSTOM_ALLOC		= 0,
	RCAR_ION_IOC_CUSTOM_FREE		= 1,
	RCAR_ION_IOC_CUSTOM_GETPHYADDR		= 2,
	RCAR_ION_IOC_CUSTOM_GETIPADDR		= 3,
};

struct rcar_ion_getphys_data {
	int fd;
	uint64_t paddr;
};

struct ion_client {
	struct rb_node node;
	struct ion_device *dev;
	struct rb_root handles;
	struct idr idr;
	struct mutex lock;
	const char *name;
	struct task_struct *task;
	pid_t pid;
	struct dentry *debug_root;
};

struct ion_handle {
	struct kref ref;
	struct ion_client *client;
	struct ion_buffer *buffer;
	struct rb_node node;
	unsigned int kmap_cnt;
	int id;
};

static int rcar_ion_heap_allocate(struct ion_heap *heap,
							 struct ion_buffer *buffer,
							 unsigned long size,
							 unsigned long align,
							 unsigned long flags)
{
	void *vaddr;
	dma_addr_t dma_handle;

	dev_dbg(&ion_pdev->dev, "rcar_ion_heap_allocate: size=%lu, align=%lu, flags=0x%lx", size, align, flags);

	vaddr = dma_alloc_coherent(&ion_pdev->dev, size, &dma_handle, GFP_KERNEL);
	if (!vaddr)
		return -ENOMEM;

	buffer->priv_phys = dma_handle;
	buffer->size = size;
	buffer->vaddr = vaddr;

	return 0;
}

static void rcar_ion_heap_free(struct ion_buffer *buffer)
{
	dma_free_coherent(&ion_pdev->dev, buffer->size, buffer->vaddr, buffer->priv_phys);
}

static int rcar_ion_heap_phys(struct ion_heap *heap,
						struct ion_buffer *buffer,
						ion_phys_addr_t *addr, size_t *len)
{
	*addr = buffer->priv_phys;
	*len = buffer->size;
	return 0;
}

static struct sg_table *rcar_ion_map_dma(struct ion_heap *heap,
										 struct ion_buffer *buffer)
{
	struct sg_table *table;
	int ret;

	table = kzalloc(sizeof(struct sg_table), GFP_KERNEL);
	if (!table)
		return ERR_PTR(-ENOMEM);
	ret = sg_alloc_table(table, 1, GFP_KERNEL);
	if (ret) {
		kfree(table);
		return ERR_PTR(ret);
	}
	sg_set_page(table->sgl, virt_to_page(buffer->vaddr),
				buffer->size, 0);
	return table;
}

static void rcar_ion_unmap_dma(struct ion_heap *heap,
							   struct ion_buffer *buffer)
{
	sg_free_table(buffer->sg_table);
	kfree(buffer->sg_table);
}

static void *rcar_ion_map_kernel(struct ion_heap *heap,
								 struct ion_buffer *buffer)
{
	return (void *)buffer->vaddr;
}

static void rcar_ion_unmap_kernel(struct ion_heap *heap,
								  struct ion_buffer *buffer)
{
	return;
}

static int rcar_ion_map_user(struct ion_heap *heap,
						 struct ion_buffer *buffer,
						 struct vm_area_struct *vma)
{

	/* implemented with reference to the arm_dma_mmap */
	int ret = -ENXIO;
#ifdef CONFIG_MMU
	unsigned long nr_vma_pages =
		(vma->vm_end - vma->vm_start) >> PAGE_SHIFT;
	unsigned long nr_pages = PAGE_ALIGN(buffer->size) >> PAGE_SHIFT;
    unsigned long pfn = ((unsigned long)((buffer->priv_phys) >> PAGE_SHIFT) + ion_pdev->dev.dma_pfn_offset);
	unsigned long off = vma->vm_pgoff;

	if (off < nr_pages && nr_vma_pages <= (nr_pages - off)) {
		ret = remap_pfn_range(vma, vma->vm_start,
				pfn + off,
				vma->vm_end - vma->vm_start,
				vma->vm_page_prot);
	}
#endif  /* CONFIG_MMU */

	return ret;
}

static struct ion_heap_ops rcar_ion_heap_ops = {
	.allocate     = rcar_ion_heap_allocate,
	.free         = rcar_ion_heap_free,
	.phys         = rcar_ion_heap_phys,
	.map_dma      = rcar_ion_map_dma,
	.unmap_dma    = rcar_ion_unmap_dma,
	.map_kernel   = rcar_ion_map_kernel,
	.unmap_kernel = rcar_ion_unmap_kernel,
	.map_user     = rcar_ion_map_user,
};

static struct
ion_heap *rcar_ion_heap_create(struct ion_platform_heap *heap_data)
{
	struct ion_heap *heap = NULL;

	heap = kzalloc(sizeof(struct ion_heap), GFP_KERNEL);
	if (!heap)
		return ERR_PTR(-ENOMEM);

	if (IS_ERR_OR_NULL(heap)) {
		pr_err("%s: error creating heap %s type %d base %llu size %zu\n",
		       __func__, heap_data->name, heap_data->type,
		       (uint64_t)heap_data->base, heap_data->size);
		return ERR_PTR(-EINVAL);
	}

	heap->ops = &rcar_ion_heap_ops;
	heap->type = heap_data->type;
	heap->name = heap_data->name;
	heap->id = heap_data->id;

	return heap;
}

static void rcar_ion_heap_destroy(struct ion_heap *heap)
{
	if (!heap)
		return;

	kfree(heap);
}

static long rcar_ion_alloc(struct ion_client *client, unsigned long arg)
{
	struct ion_allocation_data data;
	struct ion_handle *handle;

	if (copy_from_user(&data, (void __user *)arg, sizeof(data)))
		return -EFAULT;

	handle = ion_alloc(client, data.len, data.align,
					   data.heap_id_mask, data.flags);

	if (IS_ERR(handle))
		return PTR_ERR(handle);

	data.handle = (struct ion_handle *)handle->id;

	if (copy_to_user((void __user *)arg, &data, sizeof(data))) {
		ion_free(client, handle);
		return -EFAULT;
	}

	return 0;
}

static struct ion_handle *rcar_ion_uhandle_get(struct ion_client *client, int id)
{
	return idr_find(&client->idr, id);
}

static long rcar_ion_free(struct ion_client *client, unsigned long arg)
{
	struct ion_handle_data data;
	struct ion_handle *handle;

	if (copy_from_user(&data, (void __user *)arg,
			   sizeof(struct ion_handle_data)))
		return -EFAULT;

	mutex_lock(&client->lock);
	handle = rcar_ion_uhandle_get(client, (int)data.handle);
	mutex_unlock(&client->lock);

	if (!handle)
		return -EINVAL;

	ion_free(client, handle);

	return 0;
}

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

	handle = ion_import_dma_buf(client, data.fd);
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

static long rcar_ion_ioctl(struct ion_client *client, unsigned int cmd,
					unsigned long arg)
{
	switch (cmd) {
	case RCAR_ION_IOC_CUSTOM_ALLOC:
		return rcar_ion_alloc(client, arg);
	case RCAR_ION_IOC_CUSTOM_FREE:
		return rcar_ion_free(client, arg);
	case RCAR_ION_IOC_CUSTOM_GETPHYADDR:
		return rcar_ion_get_phys_addr(client, arg);
	default:
		pr_err("%s: Unknown custom ioctl\n", __func__);
	return -ENOTTY;
	}
	return 0;
}

static int rcar_ion_set_platform_data(struct platform_device *pdev)
{
	unsigned int id;
	const char *heap_name;
	enum ion_heap_type type = ION_HEAP_TYPE_CUSTOM;
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

#ifndef CONST_HEAP_ID
		ret = of_property_read_u32(np, "heap-id", &id);
		if (ret < 0) {
			pr_err("check the id %s\n", np->name);
			continue;
		}
#else
		id = ION_HEAP_TYPE_CUSTOM;
#endif

		p_data = devm_kzalloc(&pdev->dev,
				      sizeof(struct ion_platform_heap),
				      GFP_KERNEL);
		if (!p_data)
			return -ENOMEM;

		p_data->name = heap_name;
		p_data->base = 0;
		p_data->size = 0;
		p_data->id = id;
		p_data->type = type;

		pr_info("heap index %d : name %s base 0x%lx size 0x%lx id %d type %d\n",
			index, p_data->name, p_data->base, p_data->size, p_data->id, p_data->type);

		rcar_ion_heaps_data[index] = p_data;
		index++;
	}
	return 0;
}

int rcar_ion_probe(struct platform_device *pdev)
{
	struct ion_platform_data *pdata = pdev->dev.platform_data;
	int err;
	int i;

	/* Create the ion devicenode */
	g_psIonDev = ion_device_create(rcar_ion_ioctl);
	if (IS_ERR_OR_NULL(g_psIonDev))
		return -ENOMEM;

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

		if (psPlatHeapData->type == ION_HEAP_TYPE_CUSTOM)
			g_apsIonHeaps[i] = rcar_ion_heap_create(psPlatHeapData);
		else
			g_apsIonHeaps[i] = ion_heap_create(psPlatHeapData);

		if (IS_ERR_OR_NULL(g_apsIonHeaps[i])) {
			err = PTR_ERR(g_apsIonHeaps[i]);
			goto failHeapCreate;
		}
		ion_device_add_heap(g_psIonDev, g_apsIonHeaps[i]);
	}

	ion_pdev = pdev;

	platform_set_drvdata(pdev, g_psIonDev);

	dev_info(&pdev->dev, "Ion initialized!\n");

 	return 0;

failHeapCreate:
	for (i = 0; i < num_heaps; i++) {
		if (g_apsIonHeaps[i]) {
			if (pdata->heaps[i].type == ION_HEAP_TYPE_CUSTOM)
				rcar_ion_heap_destroy(g_apsIonHeaps[i]);
			else
				ion_heap_destroy(g_apsIonHeaps[i]);
		}
	}
	kfree(g_apsIonHeaps);
	ion_device_destroy(g_psIonDev);

	return -ENOMEM;
}

int rcar_ion_remove(struct platform_device *pdev)
{
	struct ion_platform_data *pdata = pdev->dev.platform_data;
	int i;

	for (i = 0; i < num_heaps; i++) {
		if (g_apsIonHeaps[i]) {
			if (pdata->heaps[i].type == ION_HEAP_TYPE_CUSTOM)
				rcar_ion_heap_destroy(g_apsIonHeaps[i]);
			else
				ion_heap_destroy(g_apsIonHeaps[i]);
		}
	}
	ion_device_destroy(g_psIonDev);
	kfree(g_apsIonHeaps);
	ion_pdev = NULL;

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
