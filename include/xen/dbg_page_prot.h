#ifndef _XEN_DBG_PAGE_PROT_H
#define _XEN_DBG_PAGE_PROT_H

void xen_dump_prot(u64 prot);
void xen_dump_page_prot(struct page *page);

bool xen_page_prot_check_init(struct page *page);
bool xen_page_prot_check_next(struct page *page);

#endif
