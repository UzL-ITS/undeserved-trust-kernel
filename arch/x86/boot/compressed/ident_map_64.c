// SPDX-License-Identifier: GPL-2.0
/*
 * This code is used on x86_64 to create page table identity mappings on
 * demand by building up a new set of page tables (or appending to the
 * existing ones), and then switching over to them when ready.
 *
 * Copyright (C) 2015-2016  Yinghai Lu
 * Copyright (C)      2016  Kees Cook
 */

/*
 * Since we're dealing with identity mappings, physical and virtual
 * addresses are the same, so override these defines which are ultimately
 * used by the headers in misc.h.
 */
#define __pa(x)  ((unsigned long)(x))
#define __va(x)  ((void *)((unsigned long)(x)))

/* No PAGE_TABLE_ISOLATION support needed either: */
#undef CONFIG_PAGE_TABLE_ISOLATION

#include "error.h"
#include "misc.h"

/* These actually do the work of building the kernel identity maps. */
#include <asm/init.h>
#include <asm/pgtable.h>
#include <asm/trap_defs.h>
#include <asm/cmpxchg.h>
/* Use the static base for this part of the boot process */
#undef __PAGE_OFFSET
#define __PAGE_OFFSET __PAGE_OFFSET_BASE
#include "../../mm/ident_map.c"

#ifdef CONFIG_X86_5LEVEL
unsigned int __pgtable_l5_enabled;
unsigned int pgdir_shift = 39;
unsigned int ptrs_per_p4d = 1;
#endif

/* Used by PAGE_KERN* macros: */
pteval_t __default_kernel_pte_mask __read_mostly = ~0;

/* Used by pgtable.h asm code to force instruction serialization. */
unsigned long __force_order;

/* Used to track our page table allocation area. */
struct alloc_pgt_data {
	unsigned char *pgt_buf;
	unsigned long pgt_buf_size;
	unsigned long pgt_buf_offset;
};

/*
 * Allocates space for a page table entry, using struct alloc_pgt_data
 * above. Besides the local callers, this is used as the allocation
 * callback in mapping_info below.
 */
static void *alloc_pgt_page(void *context)
{
	struct alloc_pgt_data *pages = (struct alloc_pgt_data *)context;
	unsigned char *entry;

	/* Validate there is space available for a new page. */
	if (pages->pgt_buf_offset >= pages->pgt_buf_size) {
		debug_putstr("out of pgt_buf in " __FILE__ "!?\n");
		debug_putaddr(pages->pgt_buf_offset);
		debug_putaddr(pages->pgt_buf_size);
		return NULL;
	}

	entry = pages->pgt_buf + pages->pgt_buf_offset;
	pages->pgt_buf_offset += PAGE_SIZE;

	return entry;
}

/* Used to track our allocated page tables. */
static struct alloc_pgt_data pgt_data;

/* The top level page table entry pointer. */
static unsigned long top_level_pgt;

phys_addr_t physical_mask = (1ULL << __PHYSICAL_MASK_SHIFT) - 1;

/*
 * Mapping information structure passed to kernel_ident_mapping_init().
 * Due to relocation, pointers must be assigned at run time not build time.
 */
static struct x86_mapping_info mapping_info;

/*
 * Adds the specified range to the identity mappings.
 */
static void add_identity_map(unsigned long start, unsigned long end)
{
	int ret;

	/* Align boundary to 2M. */
	start = round_down(start, PMD_SIZE);
	end = round_up(end, PMD_SIZE);
	if (start >= end)
		return;

	/* Build the mapping. */
	ret = kernel_ident_mapping_init(&mapping_info, (pgd_t *)top_level_pgt, start, end);
	if (ret)
		error("Error: kernel_ident_mapping_init() failed\n");
}

/* Locates and clears a region for a new top level page table. */
void initialize_identity_maps(void)
{
	/* Exclude the encryption mask from __PHYSICAL_MASK */
	physical_mask &= ~sme_me_mask;

	/* Init mapping_info with run-time function/buffer pointers. */
	mapping_info.alloc_pgt_page = alloc_pgt_page;
	mapping_info.context = &pgt_data;
	mapping_info.page_flag = __PAGE_KERNEL_LARGE_EXEC | sme_me_mask;
	mapping_info.kernpg_flag = _KERNPG_TABLE;

	/*
	 * It should be impossible for this not to already be true,
	 * but since calling this a second time would rewind the other
	 * counters, let's just make sure this is reset too.
	 */
	pgt_data.pgt_buf_offset = 0;

	/*
	 * If we came here via startup_32(), cr3 will be _pgtable already
	 * and we must append to the existing area instead of entirely
	 * overwriting it.
	 *
	 * With 5-level paging, we use '_pgtable' to allocate the p4d page table,
	 * the top-level page table is allocated separately.
	 *
	 * p4d_offset(top_level_pgt, 0) would cover both the 4- and 5-level
	 * cases. On 4-level paging it's equal to 'top_level_pgt'.
	 */
	top_level_pgt = read_cr3_pa();
	if (p4d_offset((pgd_t *)top_level_pgt, 0) == (p4d_t *)_pgtable) {
		pgt_data.pgt_buf = _pgtable + BOOT_INIT_PGT_SIZE;
		pgt_data.pgt_buf_size = BOOT_PGT_SIZE - BOOT_INIT_PGT_SIZE;
		memset(pgt_data.pgt_buf, 0, pgt_data.pgt_buf_size);
	} else {
		pgt_data.pgt_buf = _pgtable;
		pgt_data.pgt_buf_size = BOOT_PGT_SIZE;
		memset(pgt_data.pgt_buf, 0, pgt_data.pgt_buf_size);
		top_level_pgt = (unsigned long)alloc_pgt_page(&pgt_data);
	}

	/*
	 * New page-table is set up - map the kernel image and load it
	 * into cr3.
	 */
	add_identity_map((unsigned long)_head, (unsigned long)_end);
	write_cr3(top_level_pgt);
}

