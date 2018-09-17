#include <linux/genalloc.h>
#include <linux/memblock.h>
#include <linux/slab.h>

#include <xen/xen.h>
#include <xen/xt_cma_helper.h>
#include <xen/mem-reservation.h>
#include <xen/grant_table.h>

/* The size of the boot memory in MiB for page allocator. */
static int xt_cma_helper_bootmem_page_pool_sz __initdata = SZ_64M;

/* The size of the boot memory in MiB for CMA allocator. */
static int xt_cma_helper_bootmem_cma_pool_sz __initdata = SZ_128M;

/* Memory pool for non-CMA allocations (page pool). */
static phys_addr_t xt_cma_helper_bootmem_page_pool_phys;
static struct gen_pool *xt_cma_helper_bootmem_page_pool;

/* Memory pool for CMA allocations. */
static phys_addr_t xt_cma_helper_bootmem_cma_pool_phys;
static struct gen_pool *xt_cma_helper_bootmem_cma_pool;

static int __init xt_cma_helper_bootmem_page_setup(char *p)
{
	xt_cma_helper_bootmem_page_pool_sz = memparse(p, &p);
	return 0;
}
early_param("xt_page_pool", xt_cma_helper_bootmem_page_setup);

static int __init xt_cma_helper_bootmem_cma_setup(char *p)
{
	xt_cma_helper_bootmem_cma_pool_sz = memparse(p, &p);
	return 0;
}
early_param("xt_cma", xt_cma_helper_bootmem_cma_setup);

void __init xt_cma_helper_init(void)
{
	if (!xen_domain())
		return;

	xt_cma_helper_bootmem_page_pool_phys =
		memblock_alloc_base(xt_cma_helper_bootmem_page_pool_sz,
				    SZ_2M, MEMBLOCK_ALLOC_ANYWHERE);

	xt_cma_helper_bootmem_cma_pool_phys =
		memblock_alloc_base(xt_cma_helper_bootmem_cma_pool_sz,
				    SZ_2M, MEMBLOCK_ALLOC_ANYWHERE);

	printk("Allocated %d bytes for Xen page allocator at 0x%llx",
	       xt_cma_helper_bootmem_page_pool_sz,
	       xt_cma_helper_bootmem_page_pool_phys);

	printk("Allocated %d bytes for Xen CMA allocator at 0x%llx",
	       xt_cma_helper_bootmem_cma_pool_sz,
	       xt_cma_helper_bootmem_cma_pool_phys);
}

static void create_page_alloc_pools(void)
{
	void *vaddr = phys_to_virt(xt_cma_helper_bootmem_page_pool_phys);
	int ret;

	/* Page pool. */
	xt_cma_helper_bootmem_page_pool =
		gen_pool_create(PAGE_SHIFT, -1);
	BUG_ON(!xt_cma_helper_bootmem_page_pool);

	gen_pool_set_algo(xt_cma_helper_bootmem_page_pool,
			  gen_pool_best_fit, NULL);
	ret = gen_pool_add_virt(xt_cma_helper_bootmem_page_pool,
				(unsigned long)vaddr,
				xt_cma_helper_bootmem_page_pool_phys,
				xt_cma_helper_bootmem_page_pool_sz, -1);
	BUG_ON(ret);

	/* CMA pool. */
	vaddr = phys_to_virt(xt_cma_helper_bootmem_cma_pool_phys);

	xt_cma_helper_bootmem_cma_pool =
		gen_pool_create(PAGE_SHIFT, -1);
	BUG_ON(!xt_cma_helper_bootmem_cma_pool);

	gen_pool_set_algo(xt_cma_helper_bootmem_cma_pool,
			  gen_pool_best_fit, NULL);
	ret = gen_pool_add_virt(xt_cma_helper_bootmem_cma_pool,
				(unsigned long)vaddr,
				xt_cma_helper_bootmem_cma_pool_phys,
				xt_cma_helper_bootmem_cma_pool_sz, -1);
	BUG_ON(ret);
}

struct page *xt_cma_alloc_page(gfp_t gfp_mask)
{
	void *va;

	/*
	 * FIXME: this is first called from xen_guest_init which is
	 * an early_init call. We can also install an early_init
	 * for the pool creation below, but cannot guarantee it runs
	 * before xen_guest_init.
	 */
	if (unlikely(!xt_cma_helper_bootmem_page_pool))
		create_page_alloc_pools();

	va = (void *)gen_pool_alloc(xt_cma_helper_bootmem_page_pool,
				    PAGE_SIZE);
	if (IS_ERR(va))
		return va;

	return virt_to_page(va);
}

unsigned long xt_cma_get_zeroed_page(gfp_t gfp_mask)
{
	void *va = page_to_virt(xt_cma_alloc_page(gfp_mask));

	memset(va, 0, PAGE_SIZE);
	return (unsigned long)va;
}

