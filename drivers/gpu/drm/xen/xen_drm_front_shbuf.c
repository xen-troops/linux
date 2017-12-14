/*
 *  Xen para-virtual DRM device
 *
 *   This program is free software; you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation; either version 2 of the License, or
 *   (at your option) any later version.
 *
 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 *
 * Copyright (C) 2016-2017 EPAM Systems Inc.
 *
 * Author: Oleksandr Andrushchenko <oleksandr_andrushchenko@epam.com>
 */

#include "xen_drm_front_shbuf.h"

#include <linux/errno.h>
#include <linux/mm.h>

#include <asm/xen/hypervisor.h>
#include <xen/balloon.h>
#include <xen/xen.h>
#include <xen/xenbus.h>
#include <xen/interface/io/ring.h>
#include <xen/interface/io/displif.h>

#include "xen_drm_balloon.h"
#include "xen_drm_front_drv.h"

grant_ref_t xen_drm_front_shbuf_get_dir_start(
	struct xen_drm_front_shbuf *buf)
{
	if (!buf->grefs)
		return GRANT_INVALID_REF;

	return buf->grefs[0];
}

struct xen_drm_front_shbuf *xen_drm_front_shbuf_get_by_dbuf_cookie(
	struct list_head *dbuf_list, uint64_t dbuf_cookie)
{
	struct xen_drm_front_shbuf *buf, *q;

	list_for_each_entry_safe(buf, q, dbuf_list, list) {
		if (buf->dbuf_cookie == dbuf_cookie)
			return buf;
	}
	return NULL;
}

void xen_drm_front_shbuf_flush_fb(struct list_head *dbuf_list,
	uint64_t fb_cookie)
{
#if defined(CONFIG_X86)
	struct xen_drm_front_shbuf *buf, *q;

	list_for_each_entry_safe(buf, q, dbuf_list, list) {
		if (buf->fb_cookie == fb_cookie)
			drm_clflush_pages(buf->pages, buf->num_pages);
	}
#endif
}

#define xen_page_to_vaddr(page) \
	((phys_addr_t)pfn_to_kaddr(page_to_xen_pfn(page)))

/*
 * number of grefs a page can hold with respect to the
 * struct xendispl_page_directory header
 */
#define XEN_DRM_NUM_GREFS_PER_PAGE ((XEN_PAGE_SIZE - \
	offsetof(struct xendispl_page_directory, gref)) / \
	sizeof(grant_ref_t))

int xen_drm_front_shbuf_be_alloc_map(
	struct xen_drm_front_shbuf *buf)
{
	struct gnttab_map_grant_ref *map_ops = NULL;
	unsigned char *ptr;
	int ret, cur_gref, cur_dir_page, cur_page, grefs_left;

	map_ops = kcalloc(buf->num_pages, sizeof(*map_ops), GFP_KERNEL);
	if (!map_ops)
		return -ENOMEM;

	buf->be_alloc_map_handles = kcalloc(buf->num_pages,
		sizeof(*buf->be_alloc_map_handles), GFP_KERNEL);
	if (!buf->be_alloc_map_handles) {
		kfree(map_ops);
		return -ENOMEM;
	}

	/*
	 * read page directory to get grefs from the backend: for external
	 * buffer we only allocate buf->grefs for the page directory,
	 * so buf->num_grefs has number of pages in the page directory itself
	 */
	ptr = buf->vdirectory;
	grefs_left = buf->num_pages;
	cur_page = 0;
	for (cur_dir_page = 0; cur_dir_page < buf->num_grefs; cur_dir_page++) {
		struct xendispl_page_directory *page_dir =
			(struct xendispl_page_directory *)ptr;
		int to_copy = XEN_DRM_NUM_GREFS_PER_PAGE;

		if (to_copy > grefs_left)
			to_copy = grefs_left;

		for (cur_gref = 0; cur_gref < to_copy; cur_gref++) {
			phys_addr_t addr;

			addr = xen_page_to_vaddr(buf->pages[cur_page]);
			gnttab_set_map_op(&map_ops[cur_page], addr,
				GNTMAP_host_map, page_dir->gref[cur_gref],
				buf->xb_dev->otherend_id);
			cur_page++;
		}

		grefs_left -= to_copy;
		ptr += XEN_PAGE_SIZE;
	}
	ret = gnttab_map_refs(map_ops, NULL, buf->pages, buf->num_pages);
	BUG_ON(ret);