static pte_t *split_large_pmd(struct x86_mapping_info *info,
			      pmd_t *pmdp, unsigned long __address)
{
	unsigned long page_flags;
	unsigned long address;
	pte_t *pte;
	pmd_t pmd;
	int i;

	pte = (pte_t *)info->alloc_pgt_page(info->context);
	if (!pte)
		return NULL;

	address     = __address & PMD_MASK;
	/* No large page - clear PSE flag */
	page_flags  = info->page_flag & ~_PAGE_PSE;

	/* Populate the PTEs */
	for (i = 0; i < PTRS_PER_PMD; i++) {
		set_pte(&pte[i], __pte(address | page_flags));
		address += PAGE_SIZE;
	}

	/*
	 * Ideally we need to clear the large PMD first and do a TLB
	 * flush before we write the new PMD. But the 2M range of the
	 * PMD might contain the code we execute and/or the stack
	 * we are on, so we can't do that. But that should be safe here
	 * because we are going from large to small mappings and we are
	 * also the only user of the page-table, so there is no chance
	 * of a TLB multihit.
	 */
	pmd = __pmd((unsigned long)pte | info->kernpg_flag);
	set_pmd(pmdp, pmd);
	/* Flush TLB to establish the new PMD */
	write_cr3(top_level_pgt);

	return pte + pte_index(__address);
}

static void clflush_page(unsigned long address)
{
	unsigned int flush_size;
	char *cl, *start, *end;

	/*
	 * Hardcode cl-size to 64 - CPUID can't be used here because that might
	 * cause another #VC exception and the GHCB is not ready to use yet.
	 */
	flush_size = 64;
	start      = (char *)(address & PAGE_MASK);
	end        = start + PAGE_SIZE;

	/*
	 * First make sure there are no pending writes on the cache-lines to
	 * flush.
	 */
	asm volatile("mfence" : : : "memory");

	for (cl = start; cl != end; cl += flush_size)
		clflush(cl);
}

static int __set_page_decrypted(struct x86_mapping_info *info,
				unsigned long address)
{
	unsigned long scratch, *target;
	pgd_t *pgdp = (pgd_t *)top_level_pgt;
	p4d_t *p4dp;
	pud_t *pudp;
	pmd_t *pmdp;
	pte_t *ptep, pte;

	/*
	 * First make sure there is a PMD mapping for 'address'.
	 * It should already exist, but keep things generic.
	 *
	 * To map the page just read from it and fault it in if there is no
	 * mapping yet. add_identity_map() can't be called here because that
	 * would unconditionally map the address on PMD level, destroying any
	 * PTE-level mappings that might already exist.  Also do something
	 * useless with 'scratch' so the access won't be optimized away.
	 */
	target = (unsigned long *)address;
	scratch = *target;
	arch_cmpxchg(target, scratch, scratch);

	/*
	 * The page is mapped at least with PMD size - so skip checks and walk
	 * directly to the PMD.
	 */
	p4dp = p4d_offset(pgdp, address);
	pudp = pud_offset(p4dp, address);
	pmdp = pmd_offset(pudp, address);

	if (pmd_large(*pmdp))
		ptep = split_large_pmd(info, pmdp, address);
	else
		ptep = pte_offset_kernel(pmdp, address);

	if (!ptep)
		return -ENOMEM;

	/* Clear encryption flag and write new pte */
	pte = pte_clear_flags(*ptep, _PAGE_ENC);
	set_pte(ptep, pte);

	/* Flush TLB to map the page unencrypted */
	write_cr3(top_level_pgt);

	/*
	 * Changing encryption attributes of a page requires to flush it from
	 * the caches.
	 */
	clflush_page(address);

	return 0;
}

int set_page_decrypted(unsigned long address)
{
	return __set_page_decrypted(&mapping_info, address);
}

static void pf_error(unsigned long error_code, unsigned long address,
		     struct pt_regs *regs)
{
	error_putstr("Unexpected page-fault:");
	error_putstr("\nError Code: ");
	error_puthex(error_code);
	error_putstr("\nCR2: 0x");
	error_puthex(address);
	error_putstr("\nRIP relative to _head: 0x");
	error_puthex(regs->ip - (unsigned long)_head);
	error_putstr("\n");

	error("Stopping.\n");
}

void do_boot_page_fault(struct pt_regs *regs)
{
	unsigned long address = native_read_cr2() & PMD_MASK;
	unsigned long end = address + PMD_SIZE;
	unsigned long error_code = regs->orig_ax;

	/*
	 * Check for unexpected error codes. Unexpected are:
	 *	- Faults on present pages
	 *	- User faults
	 *	- Reserved bits set
	 */
	if (error_code & (X86_PF_PROT | X86_PF_USER | X86_PF_RSVD))
		pf_error(error_code, address, regs);

	/*
	 * Error code is sane - now identity map the 2M region around
	 * the faulting address.
	 */
	add_identity_map(address, end);
}
