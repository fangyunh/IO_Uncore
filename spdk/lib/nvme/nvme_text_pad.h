/*   SPDX-License-Identifier: BSD-3-Clause
 *   IAU-next P4.5 (the host twin of spdk/lib/nvmf/vfio_user_text_pad.h, P4.4 review I1): NVME_TEXT_PAD64() pads
 *   this object's .text to a multiple of 64 B, so every object linked after it in libspdk_nvme, and every library
 *   linked after libspdk_nvme (in nvmf_tgt: the reactor, thread and bdev code), keeps its 64 B phase.  Use it once
 *   per translation unit, at file scope, and keep the object's other text sections (.text.unlikely, .text.hot,
 *   .text.startup, .text.exit: the linker places them before every object's .text) unchanged or absent:
 *   tests/realcore/test_pcie_hot_layout.sh checks both.  GCC emits top-level asm before the functions
 *   (-ftoplevel-reorder), so the begin label sits at offset 0 of .text; the padding goes to .text subsection 1,
 *   which the assembler appends after all of the compiler's code (subsection 0) and sizes at the end of assembly.
 *   The code itself is unchanged.  It needs one .text per object: the lean build has no -ffunction-sections and
 *   no LTO (the layout guard refuses a lean tree configured with either).  Other compilers: no padding.
 */
#ifndef SPDK_NVME_TEXT_PAD_H
#define SPDK_NVME_TEXT_PAD_H

#if defined(__GNUC__) && !defined(__clang__)
#define NVME_TEXT_PAD64() \
	__asm__(".pushsection .text,\"ax\",@progbits\n.Lnvme_text_pad64_begin:\n\t.popsection\n" \
		".pushsection .text,\"ax\",@progbits\n\t.subsection 1\n" \
		"\t.skip (-(. - .Lnvme_text_pad64_begin)) & 63, 0xcc\n\t.popsection")
/* IAU-next P4.8: the same for an existing object whose .text a change moved: pads it to `phase` modulo 64 (the
 * object's footprint modulo 64 before the change), so everything linked after it keeps its 64 B phase */
#define NVME_TEXT_PHASE64(phase) \
	__asm__(".pushsection .text,\"ax\",@progbits\n.Lnvme_text_phase64_begin:\n\t.popsection\n" \
		".pushsection .text,\"ax\",@progbits\n\t.subsection 1\n" \
		"\t.skip ((" #phase ") - (. - .Lnvme_text_phase64_begin)) & 63, 0xcc\n\t.popsection")
/* IAU-next P4.9: NVME_TEXT_PAD64() for a named code section of this object (sec, a string literal, e.g.
 * ".text.nvme_iau_hook"): the functions placed there with __attribute__((section(sec))) are padded to a multiple of
 * 64 B, so a section new to an existing object adds 0 mod 64 to the objects linked after it. Once per translation
 * unit */
#define NVME_TEXT_PAD64_SEC(sec) \
	__asm__(".pushsection " sec ",\"ax\",@progbits\n.Lnvme_text_pad64_sec_begin:\n\t.popsection\n" \
		".pushsection " sec ",\"ax\",@progbits\n\t.subsection 1\n" \
		"\t.skip (-(. - .Lnvme_text_pad64_sec_begin)) & 63, 0xcc\n\t.popsection")
#else
#define NVME_TEXT_PAD64() struct nvme_text_pad64_unused
#define NVME_TEXT_PHASE64(phase) struct nvme_text_phase64_unused
#define NVME_TEXT_PAD64_SEC(sec) struct nvme_text_pad64_sec_unused
#endif

#endif /* SPDK_NVME_TEXT_PAD_H */