	/* save handles so we can unmap on free */
	for (cur_page = 0; cur_page < buf->num_pages; cur_page++) {
		buf->be_alloc_map_handles[cur_page] = map_ops[cur_page].handle;
		if (unlikely(map_ops[cur_page].status != GNTST_okay))
			DRM_ERROR("Failed to map page %d: %d\n",
				cur_page, map_ops[cur_page].status);
	}

	kfree(map_ops);
	return 0;
}

static int shbuf_be_alloc_unmap(struct xen_drm_front_shbuf *buf)
{
	struct gnttab_unmap_grant_ref *unmap_ops;
	int i;

	if (!buf->pages || !buf->be_alloc_map_handles)
		return 0;

	unmap_ops = kcalloc(buf->num_pages, sizeof(*unmap_ops),
		GFP_KERNEL);
	if (!unmap_ops) {
		DRM_ERROR("Failed to get memory while unmapping\n");
		return -ENOMEM;
	}

	for (i = 0; i < buf->num_pages; i++) {
		phys_addr_t addr;

		addr = xen_page_to_vaddr(buf->pages[i]);
		gnttab_set_unmap_op(&unmap_ops[i], addr, GNTMAP_host_map,
			buf->be_alloc_map_handles[i]);
	}

	BUG_ON(gnttab_unmap_refs(unmap_ops, NULL, buf->pages,
		buf->num_pages));

	for (i = 0; i < buf->num_pages; i++) {
		if (unlikely(unmap_ops[i].status != GNTST_okay))
			DRM_ERROR("Failed to unmap page %d: %d\n",
				i, unmap_ops[i].status);
	}

	kfree(unmap_ops);
	kfree(buf->be_alloc_map_handles);
	buf->be_alloc_map_handles = NULL;
	return 0;
}

static void shbuf_free(struct xen_drm_front_shbuf *buf)
{
	int i;

	if (buf->grefs) {
		if (buf->be_alloc)
			shbuf_be_alloc_unmap(buf);

		for (i = 0; i < buf->num_grefs; i++)
			if (buf->grefs[i] != GRANT_INVALID_REF)
				gnttab_end_foreign_access(buf->grefs[i],
					0, 0UL);
	}
	kfree(buf->grefs);
	kfree(buf->vdirectory);
	if (buf->sgt) {
		sg_free_table(buf->sgt);
		drm_free_large(buf->pages);
	}
	kfree(buf);
}

void xen_drm_front_shbuf_free_by_dbuf_cookie(struct list_head *dbuf_list,
	uint64_t dbuf_cookie)
{
	struct xen_drm_front_shbuf *buf, *q;

	list_for_each_entry_safe(buf, q, dbuf_list, list) {
		if (buf->dbuf_cookie == dbuf_cookie) {
			list_del(&buf->list);
			shbuf_free(buf);
			break;
		}
	}
}

void xen_drm_front_shbuf_free_all(struct list_head *dbuf_list)
{
	struct xen_drm_front_shbuf *buf, *q;

	list_for_each_entry_safe(buf, q, dbuf_list, list) {
		list_del(&buf->list);
		shbuf_free(buf);
	}
}

static void shbuf_fill_page_dir_be_alloc(
	struct xen_drm_front_shbuf *buf, int num_pages_dir)
{
	struct xendispl_page_directory *page_dir;
	unsigned char *ptr;
	int i;

	ptr = buf->vdirectory;

	/* fill only grefs for the page directory itself */
	for (i = 0; i < num_pages_dir - 1; i++) {
		page_dir = (struct xendispl_page_directory *)ptr;

		page_dir->gref_dir_next_page = buf->grefs[i + 1];
		ptr += XEN_PAGE_SIZE;
	}
	/* last page must say there is no more pages */
	page_dir = (struct xendispl_page_directory *)ptr;
	page_dir->gref_dir_next_page = GRANT_INVALID_REF;
}

static void shbuf_fill_page_dir(struct xen_drm_front_shbuf *buf,
	int num_pages_dir)
{
	unsigned char *ptr;
	int cur_gref, grefs_left, to_copy, i;

	ptr = buf->vdirectory;

	/*
	 * while copying, skip grefs at start, they are for pages
	 * granted for the page directory itself
	 */
	cur_gref = num_pages_dir;
	grefs_left = buf->num_pages;
	for (i = 0; i < num_pages_dir; i++) {
		struct xendispl_page_directory *page_dir =
			(struct xendispl_page_directory *)ptr;

		if (grefs_left <= XEN_DRM_NUM_GREFS_PER_PAGE) {
			to_copy = grefs_left;
			page_dir->gref_dir_next_page = GRANT_INVALID_REF;
		} else {
			to_copy = XEN_DRM_NUM_GREFS_PER_PAGE;
			page_dir->gref_dir_next_page = buf->grefs[i + 1];
		}
		memcpy(&page_dir->gref, &buf->grefs[cur_gref],
			to_copy * sizeof(grant_ref_t));
		ptr += XEN_PAGE_SIZE;
		grefs_left -= to_copy;
		cur_gref += to_copy;
	}
}

