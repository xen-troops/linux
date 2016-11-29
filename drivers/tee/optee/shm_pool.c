/*
 * Copyright (c) 2015, Linaro Limited
 * Copyright (c) 2016, EPAM Systems
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
#include <linux/device.h>
#include <linux/dma-buf.h>
#include <linux/genalloc.h>
#include <linux/slab.h>
#include <linux/tee_drv.h>
#include "optee_private.h"
#include "optee_smc.h"
#include "shm_pool.h"

static int pool_op_alloc(struct tee_shm_pool_mgr *poolm,
			 struct tee_shm *shm, size_t size)
{
	unsigned int order = get_order(size);
	struct page *page;

	page = alloc_pages(GFP_KERNEL | __GFP_ZERO, order);
	if (!page)
		return -ENOMEM;

	shm->kaddr = page_address(page);
	shm->paddr = page_to_phys(page);
	shm->size = PAGE_SIZE << order;

	return 0;
}

static void pool_op_free(struct tee_shm_pool_mgr *poolm,
			     struct tee_shm *shm)
{
	free_pages((unsigned long)shm->kaddr, get_order(shm->size));
	shm->kaddr = NULL;
}

static const struct tee_shm_pool_mgr_ops pool_ops = {
	.alloc = pool_op_alloc,
	.free = pool_op_free,
};

static int pool_priv_mgr_init(struct tee_shm_pool_mgr *mgr, void *priv)
{
	mgr->ops = &pool_ops;
	mgr->private_data = priv;
	return 0;
}

static int pool_op_dma_alloc(struct tee_shm_pool_mgr *poolm,
			     struct tee_shm *shm, size_t size)
{
	unsigned long va;
	struct gen_pool *genpool = poolm->private_data;
	size_t s = roundup(size, 1 << genpool->min_alloc_order);

	va = gen_pool_alloc(genpool, s);
	if (!va)
		return -ENOMEM;

	memset((void *)va, 0, s);
	shm->kaddr = (void *)va;
	shm->paddr = gen_pool_virt_to_phys(genpool, va);
	shm->size = s;
	return 0;
}

static void pool_op_dma_free(struct tee_shm_pool_mgr *poolm,
			     struct tee_shm *shm)
{
	gen_pool_free(poolm->private_data, (unsigned long)shm->kaddr,
		      shm->size);
	shm->kaddr = NULL;
}

static const struct tee_shm_pool_mgr_ops pool_ops_dma = {
	.alloc = pool_op_dma_alloc,
	.free = pool_op_dma_free,
};

static void pool_destroy(struct tee_shm_pool *pool)
{
	gen_pool_destroy(pool->dma_buf_mgr.private_data);
}

static int pool_dma_mgr_init(struct tee_shm_pool_mgr *mgr,
				 struct tee_shm_pool_mem_info *info,
				 int min_alloc_order)
{
	size_t page_mask = PAGE_SIZE - 1;
	struct gen_pool *genpool = NULL;
	int rc;

	/*
	 * Start and end must be page aligned
	 */
	if ((info->vaddr & page_mask) || (info->paddr & page_mask) ||
	    (info->size & page_mask))
		return -EINVAL;

	genpool = gen_pool_create(min_alloc_order, -1);
	if (!genpool)
		return -ENOMEM;

	gen_pool_set_algo(genpool, gen_pool_best_fit, NULL);
	rc = gen_pool_add_virt(genpool, info->vaddr, info->paddr, info->size,
			       -1);
	if (rc) {
		gen_pool_destroy(genpool);
		return rc;
	}

	mgr->private_data = genpool;
	mgr->ops = &pool_ops_dma;
	return 0;
}

struct tee_shm_pool *
optee_shm_get_pool(struct tee_shm_pool_mem_info *dmabuf_info)
{
	struct tee_shm_pool *pool = NULL;
	int ret;

	pool = kzalloc(sizeof(*pool), GFP_KERNEL);
	if (!pool) {
		ret = -ENOMEM;
		goto err;
	}

	/*
	 * Create the pool for driver private shared memory
	 */
	ret = pool_priv_mgr_init(&pool->private_mgr, NULL);
	if (ret)
		goto err;

	/*
	 * Create the pool for dma_buf shared memory
	 */
	ret = pool_dma_mgr_init(&pool->dma_buf_mgr, dmabuf_info,
				PAGE_SHIFT);
	if (ret)
		goto err;

	pool->destroy = pool_destroy;
	return pool;
err:
	if (ret == -ENOMEM)
		pr_err("can't allocate memory for res_mem shared memory pool\n");
	kfree(pool);
	return ERR_PTR(ret);
}

void optee_shm_pool_free(struct tee_shm_pool *pool)
{
	pool->destroy(pool);
	kfree(pool);
}
