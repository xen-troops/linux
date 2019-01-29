#include <asm/pgtable.h>
#include <linux/mm.h>

struct prot_bits {
	u64		mask;
	u64		val;
	const char	*set;
	const char	*clear;
};

static const struct prot_bits pte_bits[] = {
	{
		.mask	= PTE_VALID,
		.val	= PTE_VALID,
		.set	= " ",
		.clear	= "F",
	}, {
		.mask	= PTE_USER,
		.val	= PTE_USER,
		.set	= "USR",
		.clear	= "   ",
	}, {
		.mask	= PTE_RDONLY,
		.val	= PTE_RDONLY,
		.set	= "ro",
		.clear	= "RW",
	}, {
		.mask	= PTE_PXN,
		.val	= PTE_PXN,
		.set	= "NX",
		.clear	= "x ",
	}, {
		.mask	= PTE_SHARED,
		.val	= PTE_SHARED,
		.set	= "SHD",
		.clear	= "   ",
	}, {
		.mask	= PTE_AF,
		.val	= PTE_AF,
		.set	= "AF",
		.clear	= "  ",
	}, {
		.mask	= PTE_NG,
		.val	= PTE_NG,
		.set	= "NG",
		.clear	= "  ",
	}, {
		.mask	= PTE_CONT,
		.val	= PTE_CONT,
		.set	= "CON",
		.clear	= "   ",
	}, {
		.mask	= PTE_TABLE_BIT,
		.val	= PTE_TABLE_BIT,
		.set	= "   ",
		.clear	= "BLK",
	}, {
		.mask	= PTE_UXN,
		.val	= PTE_UXN,
		.set	= "UXN",
	}, {
		.mask	= PTE_ATTRINDX_MASK,
		.val	= PTE_ATTRINDX(MT_DEVICE_nGnRnE),
		.set	= "DEVICE/nGnRnE",
	}, {
		.mask	= PTE_ATTRINDX_MASK,
		.val	= PTE_ATTRINDX(MT_DEVICE_nGnRE),
		.set	= "DEVICE/nGnRE",
	}, {
		.mask	= PTE_ATTRINDX_MASK,
		.val	= PTE_ATTRINDX(MT_DEVICE_GRE),
		.set	= "DEVICE/GRE",
	}, {
		.mask	= PTE_ATTRINDX_MASK,
		.val	= PTE_ATTRINDX(MT_NORMAL_NC),
		.set	= "MEM/NORMAL-NC",
	}, {
		.mask	= PTE_ATTRINDX_MASK,
		.val	= PTE_ATTRINDX(MT_NORMAL),
		.set	= "MEM/NORMAL",
	}
};

enum ENTRY_TYPE {
	ENTRY_BAD,
	ENTRY_PUD,
	ENTRY_PMD,
	ENTRY_PTE
};

static int get_kernel_page_prot(struct page *page, u64 *prot)
{
	pgd_t *pgd;
	pud_t *pud;
	pmd_t *pmd;
	pte_t *pte;
	unsigned long addr = (unsigned long)page_address(page);

	pgd = pgd_offset_k(addr);
	if (pgd_none(*pgd))
		return ENTRY_BAD;

	pud = pud_offset(pgd, addr);
	if (pud_none(*pud))
		return ENTRY_BAD;
	if (pud_sect(*pud)) {
		*prot = pud_val(*pud);
		return ENTRY_PUD;
	}

	pmd = pmd_offset(pud, addr);
	if (pmd_none(*pmd))
		return ENTRY_BAD;
	if (pmd_sect(*pmd)) {
		*prot = pmd_val(*pmd);
		return ENTRY_PMD;
	}

	pte = pte_offset_kernel(pmd, addr);
	if (!pte_valid(*pte))
		return ENTRY_BAD;
	*prot = pte_val(*pte);
	return ENTRY_PTE;
}

void xen_dump_prot(u64 prot)
{
	const struct prot_bits *bits = pte_bits;
	int i;
	char str[128];

	sprintf(str, "Raw prot %llx ", prot);
	for (i = 0; i < ARRAY_SIZE(pte_bits); i++, bits++) {
		const char *s;

		if ((prot & bits->mask) == bits->val)
			s = bits->set;
		else
			s = bits->clear;

		if (s) {
			strcat(str, s);
			strcat(str, " ");
		}

	}
	printk("%s\n", str);
}
EXPORT_SYMBOL(xen_dump_prot);

void xen_dump_page_prot(struct page *page)
{
	int entry;
	u64 prot;

	entry = get_kernel_page_prot(page, &prot);
	if (entry == ENTRY_BAD) {
		printk("Page at %llx is not valid\n", page_to_phys(page));
		return;
	}
	printk("%s Page at %llx\n",
	       entry == ENTRY_PUD ? "PUD" : entry == ENTRY_PMD ? "PMD" : "PTE",
	       page_to_phys(page));
	xen_dump_prot(prot);
}
EXPORT_SYMBOL(xen_dump_page_prot);

static bool xen_page_get_prot(struct page *page, u64 *prot)
{
	int entry;

	entry = get_kernel_page_prot(page, prot);
	if (entry == ENTRY_BAD)
		return false;
	return true;
}

static u64 xen_page_prot_val;

bool xen_page_prot_check_init(struct page *page)
{
	u64 prot;

	if (!xen_page_get_prot(page, &prot)) {
		printk("Failed to initialize page range prot check\n");
		return false;
	}
	xen_page_prot_val = prot & PTE_ATTRINDX_MASK;
}
EXPORT_SYMBOL(xen_page_prot_check_init);

bool xen_page_prot_check_next(struct page *page)
{
	u64 prot;

	if (!xen_page_get_prot(page, &prot)) {
		printk("Failed to get page prot for check\n");
		return false;
	}
	if (xen_page_prot_val == (prot & PTE_ATTRINDX_MASK))
		return true;
	return false;
}
EXPORT_SYMBOL(xen_page_prot_check_next);

