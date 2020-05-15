#ifndef __XENDRM_CACHE_DEBUG_H
#define __XENDRM_CACHE_DEBUG_H

#ifdef CONFIG_XENDRM_CACHE_DEBUG
void xendrm_cache_debug_flush(phys_addr_t paddr, size_t size);
#endif

#endif