void xt_cma_free_page(unsigned long addr)
{
	gen_pool_free(xt_cma_helper_bootmem_page_pool,
		      addr, PAGE_SIZE);
}

static int xt_cma_alloc_pages(gfp_t gfp_mask, int count,
			      struct page **pages)
{
	int i;

	/*
	 * Alocate pages one by one - mimic what balloon driver does:
	 * this gives a possibilyty then to free individual pages which
	 * is a problem if we allocate all pages at once from the pool.
	 */
	for (i = 0; i < count; i++) {
		pages[i] = xt_cma_alloc_page(gfp_mask);
		if (IS_ERR(pages[i]))
			goto fail;
	}
	return 0;

fail:
	for (; i >=0; i--)
		xt_cma_free_page((unsigned long)page_to_virt(pages[i]));
	return -ENOMEM;
}

static void xt_cma_free_pages(struct page **pages, int count)
{
	int i;

	for (i = 0; i < count; i++)
		xt_cma_free_page((unsigned long)page_to_virt(pages[i]));
}

void *xt_cma_dma_alloc_coherent(struct device *dev, size_t size,
				dma_addr_t *dma_handle, gfp_t gfp_mask)
{
	void *va;

	va = (void *)gen_pool_alloc(xt_cma_helper_bootmem_cma_pool,
				    ALIGN(size, PAGE_SIZE));
	if (IS_ERR(va))
		return va;

	*dma_handle = virt_to_phys(va);
	return va;
}
EXPORT_SYMBOL(xt_cma_dma_alloc_coherent);

void *xt_cma_dma_alloc_wc(struct device *dev, size_t size,
			  dma_addr_t *dma_handle, gfp_t gfp_mask)
{
	return xt_cma_dma_alloc_coherent(dev, size, dma_handle, gfp_mask);
}
EXPORT_SYMBOL(xt_cma_dma_alloc_wc);

void xt_cma_dma_free_coherent(struct device *dev, size_t size,
			      void *cpu_addr, dma_addr_t dma_handle)
{
	gen_pool_free(xt_cma_helper_bootmem_cma_pool,
		      (unsigned long)cpu_addr, ALIGN(size, PAGE_SIZE));
}
EXPORT_SYMBOL(xt_cma_dma_free_coherent);

void xt_cma_dma_free_wc(struct device *dev, size_t size,
			void *cpu_addr, dma_addr_t dma_handle)
{
	xt_cma_dma_free_coherent(dev, size, cpu_addr, dma_handle);
}
EXPORT_SYMBOL(xt_cma_dma_free_wc);

int alloc_xenballooned_pages(int nr_pages, struct page **pages)
{
	xen_pfn_t *frames;
	int i, ret;

	ret = xt_cma_alloc_pages(GFP_KERNEL, nr_pages, pages);
	if (ret < 0)
		return ret;

	frames = kcalloc(nr_pages, sizeof(*frames), GFP_KERNEL);
	if (!frames) {
		pr_debug("Failed to allocate frames to decrease reservation\n");
		ret = -ENOMEM;
		goto fail;
	}

	for (i = 0; i < nr_pages; i++) {
		struct page *page = pages[i];

		frames[i] = xen_page_to_gfn(page);
		xenmem_reservation_scrub_page(page);
	}

	xenmem_reservation_va_mapping_reset(nr_pages, pages);

	ret = xenmem_reservation_decrease(nr_pages, frames);
	if (ret != nr_pages) {
		pr_debug("Failed to decrease reservation for pages\n");
		ret = -EFAULT;
		goto fail;
	}

	ret = gnttab_pages_set_private(nr_pages, pages);
	if (ret < 0)
		goto fail;

	kfree(frames);
	return 0;

fail:
	xt_cma_free_pages(pages, nr_pages);
	kfree(frames);
	return ret;
}
EXPORT_SYMBOL(alloc_xenballooned_pages);

void free_xenballooned_pages(int nr_pages, struct page **pages)
{
	xen_pfn_t *frames;
	int i, ret;

	gnttab_pages_clear_private(nr_pages, pages);

	frames = kcalloc(nr_pages, sizeof(*frames), GFP_KERNEL);
	if (!frames) {
		pr_debug("Failed to allocate frames to increase reservation\n");
		return;
	}

	for (i = 0; i < nr_pages; i++)
		frames[i] = xen_page_to_gfn(pages[i]);

	ret = xenmem_reservation_increase(nr_pages, frames);
	if (ret != nr_pages)
		pr_debug("Failed to increase reservation for pages\n");

	xenmem_reservation_va_mapping_update(nr_pages, pages, frames);

	xt_cma_free_pages(pages, nr_pages);

	kfree(frames);
}
EXPORT_SYMBOL(free_xenballooned_pages);

