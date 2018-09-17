#ifndef XT_CMA_HELPER_H
#define XT_CMA_HELPER_H

#include <asm/page.h>
#include <linux/module.h>

void xt_cma_helper_init(void);

unsigned long xt_cma_get_zeroed_page(gfp_t gfp_mask);
struct page *xt_cma_alloc_page(gfp_t gfp_mask);
void xt_cma_free_page(unsigned long addr);

void *xt_cma_dma_alloc_coherent(struct device *dev, size_t size,
				dma_addr_t *dma_handle, gfp_t gfp_mask);
void *xt_cma_dma_alloc_wc(struct device *dev, size_t size,
			  dma_addr_t *dma_handle, gfp_t gfp_mask);

void xt_cma_dma_free_coherent(struct device *dev, size_t size,
			      void *cpu_addr, dma_addr_t dma_handle);
void xt_cma_dma_free_wc(struct device *dev, size_t size,
			void *cpu_addr, dma_addr_t dma_addr);

#endif /* XT_CMA_HELPER_H */