static int shbuf_grant_refs(struct xen_drm_front_shbuf *buf,
	int num_pages_dir)
{
	grant_ref_t priv_gref_head;
	int ret, i, j, cur_ref;
	int otherend_id;

	ret = gnttab_alloc_grant_references(buf->num_grefs, &priv_gref_head);
	if (ret < 0) {
		DRM_ERROR("Cannot allocate grant references\n");
		return ret;
	}
	otherend_id = buf->xb_dev->otherend_id;
	j = 0;
	for (i = 0; i < num_pages_dir; i++) {
		cur_ref = gnttab_claim_grant_reference(&priv_gref_head);
		if (cur_ref < 0)
			return cur_ref;
		gnttab_grant_foreign_access_ref(cur_ref, otherend_id,
			xen_page_to_gfn(virt_to_page(buf->vdirectory +
				XEN_PAGE_SIZE * i)), 0);
		buf->grefs[j++] = cur_ref;
	}
	if (!buf->be_alloc)
		/* also claim grant references for the pages of the buffer */
		for (i = 0; i < buf->num_pages; i++) {
			cur_ref = gnttab_claim_grant_reference(&priv_gref_head);
			if (cur_ref < 0)
				return cur_ref;
			gnttab_grant_foreign_access_ref(cur_ref,
				otherend_id, xen_page_to_gfn(buf->pages[i]), 0);
			buf->grefs[j++] = cur_ref;
		}
	gnttab_free_grant_references(priv_gref_head);
	return 0;
}

static int shbuf_alloc_storage(struct xen_drm_front_shbuf *buf,
	int num_pages_dir)
{
	if (buf->sgt) {
		buf->pages = drm_malloc_ab(buf->num_pages,
			sizeof(struct page *));
		if (!buf->pages)
			return -ENOMEM;

		if (drm_prime_sg_to_page_addr_arrays(buf->sgt, buf->pages,
				NULL, buf->num_pages) < 0)
			return -EINVAL;
	}

	buf->grefs = kcalloc(buf->num_grefs, sizeof(*buf->grefs), GFP_KERNEL);
	if (!buf->grefs)
		return -ENOMEM;

	buf->vdirectory = kcalloc(num_pages_dir, XEN_PAGE_SIZE, GFP_KERNEL);
	if (!buf->vdirectory)
		return -ENOMEM;

	return 0;
}

struct xen_drm_front_shbuf *xen_drm_front_shbuf_alloc(
	struct xen_drm_front_shbuf_alloc *info)
{
	struct xen_drm_front_shbuf *buf;
	int num_pages_dir;

	/* either pages or sgt, not both */
	BUG_ON(info->pages && info->sgt);

	buf = kzalloc(sizeof(*buf), GFP_KERNEL);
	if (!buf)
		return NULL;

	buf->xb_dev = info->xb_dev;
	buf->dbuf_cookie = info->dbuf_cookie;
	buf->be_alloc = info->be_alloc;
	buf->num_pages = DIV_ROUND_UP(info->size, PAGE_SIZE);
	buf->sgt = info->sgt;
	buf->pages = info->pages;

	/* number of pages the page directory consumes itself */
	num_pages_dir = DIV_ROUND_UP(buf->num_pages,
		XEN_DRM_NUM_GREFS_PER_PAGE);

	if (buf->be_alloc)
		buf->num_grefs = num_pages_dir;
	else
		buf->num_grefs = num_pages_dir + buf->num_pages;

	if (shbuf_alloc_storage(buf, num_pages_dir) < 0)
		goto fail;

	if (shbuf_grant_refs(buf, num_pages_dir) < 0)
		goto fail;

	if (buf->be_alloc)
		shbuf_fill_page_dir_be_alloc(buf, num_pages_dir);
	else
		shbuf_fill_page_dir(buf, num_pages_dir);

	list_add(&buf->list, info->dbuf_list);
	return buf;

fail:
	shbuf_free(buf);
	return NULL;
}
