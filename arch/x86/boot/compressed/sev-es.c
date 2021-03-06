// SPDX-License-Identifier: GPL-2.0
/*
 * AMD Encrypted Register State Support
 *
 * Author: Joerg Roedel <jroedel@suse.de>
 */

#undef CONFIG_FUNCTION_TRACER
#undef CONFIG_STACK_TRACER
#undef CONFIG_DYNAMIC_FTRACE

#include <linux/kernel.h>

#include <asm/sev-es.h>
#include <asm/trap_defs.h>
#include <asm/msr-index.h>
#include <asm/fpu/xcr.h>
#include <asm/ptrace.h>
#include <asm/svm.h>

#include "misc.h"

struct ghcb boot_ghcb_page __aligned(PAGE_SIZE);
struct ghcb *boot_ghcb;

static inline u64 sev_es_rd_ghcb_msr(void)
{
	unsigned long low, high;

	asm volatile("rdmsr\n" : "=a" (low), "=d" (high) :
			"c" (MSR_AMD64_SEV_ES_GHCB));

	return ((high << 32) | low);
}

static inline void sev_es_wr_ghcb_msr(u64 val)
{
	u32 low, high;

	low  = val & 0xffffffffUL;
	high = val >> 32;

	asm volatile("wrmsr\n" : : "c" (MSR_AMD64_SEV_ES_GHCB),
			"a"(low), "d" (high) : "memory");
}

static enum es_result vc_fetch_insn_byte(struct es_em_ctxt *ctxt,
					 unsigned int offset,
					 char *buffer)
{
	char *rip = (char *)ctxt->regs->ip;

	buffer[offset] = rip[offset];

	return ES_OK;
}

static enum es_result vc_write_mem(struct es_em_ctxt *ctxt,
				   void *dst, char *buf, size_t size)
{
	memcpy(dst, buf, size);

	return ES_OK;
}

static enum es_result vc_read_mem(struct es_em_ctxt *ctxt,
				  void *src, char *buf, size_t size)
{
	memcpy(buf, src, size);

	return ES_OK;
}

static phys_addr_t vc_slow_virt_to_phys(struct ghcb *ghcb, long vaddr)
{
	return (phys_addr_t)vaddr;
}

#undef __init
#undef __pa
#define __init
#define __pa(x)	((unsigned long)(x))

#define __BOOT_COMPRESSED

/* Basic instruction decoding support needed */
#include "../../lib/inat.c"
#include "../../lib/insn.c"

/* Include code for early handlers */
#include "../../kernel/sev-es-shared.c"

static bool sev_es_setup_ghcb(void)
{
	if (!sev_es_negotiate_protocol())
		sev_es_terminate(GHCB_SEV_ES_REASON_PROTOCOL_UNSUPPORTED);

	if (set_page_decrypted((unsigned long)&boot_ghcb_page))
		return false;

	/* Page is now mapped decrypted, clear it */
	memset(&boot_ghcb_page, 0, sizeof(boot_ghcb_page));

	boot_ghcb = &boot_ghcb_page;

	/* Initialize lookup tables for the instruction decoder */
	inat_init_tables();

	return true;
}

void boot_vc_handler(struct pt_regs *regs, unsigned long exit_code)
{
	struct es_em_ctxt ctxt;
	enum es_result result;

	if (!boot_ghcb && !sev_es_setup_ghcb())
		sev_es_terminate(GHCB_SEV_ES_REASON_GENERAL_REQUEST);

	vc_ghcb_invalidate(boot_ghcb);
	result = vc_init_em_ctxt(&ctxt, regs, exit_code);
	if (result != ES_OK)
		goto finish;

	switch (exit_code) {
	case SVM_EXIT_IOIO:
		result = vc_handle_ioio(boot_ghcb, &ctxt);
		break;
	case SVM_EXIT_CPUID:
		result = vc_handle_cpuid(boot_ghcb, &ctxt);
		break;
	case SVM_EXIT_NPF:
		result = vc_handle_mmio(boot_ghcb, &ctxt);
		break;
	default:
		result = ES_UNSUPPORTED;
		break;
	}

finish:
	if (result == ES_OK) {
		vc_finish_insn(&ctxt);
	} else if (result != ES_RETRY) {
		/*
		 * For now, just halt the machine. That makes debugging easier,
		 * later we just call sev_es_terminate() here.
		 */
		while (true)
			asm volatile("hlt\n");
	}
}
